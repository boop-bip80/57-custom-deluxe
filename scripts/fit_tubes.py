#!/usr/bin/env python3
"""
Fit constitutive tube models to the data points digitized from the Dossier's JJ
characteristic sheets (S21, S24) plus published manufacturer operating points (S08, S19).

Evidence labelling follows the dossier (Sec. 01):
  DIGITIZED  - read from the dossier's embedded manufacturer figure (pixel-calibrated, +/-~0.05 mA)
  DATASHEET  - published manufacturer operating point cited by the dossier
  ASSUMED    - engineering choice; carries an uncertainty note

Nothing here is measured from a Custom Deluxe specimen: the fitted values are *candidates*
for a nominal public-source model (dossier Sec. 16, 28).

Outputs: data/tubes/*.csv (inputs), data/tubes/fitted_models.json, generated/TubeFits.h
"""
import json, math, os, sys
import numpy as np
from scipy.optimize import least_squares

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def softplus(x):
    return np.where(x > 30, x, np.log1p(np.exp(np.minimum(x, 30))))

# ---------------------------------------------------------------- Koren triode
def koren_triode(vgk, vpk, p):
    mu, ex, kg1, kp, kvb = p
    s = np.sqrt(kvb + vpk * vpk)
    e1 = (vpk / kp) * softplus(kp * (1.0 / mu + vgk / s))
    return np.power(np.maximum(e1, 1e-30), ex) / kg1          # amps

# ---------------------------------------------------------------- 12AY7 data (JJ S21, Fig. 9, transfer sheet)
# (Ua [V], Ug [V], Ia [mA]) -- digitized with the calibrated column scan in scripts/README (see docs)
AY7 = []
def add(lst, ua, pts):
    for ug, ia in pts: lst.append((ua, ug, ia))
add(AY7, 300, [(-7.5,.57),(-7,.9),(-6.5,1.32),(-6,1.85),(-5.5,2.58),(-5,3.43),(-4.5,4.32),(-4,5.32),(-3.5,6.46)])
add(AY7, 250, [(-6,.66),(-5,1.61),(-4.5,2.34),(-4,3.13),(-3.5,4.07),(-3,5.06),(-2.5,6.11)])
add(AY7, 200, [(-5,.46),(-4.5,.79),(-4,1.31),(-3.5,2.07),(-3,2.91),(-2.5,3.81),(-2,4.77),(-1.5,5.87)])
add(AY7, 150, [(-4,.33),(-3.5,.61),(-3,1.11),(-2.5,1.85),(-2,2.67),(-1.5,3.59),(-1,4.55),(-.5,5.6)])
add(AY7, 100, [(-2.5,.44),(-2,.98),(-1.5,1.68),(-1,2.47),(-.5,3.37)])
add(AY7, 50,  [(-1.5,.3),(-1,.74),(-.5,1.39)])
AY7 = np.array(AY7)

# ---------------------------------------------------------------- 12AX7 data (JJ S24, Fig. 10, transfer sheet)
AX7 = []
add(AX7, 250, [(-2.5,.57),(-2.1,1.06),(-1.7,1.71),(-1.3,2.45),(-.9,3.53),(-.5,4.55),(-.1,5.68)])
add(AX7, 200, [(-2.5,.13),(-2.1,.48),(-1.9,.71),(-1.7,.94),(-.9,2.36),(-.7,2.84),(-.5,3.28),(-.3,3.83),(-.1,4.35)])
add(AX7, 150, [(-2.1,.05),(-1.9,.17),(-1.7,.32),(-1.5,.49),(-1.3,.71),(-1.1,.95),(-.9,1.35),(-.7,1.66),(-.5,2.08),(-.3,2.54),(-.1,3.06)])
add(AX7, 100, [(-1.0,.54),(-.7,.9),(-.5,1.18),(-.3,1.54),(-.1,1.92)])
AX7 = np.array(AX7)

