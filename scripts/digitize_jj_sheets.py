#!/usr/bin/env python3
"""
Pixel-level digitisation of the four JJ characteristic sheets reproduced in the dossier
(Fig. 9 12AY7 / S21, Fig. 10 ECC83S / S24, Fig. 11 6V6S / S22, Fig. 12 5Y3S / S23).

Pipeline (re-runnable end to end:  python3 scripts/digitize_jj_sheets.py)

 1. Axis calibration, programmatic.  Every panel's grid lines (and, where the sheet prints them, the
    axis tick marks) are located at sub-pixel precision from the image.  The k-th line is assigned the
    value  v0 + k*step  and a straight line  pixel = a*value + b  is fitted by least squares per axis.
    The residual of every line is reported in pixels and in data units.  (Tick marks are used where the
    printed grid is not aligned with the labelled values: ECC83S plate-panel x axis.)
 2. Curve extraction.  Each curve is entered through a seed (an approximate point read from the label order
    on the sheet), snapped to the nearest ridge, then followed in both directions by a ridge tracker that
    works in the pixel image: perpendicular darkness profile, sub-pixel centroid, tangent from the last
    samples.  Pixels lying on the (calibrated, known) printed grid lines are removed from the profile
    (normalised convolution), so the dense ECC83S minor grid -- the same colour as the curves -- does not
    capture the tracker.  Label text and dashes are bridged by polynomial extrapolation and re-acquired
    only if the ridge re-appears on the predicted path.  Traced samples are smoothed with a spline whose
    residual (pixels) is reported per curve; outliers are rejected.
 3. Outputs: dense CSVs  data/tubes/digitized/<tube>_JJ_<S>.csv  (panel,parameter,x,y),
    overlay PNGs of the traced points drawn over the original image, and digitization_report.json
    (calibration residuals, per-curve statistics, Pa=14 W hyperbola check).

Evidence label for everything written here: DIGITIZED (read from the dossier's embedded manufacturer
figure).  Nothing here is measured on a specimen.
"""
import argparse
import csv
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy import interpolate, ndimage, signal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG_DIR_DEFAULT = os.path.join(ROOT, "data", "tubes", "source_images")
OUT_DIR_DEFAULT = os.path.join(ROOT, "data", "tubes", "digitized")

SHEETS = {
    "12AY7": ("fig09_JJ_12AY7_S21.png", "S21"),
    "12AX7": ("fig10_JJ_ECC83S_S24.png", "S24"),
    "6V6":   ("fig11_JJ_6V6S_S22.png", "S22"),
    "5Y3":   ("fig12_JJ_5Y3S_S23.png", "S23"),
}


# ------------------------------------------------------------------------------------------ image helpers
def load_dark(path):
    g = np.asarray(Image.open(path).convert("L"), dtype=float)
    return 1.0 - g / 255.0


def runs(mask):
    out, i, n = [], 0, len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j + 1 < n and mask[j + 1]:
                j += 1
            out.append((i, j))
            i = j + 1
        else:
            i += 1
    return out


# ------------------------------------------------------------------------------------------ line detection
def detect_lines(d, frame, orient, margin=3, min_frac=0.40, expect=None):
    """Sub-pixel positions of long dark straight lines inside the frame bbox (x0,y0,x1,y1).
    orient 'v' -> vertical lines (x positions), 'h' -> horizontal lines (y positions)."""
    x0, y0, x1, y1 = frame
    X0, X1, Y0, Y1 = x0 - margin, x1 + margin, y0 - margin, y1 + margin
    roi = d[Y0:Y1 + 1, X0:X1 + 1]
    if orient == "h":
        roi = roi.T                       # lines now run along axis 0 in both cases
        lo = Y0
    else:
        lo = X0
    frac = (roi > 0.45).mean(axis=0)      # fraction of the line length that is dark, per cross-line position
    cand = [i for i in range(1, len(frac) - 1) if frac[i] >= min_frac and frac[i] >= frac[i - 1] and frac[i] > frac[i + 1]]
    # a printed grid line has one long unbroken dark run; a dashed curve only has short dashes
    cand = [c for c in cand if max((b - a + 1 for a, b in runs(roi[:, c] > 0.45)), default=0) >= 0.15 * roi.shape[0]]
    pos = []
    for c in cand:
        w = roi[:, max(0, c - 3):c + 4]
        cols = np.arange(max(0, c - 3), min(len(frac), c + 4))
        rowsum = w.sum(axis=1)
        cen = (w * cols[None, :]).sum(axis=1) / np.maximum(rowsum, 1e-9)
        good = (roi[:, c] > 0.5)
        if good.sum() < 5:
            continue
        med = np.median(rowsum[good])
        good &= (rowsum > 0.75 * med) & (rowsum < 1.3 * med)
        pos.append(float(np.median(cen[good])) + lo)
    if expect is not None and len(pos) != expect:
        raise RuntimeError(f"expected {expect} {orient}-lines in {frame}, found {len(pos)}: {np.round(pos, 1)}")
    return pos


