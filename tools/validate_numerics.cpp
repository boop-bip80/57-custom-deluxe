// validate_numerics -- numerical-rigor study of the circuit solver and the resampling chain (dossier Sec. 21, 23, 26, 27 exp. 8).
//
//   validate_numerics [--quick] [--only A,B,...] [--report FILE.md]
//
// Sections
//   A  what the legacy "alias" figure of cd57_validate actually measured (spectral identification of the lines)
//   B  resampler specification, measured          C  aliasing / total error against a 16x reference
//   D  step-size convergence of the transient solver
//   E  solver self-checks (dossier Sec. 21/31): assembled Jacobian, KCL along a transient, charge conservation, energy balance, counters
//
// Result vocabulary follows cd57_validate: PASS / FAIL (a stated criterion fixed in advance), INFO (a number for the record).
// Nothing here certifies a match with a physical amplifier; it measures the numerical error of THIS model against a
// higher-rate run of THE SAME model (dossier Sec. 23: "establish a high-accuracy offline reference at a substantially
// higher internal rate").
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
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

template <class Fn> static void parallelFor(size_t n, Fn fn)
{
    std::atomic<size_t> next{0};
    const unsigned nt = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; ++t) th.emplace_back([&] { for (;;) { const size_t i = next++; if (i >= n) break; fn(i); } });
    for (auto& t : th) t.join();
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

// ================================================================================================ B. resampler specification
// Dossier Sec. 23: "Specify passband ripple, transition width and stopband attenuation for the chosen resamplers. Report their
// latency and phase behavior."  Everything is measured on the implementation itself (Oversampler::up / ::down), not on its design formula.
struct ChainSpec {
    double upRippleDb = 0, dnRippleDb = 0, rtRippleDb = 0;   // peak-to-peak gain variation over the audio band (dB)
    double minus3Hz = 0, flatEdgeHz = 0;                      // down path: -3 dB frequency, first frequency below -0.01 dB
    double imageRejDb = 1e9, imageHz = 0;                     // up path: smallest rejection of an image of 0-20 kHz, and where
    double aliasRejDb = 1e9, aliasHz = 0;                     // down path: smallest rejection of input that folds into 0-20 kHz, and where
    double delayDevMax = 0;                                   // max |measured delay - latency()| over 1-20 kHz (samples)
    double latency = 0, latUp = 0, latDown = 0;
};

// least-squares fit of A sin(w n + phi) to y (Hann-weighted, exact 2x2 normal equations)
static void fitTone(const double* y, size_t N, double w, double& A, double& phi)
{
    double scc = 0, sss = 0, scs = 0, syc = 0, sys = 0;
    for (size_t n = 0; n < N; ++n) {
        const double wt = 0.5 - 0.5 * std::cos(2 * M_PI * ((double)n + 0.5) / (double)N), c = std::cos(w * (double)n), s = std::sin(w * (double)n);
        scc += wt * c * c; sss += wt * s * s; scs += wt * c * s; syc += wt * y[n] * c; sys += wt * y[n] * s;
    }
    const double det = scc * sss - scs * scs;
    const double a = (syc * sss - sys * scs) / det, b = (sys * scc - syc * scs) / det;     // y ~ a cos + b sin
    A = std::sqrt(a * a + b * b); phi = std::atan2(a, b);
}

static ChainSpec measureChain(double fs, int log2F)
{
    ChainSpec sp;
    const int F = 1 << log2F;
    Oversampler os; os.prepare(log2F, fs);
    sp.latency = os.latency(); sp.latUp = os.latencyUp(); sp.latDown = os.latencyDown();
    const double fr = fs * F;

    // ---- up path: impulse response at the internal rate -> H(f') for any f'; unity passband gain means sum(h) = F
    {
        const int nIn = 1024;
        std::vector<double> imp(nIn, 0.0), h((size_t)nIn * F);
        imp[0] = 1.0;
        os.up(imp.data(), nIn, h.data());
        size_t last = 0; for (size_t j = 0; j < h.size(); ++j) if (std::fabs(h[j]) > 1e-14) last = j;
        auto H = [&](double fq) { cplx s = 0; const double w = 2 * M_PI * fq / fr; for (size_t j = 0; j <= last; ++j) s += h[j] * std::polar(1.0, -w * (double)j); return std::abs(s) / F; };
        double lo = 1e9, hi = -1e9;
        for (double fq = 0; fq <= kPassbandHz; fq += 50.0) { const double g = 20 * std::log10(H(fq)); lo = std::min(lo, g); hi = std::max(hi, g); }
        sp.upRippleDb = hi - lo;
        for (int k = 1; k <= F / 2; ++k)
            for (double fq = 0; fq <= kPassbandHz; fq += 50.0)
                for (double sgn : {-1.0, 1.0}) {
                    const double fi = k * fs + sgn * fq;
                    if (fi <= kPassbandHz || fi > fr / 2) continue;
                    const double rej = -20 * std::log10(H(fi) + 1e-30);
                    if (rej < sp.imageRejDb) { sp.imageRejDb = rej; sp.imageHz = fi; }
                }
        if (F == 1) sp.imageRejDb = 300.0;
    }
    // ---- down path: tone sweep through Oversampler::down; output tone amplitude = |H(f')| at the folded frequency
    if (F > 1) {
        const size_t nOut = 3072, nIn = nOut * F, nFit = 1536;
        std::vector<double> x(nIn), y(nOut);
        auto gain = [&](double fq) {
            double fo = std::fabs(fq - fs * std::round(fq / fs));
            if (fo < 30.0 || fo > fs / 2 - 30.0) { fq += 41.0; fo = std::fabs(fq - fs * std::round(fq / fs)); }
            for (size_t j = 0; j < nIn; ++j) x[j] = std::sin(2 * M_PI * fq * (double)j / fr);
            os.reset(); os.down(x.data(), (int)nOut, y.data());
            double A, phi; fitTone(y.data() + (nOut - nFit), nFit, 2 * M_PI * fo / fs, A, phi);
            return std::pair<double, double>(A, fo);
        };
        double lo = 1e9, hi = -1e9; bool flatFound = false, m3Found = false;
        std::vector<std::pair<double, double>> sweep;                         // (f', gain dB)
        for (double fq = 50.0; fq < fr / 2; fq += (fq < 1.5 * fs ? 50.0 : 500.0)) {
            const auto g = gain(fq);
            const double db = 20 * std::log10(g.first + 1e-30);
            if (fq <= kPassbandHz) { lo = std::min(lo, db); hi = std::max(hi, db); }
            if (!flatFound && fq > 1000.0 && db < -0.01) { sp.flatEdgeHz = fq; flatFound = true; }
            if (!m3Found && fq > 1000.0 && db < -3.0103) { sp.minus3Hz = fq; m3Found = true; }
            if (fq > kPassbandHz && g.second <= kPassbandHz && -db < sp.aliasRejDb) { sp.aliasRejDb = -db; sp.aliasHz = fq; }
        }
        sp.dnRippleDb = hi - lo;
    } else {
        sp.aliasRejDb = 300.0;
    }
    // ---- round trip host -> up -> down -> host: gain flatness and delay against the reported latency
    {
        const size_t N = 4096, nFit = 2048;
        std::vector<double> x(N), mid(N * F), y(N);
        double lo = 1e9, hi = -1e9;
        for (double fq = 1000.0; fq <= kPassbandHz; fq += 500.0) {
            const double fz = fq + 13.0;                                      // avoid bin-centred coincidences
            for (size_t n = 0; n < N; ++n) x[n] = std::sin(2 * M_PI * fz * (double)n / fs);
            os.reset(); os.up(x.data(), (int)N, mid.data()); os.down(mid.data(), (int)N, y.data());
            double A, phi; const double w = 2 * M_PI * fz / fs;
            fitTone(y.data() + (N - nFit), nFit, w, A, phi);
            phi -= w * (double)(N - nFit);                                    // phase referred to sample 0
            const double db = 20 * std::log10(A); lo = std::min(lo, db); hi = std::max(hi, db);
            double d = phi + w * sp.latency; d -= 2 * M_PI * std::round(d / (2 * M_PI));
            sp.delayDevMax = std::max(sp.delayDevMax, std::fabs(d / w));
        }
        sp.rtRippleDb = hi - lo;
    }
    return sp;
}

