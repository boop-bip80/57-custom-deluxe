// Devices.h -- nonlinear constitutive laws with analytic Jacobians.
//
// Dossier Sec. 10/16: every valve exposes plate, screen AND control-grid current, with current
// conserved at the cathode (I_k = I_p + I_s + I_g).  Exponentials are evaluated with overflow-safe
// softplus; all derivatives are analytic and checked against finite differences in the tests.
//
// Parameter provenance (see data/tubes/fitted_models.json, scripts/fit_tubes.py):
//   triodes  : Koren form fitted to the pixel-digitised JJ transfer + plate sheets, validated on held-out curves (CANDIDATE)
//   6V6      : cathode-current law fitted to the digitised JJ 6V6S triode-connected sheet (Kvb generalisation of Koren's
//              pentode law); screen partition and Early voltage from an EXTERNAL RCA 6V6GT rating point (not in the dossier)
//   grid I   : 6V6 plateau digitised from the sheet's dashed Ig curves (saturating law); onset (vc, sg) and every triode
//              grid law ASSUMED (dossier Sec. 16: "onset and slope need measurements")
//   rectifier: digitised 5Y3S forward curve (CANDIDATE), or silicon diode for the optional mod
// See data/tubes/fitted_models.json for residuals, validation and provenance.
#pragma once
#include <cmath>
#include <algorithm>
#include <string>
#include "TubeFits.h"
#include "HermiteTable.h"
#include <memory>
#include <mutex>

