#!/usr/bin/env python3
"""
Fit constitutive tube models to the curves digitized from the dossier's JJ characteristic sheets
(Figs 9-12, sources S21-S24) by scripts/digitize_jj_sheets.py  ->  data/tubes/digitized/*.csv

Evidence labels (dossier Sec. 01):
  DIGITIZED  read by pixel analysis from the dossier's embedded manufacturer figure
  EXTERNAL   a published manufacturer number that is NOT contained in the dossier text or figures
             (used only where the dossier's own data cannot constrain a behaviour; always labelled)
  ASSUMED    engineering choice with no data in the dossier

Nothing here is measured from a Custom Deluxe specimen.  The fitted values are *candidates* for a nominal
public-source model (dossier Sec. 16, 28).  Following Sec. 16 every fit is validated on data it did not see
(whole held-out curves and cross-panel prediction), and the error INSIDE and OUTSIDE the operating region is reported.

Outputs: data/tubes/fitted_models.json, generated/TubeFits.h
"""
import csv, json, math, os, sys
import numpy as np
from scipy.optimize import differential_evolution, least_squares

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIG = os.path.join(ROOT, "data", "tubes", "digitized")


# ---------------------------------------------------------------------------------------------- data access
def load_curves(name):
    """-> {(panel, parameter): (x ndarray, y ndarray)} sorted by x, from a digitized CSV."""
    with open(os.path.join(DIG, name)) as f:
        lines = [l for l in f if not l.startswith("#")]
    rd = csv.reader(lines)
    hdr = next(rd)
    assert hdr == ["panel", "parameter", "x", "y"], hdr
    acc = {}
    for panel, par, x, y in rd:
        acc.setdefault((panel, par), []).append((float(x), float(y)))
    out = {}
    for k, v in acc.items():
        a = np.array(sorted(v))
        out[k] = (a[:, 0], a[:, 1])
    return out


def val(par):
    """'Ug=-0.5V' / 'Ua=100V' / 'Ug=+20V' -> float."""
    return float(par.split("=")[1].rstrip("V").replace("+", ""))


def thin(x, y, n):
    """Uniform-by-index thinning to at most n points (the digitizer oversamples along the curve)."""
    if len(x) <= n:
        return x, y
    idx = np.unique(np.linspace(0, len(x) - 1, n).round().astype(int))
    return x[idx], y[idx]


def softplus(x):
    return np.where(x > 30, x, np.log1p(np.exp(np.minimum(x, 30))))


# ---------------------------------------------------------------------------------------------- triode (Koren)
def koren_triode(vgk, vpk, p):
    mu, ex, kg1, kp, kvb = p
    s = np.sqrt(kvb + vpk * vpk)
    e1 = (vpk / kp) * softplus(kp * (1.0 / mu + vgk / s))
    return np.power(np.maximum(e1, 1e-30), ex) / kg1 * 1e3          # mA


def triode_dataset(curves, per_curve=70):
    """arrays (vgk, vpk, ia_mA, tag) from both panels; tag = ('transfer'|'plate', parameter-value)"""
    vg, vp, ia, panel, par = [], [], [], [], []
    for (pn, pr), (x, y) in sorted(curves.items()):
        x, y = thin(x, y, per_curve)
        for xi, yi in zip(x, y):
            if pn == "transfer":
                vg.append(xi); vp.append(val(pr))
            else:
                vg.append(val(pr)); vp.append(xi)
            ia.append(yi); panel.append(pn); par.append(val(pr))
    return dict(vg=np.array(vg), vp=np.array(vp), ia=np.array(ia), panel=np.array(panel), par=np.array(par))


def sigma_tri(ia):
    return 0.03 + 0.03 * ia                    # pixel floor (~0.02 mA/px) + 3 % relative, mA


def global_fit(fun, args, ia, sig, bounds, seed, popsize=24, maxiter=400):
    """Differential evolution in log-parameter space (global, deterministic seed) followed by a local polish.
    fun(*args, p) -> model current in mA;  sig = per-point sigma (mA)."""
    lo = np.log([b[0] for b in bounds]); hi = np.log([b[1] for b in bounds])
    res = lambda lp: (fun(*args, np.exp(lp)) - ia) / sig
    cost = lambda lp: float(np.sum(res(lp) ** 2))
    r = differential_evolution(cost, list(zip(lo, hi)), seed=seed, maxiter=maxiter, popsize=popsize, tol=1e-9, polish=False)
    s = least_squares(res, np.clip(r.x, lo + 1e-9, hi - 1e-9), bounds=(lo, hi), max_nfev=4000)
    return np.exp(s.x)