static void sectionB()
{
    section("B. Resampler specification, measured (dossier Sec. 23)");
    para("Half-band Kaiser cascade; stage 0 (host rate to 2x) is designed for the host rate with the protected band 0-20 kHz, every higher stage for the same band. All numbers are measured on `Oversampler::up/down` themselves. "
         "*Alias rejection* = the weakest attenuation, over all input frequencies that fold into 0-20 kHz at the host rate, seen by the down path; "
         "*image rejection* = the same for the images of 0-20 kHz created by the up path; ripple is peak-to-peak over the passband; *delay error* is the largest deviation of the measured round-trip delay from the reported latency over 1-20 kHz.");
    struct Row { double fs; int l2; ChainSpec s; };
    std::vector<Row> rows;
    for (double fs : {44100.0, 48000.0, 96000.0}) for (int l2 = 1; l2 <= 3; ++l2) rows.push_back({fs, l2, ChainSpec()});
    parallelFor(rows.size(), [&](size_t i) { rows[i].s = measureChain(rows[i].fs, rows[i].l2); });
    R << "| host | factor | latency (up + down) | up ripple p-p | down ripple p-p | round-trip ripple p-p | -3 dB point | first below -0.01 dB | alias rejection (worst at) | image rejection (worst at) | delay error |\n|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (auto& r : rows) {
        const auto& s = r.s;
        R << "| " << f(r.fs / 1e3, 1) << " kHz | " << (1 << r.l2) << "x | " << f(s.latency, 0) << " smp (" << f(s.latUp, 3) << " + " << f(s.latDown, 3) << ") | " << f(s.upRippleDb, 5) << " dB | " << f(s.dnRippleDb, 5) << " dB | " << f(s.rtRippleDb, 5)
          << " dB | " << f(s.minus3Hz / 1e3, 2) << " kHz | " << f(s.flatEdgeHz / 1e3, 2) << " kHz | " << f(s.aliasRejDb, 1) << " dB (" << f(s.aliasHz / 1e3, 1) << " kHz) | " << f(s.imageRejDb, 1) << " dB (" << f(s.imageHz / 1e3, 1) << " kHz) | " << f(s.delayDevMax, 4) << " smp |\n";
        std::cout << "  " << r.fs << " " << (1 << r.l2) << "x: lat " << s.latency << " (" << s.latUp << "+" << s.latDown << ") ripple up/dn/rt " << f(s.upRippleDb, 5) << "/" << f(s.dnRippleDb, 5) << "/" << f(s.rtRippleDb, 5)
                  << " -3dB " << f(s.minus3Hz, 0) << " alias " << f(s.aliasRejDb, 1) << "@" << f(s.aliasHz, 0) << " image " << f(s.imageRejDb, 1) << "@" << f(s.imageHz, 0) << " delayErr " << f(s.delayDevMax, 5) << "\n";
    }
    R << "\n";
    bool allInt = true, delayOk = true, rippleOk = true, rejOk = true; double worstRip = 0, worstDelay = 0, worstRej = 1e9;
    for (auto& r : rows) {
        allInt = allInt && std::fabs(r.s.latency - std::round(r.s.latency)) < 1e-9;
        delayOk = delayOk && r.s.delayDevMax < 0.01;
        rippleOk = rippleOk && r.s.rtRippleDb <= 0.001 && r.s.upRippleDb <= 0.001 && r.s.dnRippleDb <= 0.001;
        rejOk = rejOk && r.s.aliasRejDb >= 100.0 && r.s.imageRejDb >= 100.0;
        worstRip = std::max(worstRip, std::max(r.s.rtRippleDb, std::max(r.s.upRippleDb, r.s.dnRippleDb))); worstDelay = std::max(worstDelay, r.s.delayDevMax);
        worstRej = std::min(worstRej, std::min(r.s.aliasRejDb, r.s.imageRejDb));
    }
    check(allInt, "the reported latency is a whole number of host samples at every factor and host rate");
    check(delayOk, "the measured round-trip delay equals the reported latency within 0.01 sample over 1-20 kHz (linear phase, exact latency)", "worst " + f(worstDelay, 5) + " sample");
    check(rippleOk, "passband ripple <= 0.001 dB peak-to-peak over 0-20 kHz in the up path, the down path and the round trip (a gain error of 1.2e-4 would already sit at -78 dBFS for a full-scale signal)", "worst " + f(worstRip, 5) + " dB");
    check(rejOk, "alias rejection of the down path and image rejection of the up path are >= 100 dB for everything that can fold into / image from 0-20 kHz", "worst " + f(worstRej, 1) + " dB");
}

// ================================================================================================ shared: reference machinery
// Dossier Sec. 23: "Establish a high-accuracy offline reference at a substantially higher internal rate; compare realtime
// candidates at several oversampling factors."  The reference is the same model run at R x the host rate with analytic
// input; the candidate is the production chain (up-sampler -> solver at F x the host rate -> down-sampler).
// Mains phase: circuit time t sees the signal x(t - D_up), where D_up is the exact delay of the up-sampler.  The
// reference and the no-resampler core are therefore fed x(t - D_up) so that the 2 x f_mains supply modulation has
// the same phase relative to the signal as in the candidate; their outputs are read at circuit time (m - D_down)/fs.
static double besselI0(double x) { double s = 1, t = 1; for (int k = 1; k < 200; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-18 * s) break; } return s; }

// linear-phase Kaiser low-pass (unity DC gain, odd length): passband edge fp, stopband edge fstop (Hz) at sample rate fr, A dB stopband
static std::vector<double> kaiserLowpass(double fr, double fp, double fstop, double A)
{
    const double dw = 2 * M_PI * (fstop - fp) / fr;
    int L = (int)std::ceil((A - 8.0) / (2.285 * dw)) + 1;
    if (L % 2 == 0) ++L;
    const double beta = 0.1102 * (A - 8.7);
    const double fc = 0.5 * (fp + fstop) / fr, c = (L - 1) / 2.0;
    std::vector<double> h(L);
    double sum = 0;
    for (int k = 0; k < L; ++k) {
        const double n = k - c, r = n / c;
        const double sinc = n == 0 ? 2 * fc : std::sin(2 * M_PI * fc * n) / (M_PI * n);
        h[k] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
        sum += h[k];
    }
    for (auto& v : h) v /= sum;
    return h;
}

// Offline "ideal" decimator by M: passband 0-20 kHz flat to 3e-7, stopband from fs-20 kHz at 130 dB (everything that could
// fold into 0-20 kHz is removed).  Evaluated only at the requested output instants, so the cost is small.
struct IdealDecimator {
    int M; std::vector<double> h;
    IdealDecimator(double fs, int m) : M(m) { if (M > 1) h = kaiserLowpass(fs * M, 20000.0, fs - 20000.0, 130.0); }
    double at(const std::vector<double>& x, long i0) const   // band-limited signal at high-rate index i0
    {
        if (M == 1) return (i0 >= 0 && i0 < (long)x.size()) ? x[(size_t)i0] : 0.0;
        const long c = (long)h.size() / 2;
        double s = 0;
        for (long k = 0; k < (long)h.size(); ++k) { const long i = i0 + k - c; if (i >= 0 && i < (long)x.size()) s += h[(size_t)k] * x[(size_t)i]; }
        return s;
    }
    // host-rate window [m0, m0+N): sample m is the circuit output at time (m - dDown)/fs, read tickShift high-rate samples later
    std::vector<double> window(const std::vector<double>& x, long m0, long N, double dDown, long tickShift = 0) const
    {
        std::vector<double> y((size_t)N);
        for (long m = 0; m < N; ++m) y[(size_t)m] = at(x, std::llround((double)(m0 + m - dDown) * M) + tickShift);
        return y;
    }
};

