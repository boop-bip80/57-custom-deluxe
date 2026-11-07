#!/usr/bin/env python3
"""make_ui_textures.py -- renders the editor's raster artwork (original, procedural; no third-party images).

Everything is synthesised from height fields and noise with one consistent light from the top left
(L = (-0.45, -0.55, 0.70)) and written to plugin/assets/.  Sizes are 2x-3x the editor's base layout so the
editor stays sharp on HiDPI displays up to 150 % window scale.  Deterministic (fixed seeds).

    python3 tools/ui/make_ui_textures.py [outdir]
"""
import sys, os
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..', 'plugin', 'assets')
os.makedirs(OUT, exist_ok=True)
L = np.array([-0.45, -0.55, 0.70]); L /= np.linalg.norm(L)
H = L + np.array([0, 0, 1.0]); H /= np.linalg.norm(H)
rng = np.random.default_rng(1957)


def blur(a, r):
    if r <= 0: return a
    return gaussian_filter(np.asarray(a, dtype=np.float64), r)


def smooth_noise(h, w, scale, seed):
    g = np.random.default_rng(seed).standard_normal((max(2, h // scale + 2), max(2, w // scale + 2)))
    im = Image.fromarray(g.astype(np.float32), mode='F').resize((w, h), Image.BICUBIC)
    a = np.asarray(im, dtype=np.float64)
    return (a - a.mean()) / (a.std() + 1e-9)


def normals(hf, strength=1.0):
    gy, gx = np.gradient(hf * strength)
    n = np.dstack([-gx, -gy, np.ones_like(hf)])
    return n / np.linalg.norm(n, axis=2, keepdims=True)


def shade(n, albedo, amb=0.35, kd=0.75, ks=0.25, p=30.0, spec_col=(1, 1, 1)):
    d = np.clip(n @ L, 0, 1)[..., None]
    s = (np.clip(n @ H, 0, 1) ** p)[..., None]
    return albedo * (amb + kd * d) + ks * s * np.array(spec_col)


def save(rgb, name, alpha=None, jpeg=False):
    rgb = np.clip(rgb, 0, 1)
    if alpha is not None:
        a = np.clip(alpha, 0, 1)
        im = Image.fromarray((np.dstack([rgb, a]) * 255 + 0.5).astype(np.uint8), 'RGBA')
    else:
        im = Image.fromarray((rgb * 255 + 0.5).astype(np.uint8), 'RGB')
    path = os.path.join(OUT, name)
    if jpeg: im.save(path, quality=90, optimize=True, progressive=False)
    else: im.save(path, optimize=True)
    print('wrote', path, im.size)


def disc_mask(n, r, cx=None, cy=None, ss=1.0):
    cx = (n - 1) / 2 if cx is None else cx
    cy = (n - 1) / 2 if cy is None else cy
    y, x = np.mgrid[0:n, 0:n]
    d = np.hypot(x - cx, y - cy)
    return np.clip(r - d + 0.5, 0, 1), d, x - cx, y - cy


# ------------------------------------------------------------------------------------------- tweed (2200 x 720)
def tweed(W=2200, Hh=720, p=5):
    y, x = np.mgrid[0:Hh, 0:W].astype(np.float64)
    i, j = np.floor(x / p).astype(int), np.floor(y / p).astype(int)
    fx, fy = (x % p) / p, (y % p) / p
    warp_over = ((i + j) // 2) % 2 == 0          # 2/2 twill -> diagonal ribs
    # yarn profile: rounded across the yarn, slight bulge along it
    across = np.where(warp_over, np.sin(np.pi * fx), np.sin(np.pi * fy))
    along = np.where(warp_over, np.sin(np.pi * fy) ** 0.3, np.sin(np.pi * fx) ** 0.3)
    fib = blur(rng.standard_normal((Hh, W)), 0.6)
    hf = 1.6 * across * (0.7 + 0.3 * along) + 0.25 * fib
    # per-thread colour: warp light straw, weft darker tan (two-colour twill), each yarn slightly different
    col_w = 1 + 0.10 * rng.standard_normal(W // p + 2)
    row_w = 1 + 0.12 * rng.standard_normal(Hh // p + 2)
    warp = np.array([0.90, 0.84, 0.68]); weft = np.array([0.62, 0.47, 0.30])
    slub = 1 + 0.10 * smooth_noise(Hh, W, 3, 21)        # thick/thin yarn sections
    alb = np.where(warp_over[..., None], warp * col_w[i][..., None], weft * row_w[j][..., None])
    alb *= (slub * (1 + 0.06 * fib))[..., None]
    rgb = shade(normals(hf, 0.45), alb, amb=0.60, kd=0.48, ks=0.06, p=12)
    # lacquer: amber glaze, nicotine clouds, darker edges, worn highlights at the top corners
    glaze = np.array([1.0, 0.83, 0.58])
    nic = 0.5 + 0.5 * np.tanh(smooth_noise(Hh, W, 180, 3) * 0.8)
    rgb = rgb * glaze * (0.92 - 0.12 * nic[..., None])
    u, v = x / W, y / Hh
    edge = np.minimum.reduce([u, 1 - u, v * W / Hh, (1 - v) * W / Hh])
    rgb *= (0.62 + 0.38 * np.clip(edge / 0.10, 0, 1) ** 0.6)[..., None]
    wear = np.exp(-((u - 0.03) ** 2 + (v - 0.05) ** 2) / 0.004) + np.exp(-((u - 0.97) ** 2 + (v - 0.05) ** 2) / 0.004)
    wear *= 0.5 + 0.5 * (smooth_noise(Hh, W, 6, 9) > 0.3)
    rgb = rgb * (1 + 0.25 * wear[..., None])
    rgb += 0.06 * np.exp(-((u - 0.25) ** 2 / 0.05 + (v - 0.15) ** 2 / 0.08))[..., None] * np.array([1, 0.9, 0.7])  # soft sheen
    save(rgb, 'tweed.jpg', jpeg=True)


# ------------------------------------------------------------------------------------------- brushed plate (2024 x 436)
def plate(W=2024, Hh=436, rad=10):
    y, x = np.mgrid[0:Hh, 0:W].astype(np.float64)
    streak = rng.standard_normal((Hh, W))
    k = 160  # directional (anisotropic) brushing along x
    c = np.cumsum(np.pad(streak, ((0, 0), (k, 0)), mode='wrap'), axis=1)
    streak = (c[:, k:] - c[:, :-k]) / np.sqrt(k)
    streak = 0.65 * streak + 0.35 * blur(rng.standard_normal((Hh, W)), 0.5)
    base = 0.74 + 0.018 * streak
    v = y / Hh; u = x / W
    # anisotropic highlight: a soft horizontal band plus a broad diagonal reflection from the top-left window
    base += 0.10 * np.exp(-((v - 0.28) / 0.16) ** 2) + 0.07 * np.exp(-((u + 0.6 * v - 0.35) / 0.22) ** 2) - 0.10 * v
    base += 0.010 * streak * np.exp(-((v - 0.28) / 0.16) ** 2)
    rgb = np.dstack([base * 0.985, base * 0.99, base * 1.01])
    # rounded corners with a bevel: light top/left edge, dark bottom/right edge
    dx = np.minimum(x, W - 1 - x); dy = np.minimum(y, Hh - 1 - y)
    cx = np.clip(rad - dx, 0, None); cy = np.clip(rad - dy, 0, None)
    inside = np.clip(rad - np.hypot(cx, cy) + 0.5, 0, 1)
    edgeDist = np.minimum(dx, dy)
    bev = np.clip(1 - edgeDist / 6.0, 0, 1)
    tl = ((x < W * 0.5) & (dx < dy)) | ((y < Hh * 0.5) & (dy <= dx))
    rgb += (np.where(tl, 0.18, -0.25) * bev)[..., None]
    save(rgb, 'plate.png', alpha=inside)


# ------------------------------------------------------------------------------------------- knob skirt + pointer
def knob(N=420):
    # skirt: dome + chamfer + knurled rim, cream urea plastic with a warm subsurface tint
    m, d, dx, dy = disc_mask(N, N * 0.40)
    r = d / (N * 0.40)
    ang = np.arctan2(dy, dx)
    dome = np.sqrt(np.clip(1 - (r * 0.75) ** 2, 0, 1)) * 30
    rim = np.clip((r - 0.78) / 0.22, 0, 1)
    knurl = 0.5 + 0.5 * np.cos(ang * 40)
    hf = dome - 14 * rim ** 1.5 + 2.2 * knurl * rim * (r < 1)
    alb = np.array([0.95, 0.89, 0.76]) * (1 - 0.06 * rim[..., None])
    alb = alb * (1 + 0.02 * blur(rng.standard_normal((N, N)), 2)[..., None])
    rgb = shade(normals(hf, 0.6), alb, amb=0.42, kd=0.62, ks=0.45, p=38, spec_col=(1, 0.98, 0.94))
    rgb += 0.05 * np.clip(1 - r, 0, 1)[..., None] * np.array([1, 0.8, 0.6])          # subsurface warmth
    rgb *= (1 - 0.35 * np.clip((r - 0.93) / 0.07, 0, 1))[..., None]                   # occluded edge
    save(rgb, 'knob_skirt.png', alpha=m)
    # pointer (points up), ambient-lit only: the editor adds screen-fixed light so it stays consistent when rotated
    y, x = np.mgrid[0:N, 0:N].astype(np.float64)
    cx, cy = (N - 1) / 2, (N - 1) / 2
    X, Y = (x - cx) / (N * 0.40), (y - cy) / (N * 0.40)
    nose, tail, half = 1.22, 0.62, 0.36
    w = np.where(Y < 0, half * np.clip(1 + Y / nose, 0, 1) ** 0.75, half * np.sqrt(np.clip(1 - (Y / tail) ** 2, 0, 1)))
    inside = np.clip((w - np.abs(X)) * N * 0.40 + 0.5, 0, 1) * (Y > -nose) * (Y < tail)
    ridge = np.clip(1 - np.abs(X) / np.maximum(w, 1e-3), 0, 1)
    hfp = 18 * ridge ** 0.6 * inside
    nrm = normals(hfp, 0.6)
    amb = 0.55 + 0.45 * nrm[..., 2]                                                    # rotation-invariant curvature shading
    alb = np.array([0.96, 0.90, 0.77]) * amb[..., None]
    line = np.exp(-(X / 0.035) ** 2) * (Y < -0.25) * (Y > -nose * 0.92)               # brown index line
    alb = alb * (1 - line[..., None]) + np.array([0.30, 0.16, 0.08]) * line[..., None]
    save(alb, 'knob_pointer.png', alpha=inside)
    sh = blur(inside, N * 0.02)
    save(np.zeros((N, N, 3)), 'knob_pointer_shadow.png', alpha=sh * 0.55)
    shs = blur(disc_mask(N, N * 0.40)[0], N * 0.03)
    save(np.zeros((N, N, 3)), 'knob_skirt_shadow.png', alpha=shs * 0.6)


# ------------------------------------------------------------------------------------------- jewel (off / on / glow)
def jewel(N=240):
    m, d, dx, dy = disc_mask(N, N * 0.46)
    r = d / (N * 0.46)
    ang = np.arctan2(dy, dx)
    lens = r < 0.70
    # bezel: knurled chrome ring
    bez_h = 10 * np.sin(np.clip((r - 0.70) / 0.30, 0, 1) * np.pi) + 1.2 * (0.5 + 0.5 * np.cos(ang * 48)) * (r > 0.86)
    chrome = shade(normals(bez_h, 0.8), np.array([0.70, 0.71, 0.73]), amb=0.25, kd=0.6, ks=0.9, p=20)
    # lens: 8 facets around a flat table, faceted normals give refraction-like contrast
    seg = np.round(ang / (2 * np.pi / 8)) * (2 * np.pi / 8)
    tilt = np.where(r < 0.25, 0.0, 0.55)
    n = np.dstack([np.cos(seg) * tilt, np.sin(seg) * tilt, np.ones_like(r)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    fres = np.clip(n @ L, 0, 1) ** 2
    spec = np.clip(n @ H, 0, 1) ** 60
    for state, base, emit in (('off', np.array([0.30, 0.03, 0.02]), 0.0), ('on', np.array([1.0, 0.16, 0.08]), 1.0)):
        inner = np.exp(-(((dx - N * 0.08) ** 2 + (dy - N * 0.08) ** 2) / (N * 0.16) ** 2))   # light refracted to the lower right
        col = base * (0.45 + 0.55 * fres[..., None]) + emit * inner[..., None] * np.array([1.0, 0.75, 0.45])
        col += 0.9 * spec[..., None]
        col *= (1 - 0.5 * np.clip((r - 0.55) / 0.15, 0, 1))[..., None]
        rgb = np.where(lens[..., None], col, chrome)
        save(rgb, 'jewel_%s.png' % state, alpha=m)
    G = 3 * N
    mg, dg, _, _ = disc_mask(G, G * 0.5)
    glow = np.exp(-(dg / (G * 0.20)) ** 2) * 0.8
    save(np.dstack([np.ones((G, G)), 0.28 * np.ones((G, G)), 0.12 * np.ones((G, G))]), 'jewel_glow.png', alpha=glow)


# ------------------------------------------------------------------------------------------- jack nut, plug, screw
def hexmask(N, R, rot=np.pi / 6):
    y, x = np.mgrid[0:N, 0:N].astype(np.float64)
    cx = cy = (N - 1) / 2
    a = np.arctan2(y - cy, x - cx) - rot
    sec = (a + np.pi / 6) % (np.pi / 3) - np.pi / 6
    rr = np.hypot(x - cx, y - cy) * np.cos(sec)
    return np.clip(R * np.cos(np.pi / 6) - rr + 0.5, 0, 1), rr / (R * np.cos(np.pi / 6)), np.hypot(x - cx, y - cy)


def jack(N=240):
    m, rr, d = hexmask(N, N * 0.46)
    hf = 14 * np.clip((1 - rr) / 0.18, 0, 1) ** 0.7                      # chamfered hex nut
    hole_r = N * 0.20
    washer = (d < N * 0.27) & (d >= hole_r)
    hf = np.where(d < N * 0.27, 14 + 3 * np.clip((N * 0.27 - d) / (N * 0.07), 0, 1), hf)
    alb = np.array([0.72, 0.73, 0.75]) * (1 + 0.03 * blur(rng.standard_normal((N, N)), 1)[..., None])
    rgb = shade(normals(hf, 0.7), alb, amb=0.28, kd=0.6, ks=0.85, p=24)
    hole = d < hole_r
    depth = np.clip(d / hole_r, 0, 1)
    rgb = np.where(hole[..., None], (0.02 + 0.10 * depth ** 4)[..., None] * np.array([1, 1, 1]), rgb)
    save(rgb, 'jack_nut.png', alpha=m)
    # inserted plug: nickel sleeve end with a dark tip insulator
    mp, dp, dxp, dyp = disc_mask(N, hole_r * 0.96)
    rp = dp / (hole_r * 0.96)
    hfp = 10 * np.sqrt(np.clip(1 - rp ** 2, 0, 1))
    rgbp = shade(normals(hfp, 0.8), np.array([0.82, 0.78, 0.66]), amb=0.3, kd=0.6, ks=0.9, p=30)
    rgbp = np.where((rp < 0.45)[..., None], np.array([0.08, 0.07, 0.06]) + 0.4 * (np.clip(normals(hfp, 0.8) @ H, 0, 1) ** 40)[..., None], rgbp)
    save(rgbp, 'jack_plug.png', alpha=mp)
    sh = blur(m, N * 0.03)
    save(np.zeros((N, N, 3)), 'nut_shadow.png', alpha=sh * 0.55)


def screw(N=72):
    m, d, dx, dy = disc_mask(N, N * 0.45)
    r = d / (N * 0.45)
    hf = 8 * np.sqrt(np.clip(1 - r ** 2, 0, 1))
    slot = np.abs(dy - dx * 0.35) < N * 0.05
    hf = np.where(slot, hf - 5, hf)
    rgb = shade(normals(hf, 0.8), np.array([0.70, 0.70, 0.72]), amb=0.3, kd=0.6, ks=0.8, p=20)
    save(rgb, 'screw.png', alpha=m)


# ------------------------------------------------------------------------------------------- bat-lever toggle (up / down)
def toggle(N=240):
    for state, s in (('up', -1), ('down', 1)):
        m, rr, d = hexmask(N, N * 0.26, rot=0.0)
        hf = 10 * np.clip((1 - rr) / 0.25, 0, 1) ** 0.7
        y, x = np.mgrid[0:N, 0:N].astype(np.float64)
        cx = cy = (N - 1) / 2
        # lever seen from above, leaning toward the active legend: tapered capsule from the bushing to a ball end
        t = np.clip((y - cy) * s / (N * 0.40), 0, 1)
        width = N * (0.055 + 0.03 * t)
        on_axis = ((y - cy) * s >= -N * 0.03) & ((y - cy) * s <= N * 0.40)
        lev = np.clip(width - np.abs(x - cx) + 0.5, 0, 1) * on_axis
        ball_c = cy + s * N * 0.40
        ball = np.clip(N * 0.085 - np.hypot(x - cx, y - ball_c) + 0.5, 0, 1)
        lever = np.maximum(lev, ball)
        hl = 30 + 18 * t + 8 * np.sqrt(np.clip(1 - ((x - cx) / np.maximum(width, 1)) ** 2, 0, 1))
        hl = np.where(ball > 0, 48 + 10 * np.sqrt(np.clip(1 - (np.hypot(x - cx, y - ball_c) / (N * 0.085)) ** 2, 0, 1)), hl)
        H2 = np.where(lever > 0, hl, hf)
        rgb = shade(normals(H2, 0.6), np.array([0.74, 0.75, 0.77]), amb=0.28, kd=0.6, ks=0.9, p=28)
        alpha = np.maximum(m, lever)
        # lever shadow cast toward the lower right
        sb = blur(lever, N * 0.02); oy, ox = int(N * 0.05), int(N * 0.04)
        shifted = np.zeros_like(sb); shifted[oy:, ox:] = sb[:-oy, :-ox]
        shadow = shifted * (lever < 0.5) * 0.55
        rgb = rgb * (1 - shadow[..., None] * (m > 0)[..., None])
        a = np.maximum(alpha, shadow)
        rgb = np.where((alpha > 0)[..., None], rgb, 0)
        save(rgb, 'toggle_%s.png' % state, alpha=a)


tweed(); plate(); knob(); jewel(); jack(); screw(); toggle()