def fit_triode(d, mask, bounds, weights, seed=1, popsize=24):
    vg, vp, ia = d["vg"][mask], d["vp"][mask], d["ia"][mask]
    sig = sigma_tri(ia) / np.sqrt(weights[mask])
    return global_fit(koren_triode, (vg, vp), ia, sig, bounds, seed, popsize)


def err_stats(d, mask, p, region=None):
    m = mask.copy()
    if region is not None:
        m &= region
    if not m.any():
        return None
    e = koren_triode(d["vg"][m], d["vp"][m], p) - d["ia"][m]
    big = d["ia"][m] >= 0.2 * d["ia"][m].max() * 0.25      # relative error only where the current is meaningful
    rel = np.sqrt(np.mean((e[big] / d["ia"][m][big]) ** 2)) if big.any() else float("nan")
    return dict(n=int(m.sum()), rms_mA=float(np.sqrt(np.mean(e ** 2))), max_mA=float(np.max(np.abs(e))),
                rel_rms=float(rel))


def local_small_signal(f, vg, vp, h=1e-3):
    gm = (f(vg + h, vp) - f(vg - h, vp)) / (2 * h)              # mA/V = mS
    gp = (f(vg, vp + h) - f(vg, vp - h)) / (2 * h)              # mS
    return float(f(vg, vp)), float(gm), float(1.0 / gp), float(gm / gp)


def run_triode(name, fname, bounds, rca_point, op_region_fn, holdout_params, outside_weight=0.3):
    cur = load_curves(fname)
    d = triode_dataset(cur)
    n = len(d["ia"])
    everything = np.ones(n, bool)
    region = op_region_fn(d)

    weights = np.where(region, 1.0, outside_weight)          # Sec. 16: weight the region the circuit actually visits
    p_all = fit_triode(d, everything, bounds, weights, seed=1, popsize=30)

    # --- Sec. 16 validation 1: cross-panel (fit one panel, predict the other)
    pt = d["panel"] == "transfer"; pp = d["panel"] == "plate"
    p_from_plate = fit_triode(d, pp, bounds, weights, seed=2, popsize=16)
    p_from_transfer = fit_triode(d, pt, bounds, weights, seed=3, popsize=16)
    cross = dict(plate_to_transfer=err_stats(d, pt, p_from_plate), transfer_to_plate=err_stats(d, pp, p_from_transfer),
                 plate_to_transfer_in_region=err_stats(d, pt, p_from_plate, region),
                 transfer_to_plate_in_region=err_stats(d, pp, p_from_transfer, region))

    # --- Sec. 16 validation 2: whole held-out curves (never seen by the optimiser)
    hold = np.zeros(n, bool)
    for panel, v in holdout_params:
        hold |= (d["panel"] == panel) & np.isclose(d["par"], v)
    p_tr = fit_triode(d, ~hold, bounds, weights, seed=4, popsize=16)
    held = dict(curves=[f"{a}:{b}" for a, b in holdout_params], held_out=err_stats(d, hold, p_tr),
                trained=err_stats(d, ~hold, p_tr), params=[float(x) for x in p_tr])

    ins = err_stats(d, everything, p_all, region)
    outs = err_stats(d, everything, p_all, ~region)
    full = err_stats(d, everything, p_all)
    f = lambda vg, vp: float(koren_triode(np.array(vg), np.array(vp), p_all))
    i0, gm, rp, mu = local_small_signal(f, rca_point["vg"], rca_point["vp"])
    print(f"[{name}] mu={p_all[0]:.3f} Ex={p_all[1]:.4f} Kg1={p_all[2]:.2f} Kp={p_all[3]:.2f} Kvb={p_all[4]:.1f}")
    print(f"   all data      : rms {full['rms_mA']:.3f} mA  max {full['max_mA']:.3f} mA  rel {100*full['rel_rms']:.1f}%  (n={full['n']})")
    print(f"   operating reg.: rms {ins['rms_mA']:.3f} mA  max {ins['max_mA']:.3f} mA  rel {100*ins['rel_rms']:.1f}%  (n={ins['n']})")
    if outs:
        print(f"   outside region: rms {outs['rms_mA']:.3f} mA  max {outs['max_mA']:.3f} mA  rel {100*outs['rel_rms']:.1f}%  (n={outs['n']})")
    print(f"   cross-panel   : plate->transfer rel {100*cross['plate_to_transfer']['rel_rms']:.1f}% rms {cross['plate_to_transfer']['rms_mA']:.3f} mA ;"
          f" transfer->plate rel {100*cross['transfer_to_plate']['rel_rms']:.1f}% rms {cross['transfer_to_plate']['rms_mA']:.3f} mA")
    print(f"   held-out curves {held['curves']}: rel {100*held['held_out']['rel_rms']:.1f}% rms {held['held_out']['rms_mA']:.3f} mA (trained rel {100*held['trained']['rel_rms']:.1f}%)")
    print(f"   at ({rca_point['vp']} V, {rca_point['vg']} V): Ip={i0:.3f} mA gm={gm:.3f} mS rp={rp:.1f} k mu={mu:.1f}   [RCA/dossier: Ip~{rca_point['ip']} gm={rca_point['gm']} rp={rca_point['rp']} mu={rca_point['mu']}]")
    return dict(mu=float(p_all[0]), ex=float(p_all[1]), kg1=float(p_all[2]), kp=float(p_all[3]), kvb=float(p_all[4]),
                weights=f'1.0 inside the operating region, {outside_weight} outside', n_points=int(n), fit_all=full, fit_operating_region=ins, fit_outside_region=outs, cross_panel=cross,
                held_out_curves=held,
                local_at_rca_point=dict(vp=rca_point["vp"], vg=rca_point["vg"], ip_mA=i0, gm_mS=gm, rp_kohm=rp, mu=mu,
                                        rca_dossier=rca_point),
                evidence="DIGITIZED (JJ sheet) fit; grid conduction is ASSUMED (no grid-current data for this tube)")