// the model without any resampler: solver at fsInt on an input given at the internal rate; returns terminal volts
struct CoreOut { std::vector<double> y; uint64_t steps = 0, substepped = 0, failures = 0; double meanIt = 0; };
// Integration method per element class (theta = 1/2 trapezoidal, 1 backward Euler).  Production = {0.5, 0.5, 1.0}: inductors and the
// output-transformer windings and all capacitors trapezoidal except the six 6V6 interelectrode capacitors (netlist th=1).
struct ThetaCfg {
    double inductive = 0.5, caps = 0.5, tube = 1.0;
    bool production() const { return inductive == 0.5 && caps == 0.5 && tube == 1.0; }
    std::string name() const { return "L/OT " + f(inductive, 1) + ", C " + f(caps, 1) + ", 6V6 C " + f(tube, 1); }
};
// the circuit alone at solver rate fsInt (the engine with oversampling off, run at the internal rate), quiescent state solved
static std::unique_ptr<CD57Engine> makeCore(const EngineParams& p0, double fsInt, const ThetaCfg& th = ThetaCfg())
{
    EngineParams p = p0; p.oversampleLog2 = 0;
    auto e = std::make_unique<CD57Engine>();
    e->prepare(fsInt, 64, gNetlist, p, gManifest);
    if (!th.production()) {
        Network& n = e->amp().network();
        for (auto& c : n.cap()) c.thetaSel = c.thetaSel > 0 ? th.tube : th.caps;      // thetaSel > 0 only on the 6V6 interelectrode capacitors
        e->amp().prepare(fsInt, th.inductive);
        e->amp().initialiseQuiescent(0.15);
    }
    return e;
}
static CoreOut runCore(const EngineParams& p0, double fsInt, const std::vector<double>& u, const ThetaCfg& th = ThetaCfg(), double tolScale = 1.0)
{
    const auto e = makeCore(p0, fsInt, th);
    CoreOut o; o.y.resize(u.size());
    AmpCircuit& a = e->amp();
    if (tolScale != 1.0) { Network& n = a.network(); n.absTolV *= tolScale; n.relTol *= tolScale; n.resTolA *= tolScale; }     // section F only
    const Network::Stats base = a.network().stats;
    for (size_t i = 0; i < u.size(); ++i) { a.tick(u[i]); o.y[i] = a.vTerminal(); }
    const Network::Stats& s = a.network().stats;
    o.steps = s.steps - base.steps; o.substepped = s.substepped - base.substepped; o.failures = s.failures - base.failures;
    o.meanIt = o.steps ? double(s.iterations - base.iterations) / double(o.steps) : 0.0;
    return o;
}

// ---- stimuli (analytic functions of time, jack EMF in volts)
static double fade(double t, double tf) { return t <= 0 ? 0.0 : t >= tf ? 1.0 : 0.5 - 0.5 * std::cos(M_PI * t / tf); }
struct Stimulus { std::string name; std::function<double(double)> x; double f0; double winStart; };
static std::string hz(double v) { return f(v, 0) + " Hz"; }
static Stimulus toneStim(double fq, double amp) { return {"tone " + hz(fq), [=](double t) { return amp * std::sin(2 * M_PI * fq * t) * fade(t, 0.1); }, fq, 0.30}; }
static Stimulus twoToneStim(double f1, double f2, double amp)
{ return {"two-tone " + hz(f1) + " + " + hz(f2), [=](double t) { return 0.5 * amp * (std::sin(2 * M_PI * f1 * t) + std::sin(2 * M_PI * f2 * t)) * fade(t, 0.1); }, 0.0, 0.30}; }
static Stimulus burstStim(double fq, double amp)
{
    return {"burst " + hz(fq) + ", 100 ms", [=](double t) { return amp * std::sin(2 * M_PI * fq * t) * std::min(fade(t - 0.15, 0.003), fade(0.25 - t, 0.003)); }, 0.0, 0.12};
}
// guitar-like chord: open E major (E2 B2 E3 G#3 B3 E4), eight harmonics per note at 1/h, fixed pseudo-random phases, peak = amp
static Stimulus chordStim(double amp)
{
    const double notes[6] = {82.41, 123.47, 164.81, 207.65, 246.94, 329.63};
    std::vector<double> fr, am, ph;
    uint32_t s = 12345u;
    for (double n : notes) for (int h = 1; h <= 8; ++h) { fr.push_back(n * h); am.push_back(1.0 / h); s = s * 1664525u + 1013904223u; ph.push_back(2 * M_PI * (s >> 8) / 16777216.0); }
    auto raw = [=](double t) { double v = 0; for (size_t i = 0; i < fr.size(); ++i) v += am[i] * std::sin(2 * M_PI * fr[i] * t + ph[i]); return v; };
    double pk = 0; for (int i = 0; i < 200000; ++i) pk = std::max(pk, std::fabs(raw(i / 100000.0)));
    return {"chord (E major, 6 notes)", [=](double t) { return amp * raw(t) / pk * fade(t, 0.1); }, 0.0, 0.30};
}

// ---- comparison metric
// Residual r = candidate - reference over a host-rate window, band-limited to 0-20 kHz (DC removed from both; the DC difference
// is reported separately).  dBFS: 0 dBFS = a full-scale sine (AES17: a full-scale sine reads 0 dBFS in rms power AND in line
// amplitude).  ESR (Sec. 26) = residual power / reference power in the same band.
struct Cmp { double resDb = -300, esrDb = -300, lineDb = -300, lineHz = 0, offDb = -300, offHz = 0, dcMv = 0; };
static Cmp compareWin(const std::vector<double>& a, const std::vector<double>& b, double fs, double FS, double f0)
{
    const size_t N = a.size();
    double ma = 0, mb = 0;
    for (size_t i = 0; i < N; ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double)N; mb /= (double)N;
    std::vector<double> r(N), rb(N);
    for (size_t i = 0; i < N; ++i) { r[i] = (a[i] - ma) - (b[i] - mb); rb[i] = b[i] - mb; }
    const auto pr = hannPower(r, N), pb = hannPower(rb, N);
    const size_t kmax = (size_t)(20000.0 * (double)N / fs);
    double er = 0, eb = 0;
    for (size_t k = 1; k <= kmax; ++k) { er += pr[k]; eb += pb[k]; }
    Cmp c;
    c.dcMv = (ma - mb) * 1e3;
    c.resDb = 10 * std::log10(er / (FS * FS / 2) + 1e-30);
    c.esrDb = 10 * std::log10(er / (eb + 1e-30) + 1e-30);
    for (size_t k = 3; k <= kmax; ++k) {
        if (!(pr[k] >= pr[k - 1] && pr[k] > pr[k + 1])) continue;
        double lp = 0; for (size_t j = k - 2; j <= k + 2; ++j) lp += pr[j];
        const double db = 10 * std::log10(2.0 * lp / (FS * FS) + 1e-30), fr = (double)k * fs / (double)N;
        if (db > c.lineDb) { c.lineDb = db; c.lineHz = fr; }
        const bool onHarm = f0 > 0 && std::fabs(fr - f0 * std::round(fr / f0)) < 40.0;
        if (!onHarm && db > c.offDb) { c.offDb = db; c.offHz = fr; }
    }
    return c;
}