def fit_triode(data, anchors, p0, name, bounds):
    """data: (Ua,Ug,Ia mA). anchors: list of (Ua,Ug,Ia mA,gm mS or None, weight)"""
    ua, ug, ia = data[:,0], data[:,1], data[:,2]
    def resid(lp):
        p = np.exp(lp)
        im = koren_triode(ug, ua, p) * 1e3
        r = (im - ia) / (0.08 + 0.06 * ia)
        ra = []
        for (a_ua, a_ug, a_ia, a_gm, w) in anchors:
            i0 = koren_triode(np.array(a_ug), np.array(a_ua), p) * 1e3
            ra.append(w * (i0 - a_ia) / (0.03 * a_ia))
            if a_gm is not None:
                d = 1e-3
                i1 = koren_triode(np.array(a_ug + d), np.array(a_ua), p) * 1e3
                ra.append(w * ((i1 - i0) / d - a_gm) / (0.04 * a_gm))
        # prior: mu stays near datasheet mu (handled as anchor below)
        return np.concatenate([r, ra])
    lo = np.log(np.array([b[0] for b in bounds])); hi = np.log(np.array([b[1] for b in bounds]))
    sol = least_squares(resid, np.log(p0), bounds=(lo, hi), x_scale=1.0, max_nfev=4000)
    p = np.exp(sol.x)
    im = koren_triode(ug, ua, p) * 1e3
    err = im - ia
    rms = float(np.sqrt(np.mean(err**2))); mx = float(np.max(np.abs(err)))
    rel = float(np.sqrt(np.mean((err/np.maximum(ia,0.3))**2)))
    # mu / rp / gm at the RCA anchor
    a = anchors[0]
    d = 1e-3
    vg, vp = a[1], a[0]
    gm = (koren_triode(np.array(vg+d), np.array(vp), p) - koren_triode(np.array(vg-d), np.array(vp), p)) / (2*d)
    gp = (koren_triode(np.array(vg), np.array(vp+d), p) - koren_triode(np.array(vg), np.array(vp-d), p)) / (2*d)
    print(f"[{name}] mu={p[0]:.3f} Ex={p[1]:.4f} Kg1={p[2]:.2f} Kp={p[3]:.2f} Kvb={p[4]:.2f}")
    print(f"   rms err {rms:.3f} mA, max {mx:.3f} mA, rel-rms {100*rel:.1f}%  | at ({vp}V,{vg}V): Ip={koren_triode(np.array(vg),np.array(vp),p)*1e3:.3f} mA gm={gm*1e3:.3f} mS rp={1/gp/1e3:.1f} k mu={gm/gp:.1f}")
    return dict(mu=p[0], ex=p[1], kg1=p[2], kp=p[3], kvb=p[4], rms_mA=rms, max_mA=mx, rel_rms=rel,
                n_points=int(len(ia)), anchor=dict(ua=vp, ug=vg, ip_mA=float(koren_triode(np.array(vg),np.array(vp),p)*1e3),
                gm_mS=float(gm*1e3), rp_kohm=float(1/gp/1e3), mu_local=float(gm/gp)))

# RCA 12AY7 (S08): 250 V / -4 V -> 3 mA, mu 40, rp 22.8k, gm 1.75 mS
fit_ay7 = fit_triode(AY7, [(250,-4,3.0,1.75,2.0)], [40,1.3,900,80,200], "12AY7",
                     [(25,60),(1.0,1.8),(100,6000),(5,2000),(1,5000)])
# RCA 12AX7 (S18): 250 V / -2 V -> 1.2 mA, mu 100, rp 62.5k, gm 1.6 mS
fit_ax7 = fit_triode(AX7, [(250,-2,1.2,1.6,2.0)], [100,1.3,1000,200,300], "12AX7",
                     [(60,130),(1.0,1.8),(100,6000),(5,2000),(1,5000)])

# ---------------------------------------------------------------- 6V6 beam-power pentode
# Model: Ik = E1^Ex/Kg1 (space-charge limited cathode current into screen+plate)
#        E1 = (Vg2k/Kp) ln(1+exp(Kp (1/mu + Vgk/Vg2k)))
#        screen fraction fs(Vpk) = fs0 + (1-fs0) exp(-(Vpk/Vk)^q);  Ip = Ik (1-fs)(1+Vpk/VA)/(1+250/VA)... see C++
# Anchors (S19, RCA 6V6GT): Vp=Vg2=250 V, Vg=-12.5 V: Ip=45 mA, Ig2=4.5 mA, gm=4.1 mS, rp=52 kOhm.
# Class-AB1 push-pull cathode bias, 250 V, Rk=250 ohm shared (S19): 2*35=70 mA zero-signal plate current.
def pentode_currents(vgk, vpk, vg2k, p):
    mu, ex, kg1, kp, fs0, vk, q, va = p
    e1 = (vg2k / kp) * softplus(kp * (1.0 / mu + vgk / vg2k))
    ik = np.power(np.maximum(e1, 1e-30), ex) / kg1
    fs = fs0 + (1.0 - fs0) * np.exp(-np.power(np.maximum(vpk, 0) / vk, q))
    ip = ik * (1 - fs) * (1.0 + vpk / va)
    ig2 = ik * fs
    return ip, ig2

