// Devices.h -- nonlinear constitutive laws with analytic Jacobians.
//
// Dossier Sec. 10/16: every valve exposes plate, screen AND control-grid current, with current
// conserved at the cathode (I_k = I_p + I_s + I_g).  Exponentials are evaluated with overflow-safe
// softplus; all derivatives are analytic and checked against finite differences in the tests.
//
// Parameter provenance (see data/tubes/fitted_models.json, scripts/fit_tubes.py):
//   triodes  : Koren-form, fitted to digitised JJ transfer sheets + RCA anchor point (CANDIDATE)
//   6V6      : Koren-form space-charge law + screen partition fitted to RCA 6V6GT operating point
//   grid I   : ASSUMED smooth law (dossier Sec. 16: "onset and slope need measurements")
//   rectifier: digitised 5Y3S forward curve (CANDIDATE), or silicon diode for the optional mod
#pragma once
#include <cmath>
#include <algorithm>
#include <string>
#include "TubeFits.h"

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
    double k  = 0.35e-3;   // A / V^1.5
    double vc = -0.5;      // V   onset offset
    double sg = 0.15;      // V   smoothing width
};

// I_g = k * (sg * softplus((vgk - vc)/sg))^1.5 ; returns I_g and dI_g/dvgk
inline void gridCurrent(const GridLaw& g, double vgk, double& ig, double& dig)
{
    const double u = (vgk - g.vc) / g.sg;
    if (u < -14.0) { ig = 0.0; dig = 0.0; return; }
    double sp, sig;
    softplus(u, sp, sig);
    const double x = g.sg * sp;
    const double r = std::sqrt(x);
    ig  = g.k * x * r;
    dig = g.k * 1.5 * r * sig;
}

// ---------------------------------------------------------------- triode (Koren form)
struct TriodeParams {
    double mu, ex, kg1, kp, kvb;
    GridLaw grid;
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

// ---------------------------------------------------------------- beam-power pentode (6V6)
struct PentodeParams {
    double mu, ex, kg1, kp, fs0, vk, q, va;
    GridLaw grid;
};

struct PentodeOut {
    double ip, is, ig;
    double dip[3];   // d/d(vgk, vpk, vsk)
    double dis[3];
    double dig_dvgk;
};

inline PentodeParams pentodeParams()
{
    PentodeParams p{fit::k6V6.mu, fit::k6V6.ex, fit::k6V6.kg1, fit::k6V6.kp, fit::k6V6.fs0, fit::k6V6.vk,
                    fit::k6V6.q, fit::k6V6.va, {}};
    p.grid.k = 0.9e-3;   // power-valve grid conduction (ASSUMED)
    p.grid.vc = -0.4;
    p.grid.sg = 0.2;
    return p;
}

inline void evalPentode(const PentodeParams& p, double vgk, double vpk, double vsk, double em, PentodeOut& o)
{
    double dvs, dvp;
    const double vs = smoothPos(vsk, 1.0, dvs);               // screen must be positive
    const double vp = smoothPos(vpk, 0.25, dvp);
    const double vsf = std::max(vs, 1e-3);
    const double u = p.kp * (1.0 / p.mu + vgk / vsf);
    double L, sg;
    softplus(u, L, sg);
    double e1 = (vsf / p.kp) * L;
    if (e1 < 1e-12) e1 = 1e-12;
    const double e1pow = std::pow(e1, p.ex - 1.0);
    const double ik = e1pow * e1 / p.kg1;
    const double dik_de1 = p.ex * e1pow / p.kg1;
    const double de1_dvgk = sg;
    const double de1_dvsf = L / p.kp - sg * vgk / vsf;
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
    const double early = 1.0 + vp / p.va;

    o.ip = em * ik * (1.0 - fs) * early;
    o.is = em * ik * fs;
    // d/d vgk
    o.dip[0] = em * dik_dvgk * (1.0 - fs) * early;
    o.dis[0] = em * dik_dvgk * fs;
    // d/d vpk
    o.dip[1] = em * ik * (-dfs * early + (1.0 - fs) / p.va) * dvp;
    o.dis[1] = em * ik * dfs * dvp;
    // d/d vsk
    o.dip[2] = em * dik_dvs * (1.0 - fs) * early;
    o.dis[2] = em * dik_dvs * fs;

    double ig, dig;
    gridCurrent(p.grid, vgk, ig, dig);
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
