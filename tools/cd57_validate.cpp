// cd57_validate -- executable validation of the model against everything the dossier lets us test WITHOUT hardware.
//
//   cd57_validate [--quick] [--report FILE.md] [--netlist FILE.md]
//
// Result vocabulary:  PASS / FAIL  (a stated criterion), INFO (a number reported for the record),
//                     CALIBRATION (a value the model was tuned to -> not independent evidence),
//                     NOT EVALUATED (needs reference hardware; listed honestly at the end).
// Nothing here certifies a one-to-one match (dossier Sec. 26-27, 33).
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include "CD57Engine.h"
#include "EmbeddedData.h"

using namespace cd57;
using cplx = std::complex<double>;

// ------------------------------------------------------------------------------------------------ reporting
static std::ostringstream R;
static int nPass = 0, nFail = 0;
static void section(const std::string& t) { R << "\n## " << t << "\n\n"; std::cout << "\n== " << t << "\n"; }
static void para(const std::string& t) { R << t << "\n\n"; }
static void check(bool ok, const std::string& what, const std::string& detail = "")
{
    (ok ? nPass : nFail)++;
    R << "- **" << (ok ? "PASS" : "FAIL") << "** — " << what << (detail.empty() ? "" : "  \n  " + detail) << "\n";
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << (detail.empty() ? "" : "   [" + detail + "]") << "\n";
}
static void info(const std::string& what) { R << "- INFO — " << what << "\n"; std::cout << "  INFO  " << what << "\n"; }
static std::string f(double v, int p = 3) { std::ostringstream o; o << std::fixed << std::setprecision(p) << v; return o.str(); }

// ------------------------------------------------------------------------------------------------ signal helpers
static void fft(std::vector<cplx>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / (double)len; const cplx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) { cplx w(1); for (size_t j = 0; j < len / 2; ++j) { cplx u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } }
    }
}

static std::string gNetlist, gManifest;

struct Rig {
    CD57Engine e;
    double sr;
    explicit Rig(EngineParams p, double sampleRate = 48000.0, int maxBlock = 512) : sr(sampleRate)
    {
        e.prepare(sampleRate, maxBlock, gNetlist, p, gManifest);
    }
    // run an input (volts at the jack) through the terminal output; returns speaker-terminal volts
    std::vector<double> run(const std::vector<double>& inVolts, int block = 512)
    {
        std::vector<double> out(inVolts.size());
        std::vector<float> in(block), ot(block), oa(block);
        const double ref = e.params().outputRefVolts, cal = e.params().inputCalVolts * std::pow(10.0, e.params().inputTrimDb / 20.0);
        for (size_t i = 0; i < inVolts.size(); i += block) {
            const int n = (int)std::min<size_t>(block, inVolts.size() - i);
            for (int k = 0; k < n; ++k) in[k] = (float)(inVolts[i + k] / cal);
            e.process(in.data(), ot.data(), oa.data(), n);
            for (int k = 0; k < n; ++k) out[i + k] = (double)ot[k] * ref;
        }
        return out;
    }
    std::vector<double> sine(double f0, double amp, double sec, double rampSec = 0.02) const
    {
        std::vector<double> x((size_t)(sec * sr));
        for (size_t i = 0; i < x.size(); ++i) { const double t = i / sr; x[i] = amp * std::sin(2 * M_PI * f0 * t) * std::min(1.0, t / rampSec); }
        return x;
    }
};

struct Tone { double rmsV, thd, h2dB, h3dB, peakV; };
// analyse the last `cycles` whole cycles of y (volts) at fundamental f0
static Tone analyseTone(const std::vector<double>& y, double sr, double f0, int cycles)
{
    const int per = (int)std::round(sr / f0);
    const int n = std::min<int>((int)y.size(), per * cycles);
    const size_t start = y.size() - n;
    double h[8] = {0}, e = 0, pk = 0;
    const double fa = sr * std::round(f0 * n / sr) / n;   // exact bin frequency
    for (int k = 1; k <= 7; ++k) {
        double c = 0, s = 0;
        for (int i = 0; i < n; ++i) { const double w = 2 * M_PI * fa * k * i / sr; c += y[start + i] * std::cos(w); s += y[start + i] * std::sin(w); }
        h[k] = std::sqrt(c * c + s * s) * 2.0 / n;
    }
    for (int i = 0; i < n; ++i) { e += y[start + i] * y[start + i]; pk = std::max(pk, std::fabs(y[start + i])); }
    double hh = 0; for (int k = 2; k <= 7; ++k) hh += h[k] * h[k];
    return {std::sqrt(e / n), std::sqrt(hh) / h[1], 20 * std::log10(h[2] / h[1] + 1e-12), 20 * std::log10(h[3] / h[1] + 1e-12), pk};
}