# ---------------------------------------------------------------------------------------------- 6V6 (triode-connected sheet)
def kor_ik(vg, vs, p):
    """Space-charge (cathode-to-screen+plate) current in mA.  Koren form with the screen as the controlling anode;
    the controlling voltage is sqrt(Kvb + Vs^2) so that the current vanishes as Vs -> 0 (needed because the JJ sheet is
    triode-connected, Vs = Vp, down to a few volts).  Kvb is a documented generalisation of Koren's published pentode law."""
    mu, ex, kg1, kp, kvb = p
    s = np.sqrt(kvb + vs * vs)
    e1 = (vs / kp) * softplus(kp * (1.0 / mu + vg / s))
    return np.power(np.maximum(e1, 1e-30), ex) / kg1 * 1e3


def sixv6_dataset(curves, per_curve=70):
    vg, vs, ia, panel, par = [], [], [], [], []
    for (pn, pr), (x, y) in sorted(curves.items()):
        if pr.startswith("Ig@") or pr.startswith("Pa="):
            continue
        x, y = thin(x, y, per_curve)
        for xi, yi in zip(x, y):
            if pn == "transfer":
                vg.append(xi); vs.append(val(pr))
            else:
                vg.append(val(pr)); vs.append(xi)
            ia.append(yi); panel.append(pn); par.append(val(pr))
    return dict(vg=np.array(vg), vs=np.array(vs), ia=np.array(ia), panel=np.array(panel), par=np.array(par))


def sig_ik(ia):
    return 0.25 + 0.03 * ia                    # mA


def fit_ik(d, mask, bounds, weights, seed, popsize=24):
    vg, vs, ia = d["vg"][mask], d["vs"][mask], d["ia"][mask]
    return global_fit(kor_ik, (vg, vs), ia, sig_ik(ia) / np.sqrt(weights[mask]), bounds, seed, popsize)


def ik_err(d, mask, p, sel=None):
    m = mask if sel is None else (mask & sel)
    e = kor_ik(d["vg"][m], d["vs"][m], p) - d["ia"][m]
    big = d["ia"][m] >= 10.0
    return dict(n=int(m.sum()), rms_mA=float(np.sqrt(np.mean(e ** 2))), max_mA=float(np.max(np.abs(e))),
                rel_rms_above_10mA=float(np.sqrt(np.mean((e[big] / d["ia"][m][big]) ** 2))) if big.any() else float("nan"))