def fit_pentode():
    vk, q = 55.0, 1.5
    fs0 = 4.5 / 49.5
    va = 52e3 * 0.045 * 1.0 / (1.0)        # Early voltage from rp at Ip=45 mA -> 2340 V
    def resid(lp):
        mu, ex, kg1, kp = np.exp(lp)
        p = (mu, ex, kg1, kp, fs0, vk, q, va)
        ip, ig2 = pentode_currents(np.array(-12.5), np.array(250.0), np.array(250.0), p)
        d = 1e-3
        ipm, _ = pentode_currents(np.array(-12.5 - d), np.array(250.0), np.array(250.0), p)
        ipp, _ = pentode_currents(np.array(-12.5 + d), np.array(250.0), np.array(250.0), p)
        gm = (ipp - ipm) / (2 * d)
        r = [(ip*1e3 - 45) / 1.0, (ig2*1e3 - 4.5) / 0.15, (gm*1e3 - 4.1) / 0.1]
        # Class AB1 (250 V, 250 V, 250 ohm shared Rk) -> total plate current 70 mA, grid at ~ -17.5 V:
        ip2, ig22 = pentode_currents(np.array(-17.5), np.array(250.0), np.array(250.0), p)
        r.append((2*ip2*1e3 - 70) / 4.0)
        # triode-connected cross-check against JJ 6V6S sheet is NOT used as a constraint (see report)
        # weak prior: Ex near 1.35 (Koren beam tetrodes), mu near 11
        r.append((np.log(ex) - np.log(1.35)) / 0.15)
        r.append((np.log(mu) - np.log(11.0)) / 0.35)
        return np.array(r)
    sol = least_squares(resid, np.log([11.0, 1.35, 700.0, 40.0]), bounds=(np.log([6,1.1,50,5]), np.log([20,1.8,20000,400])))
    mu, ex, kg1, kp = np.exp(sol.x)
    p = (mu, ex, kg1, kp, fs0, vk, q, va)
    ip, ig2 = pentode_currents(np.array(-12.5), np.array(250.0), np.array(250.0), p)
    d=1e-3
    gm = (pentode_currents(np.array(-12.5+d), np.array(250.0), np.array(250.0), p)[0]-pentode_currents(np.array(-12.5-d), np.array(250.0), np.array(250.0), p)[0])/(2*d)
    gp = (pentode_currents(np.array(-12.5), np.array(250.0+d), np.array(250.0), p)[0]-pentode_currents(np.array(-12.5), np.array(250.0-d), np.array(250.0), p)[0])/(2*d)
    print(f"[6V6] mu={mu:.3f} Ex={ex:.4f} Kg1={kg1:.1f} Kp={kp:.2f} fs0={fs0:.4f} Vk={vk} q={q} VA={va:.0f}")
    print(f"   Ip={ip*1e3:.2f} mA Ig2={ig2*1e3:.2f} mA gm={gm*1e3:.2f} mS rp={1/gp/1e3:.1f} k")
    i0, _ = pentode_currents(np.array(0.0), np.array(250.0), np.array(250.0), p)
    print(f"   Ip(Vg=0,250V)={i0*1e3:.1f} mA ; class-AB1 2*Ip(Vg=-17.5)={2*pentode_currents(np.array(-17.5),np.array(250.0),np.array(250.0),p)[0]*1e3:.1f} mA")
    # Cross-check vs JJ 6V6S transfer sheet *if* it is triode connected (Vg2=Ua)
    jj = [(200,-2,75.6),(150,-2,48.97),(100,-2,28.29),(50,-2,10.7),(200,-6,56.11),(150,-6,33.28),(100,-6,15.93),(50,-6,3.8),
          (200,-12,33.28),(150,-12,15.22),(100,-12,4.75),(200,-20,13.08),(150,-20,3.57)]
    print("   JJ-sheet cross-check assuming screen tied to plate (Ip+Ig2 model vs sheet):")
    errs=[]
    for ua,ug,ia in jj:
        ip_,ig_ = pentode_currents(np.array(float(ug)),np.array(float(ua)),np.array(float(ua)),p)
        m=(ip_+ig_)*1e3; errs.append(m/ia)
        print(f"     Ua=Ug2={ua:3d} Ug={ug:4d}: sheet {ia:6.2f} mA  model {m:6.2f} mA  ratio {m/ia:.2f}")
    return dict(mu=mu, ex=ex, kg1=kg1, kp=kp, fs0=fs0, vk=vk, q=q, va=va,
                anchor=dict(ip_mA=float(ip*1e3), ig2_mA=float(ig2*1e3), gm_mS=float(gm*1e3), rp_kohm=float(1/gp/1e3)),
                jj_triode_connected_ratio_mean=float(np.mean(errs)), jj_triode_connected_ratio_spread=float(np.std(errs)))
