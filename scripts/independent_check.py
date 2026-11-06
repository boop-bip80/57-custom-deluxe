#!/usr/bin/env python3
"""
Independent re-implementation of the DC operating point and small-signal gains of the nominal netlist (dossier Sec. 21 'verify against
an independent circuit solver', Sec. 29 milestone C 'independent simulation').

SHARES NO CODE with src/dsp: its own netlist parser, own element stamps, own Koren / pentode / grid-conduction equations (typed in from
the model definition in scripts/fit_tubes.py and the fitted constants in generated/TubeFits.h), own DC solver (scipy.optimize.root) and
own complex AC solve.  What it therefore checks: that the C++ netlist reader, stamps, device-law implementation, DC solver and AC solver
all implement the model that is written down.  It does NOT check the model against a Custom Deluxe (nothing can, without hardware) and it
shares the *definition* of the device laws with the C++ code, as any transcription check must.

The C++ side is `cd57_dump_reference` (tools/dump_reference.cpp): the engine configured as the validation baseline (Instrument 1, inst vol
0.5, mic vol 0, tone 0.7, 8 ohm dummy load) with the supply capacitors scaled x1000 so the rails carry no ripple (the AC solver linearises at
one point).  B+1 is held at the value the C++ solver found (rectifier and power transformer are omitted here).

Usage: independent_check.py --cpp-ref-exe PATH      exit status 0 = every threshold met
"""
import argparse, math, os, re, subprocess, sys
import numpy as np
from scipy.optimize import root

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# acceptance thresholds (stated project limits, not Fender specifications)
DC_TOL_V = 0.5            # per node, volts
AC_TOL_DB = 0.02          # magnitude, dB
AC_TOL_DEG = 0.1          # phase, degrees


def parse_val(s):
    m = re.match(r"^([-+0-9.eE]+)([a-zA-Z]*)$", s)
    v = float(m.group(1)); suf = m.group(2).lower()
    if suf.startswith("meg"):
        return v * 1e6
    mult = {"": 1, "t": 1e12, "g": 1e9, "k": 1e3, "m": 1e-3, "u": 1e-6, "n": 1e-9, "p": 1e-12, "f": 1e-15}
    return v * mult[suf[:1]] if suf else v


def softplus(x):
    return x if x > 30 else math.log1p(math.exp(x))


