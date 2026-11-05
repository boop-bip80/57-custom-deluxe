// validate_numerics -- numerical-rigor study of the circuit solver and the resampling chain (dossier Sec. 21, 23, 26, 27 exp. 8).
//
//   validate_numerics [--quick] [--only A,B,...] [--report FILE.md]
//
// Sections
//   A  what the legacy "alias" figure of cd57_validate actually measured (spectral identification of the lines)
//
// Result vocabulary follows cd57_validate: PASS / FAIL (a stated criterion fixed in advance), INFO (a number for the record).
// Nothing here certifies a match with a physical amplifier; it measures the numerical error of THIS model against a
// higher-rate run of THE SAME model (dossier Sec. 23: "establish a high-accuracy offline reference at a substantially
// higher internal rate").
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
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

// Single-sided power spectrum of the last N samples of x under a Hann window, normalised so that a sine of
// peak amplitude A contributes A^2/2 in total (summed over its main lobe) and the sum over all bins is the
// window-weighted mean square.
static std::vector<double> hannPower(const std::vector<double>& x, size_t N)
{
    std::vector<cplx> a(N);
    double s2 = 0.0;
    for (size_t i = 0; i < N; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)N);
        a[i] = x[x.size() - N + i] * w; s2 += w * w;
    }
    fft(a);
    std::vector<double> p(N / 2);
    for (size_t k = 0; k < N / 2; ++k) p[k] = (k == 0 ? 1.0 : 2.0) * std::norm(a[k]) / ((double)N * s2);
    return p;
}

static std::string gNetlist, gManifest;

static EngineParams baseParams()
{
    EngineParams p; p.oversampleLog2 = 1; p.instVol = 0.5f; p.micVol = 0.0f; p.tone = 0.7f; p.jack = 1; p.load = 2;   // 8 ohm dummy load
    p.speakerHeating = false; p.inputCalVolts = 1.0f; p.outputRefVolts = 28.28f;
    return p;
}

// Run the production engine (host-rate float I/O, internal oversampling) on `inVolts`; returns terminal volts.
// `rippleFree` scales the three supply capacitors up by 1000 (diagnostic only, as in cd57_validate section 5).
static std::vector<double> runEngine(const EngineParams& p, double fs, const std::vector<double>& inVolts, bool rippleFree = false, int block = 512)
{
    CD57Engine e; e.prepare(fs, block, gNetlist, p, gManifest);
    if (rippleFree) {
        Network& n = e.amp().network();
        for (const char* c : {"C1", "C2", "C3"}) n.setC(n.capId(c), n.cap()[n.capId(c)].Cnom * 1000.0);
        n.refreshLinear();
        e.amp().initialiseQuiescent(0.05);
    }
    const double cal = (double)p.inputCalVolts * std::pow(10.0, (double)p.inputTrimDb / 20.0), ref = (double)p.outputRefVolts;
    std::vector<double> out(inVolts.size());
    std::vector<float> in(block), ot(block), oa(block);
    for (size_t i = 0; i < inVolts.size(); i += block) {
        const int n = (int)std::min<size_t>(block, inVolts.size() - i);
        for (int k = 0; k < n; ++k) in[k] = (float)(inVolts[i + k] / cal);
        e.process(in.data(), ot.data(), oa.data(), n);
        for (int k = 0; k < n; ++k) out[i + k] = (double)ot[k] * ref;
    }
    return out;
}

// ================================================================================================ A. what the legacy figure measured
struct Line { double f, db, off; int h; };

// strongest local maxima of the power spectrum that lie more than exclHz from every harmonic of f0 (lobe-summed level, dB re fullScale)
static std::vector<Line> offHarmonicLines(const std::vector<double>& y, double fs, double f0, size_t N, double fullScale, double exclHz, double fMax, int top)
{
    const auto p = hannPower(y, N);
    std::vector<Line> out;
    for (size_t k = 3; k + 2 < p.size(); ++k) {
        if (!(p[k] >= p[k - 1] && p[k] > p[k + 1])) continue;
        const double fr = (double)k * fs / (double)N;
        if (fr > fMax) break;
        const int h = (int)std::lround(fr / f0);
        const double off = fr - h * f0;
        if (std::fabs(off) < exclHz) continue;
        double lp = 0; for (size_t j = k - 2; j <= k + 2; ++j) lp += p[j];
        out.push_back({fr, 10 * std::log10(2.0 * lp / (fullScale * fullScale) + 1e-30), off, h});
    }
    std::sort(out.begin(), out.end(), [](const Line& a, const Line& b) { return a.db > b.db; });
    if ((int)out.size() > top) out.resize(top);
    return out;
}