namespace cd57 {

// ---------------------------------------------------------------- numeric helpers
inline void softplus(double u, double& y, double& s)
{
    if (u > 30.0)       { y = u;            s = 1.0; }
    else if (u < -30.0) { const double e = std::exp(u); y = e; s = e; }
    else                { const double e = std::exp(u); y = std::log1p(e); s = e / (1.0 + e); }
}

// smooth max(x,0): returns value, derivative in d
inline double smoothPos(double x, double eps2, double& d)
{
    const double r = std::sqrt(x * x + eps2);
    d = 0.5 * (1.0 + x / r);
    return 0.5 * (x + r);
}

// ---------------------------------------------------------------- grid conduction (ASSUMED law)
struct GridLaw {
    double k    = 0.35e-3; // A / V^1.5
    double vc   = -0.5;    // V   onset offset
    double sg   = 0.15;    // V   smoothing width
    double imax = 0.0;     // A   saturation current; 0 = unsaturated power law
};

// ---------------------------------------------------------------- lookup tables for the realtime transient solve
// The device laws below are unchanged.  The transient solver evaluates them through exact algebraic factorisations
// whose transcendental factors are 1-D functions tabulated with cubic Hermite interpolation (value AND derivative
// at each knot, so the Jacobian stays consistent with the residual):
//   Koren:   e1^ex = (v/kp)^ex * softplus(u)^ex  ->  A(u) = softplus(u)^ex,  B(v) = v^ex       (one table pair per exponent)
//   grid:    (sg*softplus(u))^1.5 = sg^1.5 * G(u), G(u) = softplus(u)^1.5
//   screen:  E(v) = exp(-(v/vk)^q)
// Outside the tabulated ranges (deep cutoff, v < 4 V, extreme positive grid) the exact closed form is used, so the
// table never extrapolates.  Interpolation error is below 1e-7 relative (checked in validate_tubes); the independent AC
// solver and every device-law validation keep using the exact closed forms.
inline void softplusPow(double u, double ex, double& y, double& dy)
{
    double L, sg;
    if (u > 30.0)       { L = u; sg = 1.0; }
    else                { const double e = std::exp(u); L = std::log1p(e); sg = e / (1.0 + e); }
    y = std::pow(L, ex); dy = ex * std::pow(L, ex - 1.0) * sg;
}
struct KorenTables {
    double ex = 0;
    HermiteTable A, B;
};
inline const KorenTables* korenTables(double ex)
{
    static std::mutex m;
    static std::vector<std::unique_ptr<KorenTables>> cache;
    std::lock_guard<std::mutex> lk(m);
    for (auto& t : cache) if (t->ex == ex) return t.get();
    auto t = std::make_unique<KorenTables>();
    t->ex = ex;
    t->A.build(-20.0, 40.0, 1.0 / 16.0, [ex](double u, double& y, double& dy) { softplusPow(u, ex, y, dy); });
    t->B.build(4.0, 640.0, 0.5, [ex](double v, double& y, double& dy) { y = std::pow(v, ex); dy = ex * std::pow(v, ex - 1.0); });
    cache.push_back(std::move(t));
    return cache.back().get();
}
inline const HermiteTable& gridTable()
{
    static const HermiteTable G = [] { HermiteTable g; g.build(-14.0, 40.0, 1.0 / 16.0, [](double u, double& y, double& dy) { softplusPow(u, 1.5, y, dy); }); return g; }();
    return G;
}

// t = k * x^1.5 with x = sg * softplus((vgk - vc)/sg);  I_g = t / sqrt(1 + (t/imax)^2)  (imax = 0: I_g = t)
// returns I_g and dI_g/dvgk
inline void gridCurrent(const GridLaw& g, double vgk, double& ig, double& dig)
{
    const double u = (vgk - g.vc) / g.sg;
    if (u < -14.0) { ig = 0.0; dig = 0.0; return; }
    double sp, sig;
    softplus(u, sp, sig);
    const double x = g.sg * sp;
    const double r = std::sqrt(x);
    const double t = g.k * x * r;
    const double dt = g.k * 1.5 * r * sig;
    if (g.imax > 0.0) {
        const double q = t / g.imax;
        const double w = 1.0 / std::sqrt(1.0 + q * q);
        ig  = t * w;
        dig = dt * w * w * w;
    } else {
        ig  = t;
        dig = dt;
    }
}

// same law through the table (exact outside it)
inline void gridCurrentFast(const GridLaw& g, double vgk, double& ig, double& dig)
{
    const double u = (vgk - g.vc) / g.sg;
    if (u < -14.0) { ig = 0.0; dig = 0.0; return; }
    const HermiteTable& G = gridTable();
    if (!G.contains(u)) { gridCurrent(g, vgk, ig, dig); return; }
    double y, dy;
    G.eval(u, y, dy);
    const double ks = g.k * g.sg * std::sqrt(g.sg);
    const double t = ks * y, dt = ks * dy / g.sg;
    if (g.imax > 0.0) {
        const double q = t / g.imax;
        const double w = 1.0 / std::sqrt(1.0 + q * q);
        ig = t * w; dig = dt * w * w * w;
    } else { ig = t; dig = dt; }
}

// ---------------------------------------------------------------- triode (Koren form)
struct TriodeParams {
    double mu, ex, kg1, kp, kvb;
    GridLaw grid;
    const KorenTables* tab = nullptr;   // realtime lookup (null: exact closed form only)
    double cK = 0.0;                    // kp^-ex / kg1, set with tab
};

struct TriodeOut {
    double ip, ig;
    double dip_dvgk, dip_dvpk, dig_dvgk;
};

inline TriodeParams triodeParams(const std::string& type)
{
    TriodeParams p{};
    if (type == "12AX7") p = {fit::k12AX7.mu, fit::k12AX7.ex, fit::k12AX7.kg1, fit::k12AX7.kp, fit::k12AX7.kvb, {}};
    else                 p = {fit::k12AY7.mu, fit::k12AY7.ex, fit::k12AY7.kg1, fit::k12AY7.kp, fit::k12AY7.kvb, {}};
    p.tab = korenTables(p.ex); p.cK = std::pow(p.kp, -p.ex) / p.kg1;
    return p;
}

inline void evalTriode(const TriodeParams& p, double vgk, double vpk, double em, TriodeOut& o)
{
    double dvp;
    const double vp = smoothPos(vpk, 0.25, dvp);             // plate cannot conduct reversed
    const double s  = std::sqrt(p.kvb + vp * vp);
    const double u  = p.kp * (1.0 / p.mu + vgk / s);
    double L, sg;
    softplus(u, L, sg);
    double e1 = (vp / p.kp) * L;
    if (e1 < 1e-12) e1 = 1e-12;
    const double e1pow = std::pow(e1, p.ex - 1.0);
    const double ip0 = e1pow * e1 / p.kg1;
    const double dip_de1 = p.ex * e1pow / p.kg1;
    const double de1_dvgk = vp * sg / s;
    const double de1_dvp = L / p.kp - vp * sg * vgk * vp / (s * s * s);
    o.ip = em * ip0;
    o.dip_dvgk = em * dip_de1 * de1_dvgk;
    o.dip_dvpk = em * dip_de1 * de1_dvp * dvp;
    double ig, dig;
    gridCurrent(p.grid, vgk, ig, dig);
    o.ig = em * ig;
    o.dig_dvgk = em * dig;
}

// Realtime form of evalTriode: identical law, transcendental factors from the tables (exact fallback outside them).
inline void evalTriodeFast(const TriodeParams& p, double vgk, double vpk, double em, TriodeOut& o)
{
    const KorenTables* T = p.tab;
    double dvp;
    const double vp = smoothPos(vpk, 0.25, dvp);
    const double is = 1.0 / std::sqrt(p.kvb + vp * vp);
    const double u  = p.kp * (1.0 / p.mu + vgk * is);
    if (!T || T->ex != p.ex || !T->A.contains(u) || !T->B.contains(vp)) { evalTriode(p, vgk, vpk, em, o); return; }
    double A, dA, B, dB;
    T->A.eval(u, A, dA);
    T->B.eval(vp, B, dB);
    const double c = p.cK;
    const double du_dvp = -p.kp * vgk * vp * is * is * is;
    o.ip = em * c * A * B;
    o.dip_dvgk = em * c * B * dA * p.kp * is;
    o.dip_dvpk = em * c * (dB * A + B * dA * du_dvp) * dvp;
    double ig, dig;
    gridCurrentFast(p.grid, vgk, ig, dig);
    o.ig = em * ig;
    o.dig_dvgk = em * dig;
}

// ---------------------------------------------------------------- beam-power pentode (6V6)
struct PentodeParams {
    double mu, ex, kg1, kp, kvb, fs0, vk, q, va;
    GridLaw grid;
    const KorenTables* tab = nullptr;   // realtime lookup (null: exact closed form only)
    double cK = 0.0;                    // kp^-ex / kg1
    std::shared_ptr<const HermiteTable> screen;   // E(vp) = exp(-(vp/vk)^q)
};

struct PentodeOut {
    double ip, is, ig;
    double dip[3];   // d/d(vgk, vpk, vsk)
    double dis[3];
    double dig_dvgk;
};

inline PentodeParams pentodeParams()
{
    const auto& f = fit::k6V6;
    const auto& g = fit::k6V6grid;
    PentodeParams p{f.mu, f.ex, f.kg1, f.kp, f.kvb, f.fs0, f.vk, f.q, f.va, {}};
    p.grid.k = g.a;  p.grid.vc = g.vc;  p.grid.sg = g.sg;  p.grid.imax = g.imax;
    p.tab = korenTables(p.ex); p.cK = std::pow(p.kp, -p.ex) / p.kg1;
    static const std::shared_ptr<const HermiteTable> scr = [vk = p.vk, q = p.q] {
        auto t = std::make_shared<HermiteTable>();
        t->build(4.0, 640.0, 0.5, [vk, q](double v, double& y, double& dy) { const double r = std::pow(v / vk, q); y = std::exp(-r); dy = -y * q * r / v; });
        return std::shared_ptr<const HermiteTable>(t);
    }();
    p.screen = scr;
    return p;
}

inline void evalPentode(const PentodeParams& p, double vgk, double vpk, double vsk, double em, PentodeOut& o)
{
    double dvs, dvp;
    const double vs = smoothPos(vsk, 1.0, dvs);               // screen must be positive
    const double vp = smoothPos(vpk, 0.25, dvp);
    const double vsf = std::max(vs, 1e-3);
    const double sc  = std::sqrt(p.kvb + vsf * vsf);               // controlling voltage: the current vanishes as Vs -> 0
    const double u = p.kp * (1.0 / p.mu + vgk / sc);
    double L, sg;
    softplus(u, L, sg);
    double e1 = (vsf / p.kp) * L;
    if (e1 < 1e-12) e1 = 1e-12;
    const double e1pow = std::pow(e1, p.ex - 1.0);
    const double ik = e1pow * e1 / p.kg1;
    const double dik_de1 = p.ex * e1pow / p.kg1;
    const double de1_dvgk = vsf * sg / sc;
    const double de1_dvsf = L / p.kp - vsf * sg * vgk * vsf / (sc * sc * sc);
    const double dik_dvgk = dik_de1 * de1_dvgk;
    const double dik_dvs = dik_de1 * de1_dvsf * dvs;

    // screen fraction fs(vp) = fs0 + (1-fs0) exp(-(vp/vk)^q)
    double ex_, dex_;
    if (vp > 1e-9) {
        const double r = std::pow(vp / p.vk, p.q);
        ex_ = std::exp(-r);
        dex_ = -ex_ * p.q * r / vp;                    // d/dvp of exp(-(vp/vk)^q)
    } else {
        ex_ = 1.0;
        dex_ = 0.0;
    }
    const double fs = p.fs0 + (1.0 - p.fs0) * ex_;
    const double dfs = (1.0 - p.fs0) * dex_;
    // plate-voltage dependence relative to the screen: the factor is exactly 1 at Vp = Vs, so a screen strapped to the plate
    // (the JJ sheet's triode connection) gives Ip + Is = Ik; its slope dIp/dVp = Ik(1-fs)/Va is what the rp anchor sets
    const double early = 1.0 + (vpk - vsk) / p.va;               // raw terminal voltages: linear, needs no smoothing

    o.ip = em * ik * (1.0 - fs) * early;
    o.is = em * ik * fs;
    // d/d vgk
    o.dip[0] = em * dik_dvgk * (1.0 - fs) * early;
    o.dis[0] = em * dik_dvgk * fs;
    // d/d vpk
    o.dip[1] = em * ik * (-dfs * dvp * early + (1.0 - fs) / p.va);
    o.dis[1] = em * ik * dfs * dvp;
    // d/d vsk
    o.dip[2] = em * (dik_dvs * (1.0 - fs) * early - ik * (1.0 - fs) / p.va);
    o.dis[2] = em * dik_dvs * fs;

    double ig, dig;
    gridCurrent(p.grid, vgk, ig, dig);
    o.ig = em * ig;
    o.dig_dvgk = em * dig;
}

// Realtime form of evalPentode: identical law, transcendental factors from the tables (exact fallback outside them).
inline void evalPentodeFast(const PentodeParams& p, double vgk, double vpk, double vsk, double em, PentodeOut& o)
{
    const KorenTables* T = p.tab;
    double dvs, dvp;
    const double vs = smoothPos(vsk, 1.0, dvs);
    const double vp = smoothPos(vpk, 0.25, dvp);
    const double vsf = std::max(vs, 1e-3);
    const double is = 1.0 / std::sqrt(p.kvb + vsf * vsf);
    const double u = p.kp * (1.0 / p.mu + vgk * is);
    if (!T || T->ex != p.ex || !p.screen || !T->A.contains(u) || !T->B.contains(vsf) || !p.screen->contains(vp)) {
        evalPentode(p, vgk, vpk, vsk, em, o); return;
    }
    double A, dA, B, dB, ex_, dex_;
    T->A.eval(u, A, dA);
    T->B.eval(vsf, B, dB);
    p.screen->eval(vp, ex_, dex_);
    const double c = p.cK;
    const double ik = c * A * B;
    const double dik_dvgk = c * B * dA * p.kp * is;
    const double dik_dvs = c * (dB * A + B * dA * (-p.kp * vgk * vsf * is * is * is)) * dvs;
    const double fs = p.fs0 + (1.0 - p.fs0) * ex_;
    const double dfs = (1.0 - p.fs0) * dex_;
    const double early = 1.0 + (vpk - vsk) / p.va;
    o.ip = em * ik * (1.0 - fs) * early;
    o.is = em * ik * fs;
    o.dip[0] = em * dik_dvgk * (1.0 - fs) * early;
    o.dis[0] = em * dik_dvgk * fs;
    o.dip[1] = em * ik * (-dfs * dvp * early + (1.0 - fs) / p.va);
    o.dis[1] = em * ik * dfs * dvp;
    o.dip[2] = em * (dik_dvs * (1.0 - fs) * early - ik * (1.0 - fs) / p.va);
    o.dis[2] = em * dik_dvs * fs;
    double ig, dig;
    gridCurrentFast(p.grid, vgk, ig, dig);
    o.ig = em * ig;
    o.dig_dvgk = em * dig;
}

// ---------------------------------------------------------------- rectifier diode
enum class RectifierType { Tube5Y3, Silicon };

struct DiodeParams {
    RectifierType type = RectifierType::Tube5Y3;
    double k1 = fit::k5Y3_k1, k2 = fit::k5Y3_k2;   // I = k1 v + k2 v^2 (per anode, 5Y3)
    double smooth = 0.25;                          // V
    double isat = 2.0e-9, nvt = 0.0259 * 2.0;      // silicon (two 1N4007-class in series per leg ~ n=2)
};

inline void evalDiode(const DiodeParams& p, double v, double em, double& i, double& di)
{
    if (p.type == RectifierType::Tube5Y3) {
        double sp, sg;
        softplus(v / p.smooth, sp, sg);
        const double vs = p.smooth * sp;
        i  = em * (p.k1 * vs + p.k2 * vs * vs);
        di = em * (p.k1 + 2.0 * p.k2 * vs) * sg;
    } else {
        const double lim = 25.0 * p.nvt;                // linearise beyond ~1.3 V to keep Newton finite
        if (v < lim) {
            const double e = std::exp(v / p.nvt);
            i = em * p.isat * (e - 1.0);
            di = em * p.isat * e / p.nvt;
        } else {
            const double e = std::exp(25.0);
            const double d0 = p.isat * e / p.nvt;
            i = em * (p.isat * (e - 1.0) + d0 * (v - lim));
            di = em * d0;
        }
    }
}

} // namespace cd57