// align: 0 = exact (production alignment), 1 = reference without the one-step mains-phase correction, 2 = reference that ignores the up-sampler delay (sensitivity tests only)
struct CaseSpec { Stimulus stim; std::string level; double fs; int log2F; int load; int align = 0; };
struct CaseResult {
    Cmp tot, integ, up, down, samp, trunc;      // total / integration+internal-rate sampling / up-sampler / down-sampler / sampling alone / solver truncation alone
    double peakV = 0, rmsV = 0;                 // reference output level over the window
    double subst = 0, failures = 0, meanIt = 0; // candidate solver: sub-stepped fraction, failed steps, Newton iterations per step
};
static const double kFsPeak = 28.28;            // product default: 28.28 V peak = 0 dBFS (rated 12.5 W sine = -6 dBFS)
static const int kRef = 16;                     // reference oversampling factor
static size_t windowLen(double fs) { return fs > 70000.0 ? 32768 : 16384; }

static CaseResult runCase(const CaseSpec& cs)
{
    const double fs = cs.fs;
    const int F = 1 << cs.log2F;
    const size_t N = windowLen(fs);
    const size_t nHost = (size_t)std::ceil(cs.stim.winStart * fs) + N;
    const long m0 = (long)(nHost - N);
    EngineParams p = baseParams(); p.oversampleLog2 = cs.log2F; p.load = cs.load;
    Oversampler os; os.prepare(cs.log2F, fs);
    const double dUp = os.latencyUp(), dDown = os.latencyDown();

    std::vector<double> xh(nHost);
    for (size_t m = 0; m < nHost; ++m) xh[m] = cs.stim.x((double)m / fs);
    // candidate: up-sampler -> solver -> down-sampler (exactly the CD57Engine::process chain)
    std::vector<double> uUp(nHost * F), yTot(nHost);
    os.up(xh.data(), (int)nHost, uUp.data());
    const CoreOut cu = runCore(p, fs * F, uUp);
    os.down(cu.y.data(), (int)nHost, yTot.data());
    // same solver rate, no resampler: analytic input shifted by the up-sampler delay
    std::vector<double> uAn(nHost * F);
    for (size_t j = 0; j < uAn.size(); ++j) uAn[j] = cs.stim.x((double)j / (fs * F) - dUp / fs);
    const CoreOut ca = runCore(p, fs * F, uAn);
    // reference.  AmpCircuit::tick() advances the mains phase one internal step BEFORE using it, so tick j of a solver at rate r sits at
    // mains time (j+1)/r while carrying the signal of stamp j/r.  The signal-to-mains phase of the candidate therefore differs by one
    // step 1/(F fs) from that of a run at another rate; the reference is fed the same signal at the same mains time by delaying its
    // input by 1/(F fs) - 1/(R fs) and reading its output (R/F - 1) ticks later.  Without this the comparison contains a first-order
    // error of 2 pi f_mains / (F fs) radians of supply-ripple phase (effect on the residual: alignment table below; on the order of convergence: section D).
    const size_t refShift = cs.align == 1 ? 0 : (size_t)(kRef / F) - 1;
    const double refDelay = (cs.align == 2 ? 0.0 : dUp / fs) + (cs.align == 1 ? 0.0 : 1.0 / (fs * F) - 1.0 / (fs * kRef));
    std::vector<double> uRef(nHost * kRef);
    for (size_t j = 0; j < uRef.size(); ++j) uRef[j] = cs.stim.x((double)j / (fs * kRef) - refDelay);
    const CoreOut cr = runCore(p, fs * kRef, uRef);
    // point-sampling of the reference at the candidate's solver rate (no solver, no resampler: pure sampling alias)
    std::vector<double> ys(nHost * F);
    for (size_t j = 0; j < ys.size(); ++j) ys[j] = cr.y[j * (size_t)(kRef / F) + refShift];

    const IdealDecimator dF(fs, F), dR(fs, kRef);
    const auto yRef = dR.window(cr.y, m0, (long)N, dDown, (long)refShift);
    const auto yAn = dF.window(ca.y, m0, (long)N, dDown);
    const auto yUpId = dF.window(cu.y, m0, (long)N, dDown);
    const auto ySamp = dF.window(ys, m0, (long)N, dDown);
    const std::vector<double> yT(yTot.begin() + m0, yTot.end());

    CaseResult r;
    r.tot = compareWin(yT, yRef, fs, kFsPeak, cs.stim.f0);
    r.integ = compareWin(yAn, yRef, fs, kFsPeak, cs.stim.f0);
    r.up = compareWin(yUpId, yAn, fs, kFsPeak, cs.stim.f0);
    r.down = compareWin(yT, yUpId, fs, kFsPeak, cs.stim.f0);
    r.samp = compareWin(ySamp, yRef, fs, kFsPeak, cs.stim.f0);
    r.trunc = compareWin(yAn, ySamp, fs, kFsPeak, cs.stim.f0);
    double e2 = 0; for (double v : yRef) { r.peakV = std::max(r.peakV, std::fabs(v)); e2 += v * v; }
    r.rmsV = std::sqrt(e2 / (double)N);
    r.subst = cu.steps ? (double)cu.substepped / (double)cu.steps : 0.0; r.failures = (double)cu.failures; r.meanIt = cu.meanIt;
    return r;
}