// level (dB re fullScale, lobe-summed) of the strongest line within +-3 bins of frequency fq
static double lineLevelAt(const std::vector<double>& y, double fs, size_t N, double fq, double fullScale)
{
    const auto p = hannPower(y, N);
    const long kc = std::lround(fq * (double)N / fs);
    double best = 0.0; long kb = kc;
    for (long k = kc - 3; k <= kc + 3; ++k) if (p[(size_t)k] > best) { best = p[(size_t)k]; kb = k; }
    double lp = 0; for (long k = kb - 2; k <= kb + 2; ++k) lp += p[(size_t)k];
    return 10 * std::log10(2.0 * lp / (fullScale * fullScale) + 1e-30);
}

// Identification of a spectral line by frequency (tolerance 2.5 Hz; the lowest harmonic order that fits wins):
//  * a harmonic above the solver's Nyquist frequency folds about the internal rate  -> "solver-rate alias";
//  * a harmonic between the host Nyquist and 0.6 x the host rate (the decimator's transition band; -6 dB at host
//    Nyquist for a half-band) reaches the output attenuated and folds about the host rate -> "decimator fold";
//  * otherwise an offset of n x 2 f_mains from a harmonic below Nyquist              -> "mains sideband".
static std::string identify(const Line& l, double f0, double fsHost, double fsInt, double f2m)
{
    const double tol = 2.5;
    char b[112];
    for (int h = 1; h <= 400; ++h) {
        const double fh = h * f0;
        if (fh <= fsHost / 2) continue;
        const bool aboveSolver = fh > fsInt / 2;
        if (!aboveSolver && fh > 0.6 * fsHost) continue;      // beyond the decimator's transition band the stopband applies: not a plausible source
        const double rate = aboveSolver ? fsInt : fsHost;
        const double fold = std::fabs(fh - rate * std::round(fh / rate));
        if (std::fabs(fold - l.f) < tol) {
            std::snprintf(b, sizeof b, "%s of H%d (%.1f kHz)", aboveSolver ? "solver-rate alias" : "decimator fold", h, fh / 1e3);
            return b;
        }
    }
    const double n = l.off / f2m;
    if (std::fabs(n - std::round(n)) * f2m < 4.0 && std::fabs(std::round(n)) >= 1) { std::snprintf(b, sizeof b, "mains sideband %+d x 2f_mains of H%d", (int)std::lround(n), l.h); return b; }
    return "unidentified";
}

