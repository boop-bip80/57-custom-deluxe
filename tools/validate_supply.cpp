// validate_supply -- in-circuit check of the 5Y3 rectifier law against the JJ 5Y3S regulation curves (dossier Fig. 12, Sec. 11, 15).
//
// Dossier Sec. 11: "model its forward characteristic and the transformer's series resistance together" and Sec. 15: "Supply behavior
// must be fitted with the transformer, reservoir and load, rather than inferred from the rectifier name alone."  The left panel of
// the JJ 5Y3S sheet plots DC output voltage against load current for several transformer voltages Utr with the printed conditions
// Rs = 30 ohm, Ci = 4 uF (solid curves, capacitor input) and Lc = 5 H (dashed curves, choke input).  Only the solid family is
// simulated here: this tool has a capacitor-input rectifier.
//
// INTERPRETATION (stated, not measured; the sheet does not say):  Utr = RMS voltage of EACH half of the secondary (double-anode tube),
// Rs = series resistance in EACH anode path, mains 50 Hz (JJ is a Czech manufacturer).  The alternatives (Rs = 15 ohm, 60 Hz) are
// simulated too and their error is printed, so the reader can see how much the interpretation matters.
//
// The simulated circuit is a full-wave rectifier: two anodes (+e, -e) -> Rs -> the repo's evalDiode law -> reservoir C with a constant
// current load.  Backward Euler, 4000 steps per mains period, Newton on the reservoir voltage with the per-anode implicit solve inside.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "Devices.h"
#include "DigitizedCsv.h"

#ifndef CD57_DIGITIZED_DIR
#define CD57_DIGITIZED_DIR "data/tubes/digitized"
#endif

using namespace cd57;

namespace {

// mean DC output (V) of the full-wave capacitor-input rectifier at load current io (A)
double simulate(const DiodeParams& dp, double utr, double rs, double cap, double io, double fmains)
{
    const double T = 1.0 / fmains, steps = 4000.0, dt = T / steps;
    const double w = 2.0 * M_PI * fmains;
    double vc = 1.2 * utr;
    const int settle = 60, average = 12;
    double acc = 0.0;
    long cnt = 0;
    for (int k = 0; k < (int)((settle + average) * steps); ++k) {
        const double t = (k + 1) * dt;
        const double e[2] = {std::sqrt(2.0) * utr * std::sin(w * t), -std::sqrt(2.0) * utr * std::sin(w * t)};
        double vnew = vc;
        for (int outer = 0; outer < 30; ++outer) {
            double itot = 0.0, dsum = 0.0;
            for (int a = 0; a < 2; ++a) {
                // implicit per-anode current: i = g(e - vnew - rs*i)
                double i = 0.0, dg = 0.0;
                for (int inner = 0; inner < 40; ++inner) {
                    const double vd = e[a] - vnew - rs * i;
                    double gi;
                    evalDiode(dp, vd, 1.0, gi, dg);
                    const double h = i - gi, hp = 1.0 + rs * dg;
                    const double step = h / hp;
                    i -= step;
                    if (std::fabs(step) < 1e-13) break;
                }
                double gi;
                evalDiode(dp, e[a] - vnew - rs * i, 1.0, gi, dg);
                itot += i;
                dsum += dg / (1.0 + rs * dg);
            }
            const double F = cap * (vnew - vc) / dt - itot + io;
            const double Fp = cap / dt + dsum;
            const double dv = F / Fp;
            vnew -= dv;
            if (std::fabs(dv) < 1e-10) break;
        }
        vc = vnew;
        if (k >= (int)(settle * steps)) { acc += vc; ++cnt; }
    }
    return acc / (double)cnt;
}

struct Curve { std::string name; std::vector<std::pair<double, double>> pts; };   // (Io mA, Ua V)

std::map<std::string, Curve> loadSolid()
{
    std::map<std::string, Curve> out;
    for (const auto& kv : cd57tools::loadDigitized(std::string(CD57_DIGITIZED_DIR) + "/5Y3_JJ_S23.csv")) {
        if (kv.first.first != "regulation" || kv.first.second.find("solid") == std::string::npos) continue;
        out[kv.first.second] = Curve{kv.first.second, kv.second};
    }
    return out;
}

double interp(const Curve& c, double x)
{
    const auto& p = c.pts;
    if (x <= p.front().first) return p.front().second;
    if (x >= p.back().first) return p.back().second;
    for (size_t i = 1; i < p.size(); ++i)
        if (x <= p[i].first) { const double a = (x - p[i - 1].first) / (p[i].first - p[i - 1].first); return p[i - 1].second + a * (p[i].second - p[i - 1].second); }
    return p.back().second;
}

struct Result { double rmsV, rmsRel, maxV, meanSigned; int n; double droopErrRel; };

Result compare(const DiodeParams& dp, const std::map<std::string, Curve>& solid, double rs, double fmains, bool print)
{
    double s2 = 0, r2 = 0, mx = 0, ms = 0, droopWorst = 0; int n = 0;
    if (print) std::printf("  regulation slope = droop between the lowest and highest load current read (V):\n");
    for (const auto& kv : solid) {
        // "Utr=350V,solid" -> 350
        const double utr = std::stod(kv.first.substr(kv.first.find('=') + 1));
        const auto& pts = kv.second.pts;
        const double lo = std::max(15.0, pts.front().first), hi = std::min(120.0, pts.back().first);
        if (print) std::printf("  Utr = %3.0f V (each half), curve read over %.0f-%.0f mA\n    Io mA  :", utr, lo, hi);
        std::vector<double> row;
        for (int k = 0; k < 8; ++k) {
            const double io = lo + (hi - lo) * k / 7.0;
            const double sheet = interp(kv.second, io);
            const double model = simulate(dp, utr, rs, 4e-6, io * 1e-3, fmains);
            const double e = model - sheet;
            s2 += e * e; r2 += (e / sheet) * (e / sheet); mx = std::max(mx, std::fabs(e)); ms += e; ++n;
            if (print) { std::printf(" %7.1f", io); row.push_back(e); }
        }
        if (print) { std::printf("\n    err V  :"); for (double e : row) std::printf(" %+7.1f", e); std::printf("\n"); }
        // droop (slope) comparison: sheet vs model between the first and last load current
        const double dSheet = interp(kv.second, lo) - interp(kv.second, hi);
        const double dModel = simulate(dp, utr, rs, 4e-6, lo * 1e-3, fmains) - simulate(dp, utr, rs, 4e-6, hi * 1e-3, fmains);
        droopWorst = std::max(droopWorst, std::fabs(dModel - dSheet) / dSheet);
        if (print) std::printf("    droop %.0f -> %.0f mA: sheet %.1f V, model %.1f V (%+.0f %%)\n", lo, hi, dSheet, dModel, 100 * (dModel - dSheet) / dSheet);
    }
    return {std::sqrt(s2 / n), std::sqrt(r2 / n), mx, ms / n, n, droopWorst};
}

} // namespace