fit_6v6 = fit_pentode()

# ---------------------------------------------------------------- 5Y3 rectifier (S23 / S20)
# Digitized forward characteristic (right panel of JJ 5Y3S sheet): Ua [V] -> Ia [mA]
Y3 = np.array([(10,14.5),(20,33),(30,51),(40,73.5),(50,96),(60,125),(70,162),(80,205),(90,243),(100,280)])
A = np.vstack([Y3[:,0], Y3[:,0]**2]).T
coef, *_ = np.linalg.lstsq(A / Y3[:,1:2], np.ones(len(Y3)), rcond=None)   # relative-error LSQ
k1, k2 = coef
pred = (A @ coef)
print(f"[5Y3] I[mA] = {k1:.4f} V + {k2:.5f} V^2 ; rel-rms {100*np.sqrt(np.mean((pred/Y3[:,1]-1)**2)):.1f}%")
fit_5y3 = dict(k1_mA_per_V=float(k1), k2_mA_per_V2=float(k2), source="S23 right panel, digitized", n_points=len(Y3))

# ---------------------------------------------------------------- write outputs
def wcsv(name, header, rows):
    with open(os.path.join(ROOT, "data", "tubes", name), "w") as f:
        f.write(header + "\n")
        for r in rows: f.write(",".join(str(x) for x in r) + "\n")
wcsv("12AY7_transfer_JJ_S21.csv", "# DIGITIZED from dossier Fig.9 (JJ 12AY7 transfer sheet, S21); columns: Ua_V,Ug_V,Ia_mA", [tuple(r) for r in AY7])
wcsv("12AX7_transfer_JJ_S24.csv", "# DIGITIZED from dossier Fig.10 (JJ ECC83S transfer sheet, S24); columns: Ua_V,Ug_V,Ia_mA", [tuple(r) for r in AX7])
wcsv("5Y3_forward_JJ_S23.csv", "# DIGITIZED from dossier Fig.12 (JJ 5Y3S sheet right panel, S23); columns: Ua_V,Ia_mA", [tuple(r) for r in Y3])

out = dict(
    note="Candidate constitutive-model parameters for a nominal public-source model. NOT measured on a specimen.",
    triode_koren={"12AY7": fit_ay7, "12AX7": fit_ax7},
    pentode_6V6=fit_6v6,
    rectifier_5Y3=fit_5y3)
with open(os.path.join(ROOT, "data", "tubes", "fitted_models.json"), "w") as f:
    json.dump(out, f, indent=2)

h = f"""// GENERATED by scripts/fit_tubes.py -- do not edit by hand.
// Candidate nominal parameters; see data/tubes/fitted_models.json for residuals and provenance.
#pragma once
namespace cd57::fit {{
struct KorenTriodeFit {{ double mu, ex, kg1, kp, kvb; }};
struct BeamPentodeFit {{ double mu, ex, kg1, kp, fs0, vk, q, va; }};
inline constexpr KorenTriodeFit k12AY7 {{ {fit_ay7['mu']:.9g}, {fit_ay7['ex']:.9g}, {fit_ay7['kg1']:.9g}, {fit_ay7['kp']:.9g}, {fit_ay7['kvb']:.9g} }};
inline constexpr KorenTriodeFit k12AX7 {{ {fit_ax7['mu']:.9g}, {fit_ax7['ex']:.9g}, {fit_ax7['kg1']:.9g}, {fit_ax7['kp']:.9g}, {fit_ax7['kvb']:.9g} }};
inline constexpr BeamPentodeFit k6V6   {{ {fit_6v6['mu']:.9g}, {fit_6v6['ex']:.9g}, {fit_6v6['kg1']:.9g}, {fit_6v6['kp']:.9g}, {fit_6v6['fs0']:.9g}, {fit_6v6['vk']:.9g}, {fit_6v6['q']:.9g}, {fit_6v6['va']:.9g} }};
// 5Y3 per-anode forward law: I[A] = k1*V + k2*V^2   (V in volts, valid 0..~100 V, extrapolated beyond)
inline constexpr double k5Y3_k1 = {fit_5y3['k1_mA_per_V']*1e-3:.9g};
inline constexpr double k5Y3_k2 = {fit_5y3['k2_mA_per_V2']*1e-3:.9g};
}} // namespace cd57::fit
"""
with open(os.path.join(ROOT, "generated", "TubeFits.h"), "w") as f: f.write(h)
print("wrote generated/TubeFits.h")