static void sectionA()
{
    section("A. What the legacy 'alias' figure measured (spectral identification)");
    para("The legacy table of `cd57_validate` reports the strongest line more than 80 Hz from any harmonic of 3.1 kHz (3.1 kHz tone, 60 mV pk into the speaker load, Instrument vol 0.6, fs = 48 kHz) "
         "and it reads about -27 dBFS at 2x, 4x and 8x. A true aliasing figure must fall as the oversampling factor rises, so this one cannot be aliasing alone. "
         "Lines are identified by frequency (rules in the source); dBFS here uses 20 V peak as full scale so the numbers are comparable with the legacy table. "
         "'Ripple-free' scales the three supply capacitors by 1000 (a diagnostic, not the product circuit) so that the supply no longer modulates the gain. "
         "Caveat on identification: a 3.1 kHz tone sampled at 48 kHz repeats every 480 samples, so every alias lands on a multiple of 100 Hz, and at 50 Hz mains the sideband offset (100 Hz) is on that same grid; "
         "the invariance tests below, not the labels, carry the argument.");
    const double fs = 48000.0, f0 = 3100.0, FS = 20.0;
    const size_t N = 16384;
    const int total = (int)(0.5 * fs);
    std::vector<double> x(total);
    for (int i = 0; i < total; ++i) { const double t = i / fs; x[i] = 0.06 * std::sin(2 * M_PI * f0 * t) * std::min(1.0, t / 0.02); }
    struct Variant { const char* name; int market; bool rippleFree; double f2m; };
    const Variant vars[] = {{"normal supply, 60 Hz mains", 0, false, 120.0}, {"normal supply, 50 Hz mains", 3, false, 100.0}, {"ripple-free supply, 60 Hz mains", 0, true, 120.0}};
    struct Row { double legacyDb, legacyF, bandDb, bandF, bandOff, sideDb; std::string legacyWhat, bandWhat; };
    Row rows[3][4];
    for (int v = 0; v < 3; ++v) {
        for (int os = 0; os <= 3; ++os) {
            EngineParams p = baseParams(); p.load = 0; p.instVol = 0.6f; p.oversampleLog2 = os; p.outputRefVolts = (float)FS; p.market = vars[v].market;
            const auto y = runEngine(p, fs, x, vars[v].rippleFree);
            const double fsInt = fs * (1 << os);
            const auto legacy = offHarmonicLines(y, fs, f0, N, FS, 80.0, fs / 2 - 50.0 * fs / N, 1);
            const auto band = offHarmonicLines(y, fs, f0, N, FS, 80.0, 20000.0, 1);
            Row& r = rows[v][os];
            r.legacyDb = legacy[0].db; r.legacyF = legacy[0].f; r.bandDb = band[0].db; r.bandF = band[0].f; r.bandOff = band[0].off;
            r.legacyWhat = identify(legacy[0], f0, fs, fsInt, vars[v].f2m);
            r.bandWhat = identify(band[0], f0, fs, fsInt, vars[v].f2m);
            r.sideDb = std::max(lineLevelAt(y, fs, N, f0 + vars[v].f2m, FS), lineLevelAt(y, fs, N, f0 - vars[v].f2m, FS));
            std::cout << "  " << vars[v].name << " " << (1 << os) << "x: legacy " << f(r.legacyDb, 1) << " (" << r.legacyWhat << "); in-band " << f(r.bandDb, 1) << " (" << r.bandWhat << "); sideband " << f(r.sideDb, 1) << "\n";
        }
    }
    R << "Legacy metric (normal supply, 60 Hz mains): strongest off-harmonic line anywhere below 23.9 kHz.\n\n| oversampling | level | frequency | identification |\n|---|---|---|---|\n";
    for (int os = 0; os <= 3; ++os) R << "| " << (1 << os) << "x | " << f(rows[0][os].legacyDb, 1) << " dBFS | " << f(rows[0][os].legacyF, 0) << " Hz | " << rows[0][os].legacyWhat << " |\n";
    R << "\nStrongest off-harmonic line up to 20 kHz, and the level of the H1 +- 2 f_mains sideband.\n\n| supply | oversampling | strongest line <= 20 kHz | frequency | identification | H1 +- 2 f_mains sideband |\n|---|---|---|---|---|---|\n";
    for (int v = 0; v < 3; ++v)
        for (int os = 0; os <= 3; ++os)
            R << "| " << vars[v].name << " | " << (1 << os) << "x | " << f(rows[v][os].bandDb, 1) << " dBFS | " << f(rows[v][os].bandF, 1) << " Hz | " << rows[v][os].bandWhat << " | " << f(rows[v][os].sideDb, 1) << " dBFS |\n";
    R << "\n";

    bool legacyIsFold = true;
    for (int os = 0; os <= 3; ++os) legacyIsFold = legacyIsFold && rows[0][os].legacyWhat.find("H8 (24.8 kHz)") != std::string::npos;
    check(legacyIsFold, "the legacy figure's strongest line is the fold of the tone's 8th harmonic (24.8 kHz folded to 23.2 kHz), not a mains sideband",
          "1x " + f(rows[0][0].legacyDb, 1) + ", 2x " + f(rows[0][1].legacyDb, 1) + ", 4x " + f(rows[0][2].legacyDb, 1) + ", 8x " + f(rows[0][3].legacyDb, 1)
          + " dBFS; from 2x up it is a decimator fold (the half-band's transition band straddles Nyquist, -6 dB there, identical at every factor), which is why the figure is flat");
    check(std::fabs(rows[0][3].sideDb - rows[0][2].sideDb) < 1.0 && std::fabs(rows[0][3].sideDb - rows[0][1].sideDb) < 2.0,
          "the H1 +- 2 f_mains sideband level does not change with the oversampling factor (2x/4x/8x)",
          f(rows[0][1].sideDb, 1) + " / " + f(rows[0][2].sideDb, 1) + " / " + f(rows[0][3].sideDb, 1) + " dBFS");
    check(rows[0][3].bandWhat.find("mains sideband") != std::string::npos && rows[0][2].bandWhat.find("mains sideband") != std::string::npos,
          "within 20 kHz, at 4x and 8x the strongest off-harmonic line is a mains-ripple sideband",
          "8x: " + f(rows[0][3].bandDb, 1) + " dBFS @ " + f(rows[0][3].bandF, 1) + " Hz = " + rows[0][3].bandWhat);
    check(std::fabs(std::fabs(rows[1][3].bandOff) - 100.0) < 4.0 && std::fabs(std::fabs(rows[0][3].bandOff) - 120.0) < 4.0 && rows[1][3].bandWhat.find("mains sideband") != std::string::npos,
          "the sideband follows the mains frequency (offset 120 Hz at 60 Hz mains, 100 Hz at 50 Hz mains)", "offsets " + f(rows[0][3].bandOff, 1) + " / " + f(rows[1][3].bandOff, 1) + " Hz");
    check(rows[2][3].sideDb < rows[0][3].sideDb - 30.0, "removing the supply ripple removes the sideband (at least 30 dB lower)",
          f(rows[0][3].sideDb, 1) + " -> " + f(rows[2][3].sideDb, 1) + " dBFS at 8x");
    bool falls = true; double prev = 1e9;
    for (int os = 0; os <= 3; ++os) { if (rows[2][os].bandDb > prev - 6.0) falls = false; prev = rows[2][os].bandDb; }
    check(falls, "with the supply ripple removed, the strongest in-band off-harmonic line falls by more than 6 dB per doubling of the oversampling factor",
          "1x " + f(rows[2][0].bandDb, 1) + ", 2x " + f(rows[2][1].bandDb, 1) + ", 4x " + f(rows[2][2].bandDb, 1) + ", 8x " + f(rows[2][3].bandDb, 1) + " dBFS (20 V pk); 8x: " + rows[2][3].bandWhat);
    info("conclusion: the legacy -27 dBFS is (a) a decimator-transition-band fold at 23.2 kHz and (b) real mains modulation of the model; neither depends on the factor, and together they hid the solver-rate aliasing that does fall with it "
         "(still " + f(rows[2][3].bandDb, 1) + " dBFS at 8x for this 60 mV drive). The reference-based sections measure aliasing by differencing against a high-rate run with identical mains phase, which cancels the sidebands.");
}