def detect_ticks(d, frame, side):
    """Axis tick marks drawn just outside the frame (ECC83S).  side 'bottom' -> x positions, 'left' -> y positions."""
    x0, y0, x1, y1 = frame
    if side == "bottom":
        strip = d[y1 + 2:y1 + 4, x0 - 4:x1 + 5].mean(axis=0)
        off = x0 - 4
    else:
        strip = d[y0 - 4:y1 + 5, x0 - 3:x0 - 1].mean(axis=1)
        off = y0 - 4
    out = []
    for a, b in runs(strip > 0.35):
        idx = np.arange(a, b + 1)
        out.append(float((strip[idx] * idx).sum() / strip[idx].sum()) + off)
    return out


class AxisCal:
    """pixel = a*value + b, fitted from matched (pixel, value) pairs."""

    def __init__(self, pix, val, name):
        pix, val = np.asarray(pix, float), np.asarray(val, float)
        A = np.vstack([val, np.ones_like(val)]).T
        (self.a, self.b), *_ = np.linalg.lstsq(A, pix, rcond=None)
        res = pix - (self.a * val + self.b)
        self.name = name
        self.n = len(pix)
        self.res_px = res
        self.max_res_px = float(np.max(np.abs(res)))
        self.max_res_val = float(np.max(np.abs(res)) / abs(self.a))
        self.rms_res_px = float(np.sqrt(np.mean(res ** 2)))

    def to_pix(self, v):
        return self.a * np.asarray(v, float) + self.b

    def to_val(self, p):
        return (np.asarray(p, float) - self.b) / self.a

    def report(self):
        return dict(axis=self.name, n_lines=self.n, px_per_unit=float(self.a), origin_px=float(self.b),
                    max_residual_px=self.max_res_px, rms_residual_px=self.rms_res_px,
                    max_residual_data_units=self.max_res_val)


