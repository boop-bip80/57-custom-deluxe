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
        self.clean = self._remove_grid()
        h, w = d.shape
        self.valid = np.ones((h, w), bool)
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
        """Pixels that belong to a printed grid line: for every detected line the pixel columns/rows within 2 px of its
        centre whose darkness along the line (median over the rows where the line itself is dark) exceeds 0.2.
        Crisp 1-px lines (ECC83S) therefore mask exactly one pixel, softer lines two or three."""
        m = np.zeros(self.d.shape, bool)
        x0, y0, x1, y1 = self.frame
        ya, yb, xa, xb = y0 - 3, y1 + 3, x0 - 3, x1 + 3
        for p in self.gx:
            c0 = int(round(p))
            rows = np.nonzero(self.d[ya:yb + 1, c0] > 0.5)[0] + ya
            for c in range(c0 - 2, c0 + 3):
                if len(rows) and np.median(self.d[rows, c]) > 0.2:
                    m[ya:yb + 1, c] = True
        for p in self.gy:
            r0 = int(round(p))
            cols = np.nonzero(self.d[r0, xa:xb + 1] > 0.5)[0] + xa
            for r in range(r0 - 2, r0 + 3):
                if len(cols) and np.median(self.d[r, cols]) > 0.2:
                    m[r, xa:xb + 1] = True
        return m

    def _remove_grid(self, evidence=0.35):
        """Grid-free darkness image.  A grid pixel keeps its ink only if the pixel just outside the grid line (on either
        side, perpendicular to it) is dark as well, i.e. a curve really crosses the line there; otherwise it is the
        grid line alone and is cleared.  Where a curve runs along a line for several pixels the evidence is absent and
        the curve is lost for that stretch; the tracker bridges such stretches."""
        out = self.d.copy()
        m = self.mask
        h, w = m.shape
        # vertical-line pixels (columns), then horizontal-line pixels (rows); crossings are handled by both passes
        for axis in (1, 0):
            mm = m if axis == 1 else m.T
            dd = self.d if axis == 1 else self.d.T
            oo = out if axis == 1 else out.T
            for r in range(mm.shape[0]):
                row = mm[r]
                for a, b in runs(row):
                    # a band of masked pixels along this axis; only treat bands narrower than 6 px as line bands
                    if b - a + 1 > 5 or a < 1 or b > len(row) - 2:
                        continue
                    ev = max(dd[r, a - 1], dd[r, b + 1])
                    for c in range(a, b + 1):
                        oo[r, c] = min(oo[r, c], ev) if ev > evidence else 0.0
        # grid-line intersections (masked in both directions) were skipped by both passes: no curve evidence there
        xs = np.zeros(m.shape[1], bool)
        ys = np.zeros(m.shape[0], bool)
        xs[:] = m.sum(axis=0) > 0.5 * h
        ys[:] = m.sum(axis=1) > 0.5 * w
        out[np.ix_(ys, xs)] = 0.0
        return out

    # data <-> pixel
    def pix(self, x, y):
        return self.xcal.to_pix(x), self.ycal.to_pix(y)

    def val(self, px, py):
        return self.xcal.to_val(px), self.ycal.to_val(py)