// ================================================================================================ C. reference comparison matrix
static void sectionC(bool quick)
{
    section("C. Aliasing and total numerical error against a 16x reference (dossier Sec. 23, 26)");
    para("Reference: the same model, solver at 16 x the host rate, analytic input, offline. Candidates: the production chain at 1/2/4/8 x. Metric: residual (candidate minus reference) band-limited to 0-20 kHz, "
         "in dBFS (0 dBFS = full-scale sine; product default full scale 28.28 V peak, so the rated 12.5 W sine is -6 dBFS; a 20 V full scale would read 3.0 dB higher) and as ESR. "
         "Alignment: only the documented, exact up-/down-sampler delays are removed; the supply modulation has identical phase in both runs (see the header comment of this section's code). "
         "DC is removed from both signals (the DC difference is listed separately); everything else, including linear gain/phase error of the resampler passbands and the solver's slow-state error, stays in the residual, so the figure is a conservative bound on aliasing. "
         "Load: 8 ohm dummy (resistive bench boundary); Instrument vol 0.5, tone 0.7. Each cell: residual dBFS (ESR dB).");
    struct Lvl { const char* name; double amp; };
    const Lvl lv[3] = {{"clean", 0.004}, {"moderate", 0.015}, {"heavy", 0.060}};
    std::vector<CaseSpec> cases;
    auto add = [&](const std::string& li, double amp, double fs, int kind, double fq) {
        Stimulus s = kind == 0 ? toneStim(fq, amp) : kind == 1 ? twoToneStim(1000, 3100, amp) : kind == 2 ? chordStim(amp) : burstStim(1000, amp);
        for (int l2 = 0; l2 <= 3; ++l2) cases.push_back({s, li, fs, l2, 2});
    };
    if (quick) {
        add("moderate", 0.015, 48000, 0, 3100); add("heavy", 0.060, 48000, 0, 3100); add("heavy", 0.060, 48000, 2, 0);
    } else {
        for (int l = 0; l < 3; ++l) {
            for (double fq : {440.0, 1000.0, 3100.0, 7000.0}) add(lv[l].name, lv[l].amp, 48000, 0, fq);
            add(lv[l].name, lv[l].amp, 48000, 1, 0); add(lv[l].name, lv[l].amp, 48000, 2, 0);
        }
        add("heavy", 0.060, 48000, 3, 0);
        for (double fs : {44100.0, 96000.0}) {
            add("moderate", 0.015, fs, 0, 3100); add("heavy", 0.060, fs, 0, 3100); add("heavy", 0.060, fs, 0, 7000);
            add("heavy", 0.060, fs, 2, 0); add("heavy", 0.060, fs, 3, 0);
        }
    }
    std::vector<CaseResult> res(cases.size());
    const auto t0 = std::chrono::steady_clock::now();
    parallelFor(cases.size(), [&](size_t i) { res[i] = runCase(cases[i]); });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    info(std::to_string(cases.size()) + " reference comparisons in " + f(secs, 0) + " s (4 threads)");

    // tables: one per host rate; rows = stimulus x level
    for (double fs : {48000.0, 44100.0, 96000.0}) {
        bool any = false;
        for (auto& c : cases) if (c.fs == fs) any = true;
        if (!any) continue;
        R << "\n**Host rate " << f(fs / 1e3, 1) << " kHz** — total error of the production chain vs the 16x reference (residual dBFS (ESR dB)); reference output level in the last column.\n\n"
          << "| stimulus | level | 1x | 2x | 4x | 8x | reference output peak / rms |\n|---|---|---|---|---|---|---|\n";
        for (size_t i = 0; i < cases.size(); i += 4) {
            if (cases[i].fs != fs) continue;
            R << "| " << cases[i].stim.name << " | " << cases[i].level << " |";
            for (int k = 0; k < 4; ++k) R << " " << f(res[i + k].tot.resDb, 1) << " (" << f(res[i + k].tot.esrDb, 0) << ") |";
            R << " " << f(20 * std::log10(res[i].peakV / kFsPeak), 1) << " dBFS / " << f(20 * std::log10(res[i].rmsV / (kFsPeak / std::sqrt(2.0))), 1) << " dBFS |\n";
        }
    }
    // decomposition at 48 kHz
    R << "\n**Error sources at 48 kHz** — residual dBFS of each part against the 16x reference: *int* = solver at F x with ideal I/O (integration error plus sampling alias at the internal rate), "
         "*sampling* = reference simply point-sampled at F x (alias of the true waveform, no solver), *trunc* = solver minus that sampled reference (integration truncation), "
         "*up* = effect of the real up-sampler (through the circuit), *down* = effect of the real down-sampler; *total* = production chain.\n\n"
      << "| stimulus | level | F | total | int | sampling | trunc | up | down | sub-stepped | Newton it/step |\n|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (size_t i = 0; i < cases.size(); ++i) {
        if (cases[i].fs != 48000.0) continue;
        const auto& r = res[i];
        R << "| " << cases[i].stim.name << " | " << cases[i].level << " | " << (1 << cases[i].log2F) << "x | " << f(r.tot.resDb, 1) << " | " << f(r.integ.resDb, 1) << " | " << f(r.samp.resDb, 1) << " | " << f(r.trunc.resDb, 1)
          << " | " << f(r.up.resDb, 1) << " | " << f(r.down.resDb, 1) << " | " << f(100 * r.subst, 2) << " % | " << f(r.meanIt, 2) << " |\n";
    }
    R << "\n";
    for (size_t i = 0; i < cases.size(); ++i)
        std::cout << "  " << cases[i].stim.name << " " << cases[i].level << " " << cases[i].fs << " " << (1 << cases[i].log2F) << "x: total " << f(res[i].tot.resDb, 1) << " int " << f(res[i].integ.resDb, 1) << " samp " << f(res[i].samp.resDb, 1)
                  << " trunc " << f(res[i].trunc.resDb, 1) << " up " << f(res[i].up.resDb, 1) << " down " << f(res[i].down.resDb, 1) << " dc " << f(res[i].tot.dcMv, 2) << " mV  out " << f(20 * std::log10(res[i].peakV / kFsPeak), 1) << " dBFS pk\n";

    // alignment sensitivity: why the exact up-/down-sampler delays and the mains-phase convention matter
    Oversampler o4; o4.prepare(2, 48000.0);
    para("Alignment sensitivity (4x, total residual dBFS). The supply modulates the gain at 2 x f_mains, so the phase of that modulation relative to the signal must be identical in candidate and reference: "
         "'exact' = the alignment used above; 'no mains-step correction' = reference fed at the raw tick time, ignoring that the engine advances the mains phase one internal step before use; "
         "'up-sampler delay ignored' = reference fed with no up-sampler delay (the 4x chain delays the signal by " + f(o4.latencyUp(), 2) + " samples before the circuit sees it).");
    struct Al { Stimulus s; const char* label; };
    std::vector<Al> als = {{toneStim(3100, 0.060), "tone 3100 Hz, heavy"}, {chordStim(0.060), "chord, heavy"}};
    std::vector<CaseSpec> ac;
    for (auto& a : als) for (int mode : {0, 1, 2}) ac.push_back({a.s, a.label, 48000.0, 2, 2, mode});
    std::vector<CaseResult> ar(ac.size());
    parallelFor(ac.size(), [&](size_t i) { ar[i] = runCase(ac[i]); });
    R << "| stimulus | exact | no mains-step correction | up-sampler delay ignored |\n|---|---|---|---|\n";
    for (size_t k = 0; k < als.size(); ++k) {
        R << "| " << als[k].label << " | " << f(ar[3 * k].tot.resDb, 1) << " | " << f(ar[3 * k + 1].tot.resDb, 1) << " | " << f(ar[3 * k + 2].tot.resDb, 1) << " |\n";
        std::cout << "  alignment " << als[k].label << ": exact " << f(ar[3 * k].tot.resDb, 1) << ", no mains-step correction " << f(ar[3 * k + 1].tot.resDb, 1) << ", up delay ignored " << f(ar[3 * k + 2].tot.resDb, 1) << "\n";
    }
    R << "\n";
    bool sens = true;
    for (size_t k = 0; k < als.size(); ++k) sens = sens && ar[3 * k + 2].tot.resDb > ar[3 * k].tot.resDb + 30.0;
    check(sens, "ignoring the up-sampler delay worsens the residual by more than 30 dB, so the exact delay split (Oversampler::latencyUp/Down) is required for a valid comparison",
          "tone " + f(ar[2].tot.resDb - ar[0].tot.resDb, 0) + " dB, chord " + f(ar[5].tot.resDb - ar[3].tot.resDb, 0) + " dB worse");
    info("the one-step mains-phase correction changes the 4x residual by " + f(ar[1].tot.resDb - ar[0].tot.resDb, 1) + " dB (tone) and " + f(ar[4].tot.resDb - ar[3].tot.resDb, 1) + " dB (chord): small here because solver truncation dominates at 4x, "
         "but it decides the measured order of convergence (section D)");
}

// ================================================================================================ D. step-size convergence
// core-only window: solver at F x host rate, analytic input, ideal decimation (no resampler, no delay)
struct CoreWin { std::vector<double> y; double subst = 0, failures = 0, meanIt = 0; };
static CoreWin coreWindow(const EngineParams& p, const Stimulus& st, double fs, int F, const ThetaCfg& th, bool naiveStamp = false, double tolScale = 1.0)
{
    const size_t N = windowLen(fs), nHost = (size_t)std::ceil(st.winStart * fs) + N;
    std::vector<double> u(nHost * (size_t)F);
    // tick j sits at mains time (j+1)/(F fs): feed the signal of that instant so every rate sees the same signal-to-mains phase
    for (size_t j = 0; j < u.size(); ++j) u[j] = st.x((double)(j + (naiveStamp ? 0 : 1)) / (fs * F));
    const CoreOut c = runCore(p, fs * F, u, th, tolScale);
    CoreWin w;
    w.y = IdealDecimator(fs, F).window(c.y, (long)(nHost - N), (long)N, 0.0, naiveStamp ? 0 : -1);
    w.subst = c.steps ? (double)c.substepped / (double)c.steps : 0.0; w.failures = (double)c.failures; w.meanIt = c.meanIt;
    return w;
}