def smooth_pos(x, eps2):
    r = math.sqrt(x * x + eps2)
    return 0.5 * (x + r)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp-ref-exe", required=True)
    args = ap.parse_args()

    net_text = open(os.path.join(ROOT, "data", "netlist", "cd57_nominal.net")).read()
    fit_h = open(os.path.join(ROOT, "generated", "TubeFits.h")).read()

    def grab(name, n):
        m = re.search(name + r"\s*\{([^}]*)\}", fit_h)
        vals = [float(x) for x in m.group(1).split(",")]
        assert len(vals) == n, (name, vals)
        return vals
    AY7, AX7 = grab("k12AY7", 5), grab("k12AX7", 5)
    V6 = grab("k6V6 ", 9)                  # mu, ex, kg1, kp, kvb, fs0, vk, q, va
    V6G = grab("k6V6grid", 4)              # a, imax, vc, sg

    # ------------------------------------------------------------------ device laws (written independently of src/dsp/Devices.h)
    def grid_triode(vgk):
        u = (vgk - (-0.5)) / 0.15
        if u < -14.0:
            return 0.0
        return 0.35e-3 * (0.15 * softplus(u)) ** 1.5

    def triode_i(vgk, vpk, p):
        mu, ex, kg1, kp, kvb = p
        vp = smooth_pos(vpk, 0.25)
        e1 = (vp / kp) * softplus(kp * (1.0 / mu + vgk / math.sqrt(kvb + vp * vp)))
        return max(e1, 1e-12) ** ex / kg1, grid_triode(vgk)

    def grid_6v6(vgk):
        a, imax, vc, sg = V6G
        u = (vgk - vc) / sg
        if u < -14.0:
            return 0.0
        t = a * (sg * softplus(u)) ** 1.5
        return t / math.sqrt(1.0 + (t / imax) ** 2)

    def pent_i(vgk, vpk, vsk):
        mu, ex, kg1, kp, kvb, fs0, vk, q, va = V6
        vs = smooth_pos(vsk, 1.0)
        vp = smooth_pos(vpk, 0.25)
        vsf = max(vs, 1e-3)
        sc = math.sqrt(kvb + vsf * vsf)
        e1 = (vsf / kp) * softplus(kp * (1.0 / mu + vgk / sc))
        ik = max(e1, 1e-12) ** ex / kg1
        fs = fs0 + (1 - fs0) * (math.exp(-((vp / vk) ** q)) if vp > 1e-9 else 1.0)
        early = 1.0 + (vpk - vsk) / va
        return ik * (1 - fs) * early, ik * fs, grid_6v6(vgk)

    # ------------------------------------------------------------------ netlist parser
    nodes = {}; R = []; C = []; L = []; TR = []; PE = []; XF = None

    def nid(n):
        if n in ("0", "gnd"):
            return -1
        if n not in nodes:
            nodes[n] = len(nodes)
        return nodes[n]
    for line in net_text.splitlines():
        line = line.split("#")[0].strip()
        if not line or line.startswith("."):
            continue
        tok = line.split(); kv = {}; pos = []
        for t in tok[1:]:
            if "=" in t:
                a, b = t.split("=", 1); kv[a] = b
            elif t in ("dyn", "novar"):
                pass
            else:
                pos.append(t)
        ty = tok[0]
        if ty == "R": R.append([pos[0], nid(pos[1]), nid(pos[2]), parse_val(pos[3])])
        elif ty == "C": C.append([pos[0], nid(pos[1]), nid(pos[2]), parse_val(pos[3])])
        elif ty == "L": L.append([pos[0], nid(pos[1]), nid(pos[2]), parse_val(pos[3])])
        elif ty == "TRIODE": TR.append((nid(pos[1]), nid(pos[2]), nid(pos[3]), AY7 if kv["type"] == "12AY7" else AX7))
        elif ty == "PENTODE": PE.append((nid(pos[1]), nid(pos[2]), nid(pos[3]), nid(pos[4])))
        elif ty == "DIODE": pass
        elif ty == "XFMR":
            g = lambda k: parse_val(kv[k])
            XF = dict(n1=[nid(kv["a1"]), nid(kv["b1"]), nid(kv["s1"])], n2=[nid(kv["a2"]), nid(kv["b2"]), nid(kv["s2"])],
                      N=[g("Nh"), g("Nh"), g("Ns")], R=[g("Ra"), g("Rb"), g("Rs")], Ll=[g("Lla"), g("Llb"), g("Lls")],
                      P0=g("P0"), Pair=g("Pair"), sat=("sat" in kv and parse_val(kv["sat"]) != 0.0))
        else:
            raise SystemExit("unknown element " + ty)
    N = len(nodes)
    if XF["sat"]:
        raise SystemExit("this check models the default linear core; the netlist enables the saturating law")

    # ------------------------------------------------------------------ runtime controls (re-derived from the documented control mapping)
    RT, Rc = 1e6, 20.0
    def alpha15(x): return (math.exp(3.4657 * x) - 1) / (math.exp(3.4657) - 1)
    inst, mic, tone = 0.5, 0.0, 0.7
    a_i, a_m, beta = alpha15(inst), alpha15(mic), tone
    ON, OFF = 0.02, 1e11
    setv = {"Rvb_lo": max(a_i * RT, Rc), "Rvb_hi": max((1 - a_i) * RT, Rc), "Rvn_lo": max(a_m * RT, Rc), "Rvn_hi": max((1 - a_m) * RT, Rc),
            "Rt_b": max(beta * RT, Rc), "Rt_a": max((1 - beta) * RT, Rc),
            "Sw_b1": OFF, "Sw_b2": ON, "Sw_n1": ON, "Sw_n2": ON, "Rsel_b1": ON, "Rsel_b2": OFF, "Rsel_n1": OFF, "Rsel_n2": OFF,
            "Rpt_b1b2": OFF, "Rpt_n1n2": OFF, "Rpt_b1n1": OFF, "Rpt_b1n2": OFF, "Rpt_b2n1": OFF, "Rpt_b2n2": OFF,
            "Re": OFF, "Rdum": 8.0, "Rext": OFF, "Rsrc": 100.0, "Rs_a": 90.0, "Rs_b": 90.0}
    fs, Mms, Re0, Qes, Qms, Le = 98.0, 0.0195, 6.4, 1.0, 5.5, 0.7e-3
    w0 = 2 * math.pi * fs; Bl = math.sqrt(w0 * Mms * Re0 / Qes); Rms = w0 * Mms / Qms; Cms = 1 / (w0 * w0 * Mms)
    spk = {"Res": Bl * Bl / Rms}; cset = {"Cmes": Mms / (Bl * Bl), "Ccab": 20e-12}; lset = {"Lces": Cms * Bl * Bl, "Le": Le, "Lpu": 1e-6}
    for r in R:
        if r[0] in setv: r[3] = setv[r[0]]
        if r[0] in spk: r[3] = spk[r[0]]
    for c in C:
        if c[0] in cset: c[3] = cset[c[0]]
        if c[0] in ("C1", "C2", "C3"): c[3] *= 1000.0
    for l in L:
        if l[0] in lset: l[3] = lset[l[0]]

    # ------------------------------------------------------------------ the C++ reference numbers
    out = subprocess.run([args.cpp_ref_exe], capture_output=True, text=True, check=True).stdout.splitlines()
    ref, cpp, sect = {}, {}, None
    for line in out:
        if line in ("DC", "AC"):
            sect = line; continue
        t = line.split()
        if sect == "DC":
            ref[t[0]] = float(t[1])
        elif sect == "AC":
            cpp[(float(t[0]), t[1])] = (float(t[2]), float(t[3]))
    VB1 = ref["b1"]
    GMIN = 1e-12
    b1 = nodes["b1"]

    def device_currents(v):
        F = np.zeros(N)
        V = lambda k: 0.0 if k < 0 else v[k]
        for (p, g, k, par) in TR:
            ip, ig = triode_i(V(g) - V(k), V(p) - V(k), par)
            if p >= 0: F[p] += ip
            if g >= 0: F[g] += ig
            if k >= 0: F[k] -= ip + ig
        for (p, g, s, k) in PE:
            ip, is_, ig = pent_i(V(g) - V(k), V(p) - V(k), V(s) - V(k))
            if p >= 0: F[p] += ip
            if s >= 0: F[s] += is_
            if g >= 0: F[g] += ig
            if k >= 0: F[k] -= ip + is_ + ig
        return F

    nu = N + 3

    def dc_resid(x):
        v = x[:N]; iw = x[N:]
        F = np.zeros(nu)
        V = lambda k: 0.0 if k < 0 else v[k]
        for (_, a, b, r) in R:
            i = (V(a) - V(b)) / r
            if a >= 0: F[a] += i
            if b >= 0: F[b] -= i
        for (_, a, b, l) in L:
            i = (V(a) - V(b)) / 1e-4
            if a >= 0: F[a] += i
            if b >= 0: F[b] -= i
        F[:N] += device_currents(v)
        F[:N] += GMIN * v
        F[b1] += 1e3 * (v[b1] - VB1)
        for k in range(3):
            a, b = XF["n1"][k], XF["n2"][k]
            if a >= 0: F[a] += iw[k]
            if b >= 0: F[b] -= iw[k]
            F[N + k] = (V(a) - V(b)) - XF["R"][k] * iw[k]
        return F
    x0 = np.zeros(nu)
    for nm, val in {"b1": VB1, "b2": 0.85 * VB1, "b3": 0.6 * VB1, "p1b": 120, "p1n": 120, "p2a": 160, "p2b": 200, "kcy": 45, "j": 45, "g2b": 45, "p3": 0.95 * VB1, "p4": 0.95 * VB1, "k34": 20}.items():
        x0[nodes[nm]] = val
    sol = root(dc_resid, x0, method="hybr", options={"xtol": 1e-12, "maxfev": 20000})
    resid = float(np.max(np.abs(dc_resid(sol.x))))
    print("independent DC solve: success=%s, max |residual| = %.2e A" % (sol.success, resid))
    xv = sol.x; v = xv[:N]
    print("DC node voltages, independent Python vs the C++ solver:")
    worst_dc = 0.0
    for nm in ["b1", "b2", "b3", "p1b", "p1n", "k1", "p2a", "kb2", "p2b", "kcy", "j", "p3", "p4", "k34", "g2b"]:
        d = v[nodes[nm]] - ref[nm]; worst_dc = max(worst_dc, abs(d))
        print(f"  {nm:4s} {v[nodes[nm]]:9.4f}  {ref[nm]:9.4f}  diff {d:+.4f} V")
    print("worst DC difference: %.4f V (limit %.2f V)" % (worst_dc, DC_TOL_V))

    # ------------------------------------------------------------------ AC small signal about that DC point
    def jac_dev(v):
        J = np.zeros((N, N)); d = 1e-6
        for j in range(N):
            vp = v.copy(); vm = v.copy(); vp[j] += d; vm[j] -= d
            J[:, j] = (device_currents(vp) - device_currents(vm)) / (2 * d)
        return J
    Jd = jac_dev(v)
    dphi = XF["P0"] + XF["Pair"]                    # linear core: constant permeance

    def ac(freq, src="pu"):
        w = 2 * math.pi * freq; n = nu
        A = np.zeros((n, n), dtype=complex)

        def st(a, b, y):
            if a >= 0: A[a, a] += y
            if b >= 0: A[b, b] += y
            if a >= 0 and b >= 0: A[a, b] -= y; A[b, a] -= y
        for i in range(N): A[i, i] += GMIN
        for (_, a, b, r) in R: st(a, b, 1 / r)
        for (_, a, b, c) in C: st(a, b, 1j * w * c)
        for (_, a, b, l) in L: st(a, b, 1 / (1j * w * l))
        A[:N, :N] += Jd
        A[b1, b1] += 1e3
        for k in range(3):
            a, b = XF["n1"][k], XF["n2"][k]
            if a >= 0: A[a, N + k] += 1
            if b >= 0: A[b, N + k] -= 1
            row = N + k
            if a >= 0: A[row, a] -= 1
            if b >= 0: A[row, b] += 1
            A[row, row] += XF["R"][k] + 1j * w * XF["Ll"][k]
            for j in range(3): A[row, N + j] += 1j * w * XF["N"][k] * dphi * XF["N"][j]
        rhs = np.zeros(n, dtype=complex); rhs[nodes[src]] = 1.0
        return np.linalg.solve(A, rhs)
    print("AC gain (EMF -> node), independent Python vs the C++ AC solver:")
    worst, worstp = 0.0, 0.0
    for f in (100.0, 1000.0, 5000.0):
        x = ac(f)
        for nm in ["b_g", "p1b", "m", "p2a", "p2b", "kcy", "g3", "p3", "sp"]:
            g = x[nodes[nm]] / 100.0
            gd = 20 * math.log10(abs(g)); ph = float(np.angle(g, deg=True))
            cg, cp = cpp[(f, nm)]
            dp = (ph - cp + 180) % 360 - 180
            worst = max(worst, abs(gd - cg)); worstp = max(worstp, abs(dp))
            print(f"  {f:6.0f} Hz {nm:4s} python {gd:9.4f} dB {ph:8.3f} deg | C++ {cg:9.4f} dB {cp:8.3f} deg | diff {gd - cg:+.4f} dB {dp:+.3f} deg")
    print("worst AC magnitude difference %.4f dB (limit %.3f), worst phase difference %.3f deg (limit %.2f)" % (worst, AC_TOL_DB, worstp, AC_TOL_DEG))

    ok = sol.success and worst_dc < DC_TOL_V and worst < AC_TOL_DB and worstp < AC_TOL_DEG
    print("PASS" if ok else "FAIL", " independent re-implementation agrees with the C++ solver")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