def run_6v6():
    cur = load_curves("6V6_JJ_S22.csv")
    d = sixv6_dataset(cur)
    n = len(d["ia"]); every = np.ones(n, bool)
    bounds = [(3, 40), (1.0, 2.4), (10, 60000), (2, 3000), (0.5, 60000)]
    # Sec. 16: weight the region the circuit visits.  The amplifier's 6V6 pair runs with the screen near 300 V and the
    # grid from about -45 V (cut-off during the opposite half-cycle) to a few volts positive; the Ug = +10/+20 V curves
    # are outside what the cathodyne can drive, and the low-Vs knee below 30 V is not visited either.
    visited = (d["vs"] >= 30) & (d["vg"] >= -45) & (d["vg"] <= 5)
    weights = np.where(visited, 1.0, 0.3)
    p = fit_ik(d, every, bounds, weights, seed=11, popsize=40)
    pt = d["panel"] == "transfer"; pp = d["panel"] == "plate"
    p_pl = fit_ik(d, pp, bounds, weights, seed=12, popsize=20); p_tr = fit_ik(d, pt, bounds, weights, seed=13, popsize=20)
    cross = dict(plate_to_transfer=ik_err(d, pt, p_pl), transfer_to_plate=ik_err(d, pp, p_tr))
    hold = ((d["panel"] == "plate") & np.isclose(d["par"], -20.0)) | ((d["panel"] == "transfer") & np.isclose(d["par"], 100.0))
    p_h = fit_ik(d, ~hold, bounds, weights, seed=14, popsize=20)
    held = dict(held_out=ik_err(d, hold, p_h), trained=ik_err(d, ~hold, p_h), curves=["plate:Ug=-20V", "transfer:Ua=100V"])
    # error where the amplifier actually works (screen ~300 V, grid about -21 V at idle) and in the clipping region
    amp_region = (d["vs"] >= 150) & (d["vs"] <= 400) & (d["vg"] <= 0) & (d["vg"] >= -30)
    pos = d["vg"] > 0
    full = ik_err(d, every, p); region = ik_err(d, every, p, amp_region); positive = ik_err(d, every, p, pos)
    vis = ik_err(d, every, p, visited)
    print(f"[6V6 cathode-current law, triode-connected JJ sheet] mu={p[0]:.3f} Ex={p[1]:.4f} Kg1={p[2]:.1f} Kp={p[3]:.2f} Kvb={p[4]:.1f}")
    for lab, st in [("all data", full), ("visited region (Vs>=30 V, Vg -45..+5 V)", vis), ("amp region (Vs 150-400 V, Vg -30..0 V)", region), ("positive grid (Ug>0)", positive)]:
        print(f"   {lab:42s}: rms {st['rms_mA']:.2f} mA  max {st['max_mA']:.2f} mA  rel(>=10 mA) {100*st['rel_rms_above_10mA']:.1f}%  (n={st['n']})")
    print(f"   cross-panel: plate->transfer rel {100*cross['plate_to_transfer']['rel_rms_above_10mA']:.1f}% rms {cross['plate_to_transfer']['rms_mA']:.2f} mA ; transfer->plate rel {100*cross['transfer_to_plate']['rel_rms_above_10mA']:.1f}% rms {cross['transfer_to_plate']['rms_mA']:.2f} mA")
    print(f"   held-out {held['curves']}: rel {100*held['held_out']['rel_rms_above_10mA']:.1f}% rms {held['held_out']['rms_mA']:.2f} mA (trained rel {100*held['trained']['rel_rms_above_10mA']:.1f}%)")

    # ---- consistency with the EXTERNAL RCA 6V6GT rating point (NOT in the dossier): 250 V / 250 V / -12.5 V
    ik = float(kor_ik(-12.5, 250.0, p))
    h = 1e-3
    gm_k = float((kor_ik(-12.5 + h, 250.0, p) - kor_ik(-12.5 - h, 250.0, p)) / (2 * h))
    print(f"   consistency with EXTERNAL RCA rating point (Ip+Ig2 = 49.5 mA, gm_plate = 4.1 mS): model Ik(250 V,-12.5 V) = {ik:.1f} mA, dIk/dVg = {gm_k:.2f} mS")

    # ---- grid conduction from the dashed Ig curves (triode-connected; plateau Ua >= 150 V)
    ig = {}
    for (pn, pr), (x, y) in cur.items():
        if pr.startswith("Ig@Ug="):
            ug = val(pr.split("@")[1])
            m = (x >= 150) & (x <= 350)
            ig[ug] = float(np.mean(y[m]))
    vc, sg = -0.4, 0.2                                                        # onset: ASSUMED (no data below Ug=+5 V)
    def xg(v):
        return sg * float(softplus((v - vc) / sg))
    def sat(a, imax, v):
        t = a * xg(v) ** 1.5
        return t / math.sqrt(1.0 + (t / imax) ** 2)
    def res(lp):
        a, imax = np.exp(lp)
        return [sat(a, imax, ug) * 1e3 - i for ug, i in sorted(ig.items())]
    s = least_squares(res, np.log([1e-3, 2e-2]), bounds=(np.log([1e-6, 1e-3]), np.log([1.0, 1.0])))
    a_g, imax_g = np.exp(s.x)
    print(f"   grid-current plateau (Ua 150-350 V) from the dashed curves: " + ", ".join(f"Ig(Ug=+{u:g} V)={i:.2f} mA" for u, i in sorted(ig.items())))
    print(f"   grid law  Ig = a x^1.5 / sqrt(1+(a x^1.5/Imax)^2), x = softplus-smoothed (Vgk-({vc})): a={a_g:.4e} A/V^1.5, Imax={imax_g*1e3:.2f} mA")

    # ---- screen partition and plate-voltage dependence: EXTERNAL RCA anchor (rp = 52 kOhm at 45 mA plate, screen 4.5 mA)
    fs0 = 4.5 / 49.5; vk = 55.0; q = 1.5; va = 52e3 * 0.045
    return dict(mu=float(p[0]), ex=float(p[1]), kg1=float(p[2]), kp=float(p[3]), kvb=float(p[4]), fs0=fs0, vk=vk, q=q, va=va,
                grid=dict(a=float(a_g), imax=float(imax_g), vc=vc, sg=sg, plateau_mA={f"{u:g}": i for u, i in ig.items()}),
                n_points=int(n), fit_all=full, fit_visited_region=vis, fit_amp_region=region, fit_positive_grid=positive, cross_panel=cross,
                held_out_curves=held,
                consistency_external_rca=dict(model_ik_mA=ik, model_dik_dvg_mS=gm_k, rca_ip_plus_ig2_mA=49.5, rca_gm_mS=4.1),
                evidence=("cathode-current law: DIGITIZED (JJ 6V6S sheet, assumed triode-connected: Ia = plate+screen current); "
                          "grid current plateau: DIGITIZED (dashed Ig curves); grid onset (vc, sg): ASSUMED; "
                          "screen partition fs0 and Early voltage va: EXTERNAL anchor (RCA 6V6GT rating point, not in the dossier); "
                          "knee shape vk, q: ASSUMED"))