static void sectionD(bool quick)
{
    section("D. Step-size convergence of the transient solver (dossier Sec. 21)");
    para("The solver is run without any resampler at F x 48 kHz (F = 1 ... 64) on the same analytic input; the outputs are ideally decimated to 48 kHz and compared in the 0-20 kHz band. "
         "The order of convergence is estimated from three successive grids without using any reference: d_F = ||s_F - s_2F||, p = log2(d_F / d_2F); "
         "the error of the F-solution is then d_F / (2^p - 1) (Richardson). Coarse factors are not in the asymptotic regime (sampling alias and Newton sub-stepping), so the order is read from 4x upwards. "
         "Heavy drive: 60 mV peak at the jack, 8 ohm dummy load, Instrument vol 0.5, tone 0.7.");
    std::vector<Stimulus> stims = {toneStim(3100, 0.060)};
    if (!quick) { stims.push_back(chordStim(0.060)); stims.push_back(toneStim(440, 0.060)); }
    const std::vector<int> Fs = quick ? std::vector<int>{1, 2, 4, 8, 16, 32} : std::vector<int>{1, 2, 4, 8, 16, 32, 64};
    EngineParams p = baseParams();
    std::vector<std::vector<CoreWin>> win(stims.size(), std::vector<CoreWin>(Fs.size()));
    parallelFor(stims.size() * Fs.size(), [&](size_t i) { win[i / Fs.size()][i % Fs.size()] = coreWindow(p, stims[i / Fs.size()], 48000.0, Fs[i % Fs.size()], ThetaCfg()); });
    for (size_t s = 0; s < stims.size(); ++s) {
        R << "**" << stims[s].name << ", 60 mV peak, production integration methods**\n\n| F | solver rate | d_F = rms of (s_F - s_2F), dBFS | observed order p | extrapolated error of s_F, dBFS | sub-stepped | Newton it/step |\n|---|---|---|---|---|---|---|\n";
        std::vector<double> d(Fs.size(), 0.0), pObs(Fs.size(), 0.0), err(Fs.size(), 0.0);
        for (size_t k = 0; k + 1 < Fs.size(); ++k) d[k] = compareWin(win[s][k].y, win[s][k + 1].y, 48000.0, kFsPeak, 0.0).resDb;
        for (size_t k = 0; k + 2 < Fs.size(); ++k) pObs[k] = (d[k] - d[k + 1]) / 6.0206;
        for (size_t k = 0; k + 1 < Fs.size(); ++k) err[k] = d[k] - 20 * std::log10(std::pow(2.0, pObs[std::min(k, Fs.size() - 3)]) - 1.0);   // the last pair reuses the previous order
        for (size_t k = 0; k + 1 < Fs.size(); ++k) {
            R << "| " << Fs[k] << "x | " << f(48.0 * Fs[k], 0) << " kHz | " << f(d[k], 1) << " | " << (k + 2 < Fs.size() ? f(pObs[k], 2) : std::string("—")) << " | "
              << f(err[k], 1) << (k + 2 < Fs.size() ? "" : " (order of the previous row assumed)") << " | " << f(100 * win[s][k].subst, 2) << " % | " << f(win[s][k].meanIt, 2) << " |\n";
            std::cout << "  " << stims[s].name << " F=" << Fs[k] << ": d " << f(d[k], 1) << (k + 2 < Fs.size() ? " p " + f(pObs[k], 2) : "") << " err " << f(err[k], 1) << " subst " << f(100 * win[s][k].subst, 2) << "%\n";
        }
        R << "| " << Fs.back() << "x | " << f(48.0 * Fs.back(), 0) << " kHz | (finest grid) | — | — | " << f(100 * win[s].back().subst, 2) << " % | " << f(win[s].back().meanIt, 2) << " |\n\n";
        // reference uncertainty: error estimate of the 16x run used in section C
        const size_t i16 = std::find(Fs.begin(), Fs.end(), 16) - Fs.begin();
        info(stims[s].name + ": the 16x reference of section C carries an estimated error of " + f(err[i16], 1) + " dBFS (extrapolated); the 8x candidate's own error is " + f(err[i16 - 1], 1) + " dBFS");
        double pMin = 1e9; for (size_t k = 2; k + 2 < Fs.size(); ++k) pMin = std::min(pMin, pObs[k]);
        // The scheme is first order by design wherever backward Euler is used (the 6V6 interelectrode capacitances, see the netlist), so an
        // exactly first-order run returns p = 1 up to estimator noise; the criterion is p >= 1 within a stated estimator tolerance of 0.05.
        check(pMin >= 0.95, stims[s].name + ": the solver converges at its designed order (>= first order, estimator tolerance 0.05) from 4x upwards", "observed p (4x, 8x, ...): " + [&] { std::string t; for (size_t k = 2; k + 2 < Fs.size(); ++k) t += f(pObs[k], 3) + " "; return t; }());
    }

    // ---- why the mains-time convention matters for the measured order
    if (!quick) {
        const int FsN[5] = {4, 8, 16, 32, 64};
        std::vector<CoreWin> nw(5);
        parallelFor(5, [&](size_t i) { nw[i] = coreWindow(p, stims[1], 48000.0, FsN[i], ThetaCfg(), true); });
        double dN[4], dA[4];
        for (int k = 0; k < 4; ++k) {
            dN[k] = compareWin(nw[k].y, nw[k + 1].y, 48000.0, kFsPeak, 0.0).resDb;
            dA[k] = compareWin(win[1][2 + k].y, win[1][3 + k].y, 48000.0, kFsPeak, 0.0).resDb;
        }
        info("convention check, " + stims[1].name + ": with the raw tick-stamp convention (every rate fed the signal of stamp j/r while the engine's mains phase is one step ahead) the measured order from 4x-8x-16x is "
             + f((dN[0] - dN[1]) / 6.0206, 2) + " and from 8x-16x-32x " + f((dN[1] - dN[2]) / 6.0206, 2) + ", against " + f((dA[0] - dA[1]) / 6.0206, 2) + " and " + f((dA[1] - dA[2]) / 6.0206, 2)
             + " with the mains-time convention used throughout: the rate-dependent mains-phase offset is a first-order artefact of the harness, not of the solver");
    }

    // ---- theta per element class: which class limits the accuracy?
    para("Integration method per element class (theta = 0.5 trapezoidal, 1 backward Euler). The three classes: inductors and output-transformer windings; all capacitors except the 6V6 interelectrode ones; "
         "the six 6V6 interelectrode capacitors (production: backward Euler to avoid ringing against the transformer leakage). Error is measured against the 32x production run of the first table; "
         "'order' is the three-grid estimate within each configuration.");
    const ThetaCfg cfgs[5] = {{0.5, 0.5, 1.0}, {0.5, 0.5, 0.5}, {1.0, 0.5, 1.0}, {0.5, 1.0, 1.0}, {1.0, 1.0, 1.0}};
    const int FsT[5] = {1, 2, 4, 8, 16};
    const size_t i32 = std::find(Fs.begin(), Fs.end(), 32) - Fs.begin();
    const size_t nThetaStims = quick ? 1 : 2;
    for (size_t s = 0; s < nThetaStims; ++s) {
        std::vector<CoreWin> tw(5 * 5);
        parallelFor(tw.size(), [&](size_t i) {
            const int F = FsT[i % 5];
            tw[i] = (i < 5) ? win[s][std::find(Fs.begin(), Fs.end(), F) - Fs.begin()] : coreWindow(p, stims[s], 48000.0, F, cfgs[i / 5]);
        });
        R << "**" << stims[s].name << ", 60 mV peak** — residual vs the 32x production reference, dBFS\n\n| integration methods | 1x | 2x | 4x | 8x | 16x | order 2x-4x-8x | order 4x-8x-16x | sub-stepped at 2x / 4x |\n|---|---|---|---|---|---|---|---|---|\n";
        for (int c = 0; c < 5; ++c) {
            double e[5], dd[4];
            for (int k = 0; k < 5; ++k) e[k] = compareWin(tw[c * 5 + k].y, win[s][i32].y, 48000.0, kFsPeak, 0.0).resDb;
            for (int k = 0; k < 4; ++k) dd[k] = compareWin(tw[c * 5 + k].y, tw[c * 5 + k + 1].y, 48000.0, kFsPeak, 0.0).resDb;
            R << "| " << cfgs[c].name() << (cfgs[c].production() ? " (production)" : "") << " | " << f(e[0], 1) << " | " << f(e[1], 1) << " | " << f(e[2], 1) << " | " << f(e[3], 1) << " | " << f(e[4], 1)
              << " | " << f((dd[1] - dd[2]) / 6.0206, 2) << " | " << f((dd[2] - dd[3]) / 6.0206, 2) << " | " << f(100 * tw[c * 5 + 1].subst, 2) << " % / " << f(100 * tw[c * 5 + 2].subst, 2) << " % |\n";
            std::cout << "  " << stims[s].name << " theta " << cfgs[c].name() << ": err " << f(e[0], 1) << " " << f(e[1], 1) << " " << f(e[2], 1) << " " << f(e[3], 1) << " " << f(e[4], 1)
                      << " order " << f((dd[1] - dd[2]) / 6.0206, 2) << " / " << f((dd[2] - dd[3]) / 6.0206, 2) << " subst2x " << f(100 * tw[c * 5 + 1].subst, 2) << "%\n";
        }
        R << "\n";
    }
}

