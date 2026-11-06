// validate_tubes -- device-level checks of src/dsp/Devices.h (dossier Sec. 10, 16, 21 'Elementary linear stamps').
//
//  1. every analytic Jacobian entry against a central finite difference over the full domain the solver can visit
//     (including Vgk > 0, Vpk ~ 0, small Vsk; Sec. 21: "differentiate with respect to every controlling terminal voltage");
//  2. continuity of current and derivative across the model's boundaries (Sec. 16: "test continuity of current and derivatives
//     across ... low-voltage limits"): a trapezoid-consistency scan along every axis;
//  3. physical sign / monotonicity of every electrode current;
//  4. TRANSCRIPTION: the C++ laws that the circuit solver actually calls reproduce the pixel-digitised JJ sheets
//     (data/tubes/digitized) with the error scripts/fit_tubes.py reported -- a bug in the C++ port of a fitted law would show here;
//  5. the strapped-screen consistency of the 6V6 law (Ip + Is = Ik at Vp = Vs) that the JJ triode-connected sheet relies on.
//
// Nothing here compares against a Custom Deluxe specimen: it checks that the implementation is the model that was fitted.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Devices.h"
#include "DigitizedCsv.h"

#ifndef CD57_DIGITIZED_DIR
#define CD57_DIGITIZED_DIR "data/tubes/digitized"
#endif

using namespace cd57;
using cd57tools::loadDigitized;
using cd57tools::paramValue;