# ---------------------------------------------------------------------------------------------- 5Y3 forward law
def run_5y3():
    cur = load_curves("5Y3_JJ_S23.csv")
    (x, y), = [v for k, v in cur.items() if k[0] == "forward"]
    solid = x < 60.0
    xs, ys = thin(x, y, 120)
    w = np.where(xs < 60.0, 1.0, 0.35)                 # the dashed (>60 V) branch is the manufacturer's extrapolation
    A = np.vstack([xs, xs ** 2]).T
    coef, *_ = np.linalg.lstsq(A * (w / ys)[:, None], w * np.ones(len(xs)), rcond=None)
    k1, k2 = coef
    pred = A @ coef
    rel_s = float(np.sqrt(np.mean((pred[xs < 60] / ys[xs < 60] - 1) ** 2)))
    rel_d = float(np.sqrt(np.mean((pred[xs >= 60] / ys[xs >= 60] - 1) ** 2)))
    print(f"[5Y3 forward, per anode] I[mA] = {k1:.4f} V + {k2:.5f} V^2 ; rel-rms solid(<60 V) {100*rel_s:.1f}%, dashed(>=60 V) {100*rel_d:.1f}%  (n={len(xs)})")
    return dict(k1_mA_per_V=float(k1), k2_mA_per_V2=float(k2), n_points=int(len(xs)),
                rel_rms_solid=rel_s, rel_rms_dashed=rel_d,
                evidence="DIGITIZED (JJ 5Y3S right panel); dashed branch weighted 0.35; in-circuit regulation check: tools/validate_supply.cpp")