# ------------------------------------------------------------------------------------------ panel
class Panel:
    def __init__(self, d, img_path, name, frame, xspec, yspec):
        """xspec/yspec: dict(v0=, step=, n=, via='grid'|'ticks', ticks_values=[...])"""
        self.d, self.name, self.frame = d, name, frame
        self.img_path = img_path
        x0, y0, x1, y1 = frame
        self.gx = detect_lines(d, frame, "v", expect=xspec.get("n_grid"))
        self.gy = detect_lines(d, frame, "h", expect=yspec.get("n_grid"))
        self.xcal = self._cal(xspec, self.gx, "bottom", "x")
        self.ycal = self._cal(yspec, self.gy, "left", "y")
        self.mask = self._gridmask()
        h, w = d.shape
        self.valid = np.ones((h, w), bool)
        self.valid[self.mask] = False
        self.left, self.right = min(self.gx), max(self.gx)
        self.top, self.bot = min(self.gy), max(self.gy)

    def _cal(self, spec, lines, side, ax):
        if spec["via"] == "grid":
            vals = spec["v0"] + spec["step"] * np.arange(len(lines))
            if len(lines) != spec["n"]:
                raise RuntimeError(f"{self.name} {ax}: {len(lines)} lines, expected {spec['n']}")
            pix = lines
            if ax == "y":                      # image rows grow downward; values listed top -> bottom
                pass
            return AxisCal(pix, vals, f"{self.name}.{ax} (grid)")
        ticks = detect_ticks(self.d, self.frame, side)
        vals = spec["ticks_values"]
        if len(ticks) != len(vals):
            raise RuntimeError(f"{self.name} {ax}: {len(ticks)} ticks, expected {len(vals)}")
        return AxisCal(ticks, vals, f"{self.name}.{ax} (tick marks)")

    def _gridmask(self):
        m = np.zeros(self.d.shape, bool)
        h, w = self.d.shape
        for p in self.gx:
            lo, hi = int(math.floor(p - 1.0)), int(math.ceil(p + 1.0))
            m[:, max(lo, 0):min(hi + 1, w)] = True
        for p in self.gy:
            lo, hi = int(math.floor(p - 1.0)), int(math.ceil(p + 1.0))
            m[max(lo, 0):min(hi + 1, h), :] = True
        x0, y0, x1, y1 = self.frame
        keep = np.zeros_like(m)
        keep[y0 - 3:y1 + 4, x0 - 3:x1 + 4] = True
        return m & keep

    # data <-> pixel
    def pix(self, x, y):
        return float(self.xcal.to_pix(x)), float(self.ycal.to_pix(y))

    def val(self, px, py):
        return float(self.xcal.to_val(px)), float(self.ycal.to_val(py))


# ------------------------------------------------------------------------------------------ ridge tracker
def _bilinear(img, xs, ys):
    return ndimage.map_coordinates(img, [ys, xs], order=1, mode="nearest")