# ------------------------------------------------------------------------------------------ ridge tracker
def rasterize_regions(shape, regions):
    """Boolean image of pixel regions to ignore (label text, legend, leader lines).  A region is
    ('rect', x0, y0, x1, y1) or ('rot', cx, cy, length, width, angle_deg): a rectangle rotated by angle (image coords)."""
    img = Image.new("L", (shape[1], shape[0]), 0)
    dr = ImageDraw.Draw(img)
    for r in regions:
        if r[0] == "rect":
            dr.rectangle([r[1], r[2], r[3], r[4]], fill=1)
        else:
            _, cx, cy, ln, wd, ang = r
            a = math.radians(ang)
            ux, uy = math.cos(a), math.sin(a)
            vx, vy = -uy, ux
            pts = [(cx + sx * ln / 2 * ux + sy * wd / 2 * vx, cy + sx * ln / 2 * uy + sy * wd / 2 * vy)
                   for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            dr.polygon(pts, fill=1)
    return np.asarray(img, bool)


def _bilinear(img, xs, ys):
    return ndimage.map_coordinates(img, [ys, xs], order=1, mode="nearest")


class Tracer:
    """Follows one curve through the pixel image.  Grid pixels (and any extra ignore rectangles) are removed from
    the perpendicular darkness profile by normalised convolution, so a printed grid of the same ink as the curve
    does not influence the centroid."""

    def __init__(self, panel, ignore=()):
        self.p = panel
        valid = panel.valid & ~rasterize_regions(panel.d.shape, ignore)
        self.num = panel.clean * valid
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

    def measure(self, q, t, gate, min_peak=0.40, max_masked=0.30, max_width=3.8, max_tilt=0.75):
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
        if (hi - lo) * 0.25 > max_width:                     # too wide to be a single curve (text, merged crossing lines)
            return None
        cen = float((w * offs[lo:hi + 1]).sum() / w.sum())
        # orientation test: the ridge ahead of and behind the sample must be parallel to the predicted path, otherwise
        # this is a different curve crossing ours at an angle (a solid curve crossing a dashed one, for example)
        pa = self._peak_near(q - 1.5 * t, t, n, cen)
        pb = self._peak_near(q + 1.5 * t, t, n, cen)
        if pa is not None and pb is not None and abs(pa - pb) > max_tilt:
            return None
        return cen, float(h)

    def _peak_near(self, q, t, n, o0, tol=1.25):
        offs, prof, _den = self.profile(q, t, n, half=3.0, tw=(-0.5, 0.0, 0.5))
        pr = np.where(np.isnan(prof), 0.0, prof)
        sel = np.abs(offs - o0) <= tol
        if not sel.any() or pr[sel].max() < 0.35:
            return None
        return float(offs[sel][np.argmax(pr[sel])])

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

    def follow(self, start, t0, step=1.0, max_gap=30.0, fit_n=14, gate0=1.0, gate_max=2.6):
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
            gate = min(gate0 + 0.06 * gap, gate_max)
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
        return self._cut_inconsistent(path)

    @staticmethod
    def _cut_inconsistent(path, long_gap=12, lead=12):
        """After a long gap (label text, dash gap, crossing) the ridge may have been re-acquired on a different curve.
        Keep the continuation only if it is on the extrapolated line (offset) and parallel to it (angle); otherwise
        cut the path at the gap."""
        P = np.array([(x, y) for x, y, _ in path])
        ok = np.array([f for _, _, f in path])
        idx = np.nonzero(ok)[0]
        for k in range(1, len(idx)):
            gap = idx[k] - idx[k - 1] - 1
            if gap <= long_gap:
                continue
            before = idx[max(0, k - lead):k]
            after = idx[k:k + lead]
            if len(before) < 5:
                continue
            tb = P[before[-1]] - P[before[0]]
            tb /= np.hypot(*tb)
            jump = P[after[0]] - P[before[-1]]
            perp = abs(tb[0] * jump[1] - tb[1] * jump[0])
            if len(after) >= 5:
                ta = P[after[-1]] - P[after[0]]
                ta /= np.hypot(*ta)
                ang = abs(math.degrees(math.atan2(tb[0] * ta[1] - tb[1] * ta[0], float(tb @ ta))))
            else:
                ang = 0.0
            if perp > 1.5 + 0.03 * gap or ang > 10.0:
                return path[:idx[k - 1] + 1]
        return path

    def trace(self, seed_px, **kw):
        q = self.snap(np.asarray(seed_px, float))
        t = self.tangent_at(q)
        fwd = self.follow(q, t, **kw)
        bwd = self.follow(q, -t, **kw)
        pts = bwd[::-1] + fwd[1:]
        return [(x, y) for (x, y, ok) in pts if ok]


# ------------------------------------------------------------------------------------------ smoothing / output
def smooth_curve(pts, sigma_px=0.2, step_px=1.0, max_bridge_px=5.0):
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


def draw_overlay(panel, curves, path, scale=4, pad=26):
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
            dr.ellipse([cx - 2.0, cy - 2.0, cx + 2.0, cy + 2.0], fill=col)
        if len(pts):
            mid = pts[len(pts) // 2]
            dr.text(((mid[0] - box[0]) * scale + 6, (mid[1] - box[1]) * scale - 12), label, fill=col)
    crop.save(path, optimize=True)


# ------------------------------------------------------------------------------------------ curve discovery
def path_length(pts):
    P = np.asarray(pts, float)
    return float(np.hypot(*np.diff(P, axis=0).T).sum()) if len(P) > 1 else 0.0


def _mark(covered, pts, max_link=35.0):
    """mark pixels within 2 px of the traced polyline (links consecutive samples unless a long gap separates them)"""
    P = np.asarray(pts, float)
    for (x, y), (u, v) in zip(P[:-1], P[1:]):
        n = max(int(math.hypot(u - x, v - y)), 1)
        if n > max_link:
            n = 1
        for k in range(n + 1):
            cx, cy = x + (u - x) * k / n, y + (v - y) * k / n
            covered[max(int(cy) - 2, 0):int(cy) + 3, max(int(cx) - 2, 0):int(cx) + 3] = True


def discover(panel, tracer, scan_axis, levels_px, lo_px, hi_px, min_len_px, min_density=0.45, region=None, **trace_kw):
    """Find curves without hand-placed seeds: scan the interior along many lines (rows if scan_axis == 'h', columns if 'v'),
    start the tracker at every dark run that is not already covered by a traced curve, keep traces that are long
    enough and mostly made of real ridge samples (min_density = measured samples per pixel of path; text strokes
    and hops between curves fail this).  Assignment of parameter labels is done by the caller from the ordering of
    the result."""
    covered = np.zeros(panel.d.shape, bool)
    found = []
    d = tracer.num
    for lv in levels_px:
        line = d[int(round(lv)), :] if scan_axis == "h" else d[:, int(round(lv))]
        for a, b in runs(line > 0.4):
            if not (lo_px < a and b < hi_px) or b - a + 1 > 4:
                continue
            c = (a + b) / 2.0
            seed = (c, lv) if scan_axis == "h" else (lv, c)
            sx, sy = int(round(seed[0])), int(round(seed[1]))
            if covered[sy, sx]:
                continue
            if region is not None and not (region[0] <= seed[0] <= region[2] and region[1] <= seed[1] <= region[3]):
                continue
            pts = tracer.trace(seed, **trace_kw)
            if len(pts) < 8:
                continue
            _mark(covered, pts)
            L = path_length(pts)
            if L >= min_len_px and len(pts) / L >= min_density:
                found.append(pts)
    return dedupe(found)


def _dense(pts, max_link=35.0):
    P = np.asarray(pts, float)
    out = [P[:1]]
    for a, b in zip(P[:-1], P[1:]):
        n = max(int(math.hypot(*(b - a)) * 2), 1)
        if n > max_link * 2:
            n = 1
        out.append(a + (b - a) * (np.arange(1, n + 1) / n)[:, None])
    return np.vstack(out)


def dedupe(traces, radius=2.0, frac=0.5):
    """Drop a trace when more than `frac` of its samples lie within `radius` px of an already kept (longer) trace."""
    from scipy.spatial import cKDTree
    kept, trees = [], []
    for pts in sorted(traces, key=path_length, reverse=True):
        P = np.asarray(pts, float)
        dup = any((tr.query(P)[0] < radius).mean() > frac for tr in trees)
        if not dup:
            kept.append(pts)
            trees.append(cKDTree(_dense(pts)))
    return kept


def eval_at(pts, axis, level, extrapolate_px=0.0):
    """position along the other axis where the polyline crosses axis-level (axis 'y': crossing row; 'x': crossing column).
    Up to extrapolate_px beyond the traced extent the end of the curve is extended linearly (last 6 samples)."""
    P = np.asarray(pts, float)
    a, b = (P[:, 1], P[:, 0]) if axis == "y" else (P[:, 0], P[:, 1])
    order = np.argsort(a)
    a, b = a[order], b[order]
    if a[0] <= level <= a[-1]:
        return float(np.interp(level, a, b))
    if level < a[0] - extrapolate_px or level > a[-1] + extrapolate_px:
        return None
    sl = slice(0, 6) if level < a[0] else slice(-6, None)
    k = np.polyfit(a[sl], b[sl], 1)
    return float(np.polyval(k, level))


# ------------------------------------------------------------------------------------------ sheet / panel configuration
def grid(v0, step, n):
    return dict(via="grid", v0=v0, step=step, n=n, n_grid=n)


def ticks(values):
    return dict(via="ticks", ticks_values=values, n_grid=None)


def lab(prefix, values, fmt="{:g}", suffix="V"):
    return [f"{prefix}={fmt.format(v)}{suffix}" for v in values]


# Every family is  dict(labels=[...], order=(axis, level), ...): curves are discovered by pixel analysis and the
# labels are assigned in the order in which the curves cross the stated line of the panel (axis 'y': the row at data
# value `level`; ascending x).  The number of discovered curves must equal the number of labels, and adjacent curves
# must not cross inside their common range, otherwise the run aborts.
SHEET_CONFIG = {
    "12AY7": dict(csv="12AY7_JJ_S21.csv", fig="Fig. 9", panels=[
        dict(name="transfer", xname="Ug [V]", yname="Ia [mA]", frame=(130, 178, 390, 497),
             x=grid(-8, 1, 9), y=grid(7, -1, 8), ignore=[("rect", 146, 206, 214, 248)],
             families=[dict(labels=lab("Ua", [300, 250, 200, 150, 100, 50]), order=("y", 1.0), min_len=90)]),
        dict(name="plate", xname="Ua [V]", yname="Ia [mA]", frame=(408, 181, 784, 497),
             x=grid(0, 50, 11), y=grid(7, -1, 8), ignore=[("rect", 424, 198, 486, 242)],
             families=[dict(labels=lab("Ug", [0] + [-k for k in range(1, 13)]), order=("y", 0.2), min_len=45)]),
    ]),
    "6V6": dict(csv="6V6_JJ_S22.csv", fig="Fig. 11", panels=[
        dict(name="transfer", xname="Ug [V]", yname="Ia [mA]", frame=(99, 179, 374, 516),
             x=grid(-30, 5, 7), y=grid(160, -20, 9), ignore=[("rect", 135, 211, 238, 253)],
             families=[dict(labels=lab("Ua", [200, 150, 100, 50]), order=("y", 10.0), min_len=90)]),
        dict(name="plate", xname="Ua [V]", yname="Ia or Ig [mA]", frame=(405, 179, 799, 515),
             x=grid(0, 50, 11), y=grid(160, -20, 9),
             ignore=[("rect", 583, 181, 655, 220), ("rect", 684, 208, 775, 254),
                     ("rot", 509, 441, 46, 14, -40), ("rect", 489, 460, 500, 490),
                     ("rect", 551, 462, 570, 480), ("rect", 545, 476, 570, 500)],
             families=[dict(labels=["Ug=+20V", "Ug=+10V", "Ug=0V"] + lab("Ug", [-10, -20, -30, -40, -50]), order=("y", 10.0), min_len=100,
                            select="rising", extra={"Ug=-10V": [(100.0, 5.5)], "Ug=-20V": [(189.0, 7.5)]}),
                       dict(labels=["Pa=14W"], order=("x", 300.0), min_len=300, select="falling")],
             seeded=[dict(label="Ig@Ug=+20V", seeds=[(160, 12.2), (250, 12.1), (330, 12.1), (94, 13.8)], bridge_px=8.0),
                     dict(label="Ig@Ug=+5V", seeds=[(150, 7.85), (230, 7.85), (290, 7.85), (380, 7.85)], bridge_px=8.0)]),
    ]),
    "12AX7": dict(csv="12AX7_JJ_S24.csv", fig="Fig. 10", panels=[
        dict(name="transfer", xname="Ug [V]", yname="Ia [mA]", frame=(117, 185, 393, 524),
             x=grid(-4, 0.2, 21), y=grid(6, -0.2, 31), ignore=[("rect", 135, 213, 233, 270)],
             families=[dict(labels=lab("Ua", [250, 200, 150, 100]), order=("y", 1.0), min_len=150)]),
        dict(name="plate", xname="Ua [V]", yname="Ia [mA]", frame=(412, 185, 812, 525),
             x=ticks([0, 50, 100, 150, 200, 250, 300, 350]), y=grid(6, -0.2, 31),
             ignore=[("rect", 709, 200, 803, 272)],
             families=[dict(labels=lab("Ug", [0, -0.5, -1, -1.5, -2, -2.5, -3, -3.5, -4]), order=("y", 0.37), min_len=60)]),
    ]),
}


# ------------------------------------------------------------------------------------------ family processing
def trace_family(panel, tracer, fam):
    """Discover the curves of one family, order them, verify, and return list of (label, pts)."""
    scan_axis = fam.get("scan", "h")
    step = fam.get("step", 6)
    if scan_axis == "h":
        levels = np.arange(panel.top + 3, panel.bot - 2, step)
        lo, hi = panel.left + 1, panel.right - 1
    else:
        levels = np.arange(panel.left + 3, panel.right - 2, step)
        lo, hi = panel.top + 1, panel.bot - 1
    found = discover(panel, tracer, scan_axis, levels, lo, hi, fam["min_len"],
                     min_density=fam.get("min_density", 0.45), region=fam.get("region"), **fam.get("trace", {}))
    sel = fam.get("select")
    if sel:
        found = [p for p in found if _rising(p) == (sel == "rising")]
    ax, lv = fam["order"]
    lvp = float(panel.ycal.to_pix(lv)) if ax == "y" else float(panel.xcal.to_pix(lv))
    keyed = []
    for pts in found:
        e = eval_at(pts, ax, lvp, extrapolate_px=fam.get('extrapolate_px', 15.0))
        if e is None:
            raise RuntimeError(f"{panel.name}: a discovered curve does not cross the ordering line {ax}={lv}")
        keyed.append((e, pts))
    keyed.sort(key=lambda t: t[0])
    labels = fam["labels"]
    if len(keyed) != len(labels):
        raise RuntimeError(f"{panel.name}: found {len(keyed)} curves, expected {len(labels)} ({labels})")
    curves = [(labels[k], pts) for k, (_e, pts) in enumerate(keyed)]
    check_non_crossing(panel, curves)
    return curves


def _rising(pts):
    P = np.asarray(pts, float)
    return (P[-1, 1] - P[0, 1]) * (P[-1, 0] - P[0, 0]) < 0 if P[-1, 0] != P[0, 0] else True


def check_non_crossing(panel, curves):
    """adjacent curves of a family must keep their order along their common extent (guards against mis-assigned labels)"""
    for (la, pa), (lb, pb) in zip(curves[:-1], curves[1:]):
        A, B = np.asarray(pa, float), np.asarray(pb, float)
        lo = max(A[:, 1].min(), B[:, 1].min())
        hi = min(A[:, 1].max(), B[:, 1].max())
        if hi - lo < 10:
            continue
        ys = np.linspace(lo + 2, hi - 2, 25)
        xa = np.array([eval_at(A, "y", y) for y in ys])
        xb = np.array([eval_at(B, "y", y) for y in ys])
        if not (np.all(xa < xb) or np.all(xa > xb)):
            raise RuntimeError(f"{panel.name}: curves {la} and {lb} cross; label assignment is not trustworthy")


def merge_traces(pieces, key=0):
    """Union of several traces of one curve, ordered along axis `key` (0: x, 1: y); samples of overlapping traces are
    averaged in 0.5 px bins."""
    P = np.vstack([np.asarray(p, float) for p in pieces])
    P = P[np.argsort(P[:, key])]
    bins = np.round(P[:, key] * 2.0) / 2.0
    return np.array([P[bins == b].mean(axis=0) for b in np.unique(bins)])


def trace_seeded(panel, tracer, spec):
    """Curves that cannot be discovered (curves broken by label text or crossing a dashed line): the tracer is started
    from one or more seeds (approximate data points on the curve, snapped to the nearest ridge) and the pieces merged."""
    pieces = [tracer.trace(panel.pix(sx, sy), **spec.get("trace", {})) for (sx, sy) in spec["seeds"]]
    return merge_traces(pieces, 0 if spec.get("sort", "x") == "x" else 1)


def process_panel(img_dir, sheet_key, cfg, pcfg, out_dir, overlay_dir):
    path = os.path.join(img_dir, SHEETS[sheet_key][0])
    d = load_dark(path)
    panel = Panel(d, path, f"{sheet_key}.{pcfg['name']}", pcfg["frame"], pcfg["x"], pcfg["y"])
    tracer = Tracer(panel, ignore=pcfg.get("ignore", ()))
    curves = []                                          # (label, raw pts, family dict)
    for fam in pcfg.get("families", ()):
        for label, pts in trace_family(panel, tracer, fam):
            if label in fam.get("extra", {}):                # seeds for stretches the discovery cut at a crossing
                more = [tracer.trace(panel.pix(sx, sy)) for (sx, sy) in fam["extra"][label]]
                pts = merge_traces([pts] + more, fam.get("sort", 0))
            curves.append((label, pts, fam))
    for spec in pcfg.get("seeded", ()):
        tr = Tracer(panel, ignore=list(pcfg.get("ignore", ())) + list(spec.get("ignore", ())))
        curves.append((spec["label"], trace_seeded(panel, tr, spec), spec))
    rows, drawn, stats = [], [], []
    for label, pts, fam in curves:
        sm, st = smooth_curve(pts, sigma_px=fam.get("sigma_px", 0.2), max_bridge_px=fam.get("bridge_px", 5.0))
        if len(sm) < 40:
            raise RuntimeError(f"{panel.name} {label}: only {len(sm)} output points (< 40)")
        xs, ys = panel.val(sm[:, 0], sm[:, 1])
        rows += [(pcfg["name"], label, float(x), float(y)) for x, y in zip(xs, ys)]
        drawn.append((label, sm))
        st.update(parameter=label, x_range=[float(xs.min()), float(xs.max())], y_range=[float(ys.min()), float(ys.max())])
        stats.append(st)
    os.makedirs(overlay_dir, exist_ok=True)
    draw_overlay(panel, drawn, os.path.join(overlay_dir, f"{SHEETS[sheet_key][0][:-4]}_{pcfg['name']}.png"))
    cal = dict(x=panel.xcal.report(), y=panel.ycal.report())
    return rows, dict(calibration=cal, curves=stats), panel


def write_csv(path, cfg, sheet_key, panels_cfg, rows):
    with open(path, "w", newline="") as f:
        f.write(f"# DIGITIZED from dossier {cfg['fig']} (JJ {sheet_key} sheet, source {SHEETS[sheet_key][1]}) by scripts/digitize_jj_sheets.py\n")
        for pc in panels_cfg:
            f.write(f"# panel {pc['name']}: x = {pc['xname']}, y = {pc['yname']}\n")
        w = csv.writer(f)
        w.writerow(["panel", "parameter", "x", "y"])
        for r in rows:
            w.writerow([r[0], r[1], f"{r[2]:.5g}", f"{r[3]:.5g}"])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--img-dir", default=IMG_DIR_DEFAULT)
    ap.add_argument("--out-dir", default=OUT_DIR_DEFAULT)
    ap.add_argument("--only", default=None, help="digitise one sheet only (12AY7, 12AX7, 6V6, 5Y3)")
    a = ap.parse_args(argv)
    report = {}
    for sheet_key, cfg in SHEET_CONFIG.items():
        if a.only and a.only != sheet_key:
            continue
        os.makedirs(a.out_dir, exist_ok=True)
        rows, rep = [], {}
        for pcfg in cfg["panels"]:
            r, pr, _panel = process_panel(a.img_dir, sheet_key, cfg, pcfg, a.out_dir, os.path.join(a.out_dir, "overlays"))
            rows += r
            rep[pcfg["name"]] = pr
            c = pr["calibration"]
            print(f"{sheet_key}.{pcfg['name']:<10} x: {c['x']['n_lines']:2d} lines max res {c['x']['max_residual_px']:.2f} px "
                  f"({c['x']['max_residual_data_units']:.3g} units)   y: {c['y']['n_lines']:2d} lines max res "
                  f"{c['y']['max_residual_px']:.2f} px ({c['y']['max_residual_data_units']:.3g} units)   curves {len(pr['curves'])}")
        write_csv(os.path.join(a.out_dir, cfg["csv"]), cfg, sheet_key, cfg["panels"], rows)
        report[sheet_key] = rep
    with open(os.path.join(a.out_dir, "digitization_report.json"), "w") as f:
        json.dump(report, f, indent=1)


if __name__ == "__main__":
    main()