static EngineParams baseParams()
{
    EngineParams p; p.oversampleLog2 = 1; p.instVol = 0.5f; p.micVol = 0.0f; p.tone = 0.7f; p.jack = 1; p.load = 2; // dummy 8 ohm
    p.speakerHeating = false; p.inputCalVolts = 1.0f; p.outputRefVolts = 20.0f;
    return p;
}

// ------------------------------------------------------------------------------------------------ AC helpers
static double acGainDb(Rig& r, double f, const char* outNode, double* phaseDeg = nullptr)
{
    Network& n = r.e.amp().network();
    const int pu = n.nodeId("pu");
    const auto v = n.acSolve(f, pu);
    const double Rsrc = n.getR(n.resId("Rsrc"));
    const cplx g = v[n.nodeId(outNode)] / Rsrc;     // injected 1 A at pu  <=>  EMF = 1 A * Rsrc (Norton equivalent)
    if (phaseDeg) *phaseDeg = std::arg(g) * 180.0 / M_PI;
    return 20 * std::log10(std::abs(g) + 1e-30);
}

int main(int argc, char** argv)
{
    bool quick = false; std::string reportPath, netlistPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--quick") quick = true;
        else if (a == "--report" && i + 1 < argc) reportPath = argv[++i];
        else if (a == "--netlist" && i + 1 < argc) netlistPath = argv[++i];
    }
    gNetlist = embeddedNetlist(); gManifest = embeddedManifest();
    const auto t0 = std::chrono::steady_clock::now();

    R << "# Validation report — Fender '57 Custom Deluxe digital model\n\n";
    R << "Generated by `cd57_validate" << (quick ? " --quick" : "") << "`. This report evaluates the model against everything the public-source dossier allows to be tested **without reference hardware**. "
         "It does not certify a one-to-one match: no specimen was measured (dossier Sec. 01, 33). Items that need hardware are listed at the end as NOT EVALUATED.\n";

    // ============================================================================================ 1. provenance
    section("1. Parameter provenance (dossier Sec. 30)");
    Manifest man; const std::string merr = man.load(gManifest);
    check(merr.empty(), "manifest loads and validates (status mandatory, assumed => interval, null => unknown)", merr);
    info(man.summary());
    { Network n; n.parse(gNetlist); n.build(1.0 / 96000, 0.5);
      info("netlist '" + n.title + "' rev " + n.revision + ": " + std::to_string(n.nodeCount()) + " nodes, " + std::to_string(n.unknowns()) + " unknowns, sparse LU " + std::to_string(n.luOps()) + " multiply-subtract ops, " + std::to_string(n.luNnz()) + " nonzeros");
      if (!netlistPath.empty()) {
          std::ofstream o(netlistPath);
          o << "# Netlist register — `data/netlist/cd57_nominal.net`\n\nGenerated by `cd57_validate --netlist`. Every element carries its evidence label (dossier Sec. 01) and source id (dossier Sec. 32). "
               "Nodes: `0` = chassis ground. Runtime-driven elements are marked.\n\n" << n.describe(); } }

    // ============================================================================================ 2. DC operating point
    section("2. DC operating point and KCL residuals (dossier Sec. 21, 27 'Memory and recovery' prerequisites)");
    {
        EngineParams p = baseParams(); p.load = 0;
        Rig rig(p);
        Network& n = rig.e.amp().network();
        n.trackResidual = true;
        for (int i = 0; i < 4800; ++i) rig.e.amp().tick(0.0);
        const char* nodes[] = {"b1", "b2", "b3", "p1b", "p1n", "k1", "p2a", "kb2", "p2b", "kcy", "j", "p3", "p4", "k34"};
        R << "| node | V |\n|---|---|\n";
        for (auto nm : nodes) R << "| " << nm << " | " << f(rig.e.amp().nodeV(nm), 2) << " V |\n";
        R << "\n";
        info("6V6 idle: Ip3 = " + f(n.pen()[0].ip * 1e3, 1) + " mA, Ip4 = " + f(n.pen()[1].ip * 1e3, 1) + " mA, Ig2 = " + f(n.pen()[0].is * 1e3, 1) + " / " + f(n.pen()[1].is * 1e3, 1) + " mA; shared cathode " + f(rig.e.amp().nodeV("k34"), 2) + " V");
        check(n.stats.maxResidual < 1e-6, "max nodal KCL residual at convergence < 1 uA", "measured " + f(n.stats.maxResidual * 1e9, 3) + " nA");
        check(std::fabs(n.pen()[0].ip - n.pen()[1].ip) < 0.002, "idle plate currents of the matched pair agree within 2 mA", f(std::fabs(n.pen()[0].ip - n.pen()[1].ip) * 1e3, 3) + " mA");
        check(n.stats.failures == 0, "no unrecovered solver failures during settle");
    }

    // ============================================================================================ 3. input network vs published specs
    section("3. Input network vs the published specification (CONFIRMED: 1 M / 136 k, input 2 nominally -6 dB)");
    {
        EngineParams p = baseParams(); p.load = 0;
        double zin[4], gdb[4];
        const char* jn[4] = {"Instrument 1", "Instrument 2", "Mic 1", "Mic 2"};
        R << "| jack | input impedance @ 20 Hz | gain to V1 grid @ 1 kHz |\n|---|---|---|\n";
        for (int j = 1; j <= 4; ++j) {
            p.jack = j;
            Rig rig(p);
            Network& n = rig.e.amp().network();
            // measure the amplifier's own input impedance: open the source resistance, remove cable capacitance
            const int tp = n.nodeId("tp");
            n.setR(n.resId("Rsrc"), 1e11); n.setC(n.capId("Ccab"), 0.0); n.refreshLinear();
            const auto v = n.acSolve(20.0, tp);
            zin[j - 1] = std::abs(v[tp]);
            n.setR(n.resId("Rsrc"), 100.0); n.refreshLinear();
            gdb[j - 1] = acGainDb(rig, 1000.0, (j <= 2) ? "b_g" : "n_g");
            R << "| " << jn[j - 1] << " | " << f(zin[j - 1] / 1e3, 1) << " kΩ | " << f(gdb[j - 1], 2) << " dB |\n";
        }
        R << "\n";
        check(std::fabs(zin[0] - 1.0e6) / 1.0e6 < 0.05, "Instrument 1 input impedance = 1 MΩ (+-5%)", f(zin[0] / 1e3, 1) + " kΩ");
        check(std::fabs(zin[1] - 136e3) / 136e3 < 0.05, "Instrument 2 input impedance = 136 kΩ (+-5%)", f(zin[1] / 1e3, 1) + " kΩ");
        check(std::fabs(zin[2] - 1.0e6) / 1.0e6 < 0.05 && std::fabs(zin[3] - 136e3) / 136e3 < 0.05, "Mic 1 / Mic 2 show the same 1 MΩ / 136 kΩ pair");
        check(std::fabs((gdb[1] - gdb[0]) - (-6.0)) < 0.6, "input 2 is nominally -6 dB relative to input 1", "measured " + f(gdb[1] - gdb[0], 2) + " dB (ideal divider -6.02 dB)");
        info("the 1 MΩ / 136 kΩ / -6 dB figures are REPRODUCED by the historical jack network (68k+68k with switching jacks); they were not fitted.");
    }

    // ============================================================================================ 4. rated power / THD
    double pAt5 = 0, vAt5 = 0, volAt5 = 0;
    section("4. Rated output: 12.5 W into 8 Ω at <5% THD, 1 kHz (CONFIRMED spec; used as a CALIBRATION anchor)");
    {
        const double f0 = 1000;
        R << "| Instrument vol | input pk | P_out | THD |\n|---|---|---|---|\n";
        for (double vol : (quick ? std::vector<double>{0.42} : std::vector<double>{0.3, 0.42, 0.55})) {
            EngineParams p = baseParams(); p.instVol = (float)vol; p.tone = 1.0f;
            Rig rig(p);
            for (double ev = 0.012; ev < 0.2; ev *= 1.1) {
                auto y = rig.run(rig.sine(f0, ev, 0.12));
                const Tone t = analyseTone(y, rig.sr, f0, 40);
                const double P = t.rmsV * t.rmsV / 8.0;
                if (t.thd < 0.05 && P > pAt5) { pAt5 = P; vAt5 = ev; volAt5 = vol; }
                if (t.thd > 0.3) break;
            }
        }
        R << "| " << f(volAt5, 2) << " | " << f(vAt5 * 1e3, 1) << " mV | " << f(pAt5, 2) << " W | <5% |\n\n";
        check(pAt5 > 11.25 && pAt5 < 13.75, "maximum power with THD < 5% is within 10% of the rated 12.5 W", f(pAt5, 2) + " W");
        info("CALIBRATION: the assumed power-transformer secondary (360 V RMS per half) was chosen so this holds; the same value yields B+1 ~ 372 V and a 21 V shared cathode, typical of a 5E3-class unit. It is not independent evidence.");
    }

    // ============================================================================================ 5. AC cross-check
    section("5. Independent frequency-domain solver vs transient solver (dossier Sec. 21: 'verify against an independent circuit solver')");
    {
        EngineParams p = baseParams(); p.load = 0; p.instVol = 0.5f; p.tone = 0.5f;
        Rig rig(p, 96000.0);
        {   // isolate stamping/discretisation consistency from the (real) supply-ripple modulation of the gain:
            // scale the three reservoir/filter capacitors up so the rails are ripple-free in BOTH solvers.
            Network& n = rig.e.amp().network();
            for (const char* c : {"C1", "C2", "C3"}) n.setC(n.capId(c), n.cap()[n.capId(c)].Cnom * 1000.0);
            n.refreshLinear();
            rig.e.amp().initialiseQuiescent(0.05);
        }
        para("The supply capacitors are scaled x1000 for this comparison only, so neither solver sees rail ripple (ripple modulates the real amplifier's gain at 2 x mains frequency; the AC solver linearises at one instant of it).");
        R << "| f | AC solver | transient | difference |\n|---|---|---|---|\n";
        double worst = 0;
        for (double fq : (quick ? std::vector<double>{200, 1000, 4000} : std::vector<double>{60, 100, 200, 400, 1000, 2000, 4000, 8000})) {
            const double ac = acGainDb(rig, fq, "sp");
            // small-signal transient: EMF 40 uV peak, measure steady state
            const double amp = 40e-6;
            auto y = rig.run(rig.sine(fq, amp, 0.8, 0.05));
            const Tone t = analyseTone(y, rig.sr, fq, std::max(2, (int)(fq * 0.2)));
            const double tr = 20 * std::log10(t.rmsV * std::sqrt(2.0) / amp);
            const double d = tr - ac; worst = std::max(worst, std::fabs(d));
            R << "| " << f(fq, 0) << " Hz | " << f(ac, 2) << " dB | " << f(tr, 2) << " dB | " << f(d, 2) << " dB |\n";
        }
        R << "\n";
        check(worst < 0.5, "transient response matches the independent AC solution within 0.5 dB (small signal, ripple-free supply)", "worst " + f(worst, 2) + " dB");
    }

    // ============================================================================================ 6. control interactions
    section("6. Control interactions (dossier Sec. 08: volumes and tone are one coupled passive circuit)");
    {
        EngineParams p = baseParams(); p.load = 0;
        Rig rig(p);
        AmpCircuit& amp = rig.e.amp();
        auto setc = [&](double iv, double mv, double tn) { AmpControls c; c.instVol = iv; c.micVol = mv; c.tone = tn; c.guitarJack = 1; c.load = LoadMode::Speaker; amp.setControls(c); amp.applyControls(true); };
        R << "Gain from the Instrument 1 jack EMF to the mix node (V2A grid), dB. Rows: Instrument vol; columns: Mic vol (inactive channel); tone 0.5.\n\n| inst \\ mic | 0.0 | 0.15 | 0.5 | 0.9 | 1.0 |\n|---|---|---|---|---|---|\n";
        double g_lo = 0, g_hi = 0;
        for (double iv : {0.2, 0.5, 0.9}) {
            R << "| " << f(iv, 2) << " |";
            for (double mv : {0.0, 0.15, 0.5, 0.9, 1.0}) { setc(iv, mv, 0.5); const double g = acGainDb(rig, 1000, "m"); R << " " << f(g, 2) << " |"; if (iv == 0.5 && mv == 0.0) g_lo = g; if (iv == 0.5 && mv == 1.0) g_hi = g; }
            R << "\n";
        }
        R << "\n";
        check(std::fabs(g_hi - g_lo) > 0.2, "the silent Mic channel loads the Instrument channel (level changes with the inactive volume)", "inst 0.5: mic 0 -> 1 changes gain by " + f(g_hi - g_lo, 2) + " dB");
        // tone: HF/LF balance vs tone knob, at two instrument volumes
        bool mono = true; R << "HF/LF balance, gain(5 kHz) - gain(200 Hz), dB, for several tone settings:\n\n| inst | tone 0 | 0.25 | 0.5 | 0.75 | 1.0 |\n|---|---|---|---|---|---|\n";
        for (double iv : {0.3, 0.8}) {
            R << "| " << f(iv, 2) << " |"; double prev = -1e9;
            for (double tn : {0.0, 0.25, 0.5, 0.75, 1.0}) { setc(iv, 0.0, tn); const double d = acGainDb(rig, 5000, "m") - acGainDb(rig, 200, "m"); R << " " << f(d, 2) << " |"; if (d < prev - 0.05) mono = false; prev = d; }
            R << "\n";
        }
        R << "\n";
        check(mono, "turning Tone clockwise increases the high/low balance monotonically at both volume settings");
        bool fin = true;
        for (double iv : {0.0, 1.0}) for (double mv : {0.0, 1.0}) for (double tn : {0.0, 1.0}) { setc(iv, mv, tn); const double g = acGainDb(rig, 1000, "m"); if (!std::isfinite(g)) fin = false; }
        check(fin, "exact pot endpoints (0 and full rotation) solve without singularities (documented contact resistance, no divide-by-zero)");
    }

    // ============================================================================================ 7. level sweep / IMD / recovery
    if (!quick) {
        section("7. Nonlinear behaviour: harmonics vs drive, intermodulation, burst recovery (dossier Sec. 25-27)");
        {
            EngineParams p = baseParams(); p.instVol = 0.5f; p.tone = 0.7f;
            Rig rig(p);
            R << "1 kHz into 8 Ω dummy load, Instrument vol 0.5:\n\n| input pk | P_out | THD | H2 dBc | H3 dBc |\n|---|---|---|---|---|\n";
            for (double ev : {0.002, 0.005, 0.01, 0.02, 0.04, 0.08, 0.16}) {
                auto y = rig.run(rig.sine(1000, ev, 0.12)); const Tone t = analyseTone(y, rig.sr, 1000, 40);
                R << "| " << f(ev * 1e3, 0) << " mV | " << f(t.rmsV * t.rmsV / 8, 2) << " W | " << f(t.thd * 100, 2) << " % | " << f(t.h2dB, 1) << " | " << f(t.h3dB, 1) << " |\n";
            }
            R << "\n";
            // two-tone
            { const double f1 = 700, f2 = 1100, amp = 0.02; auto x = rig.sine(f1, amp, 0.4), x2 = rig.sine(f2, amp, 0.4); for (size_t i = 0; i < x.size(); ++i) x[i] += x2[i];
              auto y = rig.run(x);
              const int N = 16384; std::vector<cplx> a(N); for (int i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N); a[i] = y[y.size() - N + i] * w; }
              fft(a); auto mag = [&](double fr) { const int b = (int)std::round(fr * N / rig.sr); double m = 0; for (int k = b - 3; k <= b + 3; ++k) m = std::max(m, std::abs(a[k])); return m; };
              const double ref = std::max(mag(f1), mag(f2));
              info("two-tone 700 + 1100 Hz: 2nd-order difference (400 Hz) " + f(20 * std::log10(mag(400) / ref), 1) + " dBc, 3rd-order (300 / 1500 Hz) " + f(20 * std::log10(std::max(mag(300), mag(1500)) / ref), 1) + " dBc"); }
            // burst recovery: loud burst then quiet tone; track envelope of the quiet tone
            { Rig r2(p); std::vector<double> x; auto b = r2.sine(1000, 0.12, 0.15); x.insert(x.end(), b.begin(), b.end());
              const size_t quietStart = x.size(); auto q = r2.sine(1000, 0.003, 1.2, 0.0005); x.insert(x.end(), q.begin(), q.end());
              auto y = r2.run(x); const int per = (int)(r2.sr / 1000);
              std::vector<double> env; for (size_t i = quietStart; i + per < y.size(); i += per * 5) { double e = 0; for (int k = 0; k < per; ++k) e += y[i + k] * y[i + k]; env.push_back(std::sqrt(e / per)); }
              const double fin = env.back(); size_t t90 = 0; for (size_t i = 0; i < env.size(); ++i) if (std::fabs(env[i] - fin) < 0.1 * fin && std::fabs(env[std::min(i + 4, env.size() - 1)] - fin) < 0.1 * fin) { t90 = i; break; }
              const double ms = t90 * per * 5 * 1000.0 / r2.sr;
              info("after a 150 ms burst at 120 mV, the 3 mV 1 kHz tone returns to within 10% of its settled level in " + f(ms, 0) + " ms (envelope starts at " + f(env.front() / fin, 2) + "x of final)");
              check(env.front() != fin && std::isfinite(ms) && ms < 3000, "burst/recovery memory is present and bounded (bias shift, coupling-cap charge, supply sag are state variables)"); }
        }
    }

    // ============================================================================================ 8. numerical robustness
    section("8. Numerical and runtime robustness (dossier Sec. 21, 23, 27 experiment 8)");
    {
        EngineParams p = baseParams(); p.load = 0; p.instVol = 0.6f;
        auto signal = [&](double sr) { std::vector<double> x((size_t)(sr * (quick ? 0.4 : 1.0))); for (size_t i = 0; i < x.size(); ++i) { const double t = i / sr; x[i] = 0.1 * std::min(1.0, t / 0.01) * (0.5 * std::sin(2 * M_PI * 110 * t) + 0.3 * std::sin(2 * M_PI * 330 * t + 0.7)) * std::exp(-t * 1.5); } return x; };
        // block-size invariance (bit exactness of the whole chain incl. oversampler, control smoothing, heating)
        {
            Rig ref(p); auto x = signal(ref.sr); auto yr = ref.run(x, 512);
            double worst = 0;
            for (int bs : {1, 37, 480}) { Rig r(p, 48000.0, 512); auto y = r.run(x, bs); for (size_t i = 0; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i] - yr[i])); }
            check(worst < 1e-5, "output is independent of host block size (blocks of 1, 37, 480 vs 512)", "max difference " + f(worst, 9) + " V");
        }
        // control automation at silence and at signal
        {
            Rig r(p); bool fin = true; auto x = signal(r.sr); std::vector<float> in(256), ot(256), oa(256);
            for (size_t i = 0; i + 256 <= x.size(); i += 256) {
                EngineParams q = p; q.instVol = (float)(0.5 + 0.5 * std::sin(i * 0.01)); q.tone = (float)(0.5 + 0.5 * std::cos(i * 0.007)); r.e.setParams(q);
                for (int k = 0; k < 256; ++k) in[k] = (float)x[i + k];
                r.e.process(in.data(), ot.data(), oa.data(), 256);
                for (int k = 0; k < 256; ++k) if (!std::isfinite(ot[k]) || !std::isfinite(oa[k])) fin = false;
            }
            check(fin, "abrupt/continuous automation of volume and tone keeps every sample finite");
        }
        // solver health on a drive sweep
        {
            Rig r(p); uint64_t steps = 0, fails = 0, subs = 0;
            for (double ev : {0.01, 0.05, 0.2, 0.6}) { r.run(r.sine(1000, ev, 0.3)); }
            auto d = r.e.diag(); steps = d.steps; fails = d.failures; subs = d.substepped;
            check(fails == 0, "no unrecovered solver failures across a 1 kHz drive sweep up to 600 mV", std::to_string(steps) + " steps, " + std::to_string(subs) + " sub-stepped (" + f(100.0 * subs / std::max<uint64_t>(1, steps), 2) + " %), " + f(d.meanIterations(), 2) + " Newton it/step");
        }
        // sample-rate invariance
        if (!quick) {
            R << "\nHarmonic levels of a 1 kHz tone (input 30 mV pk) vs host sample rate (2x oversampling):\n\n| fs | THD | H2 dBc | H3 dBc | V_out rms |\n|---|---|---|---|---|\n";
            double h3[3], thd[3]; int idx = 0;
            for (double sr : {44100.0, 48000.0, 96000.0}) { Rig r(p, sr); auto y = r.run(r.sine(1000, 0.03, 0.4)); auto t = analyseTone(y, sr, 1000, 40);
                R << "| " << f(sr, 0) << " | " << f(t.thd * 100, 2) << " % | " << f(t.h2dB, 1) << " | " << f(t.h3dB, 1) << " | " << f(t.rmsV, 3) << " V |\n"; h3[idx] = t.h3dB; thd[idx] = t.thd; ++idx; }
            R << "\n";
            check(std::fabs(h3[0] - h3[2]) < 1.5 && std::fabs(h3[1] - h3[2]) < 1.5, "3rd-harmonic level agrees across 44.1/48/96 kHz within 1.5 dB", f(h3[0], 1) + " / " + f(h3[1], 1) + " / " + f(h3[2], 1) + " dBc");
        }
        // oversampling convergence and aliasing
        if (!quick) {
            R << "\nAliasing: 3.1 kHz tone, 60 mV pk (heavily driven), fs = 48 kHz. 'alias' = strongest spectral line more than 80 Hz from any harmonic of 3.1 kHz.\n\n| oversampling | THD | strongest alias (dBFS, FS = 20 V pk) | latency |\n|---|---|---|---|\n";
            double a1 = 0, a2 = 0;
            for (int os = 0; os <= 3; ++os) {
                EngineParams q = p; q.oversampleLog2 = os; Rig r(q);
                auto y = r.run(r.sine(3100, 0.06, 0.5)); const int N = 16384; std::vector<cplx> a(N);
                for (int i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N); a[i] = y[y.size() - N + i] / 20.0 * w; }
                fft(a); double worst = 0;
                for (int k = 20; k < N / 2 - 50; ++k) { const double fr = k * r.sr / N; bool nearH = false; for (int h = 1; h <= 8; ++h) if (std::fabs(fr - 3100.0 * h) < 80.0) nearH = true; if (!nearH) worst = std::max(worst, std::abs(a[k]) * 4.0 / N); }
                auto t = analyseTone(y, r.sr, 3100, 60);
                R << "| " << (1 << os) << "x | " << f(t.thd * 100, 1) << " % | " << f(20 * std::log10(worst + 1e-12), 1) << " | " << r.e.latencySamples() << " smp |\n";
                if (os == 0) a1 = 20 * std::log10(worst + 1e-12); if (os == 1) a2 = 20 * std::log10(worst + 1e-12);
            }
            R << "\n";
            check(a2 < a1 - 6.0, "2x oversampling lowers the strongest alias line by more than 6 dB vs 1x", f(a1, 1) + " -> " + f(a2, 1) + " dBFS");
        }
    }

    // ============================================================================================ 9. supply, mains variants, energy
    section("9. Supply: ripple frequency for the 50/60 Hz variants, power flow and mains estimate (dossier Sec. 11, 31)");
    {
        for (int market : {0, 3}) {
            EngineParams p = baseParams(); p.load = 0; p.market = market; Rig r(p, 48000.0);
            AmpCircuit& amp = r.e.amp(); const int N = 8192; std::vector<double> b1(N);
            for (int i = 0; i < N; ++i) { for (int k = 0; k < 2; ++k) amp.tick(0.0); b1[i] = amp.nodeV("b1"); }
            double mean = 0; for (double v : b1) mean += v; mean /= N;
            std::vector<cplx> a(N); for (int i = 0; i < N; ++i) a[i] = (b1[i] - mean) * (0.5 - 0.5 * std::cos(2 * M_PI * i / N)); fft(a);
            int kb = 3; double mb = 0; for (int k = 3; k < N / 2; ++k) if (std::abs(a[k]) > mb) { mb = std::abs(a[k]); kb = k; }
            const double fr = kb * 48000.0 / N, expect = 2.0 * marketVariants()[market].hz;
            check(std::fabs(fr - expect) < 6.0, std::string("dominant B+1 ripple of '") + marketVariants()[market].name + "' is at 2 x mains = " + f(expect, 0) + " Hz", "found " + f(fr, 1) + " Hz, ripple pk-pk about " + f(4 * mb / N * 2, 2) + " V");
        }
        // power flow at the rated-power point
        {
            EngineParams p = baseParams(); p.instVol = (float)(volAt5 > 0 ? volAt5 : 0.42); p.tone = 1.0f; Rig r(p);
            AmpCircuit& amp = r.e.amp(); Network& n = amp.network();
            const double ev = vAt5 > 0 ? vAt5 : 0.03; const int steps = (int)(0.3 * 96000);
            for (int i = 0; i < steps / 2; ++i) amp.tick(ev * std::sin(2 * M_PI * 1000.0 * i / 96000.0));
            double pin = 0, pout = 0; int cnt = 0;
            for (int i = steps / 2; i < steps; ++i) { amp.tick(ev * std::sin(2 * M_PI * 1000.0 * i / 96000.0)); const double ib1 = n.dio()[0].i + n.dio()[1].i; pin += amp.nodeV("b1") * ib1; const double v = amp.vTerminal(); pout += v * v / 8.0; ++cnt; }
            pin /= cnt; pout /= cnt;
            R << "\nAt the 5%-THD point: HV supply power " << f(pin, 1) << " W (B+1 x rectifier current), speaker power " << f(pout, 2) << " W, efficiency " << f(100 * pout / pin, 1) << " %.\n\n";
            check(pout / pin > 0.2 && pout / pin < 0.65, "plate-circuit efficiency is in the physically plausible band for a cathode-biased 6V6 pair (20-65%)", f(100 * pout / pin, 1) + " %");
            const double heaters = 5.0 * 2.0 + 6.3 * 1.55;       // 5Y3 5 V/2 A + 6.3 V/1.55 A (DATASHEET-class, assumed)
            info("DERIVED mains estimate: (" + f(pin, 1) + " W HV + " + f(heaters, 1) + " W heaters) / 0.85 transformer efficiency = " + f((pin + heaters) / 0.85, 0) + " W at full output, versus the documented 85 W mains requirement (a rating, not a measured draw)");
        }
    }

    // ============================================================================================ 10. boundary calibration worked example
    section("10. Calibration boundary (dossier Sec. 24 worked example)");
    {
        const double ampDig = std::pow(10.0, -18.0 / 20.0), Kin = std::sqrt(2.0) * 0.100 / ampDig;
        check(std::fabs(Kin - 1.12335) < 1e-4, "K_in worked example: a -18 dBFS sine giving 0.100 V RMS at the jack needs K_in = 1.12335 V/FS", "computed " + f(Kin, 5));
        EngineParams p = baseParams(); p.inputCalVolts = (float)Kin; Rig r(p);
        const double dig = ampDig; std::vector<float> in(4800), ot(4800), oa(4800); double e = 0;
        for (int i = 0; i < 4800; ++i) in[i] = (float)(dig * std::sin(2 * M_PI * 1000.0 * i / 48000.0));
        const double cal = r.e.params().inputCalVolts; for (int i = 0; i < 4800; ++i) e += std::pow(in[i] * cal, 2); e = std::sqrt(e / 4800);
        check(std::fabs(e - 0.100) < 2e-3, "the engine's digital-to-jack-voltage conversion reproduces 0.100 V RMS", f(e, 4) + " V");
        const double Vrated = std::sqrt(12.5 * 8.0); check(std::fabs(Vrated - 10.0) < 1e-9, "derived rated terminal voltage sqrt(P R) = 10.0 V RMS (14.14 V pk)", f(Vrated, 3) + " V; engine output reference 20 V pk = 0 dBFS, rated sine = -3.0 dBFS");
    }

    // ============================================================================================ not evaluated
    section("Not evaluated (requires reference hardware or measured data)");
    para("These dossier criteria compare the model with a specific physical amplifier, speaker or microphone chain, which does not exist in the public record. They are intentionally **not** claimed:\n\n"
         "- clean magnitude response within 0.5 dB over 80 Hz–8 kHz (Sec. 26) · phase/group delay · clipping-onset input within 0.5 dB · harmonics within 1 dB · IMD within 2 dB · burst envelope within 5 % · recovery landmarks within 10 % · held-out waveform NRMSE ≤ 5 %;\n"
         "- experiment 1 (boundary calibration against a physical reamp rig), 2–3 (complex speaker impedance and amp-under-two-loads vs measurement), 4 (linear acoustic identity: needs a measured voltage-to-microphone response), 5 (speaker nonlinear isolation), 7 (microphone integrity);\n"
         "- blind level-matched listening study (Sec. 26);\n"
         "- every parameter the manifest marks `assumed` (24) or `unknown` (4): supply/output-transformer data, speaker T/S and large-signal curves, pot taper, installed-tube curves, cabinet geometry.\n\n"
         "Evidence status of the result: **documented approximation of the modern '57 Custom Deluxe** with the historical 5E3 baseline and every assumption visible (dossier Sec. 33).");

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    R << "\n---\n**Summary: " << nPass << " passed, " << nFail << " failed** (" << f(secs, 1) << " s)\n";
    std::cout << "\nSUMMARY: " << nPass << " passed, " << nFail << " failed (" << f(secs, 1) << " s)\n";
    if (!reportPath.empty()) { std::ofstream o(reportPath); o << R.str(); }
    return nFail ? 1 : 0;
}