class Tracer:
    """Follows one curve through the pixel image.  Grid pixels (and any extra ignore rectangles) are removed from
    the perpendicular darkness profile by normalised convolution, so a printed grid of the same ink as the curve
    does not influence the centroid."""

    def __init__(self, panel, ignore=()):
        self.p = panel
        valid = panel.valid.copy()
        for (xa, ya, xb, yb) in ignore:                      # pixel rectangles (label text on top of a curve)
            valid[int(ya):int(yb) + 1, int(xa):int(xb) + 1] = False
        self.num = panel.d * valid
        self.den = valid.astype(float)
        x0, y0, x1, y1 = panel.frame
        self.box = (panel.left - 1.0, panel.top - 1.0, panel.right + 1.0, panel.bot + 1.0)

    # ---- perpendicular profile at point q with normal n, tangent t
    def profile(self, q, t, n, half=4.0, dt=0.25, tw=(-1.0, 0.0, 1.0)):
        offs = np.arange(-half, half + 1e-9, dt)
        num = np.zeros_like(offs)
        den = np.zeros_like(offs)
        for s in tw:
            xs = q[0] + s * t[0] + offs * n[0]
            ys = q[1] + s * t[1] + offs * n[1]
            num += _bilinear(self.num, xs, ys)
            den += _bilinear(self.den, xs, ys)
        prof = np.where(den > 0.5 * len(tw), num / np.maximum(den, 1e-9), np.nan)
        return offs, prof, den / len(tw)

    def measure(self, q, t, gate, min_peak=0.40, max_masked=0.30):
        """Return (offset, peak darkness) of the ridge nearest the prediction, or None."""
        n = np.array([-t[1], t[0]])
        offs, prof, den = self.profile(q, t, n)
        pr = np.where(np.isnan(prof), 0.0, prof)
        pk, _ = signal.find_peaks(pr, height=min_peak)
        best = None
        for i in pk:
            o = offs[i]
            if abs(o) <= gate and (best is None or abs(o) < abs(offs[best])):
                best = i
        if best is None:
            return None
        # sub-pixel centroid over the ridge (contiguous region above half height), reject if grid-masked on one side
        h = pr[best]
        lo = best
        while lo > 0 and pr[lo - 1] > 0.5 * h:
            lo -= 1
        hi = best
        while hi < len(pr) - 1 and pr[hi + 1] > 0.5 * h:
            hi += 1
        lo, hi = max(lo - 2, 0), min(hi + 2, len(pr) - 1)
        w = np.clip(pr[lo:hi + 1] - 0.25 * h, 0, None)
        if w.sum() <= 0:
            return None
        if np.mean(den[lo:hi + 1] < 0.999) > max_masked:     # ridge partly hidden by grid / ignore mask
            return None
        if (hi - lo) * 0.25 > 5.0:                           # too wide to be a single curve (text, merged lines)
            return None
        return float((w * offs[lo:hi + 1]).sum() / w.sum()), float(h)

    def tangent_at(self, q, search=6.0, length=14.0):
        """Direction of the dominant dark stroke through q (grid pixels excluded)."""
        best, ang_best = -1.0, 0.0
        for ang in np.linspace(0, math.pi, 180, endpoint=False):
            t = np.array([math.cos(ang), math.sin(ang)])
            ss = np.linspace(-length / 2, length / 2, 29)
            v = _bilinear(self.num, q[0] + ss * t[0], q[1] + ss * t[1]).sum()
            m = _bilinear(self.den, q[0] + ss * t[0], q[1] + ss * t[1]).sum()
            if m < 0.7 * len(ss):
                continue
            v /= m
            if v > best:
                best, ang_best = v, ang
        return np.array([math.cos(ang_best), math.sin(ang_best)])

    def snap(self, q, radius=4.0):
        """Move a seed to the darkest valid pixel centroid within the radius (tangent-agnostic)."""
        x, y = int(round(q[0])), int(round(q[1]))
        r = int(radius)
        sub = self.num[y - r:y + r + 1, x - r:x + r + 1]
        den = self.den[y - r:y + r + 1, x - r:x + r + 1]
        # blur lightly to avoid snapping onto isolated noise
        sub = ndimage.gaussian_filter(sub, 0.6)
        yy, xx = np.unravel_index(np.argmax(sub), sub.shape)
        return np.array([x - r + xx, y - r + yy], float)

    def follow(self, start, t0, step=1.0, max_gap=30.0, fit_n=14):
        """March from `start` along t0.  Returns list of (x, y, measured_flag)."""
        path = [(start[0], start[1], True)]
        meas = [(0.0, start[0], start[1])]                       # (path length, x, y) of measured samples
        s_total, gap = 0.0, 0
        t = np.array(t0, float)
        x0, y0, x1, y1 = self.box
        while True:
            # prediction from a local polynomial of the recent measured samples
            k = min(len(meas), fit_n)
            if k >= 6:
                S = np.array([m[0] for m in meas[-k:]])
                X = np.array([m[1] for m in meas[-k:]])
                Y = np.array([m[2] for m in meas[-k:]])
                deg = 2 if k >= 10 else 1
                cx, cy = np.polyfit(S - S[-1], X, deg), np.polyfit(S - S[-1], Y, deg)
                sn = s_total + step - S[-1]
                q = np.array([np.polyval(cx, sn), np.polyval(cy, sn)])
                tt = np.array([np.polyval(np.polyder(cx), sn), np.polyval(np.polyder(cy), sn)])
                nt = np.hypot(*tt)
                if nt > 1e-9:
                    t = tt / nt
            else:
                q = np.array([path[-1][0], path[-1][1]]) + t * step
            if not (x0 <= q[0] <= x1 and y0 <= q[1] <= y1):
                break
            gate = min(1.0 + 0.06 * gap, 2.6)
            m = self.measure(q, t, gate)
            s_total += step
            if m is None:
                gap += 1
                path.append((q[0], q[1], False))
                if gap > max_gap:
                    break
            else:
                n = np.array([-t[1], t[0]])
                p = q + m[0] * n
                path.append((p[0], p[1], True))
                meas.append((s_total, p[0], p[1]))
                gap = 0
        while path and not path[-1][2]:                          # drop trailing extrapolation
            path.pop()
        return path

    def trace(self, seed_px, **kw):
        q = self.snap(np.asarray(seed_px, float))
        t = self.tangent_at(q)
        fwd = self.follow(q, t, **kw)
        bwd = self.follow(q, -t, **kw)
        pts = bwd[::-1] + fwd[1:]
        return [(x, y) for (x, y, ok) in pts if ok]