int main()
{
    const auto solid = loadSolid();
    if (solid.size() != 4) { std::fprintf(stderr, "expected 4 solid regulation curves, found %zu (is %s present?)\n", solid.size(), CD57_DIGITIZED_DIR); return 2; }

    std::printf("5Y3 rectifier law vs the JJ 5Y3S regulation curves (solid = capacitor input, Ci = 4 uF)\n");
    std::printf("law (per anode): I = k1 v + k2 v^2, k1 = %.5g A/V, k2 = %.5g A/V^2 (fit::k5Y3_k1/k2), smooth = %.2f V\n\n", fit::k5Y3_k1, fit::k5Y3_k2, DiodeParams{}.smooth);

    DiodeParams dp;
    const Result main_ = compare(dp, solid, 30.0, 50.0, true);
    std::printf("\nPRIMARY interpretation (Rs = 30 ohm per anode, 50 Hz): rms %.1f V (%.1f %%), max %.1f V, mean signed error %+.1f V over %d points\n",
                main_.rmsV, 100 * main_.rmsRel, main_.maxV, main_.meanSigned, main_.n);
    const Result a1 = compare(dp, solid, 15.0, 50.0, false), a2 = compare(dp, solid, 30.0, 60.0, false), a3 = compare(dp, solid, 15.0, 60.0, false);
    std::printf("sensitivity to the interpretation (the sheet does not state them):\n");
    std::printf("  Rs = 15 ohm, 50 Hz : rms %.1f V (%.1f %%), mean signed %+.1f V\n", a1.rmsV, 100 * a1.rmsRel, a1.meanSigned);
    std::printf("  Rs = 30 ohm, 60 Hz : rms %.1f V (%.1f %%), mean signed %+.1f V\n", a2.rmsV, 100 * a2.rmsRel, a2.meanSigned);
    std::printf("  Rs = 15 ohm, 60 Hz : rms %.1f V (%.1f %%), mean signed %+.1f V\n", a3.rmsV, 100 * a3.rmsRel, a3.meanSigned);
    std::printf("\nNOTE: digitisation uncertainty of the sheet is about +-3 V; the dashed (choke-input) family is not simulated.\n");

    std::printf("\nDECOMPOSITION: the error is a near-constant offset (mean %+.1f V), not a wrong regulation shape (worst droop error %.0f %%).\n"
                "The offset depends on the unstated Utr / Rs / frequency interpretation (sensitivity above) and on the forward law's extrapolation\n"
                "beyond the sheet's 270 mA forward range; in the amplifier it is absorbed by the assumed, calibrated PT secondary voltage.\n",
                main_.meanSigned, 100 * main_.droopErrRel);

    // acceptance (stated project limits, not Fender specifications; chosen to catch a broken rectifier law; measured values printed above):
    //   offset+shape rms error below 6 % of the output voltage, and the regulation slope within 20 % of the sheet's on every curve.
    const double limitRel = 0.06, limitDroop = 0.20;
    const bool ok1 = main_.rmsRel < limitRel, ok2 = main_.droopErrRel < limitDroop;
    std::printf("\n%s  regulation error %.1f %% rms (limit %.0f %%)\n", ok1 ? "PASS" : "FAIL", 100 * main_.rmsRel, 100 * limitRel);
    std::printf("%s  regulation slope within %.0f %% of the sheet on every curve (worst %.0f %%)\n", ok2 ? "PASS" : "FAIL", 100 * limitDroop, 100 * main_.droopErrRel);
    return (ok1 && ok2) ? 0 : 1;
}