// ================================================================================================ main
int main(int argc, char** argv)
{
    bool quick = false; std::string reportPath, only = "ABCDEFG";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quick") quick = true;
        else if (a == "--report" && i + 1 < argc) reportPath = argv[++i];
        else if (a == "--only" && i + 1 < argc) { only.clear(); for (char c : std::string(argv[++i])) if (c != ',') only += (char)std::toupper((unsigned char)c); }
    }
    gNetlist = embeddedNetlist(); gManifest = embeddedManifest();
    const auto t0 = std::chrono::steady_clock::now();
    auto want = [&](char c) { return only.find(c) != std::string::npos; };

    R << "# Numerical-rigor report — solver, resampling chain and aliasing\n\n";
    R << "Generated by `validate_numerics" << (quick ? " --quick" : "") << "`. It measures the numerical error of this model against higher-rate runs of the same model (dossier Sec. 21, 23, 26, 27 experiment 8). "
         "It says nothing about agreement with a physical amplifier.\n";

    if (want('A')) sectionA();

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    R << "\n---\n**Summary: " << nPass << " passed, " << nFail << " failed** (" << f(secs, 1) << " s)\n";
    std::cout << "\nSUMMARY: " << nPass << " passed, " << nFail << " failed (" << f(secs, 1) << " s)\n";
    if (!reportPath.empty()) { std::ofstream o(reportPath); o << R.str(); }
    return nFail ? 1 : 0;
}