# ------------------------------------------------------------------------------------------ smoothing / output
def smooth_curve(pts, sigma_px=0.35, step_px=1.5, max_bridge_px=3.0):
    """pts: traced sample positions (pixels) in path order.  A smoothing spline x(s), y(s) (s = path length) is fitted
    with iterative 3-sigma outlier rejection; the curve is re-sampled every step_px, only where raw samples exist
    within max_bridge_px (dash gaps and label gaps are NOT filled).  Returns (resampled Nx2, stats)."""
    P = np.asarray(pts, float)
    seg = np.hypot(*np.diff(P, axis=0).T)
    s = np.concatenate([[0.0], np.cumsum(seg)])
    keep = np.ones(len(P), bool)
    for _ in range(4):
        sk = s[keep]
        # strictly increasing parameter
        ok = np.concatenate([[True], np.diff(sk) > 1e-6])
        sk, Pk = sk[ok], P[keep][ok]
        tck, _u = interpolate.splprep([Pk[:, 0], Pk[:, 1]], u=sk, s=len(sk) * sigma_px ** 2, k=3)
        fit = np.array(interpolate.splev(s, tck)).T
        res = np.hypot(*(fit - P).T)
        new_keep = res < max(3 * sigma_px, 3 * np.median(res[keep]) + 0.2)
        if new_keep.sum() == keep.sum():
            break
        keep = new_keep
    sk = s[keep]
    ok = np.concatenate([[True], np.diff(sk) > 1e-6])
    sk, Pk = sk[ok], P[keep][ok]
    tck, _u = interpolate.splprep([Pk[:, 0], Pk[:, 1]], u=sk, s=len(sk) * sigma_px ** 2, k=3)
    fit = np.array(interpolate.splev(s, tck)).T
    res = np.hypot(*(fit - P).T)
    grid = []
    for lo, hi in _segments(sk, max_bridge_px * 2.0 + 1.5):
        grid.extend(np.arange(lo, hi + 1e-9, step_px))
    out = np.array(interpolate.splev(np.array(grid), tck)).T if grid else np.zeros((0, 2))
    stats = dict(n_raw=int(len(P)), n_rejected=int((~keep).sum()), n_out=int(len(out)),
                 rms_residual_px=float(np.sqrt(np.mean(res[keep] ** 2))), max_residual_px=float(res[keep].max()))
    return out, stats


def _segments(s, max_gap):
    """contiguous ranges of the parameter s without a gap larger than max_gap"""
    segs, lo = [], s[0]
    for a, b in zip(s[:-1], s[1:]):
        if b - a > max_gap:
            segs.append((lo, a))
            lo = b
    segs.append((lo, s[-1]))
    return segs


PALETTE = [(220, 30, 30), (30, 120, 220), (20, 160, 60), (230, 130, 0), (150, 40, 190), (0, 170, 170),
           (200, 0, 130), (110, 110, 0), (90, 90, 90), (240, 60, 130), (0, 100, 0), (120, 70, 20), (0, 0, 160)]


def draw_overlay(panel, curves, path, scale=3, pad=26):
    """curves: list of (label, Nx2 pixel array).  Draws the traced samples over the original image."""
    base = Image.open(panel.img_path).convert("RGB")
    x0, y0, x1, y1 = panel.frame
    box = (x0 - pad, y0 - pad, x1 + pad, y1 + pad)
    crop = base.crop(box).resize(((box[2] - box[0]) * scale, (box[3] - box[1]) * scale), Image.LANCZOS)
    dr = ImageDraw.Draw(crop)
    for k, (label, pts) in enumerate(curves):
        col = PALETTE[k % len(PALETTE)]
        for (x, y) in pts:
            cx, cy = (x - box[0] + 0.5) * scale, (y - box[1] + 0.5) * scale
            dr.ellipse([cx - 1.4, cy - 1.4, cx + 1.4, cy + 1.4], fill=col)
        if len(pts):
            mid = pts[len(pts) // 2]
            dr.text(((mid[0] - box[0]) * scale + 6, (mid[1] - box[1]) * scale - 12), label, fill=col)
    crop.save(path, optimize=True)