// ================================================================================================ F. is the Newton tolerance what limits the accuracy?
static void sectionF(bool quick)
{
    section("F. The Newton tolerance is not what limits the accuracy (dossier Sec. 21)");
    para("Section D shows the error of the heavy-drive tone falling as a power of the step size, which is the signature of time-step truncation; this section tests the alternative explanation directly. "
         "The solver's convergence tolerances (step tolerance in volts, relative tolerance, nodal KCL residual in amperes) are tightened by a factor of 10^4 and the residual against a much finer run is compared "
         "with the production tolerances. If convergence limited the accuracy the residual would fall; if it is truncation of the integration it does not move. 3.1 kHz tone, 60 mV peak at the jack, 8 ohm dummy load.");
    EngineParams p = baseParams();
    const Stimulus st = toneStim(3100, 0.060);
    const int Fref = quick ? 16 : 32;
    const int Fc[2] = {2, 8};
    CoreWin ref, w[2][2];
    parallelFor(5, [&](size_t i) {
        if (i == 0) ref = coreWindow(p, st, 48000.0, Fref, ThetaCfg());
        else { const int k = (int)(i - 1) / 2, t = (int)(i - 1) % 2; w[k][t] = coreWindow(p, st, 48000.0, Fc[k], ThetaCfg(), false, t == 0 ? 1.0 : 1e-4); }
    });
    R << "| solver rate | tolerances | residual vs " << Fref << "x, dBFS | Newton it/step | sub-stepped |\n|---|---|---|---|---|\n";
    double worst = 0;
    for (int k = 0; k < 2; ++k) {
        const double d0 = compareWin(w[k][0].y, ref.y, 48000.0, kFsPeak, 0.0).resDb, d1 = compareWin(w[k][1].y, ref.y, 48000.0, kFsPeak, 0.0).resDb;
        R << "| " << Fc[k] << "x | production | " << f(d0, 1) << " | " << f(w[k][0].meanIt, 2) << " | " << f(100 * w[k][0].subst, 2) << " % |\n";
        R << "| " << Fc[k] << "x | tightened 10^4 x | " << f(d1, 1) << " | " << f(w[k][1].meanIt, 2) << " | " << f(100 * w[k][1].subst, 2) << " % |\n";
        worst = std::max(worst, std::fabs(d1 - d0));
        std::cout << "  F=" << Fc[k] << ": production " << f(d0, 1) << " dBFS, tightened " << f(d1, 1) << " dBFS\n";
    }
    R << "\n";
    check(worst < 1.0, "tightening the Newton and KCL tolerances 10^4 x changes the heavy-drive residual by less than 1 dB at 2x and 8x: the residual is time-step truncation, not convergence", "largest change " + f(worst, 2) + " dB");
    info("consequence: the only ways to lower the heavy-drive residual are smaller steps (more oversampling) or a more accurate integrator for the kinks of the device laws; neither more Newton iterations nor tighter tolerances help");
}