namespace {

int nFail = 0, nPass = 0;
void check(bool ok, const std::string& what, const std::string& detail = "")
{
    (ok ? nPass : nFail)++;
    std::printf("  %s  %s%s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "   [", detail.c_str(), detail.empty() ? "" : "]");
}
std::string fmt(const char* f, double a, double b = 0, double c = 0)
{
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

// relative-or-absolute mismatch between an analytic derivative and a central difference
double mismatch(double analytic, double fd, double absFloor)
{
    return std::fabs(analytic - fd) / (std::fabs(fd) + std::fabs(analytic) + absFloor);
}

// ---------------------------------------------------------------------------------------------- 1. Jacobians
void jacobians()
{
    std::printf("\n== 1. Analytic Jacobians vs central finite differences\n");
    const double h = 1e-5;
    for (const char* type : {"12AY7", "12AX7"}) {
        const TriodeParams p = triodeParams(type);
        double worst = 0; int n = 0; double wg = 0, wp = 0;
        for (double vg = -14; vg <= 4.01; vg += 0.7)
            for (double vp = -5; vp <= 420; vp += 17) {
                TriodeOut o, a, b;
                evalTriode(p, vg, vp, 1.0, o);
                evalTriode(p, vg + h, vp, 1.0, a); evalTriode(p, vg - h, vp, 1.0, b);
                const double gm = (a.ip - b.ip) / (2 * h), gmg = (a.ig - b.ig) / (2 * h);
                evalTriode(p, vg, vp + h, 1.0, a); evalTriode(p, vg, vp - h, 1.0, b);
                const double gp = (a.ip - b.ip) / (2 * h);
                const double m1 = mismatch(o.dip_dvgk, gm, 1e-9), m2 = mismatch(o.dip_dvpk, gp, 1e-9), m3 = mismatch(o.dig_dvgk, gmg, 1e-9);
                wg = std::max(wg, m1); wp = std::max(wp, m2); worst = std::max({worst, m1, m2, m3}); ++n;
            }
        check(worst < 2e-3, std::string(type) + ": dIp/dVgk, dIp/dVpk, dIg/dVgk match finite differences", fmt("worst mismatch %.2e over %.0f points", worst, n) + " (gm " + fmt("%.1e", wg) + ", gp " + fmt("%.1e", wp) + ")");
    }
    {
        const PentodeParams p = pentodeParams();
        double worst = 0; int n = 0;
        for (double vg = -50; vg <= 12.01; vg += 3.1)
            for (double vp = -5; vp <= 560; vp += 31)
                for (double vs = 0.5; vs <= 430; vs += 41) {
                    PentodeOut o, a, b;
                    evalPentode(p, vg, vp, vs, 1.0, o);
                    for (int k = 0; k < 3; ++k) {
                        double d[3] = {0, 0, 0};
                        d[k] = h;
                        evalPentode(p, vg + d[0], vp + d[1], vs + d[2], 1.0, a);
                        evalPentode(p, vg - d[0], vp - d[1], vs - d[2], 1.0, b);
                        worst = std::max({worst, mismatch(o.dip[k], (a.ip - b.ip) / (2 * h), 1e-8), mismatch(o.dis[k], (a.is - b.is) / (2 * h), 1e-8)});
                        if (k == 0) worst = std::max(worst, mismatch(o.dig_dvgk, (a.ig - b.ig) / (2 * h), 1e-8));
                    }
                    ++n;
                }
        check(worst < 5e-3, "6V6: all six dIp,dIs/d(Vgk,Vpk,Vsk) and dIg/dVgk match finite differences", fmt("worst mismatch %.2e over %.0f points", worst, n));
    }
    for (int silicon = 0; silicon < 2; ++silicon) {
        DiodeParams dp; if (silicon) dp.type = RectifierType::Silicon;
        double worst = 0;
        for (double v = -60; v <= 150; v += 0.37) {
            double i, di, ia, ib, d_;
            evalDiode(dp, v, 1.0, i, di);
            evalDiode(dp, v + 1e-4, 1.0, ia, d_); evalDiode(dp, v - 1e-4, 1.0, ib, d_);
            worst = std::max(worst, mismatch(di, (ia - ib) / 2e-4, 1e-9));
        }
        check(worst < 2e-3, silicon ? "silicon rectifier: dI/dV matches a finite difference" : "5Y3 rectifier: dI/dV matches a finite difference", fmt("worst mismatch %.2e", worst));
    }
}

// ---------------------------------------------------------------------------------------------- 2. continuity
// trapezoid consistency: |I(x+d) - I(x) - d (I'(x) + I'(x+d))/2| small for every step => no jump in value or derivative
template <class F>
double trapezoidDefect(F fn, double lo, double hi, double step)
{
    double worst = 0, scale = 0;
    for (double x = lo; x <= hi; x += step) { double i0, d0; fn(x, i0, d0); scale = std::max(scale, std::fabs(d0)); }
    for (double x = lo; x + step <= hi; x += step) {
        double i0, d0, i1, d1;
        fn(x, i0, d0); fn(x + step, i1, d1);
        worst = std::max(worst, std::fabs(i1 - i0 - step * 0.5 * (d0 + d1)) / (step * scale + 1e-30));
    }
    return worst;
}

void continuity()
{
    std::printf("\n== 2. Continuity of current and derivative across the model's boundaries (trapezoid-consistency scan, step 0.02 V)\n");
    const double st = 0.02;
    for (const char* type : {"12AY7", "12AX7"}) {
        const TriodeParams p = triodeParams(type);
        const double d1 = trapezoidDefect([&](double v, double& i, double& d) { TriodeOut o; evalTriode(p, v, 150.0, 1.0, o); i = o.ip; d = o.dip_dvgk; }, -8, 3, st);
        const double d2 = trapezoidDefect([&](double v, double& i, double& d) { TriodeOut o; evalTriode(p, -1.0, v, 1.0, o); i = o.ip; d = o.dip_dvpk; }, -3, 10, st);
        const double d3 = trapezoidDefect([&](double v, double& i, double& d) { TriodeOut o; evalTriode(p, v, 150.0, 1.0, o); i = o.ig; d = o.dig_dvgk; }, -3, 3, st);
        check(std::max({d1, d2, d3}) < 0.05, std::string(type) + ": Ip(Vgk), Ip(Vpk) through the plate-reversal knee, Ig(Vgk) through the onset are C1", fmt("worst defect %.3f of the local slope", std::max({d1, d2, d3})));
    }
    const PentodeParams p = pentodeParams();
    const double e1 = trapezoidDefect([&](double v, double& i, double& d) { PentodeOut o; evalPentode(p, v, 300.0, 280.0, 1.0, o); i = o.ip; d = o.dip[0]; }, -40, 5, st);
    const double e2 = trapezoidDefect([&](double v, double& i, double& d) { PentodeOut o; evalPentode(p, -20.0, v, 280.0, 1.0, o); i = o.ip; d = o.dip[1]; }, -3, 60, st);
    const double e3 = trapezoidDefect([&](double v, double& i, double& d) { PentodeOut o; evalPentode(p, -20.0, 300.0, v, 1.0, o); i = o.is; d = o.dis[2]; }, -2, 20, st);
    const double e4 = trapezoidDefect([&](double v, double& i, double& d) { PentodeOut o; evalPentode(p, v, 300.0, 280.0, 1.0, o); i = o.ig; d = o.dig_dvgk; }, -2, 20, st);
    check(std::max({e1, e2, e3, e4}) < 0.05, "6V6: Ip(Vgk), Ip(Vpk) through the plate knee, Is(Vsk) through the screen-low limit, Ig(Vgk) through the onset are C1", fmt("worst defect %.3f of the local slope", std::max({e1, e2, e3, e4})));
}

// ---------------------------------------------------------------------------------------------- 3. signs / monotonicity
void physical()
{
    std::printf("\n== 3. Physical signs and monotonicity\n");
    bool ok = true; std::string why;
    for (const char* type : {"12AY7", "12AX7"}) {
        const TriodeParams p = triodeParams(type);
        for (double vp = 0; vp <= 400; vp += 20) {
            double prev = -1, prevg = -1;
            for (double vg = -12; vg <= 3; vg += 0.25) {
                TriodeOut o; evalTriode(p, vg, vp, 1.0, o);
                if (o.ip < 0 || o.ig < 0 || o.ip + 1e-15 < prev || o.ig + 1e-15 < prevg) { ok = false; why = std::string(type) + " Vp=" + std::to_string(vp) + " Vg=" + std::to_string(vg); }
                prev = o.ip; prevg = o.ig;
            }
        }
    }
    check(ok, "triodes: Ip and Ig >= 0 and non-decreasing in Vgk everywhere", why);
    ok = true; why.clear();
    const PentodeParams p = pentodeParams();
    for (double vs : {50.0, 150.0, 300.0}) for (double vp : {10.0, 100.0, 300.0, 500.0}) {
        double pi = -1, ps = -1, pg = -1;
        for (double vg = -45; vg <= 6; vg += 0.5) {
            PentodeOut o; evalPentode(p, vg, vp, vs, 1.0, o);
            if (o.ip < 0 || o.is < 0 || o.ig < 0 || o.ip + 1e-15 < pi || o.is + 1e-15 < ps || o.ig + 1e-15 < pg) { ok = false; why = "Vs=" + std::to_string(vs) + " Vp=" + std::to_string(vp) + " Vg=" + std::to_string(vg); }
            pi = o.ip; ps = o.is; pg = o.ig;
        }
    }
    check(ok, "6V6: Ip, Is, Ig >= 0 and non-decreasing in Vgk everywhere", why);
    PentodeOut a, b; evalPentode(p, -12.5, 250, 250, 1.0, a);
    check(a.ip > 0.8 * (a.ip + a.is) && a.is > 0.03 * (a.ip + a.is), "6V6: the screen takes a physical minority share of the cathode current at 250 V / -12.5 V", fmt("screen share %.1f %%", 100 * a.is / (a.ip + a.is)));
    // strapped screen: Ip + Is must equal the cathode current law the JJ sheet measures (Early factor is exactly 1 at Vp = Vs)
    double worst = 0;
    for (double v : {60.0, 120.0, 200.0, 300.0, 450.0}) for (double vg : {-30.0, -15.0, -5.0, 0.0}) {
        PentodeOut lo; evalPentode(p, vg, v, v, 1.0, lo);
        const double ik = lo.ip + lo.is;
        const double mu = p.mu, kp = p.kp, kvb = p.kvb, ex = p.ex, kg1 = p.kg1;
        double dsm;
        const double vsm = smoothPos(v, 1.0, dsm);                 // the model's documented smoothing of the screen voltage near 0
        const double s = std::sqrt(kvb + vsm * vsm), u = kp * (1.0 / mu + vg / s);
        const double e1 = (vsm / kp) * (u > 30 ? u : std::log1p(std::exp(u)));
        const double expect = std::pow(e1, ex) / kg1;
        worst = std::max(worst, std::fabs(ik - expect) / (expect + 1e-9));
    }
    check(worst < 1e-9, "6V6: with the screen strapped to the plate (Vs = Vp) Ip + Is equals the Koren cathode-current law that was fitted (at the smoothed Vs)", fmt("worst relative difference %.1e", worst));
}

// ---------------------------------------------------------------------------------------------- 4. transcription vs the digitised sheets
struct Err { double rms = 0, mx = 0, relRms = 0; int n = 0; };

Err triodeVsSheet(const char* type, const std::string& csv, bool region)
{
    const auto cur = loadDigitized(std::string(CD57_DIGITIZED_DIR) + "/" + csv);
    const TriodeParams p = triodeParams(type);
    double s2 = 0, r2 = 0, mx = 0; int n = 0, nr = 0;
    for (const auto& kv : cur) {
        const bool transfer = kv.first.first == "transfer";
        const double par = paramValue(kv.first.second);
        for (const auto& pt : kv.second) {
            const double vg = transfer ? pt.first : par, vp = transfer ? par : pt.first;
            if (region && !(vp >= 50 && vp <= 350 && vg >= -6.0 && vg <= 0.0)) continue;
            TriodeOut o; evalTriode(p, vg, vp, 1.0, o);
            const double e = o.ip * 1e3 - pt.second;
            s2 += e * e; mx = std::max(mx, std::fabs(e)); ++n;
            if (pt.second > 0.5) { r2 += (e / pt.second) * (e / pt.second); ++nr; }
        }
    }
    return {std::sqrt(s2 / n), mx, nr ? std::sqrt(r2 / nr) : 0.0, n};
}

void transcription()
{
    std::printf("\n== 4. The C++ laws reproduce the pixel-digitised JJ sheets (Sec. 16; every point, no thinning)\n");
    const Err a7 = triodeVsSheet("12AY7", "12AY7_JJ_S21.csv", true), x7 = triodeVsSheet("12AX7", "12AX7_JJ_S24.csv", true);
    std::printf("  INFO  12AY7 operating region (Vp 50-350 V, Vg -6..0 V): rms %.3f mA, max %.3f mA, relative %.1f %% above 0.5 mA, n=%d\n", a7.rms, a7.mx, 100 * a7.relRms, a7.n);
    std::printf("  INFO  12AX7 operating region                          : rms %.3f mA, max %.3f mA, relative %.1f %% above 0.5 mA, n=%d\n", x7.rms, x7.mx, 100 * x7.relRms, x7.n);
    check(a7.rms < 0.12 && a7.mx < 0.40, "12AY7: C++ law vs digitised JJ sheet within the fit's reported error (rms < 0.12 mA, max < 0.40 mA)", fmt("rms %.3f mA, max %.3f mA", a7.rms, a7.mx));
    check(x7.rms < 0.10 && x7.mx < 0.35, "12AX7: C++ law vs digitised JJ sheet within the fit's reported error (rms < 0.10 mA, max < 0.35 mA)", fmt("rms %.3f mA, max %.3f mA", x7.rms, x7.mx));

    // 6V6 (screen strapped to the plate)
    const auto cur = loadDigitized(std::string(CD57_DIGITIZED_DIR) + "/6V6_JJ_S22.csv");
    const PentodeParams p = pentodeParams();
    double s2 = 0, s2all = 0, r2 = 0, mx = 0; int n = 0, nall = 0, nr = 0;
    for (const auto& kv : cur) {
        const std::string& panel = kv.first.first; const std::string& par = kv.first.second;
        if (par.rfind("Ig@", 0) == 0 || par.rfind("Pa=", 0) == 0) continue;
        const double pv = paramValue(par);
        for (const auto& pt : kv.second) {
            const double vg = panel == "transfer" ? pt.first : pv, vp = panel == "transfer" ? pv : pt.first;
            PentodeOut o; evalPentode(p, vg, vp, vp, 1.0, o);
            const double e = (o.ip + o.is) * 1e3 - pt.second;
            s2all += e * e; ++nall;
            if (vp >= 150 && vp <= 400 && vg <= 0 && vg >= -30) { s2 += e * e; mx = std::max(mx, std::fabs(e)); ++n; if (pt.second >= 10) { r2 += (e / pt.second) * (e / pt.second); ++nr; } }
        }
    }
    std::printf("  INFO  6V6 (strapped) amp region (Vs=Vp 150-400 V, Vg -30..0 V): rms %.2f mA, max %.2f mA, relative %.1f %% above 10 mA, n=%d; whole sheet rms %.2f mA\n", std::sqrt(s2 / n), mx, 100 * std::sqrt(r2 / nr), n, std::sqrt(s2all / nall));
    check(std::sqrt(s2 / n) < 3.2 && std::sqrt(r2 / nr) < 0.10, "6V6: C++ law (Ip + Is, screen strapped) vs digitised JJ sheet within the fit's reported error (rms < 3.2 mA, relative < 10 %)", fmt("rms %.2f mA, relative %.1f %%", std::sqrt(s2 / n), 100 * std::sqrt(r2 / nr)));

    // grid-current plateau from the dashed curves (Ua 150-350 V)
    double worstIg = 0;
    for (const auto& kv : cur) {
        if (kv.first.second.rfind("Ig@", 0) != 0) continue;
        const double ug = paramValue(kv.first.second);
        double acc = 0; int cnt = 0;
        for (const auto& pt : kv.second) if (pt.first >= 150 && pt.first <= 350) { acc += pt.second; ++cnt; }
        PentodeOut o; evalPentode(p, ug, 250.0, 250.0, 1.0, o);
        const double sheet = acc / cnt, model = o.ig * 1e3;
        std::printf("  INFO  6V6 grid current at Ug=%+.0f V, Ua 150-350 V: sheet plateau %.2f mA, model %.2f mA\n", ug, sheet, model);
        worstIg = std::max(worstIg, std::fabs(model - sheet) / sheet);
    }
    check(worstIg < 0.05, "6V6: grid-conduction law reproduces the digitised dashed Ig plateaus at Ug = +5 and +20 V within 5 %", fmt("worst %.1f %%", 100 * worstIg));

    // external anchor consistency (RCA 6V6GT rating point, NOT in the dossier)
    PentodeOut o; evalPentode(p, -12.5, 250.0, 250.0, 1.0, o);
    std::printf("  INFO  EXTERNAL RCA 6V6GT rating point 250 V / 250 V / -12.5 V (not in the dossier): model Ip = %.1f mA (RCA 45), Is = %.1f mA (RCA 4.5)\n", o.ip * 1e3, o.is * 1e3);
}

// the realtime solver evaluates the same laws through lookup tables (Devices.h): they must agree with the closed forms
void tables()
{
    auto rel = [](double a, double b, double scale) { return std::fabs(a - b) / (std::fabs(b) + scale); };
    for (const char* type : {"12AY7", "12AX7"}) {
        const TriodeParams p = triodeParams(type);
        double worst = 0, worstD = 0;
        for (double vg = -60.0; vg <= 6.0; vg += 0.0371)
            for (double vp = -20.0; vp <= 500.0; vp += 1.37) {
                TriodeOut a, b;
                evalTriode(p, vg, vp, 1.0, a); evalTriodeFast(p, vg, vp, 1.0, b);
                worst = std::max({worst, rel(b.ip, a.ip, 1e-9), rel(b.ig, a.ig, 1e-9)});
                worstD = std::max({worstD, rel(b.dip_dvgk, a.dip_dvgk, 1e-9), rel(b.dip_dvpk, a.dip_dvpk, 1e-10), rel(b.dig_dvgk, a.dig_dvgk, 1e-9)});
            }
        check(worst < 1e-6 && worstD < 1e-5, std::string(type) + ": realtime table evaluation equals the closed-form law (currents and Jacobian)", fmt("worst relative error %.1e (currents), %.1e (derivatives)", worst, worstD));
    }
    const PentodeParams p = pentodeParams();
    double worst = 0, worstD = 0;
    for (double vg = -80.0; vg <= 25.0; vg += 0.0731)
        for (double vp = -20.0; vp <= 520.0; vp += 3.7)
            for (double vs = -10.0; vs <= 480.0; vs += 23.3) {
                PentodeOut a, b;
                evalPentode(p, vg, vp, vs, 1.0, a); evalPentodeFast(p, vg, vp, vs, 1.0, b);
                worst = std::max({worst, rel(b.ip, a.ip, 1e-9), rel(b.is, a.is, 1e-9), rel(b.ig, a.ig, 1e-9)});
                for (int k = 0; k < 3; ++k) worstD = std::max({worstD, rel(b.dip[k], a.dip[k], 1e-10), rel(b.dis[k], a.dis[k], 1e-10)});
                worstD = std::max(worstD, rel(b.dig_dvgk, a.dig_dvgk, 1e-9));
            }
    check(worst < 1e-6 && worstD < 1e-5, "6V6: realtime table evaluation equals the closed-form law (currents and Jacobian)", fmt("worst relative error %.1e (currents), %.1e (derivatives)", worst, worstD));
}

} // namespace

int main()
{
    std::printf("validate_tubes: device laws in src/dsp/Devices.h\n");
    jacobians();
    continuity();
    physical();
    transcription();
    tables();
    std::printf("\nSUMMARY: %d passed, %d failed\n", nPass, nFail);
    return nFail ? 1 : 0;
}