# ---------------------------------------------------------------------------------------------- main
def main():
    # operating region of the preamp triodes in this amplifier: plate 50-350 V, grid -6..0 V
    in_region = lambda d: (d["vp"] >= 50) & (d["vp"] <= 350) & (d["vg"] >= -6.0) & (d["vg"] <= 0.0)
    ay7 = run_triode("12AY7", "12AY7_JJ_S21.csv",
                     [(20, 80), (1.0, 2.0), (50, 8000), (5, 3000), (1, 20000)],
                     dict(vp=250.0, vg=-4.0, ip=3.0, gm=1.75, rp=22.8, mu=40), in_region,
                     holdout_params=[("transfer", 200.0), ("plate", -3.0)])
    ax7 = run_triode("12AX7", "12AX7_JJ_S24.csv",
                     [(50, 160), (1.0, 2.0), (50, 8000), (5, 3000), (1, 20000)],
                     dict(vp=250.0, vg=-2.0, ip=1.2, gm=1.6, rp=62.5, mu=100), in_region,
                     holdout_params=[("transfer", 150.0), ("plate", -1.5)])
    v6 = run_6v6()
    y3 = run_5y3()

    out = dict(
        note="Candidate constitutive-model parameters for a nominal public-source model. NOT measured on a specimen.",
        method="scripts/digitize_jj_sheets.py -> data/tubes/digitized/*.csv ; scripts/fit_tubes.py (Sec. 16 objective, held-out validation)",
        triode_koren={"12AY7": ay7, "12AX7": ax7}, pentode_6V6=v6, rectifier_5Y3=y3)
    with open(os.path.join(ROOT, "data", "tubes", "fitted_models.json"), "w") as f:
        json.dump(out, f, indent=2)

    g = v6["grid"]
    h = f"""// GENERATED by scripts/fit_tubes.py -- do not edit by hand.
// Candidate nominal parameters; see data/tubes/fitted_models.json for residuals, validation and provenance.
#pragma once
namespace cd57::fit {{
struct KorenTriodeFit {{ double mu, ex, kg1, kp, kvb; }};
struct BeamPentodeFit {{ double mu, ex, kg1, kp, kvb, fs0, vk, q, va; }};
struct SaturatingGridFit {{ double a, imax, vc, sg; }};
inline constexpr KorenTriodeFit k12AY7 {{ {ay7['mu']:.9g}, {ay7['ex']:.9g}, {ay7['kg1']:.9g}, {ay7['kp']:.9g}, {ay7['kvb']:.9g} }};
inline constexpr KorenTriodeFit k12AX7 {{ {ax7['mu']:.9g}, {ax7['ex']:.9g}, {ax7['kg1']:.9g}, {ax7['kp']:.9g}, {ax7['kvb']:.9g} }};
// 6V6: mu..kvb = cathode-current law fitted to the JJ 6V6S triode-connected sheet (Ik = Ip + Is when Vs = Vp);
//      fs0, va = screen partition and Early voltage from an EXTERNAL RCA 6V6GT anchor (not in the dossier); vk, q = knee shape, ASSUMED
inline constexpr BeamPentodeFit k6V6   {{ {v6['mu']:.9g}, {v6['ex']:.9g}, {v6['kg1']:.9g}, {v6['kp']:.9g}, {v6['kvb']:.9g}, {v6['fs0']:.9g}, {v6['vk']:.9g}, {v6['q']:.9g}, {v6['va']:.9g} }};
// 6V6 control-grid conduction: Ig = a x^1.5 / sqrt(1 + (a x^1.5 / imax)^2), x = sg*softplus((Vgk - vc)/sg); a, imax DIGITIZED, vc, sg ASSUMED
inline constexpr SaturatingGridFit k6V6grid {{ {g['a']:.9g}, {g['imax']:.9g}, {g['vc']:.9g}, {g['sg']:.9g} }};
// 5Y3 per-anode forward law: I[A] = k1*V + k2*V^2   (V in volts, fitted 0..100 V, extrapolated beyond)
inline constexpr double k5Y3_k1 = {y3['k1_mA_per_V']*1e-3:.9g};
inline constexpr double k5Y3_k2 = {y3['k2_mA_per_V2']*1e-3:.9g};
}} // namespace cd57::fit
"""
    with open(os.path.join(ROOT, "generated", "TubeFits.h"), "w") as f:
        f.write(h)
    print("wrote data/tubes/fitted_models.json and generated/TubeFits.h")


if __name__ == "__main__":
    main()