// ================================================================================================ E. solver self-checks
static void sectionE(bool quick)
{
    section("E. Solver self-checks (dossier Sec. 21: KCL residuals, DC equilibrium, capacitor charge, power flow; Sec. 31: verified Jacobian)");

    // E1. the assembled analytic Jacobian against central differences, at heavy-drive states and at control extremes
    {
        struct Ctl { double iv, mv, tn; LoadMode ld; const char* name; };
        const Ctl ctls[] = {{0.7, 0.0, 0.7, LoadMode::Speaker, "nominal"}, {0.0, 0.0, 0.0, LoadMode::Dummy8, "all knobs at 0"},
                            {1.0, 1.0, 1.0, LoadMode::Speaker, "all knobs at full"}, {0.02, 1.0, 0.3, LoadMode::Dummy4, "instrument 0.02, mic full, 4 ohm"}};
        double worstAll = 0; std::string where;
        for (const Ctl& c : ctls) {
            AmpCircuit a; a.loadNetlist(gNetlist); a.prepare(96000.0, 0.5);
            AmpControls k; k.instVol = c.iv; k.micVol = c.mv; k.tone = c.tn; k.guitarJack = 1; k.load = c.ld;
            a.setControls(k); a.applyControls(true); a.initialiseQuiescent(0.1);
            std::vector<std::vector<double>> snaps;
            for (int i = 0; i < 9600; ++i) {
                if (i % 64 == 0) a.applyControls(false);
                a.tick(0.08 * std::sin(2 * M_PI * 220.0 * i / 96000.0) * std::min(1.0, i / 1500.0));
                if (i % 1200 == 600) snaps.push_back(a.network().state());
            }
            Network& n = a.network();
            for (const auto& zt : snaps) {
                int wr, wc; double an, nu;
                const double w = n.checkJacobian(zt, wr, wc, an, nu);
                if (w > worstAll) { worstAll = w; where = std::string(c.name) + ", row " + std::to_string(wr) + " col " + std::to_string(wc); }
            }
        }
        check(worstAll < 1e-3, "assembled system Jacobian matches central finite differences at heavy-drive states and control extremes (4 control sets x 8 states)", "worst relative error " + f(worstAll * 1e6, 1) + " e-6 at " + where);
    }

    // E2. nodal KCL residual and honest failure counters along a heavy-drive transient (not only at DC)
    {
        AmpCircuit a; a.loadNetlist(gNetlist); a.prepare(96000.0, 0.5);
        AmpControls k; k.instVol = 0.8; k.tone = 0.7; k.guitarJack = 1; k.load = LoadMode::Speaker;
        a.setControls(k); a.applyControls(true); a.initialiseQuiescent(0.15);
        Network& n = a.network(); n.trackResidual = true; n.stats = {};
        const int N = (int)(96000 * (quick ? 0.3 : 0.8));
        for (int i = 0; i < N; ++i) { if (i % 64 == 0) a.applyControls(false); a.tick(0.25 * std::sin(2 * M_PI * 330.0 * i / 96000.0) * std::min(1.0, i / 1500.0)); }
        uint64_t hist = 0; for (uint64_t v : n.failHist) hist += v;
        check(n.stats.maxResidual <= n.resTolA && n.stats.failures == 0, "heavy-drive transient (250 mV at the jack, speaker load): nodal KCL residual stays below the acceptance threshold at every accepted step",
              "max residual " + f(n.stats.maxResidual * 1e6, 2) + " uA (threshold " + f(n.resTolA * 1e6, 0) + " uA), " + std::to_string(n.stats.steps) + " steps, " + std::to_string(n.stats.substepped) + " sub-stepped, " + std::to_string(n.stats.failures) + " unrecovered");
        check(hist == n.stats.failures, "failure counters are consistent: every unrecovered step is attributed to an unknown in the failure histogram", std::to_string(hist) + " attributed / " + std::to_string(n.stats.failures) + " counted");
    }

    // E3. one-stage charge conservation and current-sign convention (dossier Sec. 07: 'verify charge conservation with a simple one-stage test')
    {
        const char* text =
            "R Rs  b 0 1k\nR Rpl b p 100k\nTRIODE V1 p g k type=12AX7\nR Rk k 0 1.5k\nC Ck k 0 25u\nR Rg g 0 1Meg\nC Cin pu g 22n\nR Rpu pu 0 1Meg\n";
        Network net; net.parse(text); const double h = 1.0 / 96000.0; net.build(h, 0.5);
        net.inj(net.nodeId("b")) = 250.0 / 1000.0;                       // Norton supply: 250 V behind 1 kohm
        net.solveDC();
        const int nk = net.nodeId("k"), nb = net.nodeId("b"), np = net.nodeId("p"), npu = net.nodeId("pu");
        const double Rk = 1500.0, Ck = 25e-6;
        double dcMis = std::fabs((net.V(nb) - net.V(np)) / 100e3 - (net.V(nk) / Rk) + net.tri()[0].ig) / (net.V(nk) / Rk);
        const double vk0 = net.V(nk);
        double q = 0, iPrev = net.tri()[0].ip + net.tri()[0].ig - net.V(nk) / Rk, worst = 0, scale = 0;
        const int Nst = 96000 / 4;
        for (int i = 0; i < Nst; ++i) {
            net.inj(npu) = 0.9 * std::sin(2 * M_PI * 200.0 * i / 96000.0) * std::min(1.0, i / 4000.0) / 1e6;
            net.step();
            const double iC = net.tri()[0].ip + net.tri()[0].ig - net.V(nk) / Rk;     // KCL at the cathode: tube current in, Rk out, the rest charges Ck
            q += 0.5 * h * (iC + iPrev); iPrev = iC;
            const double dq = Ck * (net.V(nk) - vk0);
            worst = std::max(worst, std::fabs(q - dq)); scale = std::max(scale, std::fabs(dq));
        }
        check(dcMis < 1e-3 && worst < 2e-3 * scale, "one triode stage: the cathode KCL holds (tube current - V_k/R_k = capacitor current) and the charge delivered equals C_k dV_k over 0.25 s of heavy drive",
              "DC mismatch " + f(dcMis, 6) + ", charge error " + f(100 * worst / scale, 4) + " % of the largest stored charge");
    }

    // E4. power-flow / energy balance over a heavy-drive burst, whole amplifier (dossier Sec. 21, 31):  E_sources = E_dissipated + dE_stored
    {
        AmpCircuit a; a.loadNetlist(gNetlist); a.prepare(96000.0, 0.5);
        AmpControls k; k.instVol = 0.8; k.tone = 0.7; k.guitarJack = 1; k.load = LoadMode::Speaker;
        a.setControls(k); a.applyControls(true); a.initialiseQuiescent(0.15);
        Network& n = a.network(); const double h = 1.0 / 96000.0;
        auto pin = [&]() { double P = 0; for (int i = 0; i < n.nodeCount(); ++i) P += n.inj(i) * n.V(i); return P; };
        auto pdiss = [&]() {
            double P = 0;
            for (auto& r : n.res()) { const double v = n.V(r.a) - n.V(r.b); P += v * v / (r.R * r.var); }
            for (auto& t : n.tri()) P += t.ip * (n.V(t.p) - n.V(t.k)) + t.ig * (n.V(t.g) - n.V(t.k));
            for (auto& q : n.pen()) P += q.ip * (n.V(q.p) - n.V(q.k)) + q.is * (n.V(q.s) - n.V(q.k)) + q.ig * (n.V(q.g) - n.V(q.k));
            for (auto& d : n.dio()) P += d.i * (n.V(d.a) - n.V(d.k));
            { const auto& x = n.xfm(); for (int j = 0; j < 3; ++j) P += x.R[j] * x.Rvar[j] * x.i[j] * x.i[j]; }
            return P;
        };
        auto stored = [&]() {
            double E = 0;
            for (auto& c : n.cap()) { const double v = n.V(c.a) - n.V(c.b); E += 0.5 * c.C * c.var * v * v; }
            for (auto& l : n.ind()) E += 0.5 * l.L * l.var * l.iprev * l.iprev;
            {
                const auto& x = n.xfm();
                double F = 0;
                for (int j = 0; j < 3; ++j) { E += 0.5 * x.Ll[j] * x.i[j] * x.i[j]; F += x.N[j] * x.i[j]; }
                E += 0.5 * (x.P0 + x.Pair) * F * F;                        // linear core (the default): energy = P F^2 / 2
            }
            return E;
        };
        const int N = (int)(96000 * (quick ? 0.25 : 0.6));
        double Ein = 0, Ed = 0, Pi1 = 0, Pd1 = 0, E0 = 0, peakLoad = 0;
        for (int i = 0; i < N; ++i) {
            if (i % 64 == 0) a.applyControls(false);
            a.tick(0.1 * std::sin(2 * M_PI * 330.0 * i / 96000.0) * std::min(1.0, i / 3000.0));
            const double Pi = pin(), Pd = pdiss();
            if (i == 0) { E0 = stored(); }
            else { Ein += 0.5 * h * (Pi + Pi1); Ed += 0.5 * h * (Pd + Pd1); }
            Pi1 = Pi; Pd1 = Pd;
            peakLoad = std::max(peakLoad, std::fabs(a.vTerminal()));
        }
        const double dE = stored() - E0, err = Ein - Ed - dE;
        check(std::fabs(err) < 0.01 * Ein, "whole amplifier, heavy-drive burst: energy delivered by the sources = energy dissipated (resistors, valves, rectifier, winding loss) + change in stored energy (capacitors, inductors, transformer flux)",
              "sources " + f(Ein, 3) + " J, dissipated " + f(Ed, 3) + " J, stored change " + f(dE, 4) + " J, imbalance " + f(100 * err / Ein, 3) + " % of the source energy (peak speaker voltage " + f(peakLoad, 1) + " V)");
    }
}

// ================================================================================================ main
int main(int argc, char** argv)
{
    bool quick = false; std::string reportPath, only = "ABCDEF";
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
    if (want('B')) sectionB();
    if (want('C')) sectionC(quick);
    if (want('D')) sectionD(quick);
    if (want('E')) sectionE(quick);
    if (want('F')) sectionF(quick);

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    R << "\n---\n**Summary: " << nPass << " passed, " << nFail << " failed** (" << f(secs, 1) << " s)\n";
    std::cout << "\nSUMMARY: " << nPass << " passed, " << nFail << " failed (" << f(secs, 1) << " s)\n";
    if (!reportPath.empty()) { std::ofstream o(reportPath); o << R.str(); }
    return nFail ? 1 : 0;
}
