// Metrics.h -- the dossier Sec. 26 error metrics as one reusable, JUCE-free, double-precision header.
//
// Every definition below is FIXED and STATED (Sec. 26: "using a fixed stated onset definition", "Report DC handling, alignment and
// any band-limiting", "Report independently after only the declared fixed-delay removal", "Never optimize a new gain correction
// separately for every validation clip").  Nothing here fits a gain to a clip: the only free operation is ONE integer-sample delay
// that the caller estimates once per configuration (estimateFixedDelay) and then applies to every comparison of that configuration.
//
// The same functions measure the model against itself (regression, rate independence, the capture-matrix reference) and, unchanged,
// measure the model against a hardware recording once a specimen exists (tools/cd57_compare.cpp).
//
//   waveform      : NRMSE = sqrt(sum (y - yhat)^2 / sum y^2),  ESR_dB = 10 log10(...)   (y = reference, yhat = test);  NRMSE 0.05 <-> ESR -26.02 dB
//   response      : H(f) = Y/X (regularised) from a log sweep; compared on 1/12-octave power-averaged points, 80 Hz .. 8 kHz
//   phase / delay : phase after removing the declared fixed delay; group delay = -dphi/domega
//   harmonics     : coherent lock-in, levels in dBc re the fundamental, components above -60 dBc are compared
//   onset         : input level where THD reaches 1 % (definition A) and where the fundamental gain is 1 dB below small-signal (B)
//   IMD           : two-tone products |a f1 + b f2| (|a|+|b| <= 5), dBc re the larger tone
//   burst         : lock-in envelope at the burst frequency; envelope deviation (5 % criterion) and recovery landmarks (10 % criterion)
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace cd57::metrics {

using cplx = std::complex<double>;
using Vec = std::vector<double>;
constexpr double kPi = 3.14159265358979323846;

inline double db20(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-300)); }
inline double db10(double ratio) { return 10.0 * std::log10(std::max(ratio, 1e-300)); }

// ------------------------------------------------------------------------------------------------- FFT
inline size_t nextPow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

// in-place radix-2 FFT; inverse is scaled by 1/N
inline void fft(std::vector<cplx>& a, bool inverse = false)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * kPi / (double)len;
        const cplx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cplx w(1.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const cplx u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl;
            }
        }
    }
    if (inverse) for (auto& v : a) v /= (double)n;
}

inline std::vector<cplx> toSpectrum(const Vec& x, size_t n)
{
    std::vector<cplx> a(n, cplx(0.0));
    for (size_t i = 0; i < x.size() && i < n; ++i) a[i] = x[i];
    fft(a);
    return a;
}

// linear (full) convolution through the FFT; output length a.size() + b.size() - 1
inline Vec convolve(const Vec& a, const Vec& b)
{
    if (a.empty() || b.empty()) return {};
    const size_t L = a.size() + b.size() - 1, n = nextPow2(L);
    auto A = toSpectrum(a, n), B = toSpectrum(b, n);
    for (size_t i = 0; i < n; ++i) A[i] *= B[i];
    fft(A, true);
    Vec o(L);
    for (size_t i = 0; i < L; ++i) o[i] = A[i].real();
    return o;
}

// ------------------------------------------------------------------------------------------------- basic signal measures
inline double energy(const Vec& x) { double e = 0; for (double v : x) e += v * v; return e; }
inline double rms(const Vec& x) { return x.empty() ? 0.0 : std::sqrt(energy(x) / (double)x.size()); }
inline double mean(const Vec& x) { return x.empty() ? 0.0 : std::accumulate(x.begin(), x.end(), 0.0) / (double)x.size(); }
inline double peakAbs(const Vec& x) { double p = 0; for (double v : x) p = std::max(p, std::fabs(v)); return p; }
inline Vec removeDc(Vec x) { const double m = mean(x); for (auto& v : x) v -= m; return x; }

// zero-phase FFT band limit (raised-cosine skirts, each 1/6 octave wide on the log axis outside [lo, hi]).  Declared band-limiting for
// waveform metrics: apply the SAME call to reference and test.
inline Vec bandLimit(const Vec& x, double sr, double lo, double hi)
{
    const size_t n = nextPow2(x.size());
    auto S = toSpectrum(x, n);
    const double skirt = std::pow(2.0, 1.0 / 6.0);
    for (size_t k = 0; k <= n / 2; ++k) {
        const double f = (double)k * sr / (double)n;
        double g = 1.0;
        if (f < lo / skirt || f > hi * skirt) g = 0.0;
        else if (f < lo) g = 0.5 - 0.5 * std::cos(kPi * std::log(f / (lo / skirt)) / std::log(skirt));
        else if (f > hi) g = 0.5 + 0.5 * std::cos(kPi * std::log(f / hi) / std::log(skirt));
        S[k] *= g;
        if (k > 0 && k < n / 2) S[n - k] *= g;
    }
    fft(S, true);
    Vec o(x.size());
    for (size_t i = 0; i < o.size(); ++i) o[i] = S[i].real();
    return o;
}

// ------------------------------------------------------------------------------------------------- waveform metrics (Sec. 26 "Waveform definition")
struct WaveformError {
    double nrmse = 0, esrDb = 0;
    double absRmsError = 0;            // rms of (y - yhat) in the signal's own units: the criterion that stays meaningful for near-silence
    double refRms = 0;
    bool illConditioned = false;       // reference nearly silent: NRMSE / ESR carry no meaning, use absRmsError
    size_t samples = 0;
};

struct WaveformOptions {
    bool removeDc = false;             // DC handling is REPORTED by the caller; the default keeps DC (a real DC offset is an error)
    double bandLo = 0.0, bandHi = 0.0; // 0/0 = no band limiting
    double sampleRate = 48000.0;
    double silenceRms = 1e-9;          // reference rms below this => ill-conditioned
    size_t skip = 0, count = 0;        // analysis window (count 0 = to the end); used to exclude start-up transients explicitly
};

inline WaveformError waveformError(const Vec& ref, const Vec& test, const WaveformOptions& o = {})
{
    WaveformError w;
    const size_t n0 = std::min(ref.size(), test.size());
    const size_t a = std::min(o.skip, n0), b = o.count ? std::min(n0, a + o.count) : n0;
    Vec y(ref.begin() + (long)a, ref.begin() + (long)b), yh(test.begin() + (long)a, test.begin() + (long)b);
    if (o.removeDc) { y = removeDc(y); yh = removeDc(yh); }
    if (o.bandHi > 0.0) { y = bandLimit(y, o.sampleRate, o.bandLo, o.bandHi); yh = bandLimit(yh, o.sampleRate, o.bandLo, o.bandHi); }
    double num = 0, den = 0;
    for (size_t i = 0; i < y.size(); ++i) { const double d = y[i] - yh[i]; num += d * d; den += y[i] * y[i]; }
    w.samples = y.size();
    w.refRms = y.empty() ? 0.0 : std::sqrt(den / (double)y.size());
    w.absRmsError = y.empty() ? 0.0 : std::sqrt(num / (double)y.size());
    w.illConditioned = w.refRms < o.silenceRms;
    w.nrmse = den > 0 ? std::sqrt(num / den) : std::numeric_limits<double>::infinity();
    w.esrDb = den > 0 ? db10(num / den) : std::numeric_limits<double>::infinity();
    return w;
}

// Estimate ONE fixed integer-sample delay of `test` relative to `ref` (positive: test lags).  Cross-correlation through the FFT, searched
// over +-maxLag.  The caller applies the result to every comparison of the configuration (Sec. 24/26: one fixed rule, no per-clip tuning).
struct DelayEstimate { int samples = 0; double peakCorrelation = 0; double fractional = 0; };
inline DelayEstimate estimateFixedDelay(const Vec& ref, const Vec& test, int maxLag)
{
    DelayEstimate d;
    const size_t n = std::min(ref.size(), test.size());
    if (n < 4) return d;
    const size_t N = nextPow2(2 * n);
    auto R = toSpectrum(Vec(ref.begin(), ref.begin() + (long)n), N), T = toSpectrum(Vec(test.begin(), test.begin() + (long)n), N);
    for (size_t i = 0; i < N; ++i) R[i] = T[i] * std::conj(R[i]);
    fft(R, true);
    const double norm = std::sqrt(energy(ref) * energy(test)) + 1e-300;
    double best = -1e300; int bl = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        const double c = R[(size_t)((lag + (long)N) % (long)N)].real();
        if (c > best) { best = c; bl = lag; }
    }
    d.samples = bl; d.peakCorrelation = best / norm;
    const double cm = R[(size_t)((bl - 1 + (long)N) % (long)N)].real(), cp = R[(size_t)((bl + 1 + (long)N) % (long)N)].real();
    const double den = cm - 2 * best + cp;
    d.fractional = den != 0.0 ? 0.5 * (cm - cp) / den : 0.0;      // reported, not applied
    return d;
}

// remove a delay of `d` samples from `test` (test[i + d] -> i); the tail is zero filled
inline Vec removeDelay(const Vec& test, int d)
{
    Vec o(test.size(), 0.0);
    for (size_t i = 0; i < o.size(); ++i) { const long j = (long)i + d; if (j >= 0 && j < (long)test.size()) o[i] = test[(size_t)j]; }
    return o;
}

// ------------------------------------------------------------------------------------------------- stimuli
// exponential (logarithmic) sine sweep (Farina, S31)
struct Sweep {
    double f1 = 20.0, f2 = 20000.0, seconds = 4.0, sr = 48000.0, fade = 0.01;
    Vec x;
    size_t length() const { return x.size(); }
    double rate() const { return std::log(f2 / f1) / seconds; }       // L / T
};
inline Sweep makeLogSweep(double f1, double f2, double seconds, double sr, double amplitude, double fadeSeconds = 0.01)
{
    Sweep s; s.f1 = f1; s.f2 = f2; s.seconds = seconds; s.sr = sr; s.fade = fadeSeconds;
    const size_t n = (size_t)std::llround(seconds * sr);
    s.x.assign(n, 0.0);
    const double L = std::log(f2 / f1), K = 2.0 * kPi * f1 * seconds / L;
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / sr;
        double v = std::sin(K * (std::exp(t * L / seconds) - 1.0));
        const double fi = std::min(1.0, t / fadeSeconds), fo = std::min(1.0, (seconds - t) / fadeSeconds);
        v *= 0.5 - 0.5 * std::cos(kPi * std::max(0.0, std::min(fi, fo)));
        s.x[i] = amplitude * v;
    }
    return s;
}

// frequency snapped to an exact bin of an n-sample window: whole number of cycles in the window, so coherent analysis has no leakage
inline double coherentFrequency(double sr, size_t n, double f0)
{
    const double cycles = std::max(1.0, std::round(f0 * (double)n / sr));
    return cycles * sr / (double)n;
}

// ------------------------------------------------------------------------------------------------- transfer function, phase, group delay
struct Response {
    Vec f, magDb, phaseRad, groupDelay;       // groupDelay in seconds, after removal of the declared fixed delay
    int removedDelaySamples = 0;
};

// H = Y conj(X) / (|X|^2 + lambda).  lambda = reg * max|X|^2 inside the band; outside the band the response is reported but is regularisation-limited.
inline Response transferFunction(const Vec& x, const Vec& y, double sr, double fLo, double fHi, int fixedDelaySamples = 0, double reg = 1e-6,
                                 double pointsPerOctave = 12.0)
{
    Response r; r.removedDelaySamples = fixedDelaySamples;
    const size_t N = nextPow2(std::max(x.size(), y.size()) * 2);
    auto X = toSpectrum(x, N), Y = toSpectrum(y, N);
    double mx = 0;
    for (size_t k = 1; k < N / 2; ++k) { const double f = (double)k * sr / (double)N; if (f >= fLo && f <= fHi) mx = std::max(mx, std::norm(X[k])); }
    const double lam = reg * mx;
    std::vector<cplx> H(N / 2 + 1);
    for (size_t k = 0; k <= N / 2; ++k) H[k] = Y[k] * std::conj(X[k]) / (std::norm(X[k]) + lam);
    // remove the declared fixed delay (pure phase)
    for (size_t k = 0; k <= N / 2; ++k) { const double f = (double)k * sr / (double)N; H[k] *= std::polar(1.0, 2.0 * kPi * f * (double)fixedDelaySamples / sr); }
    // resample onto a 1/12-octave grid with power-averaging of the magnitude and linear interpolation of the phase
    const double oct = std::log2(fHi / fLo);
    const int np = (int)std::floor(oct * pointsPerOctave) + 1;
    const double step = std::pow(2.0, 1.0 / pointsPerOctave), half = std::pow(2.0, 1.0 / (2.0 * pointsPerOctave));
    for (int i = 0; i < np; ++i) {
        const double fc = fLo * std::pow(step, i);
        const double a = fc / half, b = fc * half;
        double p = 0; int cnt = 0;
        for (size_t k = (size_t)std::ceil(a * (double)N / sr); k <= (size_t)std::floor(b * (double)N / sr) && k <= N / 2; ++k) { p += std::norm(H[k]); ++cnt; }
        const double kx = fc * (double)N / sr; const size_t k0 = (size_t)std::floor(kx); const double fr = kx - (double)k0;
        cplx hc = H[std::min(k0, N / 2)] * (1.0 - fr) + H[std::min(k0 + 1, N / 2)] * fr;
        r.f.push_back(fc);
        r.magDb.push_back(cnt > 0 ? db10(p / cnt) : db20(std::abs(hc)));
        r.phaseRad.push_back(std::arg(hc));
    }
    // unwrap + group delay -dphi/domega
    for (size_t i = 1; i < r.phaseRad.size(); ++i) { double d = r.phaseRad[i] - r.phaseRad[i - 1]; while (d > kPi) { r.phaseRad[i] -= 2 * kPi; d -= 2 * kPi; } while (d < -kPi) { r.phaseRad[i] += 2 * kPi; d += 2 * kPi; } }
    r.groupDelay.assign(r.f.size(), 0.0);
    for (size_t i = 0; i < r.f.size(); ++i) {
        const size_t a = i ? i - 1 : i, b = i + 1 < r.f.size() ? i + 1 : i;
        if (b > a) r.groupDelay[i] = -(r.phaseRad[b] - r.phaseRad[a]) / (2.0 * kPi * (r.f[b] - r.f[a]));
    }
    return r;
}

struct ResponseComparison {
    double maxAbsDb = 0, rmsDb = 0, atHz = 0;                 // clean magnitude response error over [fLo, fHi]
    double maxPhaseDeg = 0, phaseAtHz = 0;                    // phase error after the declared fixed delay (reported independently)
    double maxGroupDelayErrUs = 0;
    size_t points = 0;
};
inline ResponseComparison compareResponses(const Response& ref, const Response& test, double fLo = 80.0, double fHi = 8000.0)
{
    ResponseComparison c;
    double s2 = 0;
    for (size_t i = 0; i < ref.f.size() && i < test.f.size(); ++i) {
        if (ref.f[i] < fLo || ref.f[i] > fHi) continue;
        const double d = test.magDb[i] - ref.magDb[i];
        if (std::fabs(d) > c.maxAbsDb) { c.maxAbsDb = std::fabs(d); c.atHz = ref.f[i]; }
        s2 += d * d; ++c.points;
        double dp = (test.phaseRad[i] - ref.phaseRad[i]) * 180.0 / kPi;
        dp = std::remainder(dp, 360.0);
        if (std::fabs(dp) > c.maxPhaseDeg) { c.maxPhaseDeg = std::fabs(dp); c.phaseAtHz = ref.f[i]; }
        c.maxGroupDelayErrUs = std::max(c.maxGroupDelayErrUs, std::fabs(test.groupDelay[i] - ref.groupDelay[i]) * 1e6);
    }
    c.rmsDb = c.points ? std::sqrt(s2 / (double)c.points) : 0.0;
    return c;
}

// ---- Farina separation of the harmonic orders from ONE sweep (S31).  g = deconvolved response; the linear IR sits at index sweepLength-1 + latency.
struct SweepDeconvolution { Vec g; size_t zeroLag = 0; };
inline SweepDeconvolution deconvolveSweep(const Sweep& s, const Vec& y)
{
    const size_t n = s.x.size();
    Vec inv(n);
    const double Lr = s.rate();
    for (size_t i = 0; i < n; ++i) inv[i] = s.x[n - 1 - i] * std::exp(-Lr * (double)i / s.sr);   // -6 dB/oct amplitude envelope
    SweepDeconvolution d;
    d.g = convolve(y, inv);
    // Normalise so that a unit-gain linear system has |FFT(g)| = 1 across the band.  The sweep times its inverse filter has a flat
    // spectrum of constant magnitude C (by construction); C is read as the mean magnitude over the geometric middle of the sweep band.
    // (The peak of the autoconvolution would be wrong: it measures the bandwidth of the pulse, not its spectral gain.)
    Vec ac = convolve(s.x, inv);
    const size_t N = nextPow2(ac.size());
    const auto A = toSpectrum(ac, N);
    const double fa = std::sqrt(s.f1 * s.f2) / 4.0, fb = std::sqrt(s.f1 * s.f2) * 4.0;
    double acc = 0; size_t cnt = 0;
    for (size_t k = 1; k < N / 2; ++k) { const double f = (double)k * s.sr / (double)N; if (f >= fa && f <= fb) { acc += std::abs(A[k]); ++cnt; } }
    const double scale = cnt ? acc / (double)cnt : 1.0;
    for (auto& v : d.g) v /= (scale > 0 ? scale : 1.0);
    d.zeroLag = n - 1;
    return d;
}
// order-k impulse response segment (k = 1 linear).  `latency` = fixed system delay in samples; `maxLen` limits the window.
inline Vec harmonicIR(const Sweep& s, const SweepDeconvolution& d, int k, int latency, size_t maxLen)
{
    const double dt = s.seconds * std::log((double)k) / std::log(s.f2 / s.f1);       // order k precedes the linear IR by this many seconds
    const double dtNext = s.seconds * std::log((double)(k + 1)) / std::log(s.f2 / s.f1);
    const long centre = (long)d.zeroLag + latency - (long)std::llround(dt * s.sr);
    const size_t room = (size_t)std::max(8.0, std::floor((dtNext - dt) * s.sr * 0.98));
    const size_t len = std::min(maxLen, room);
    Vec o(len, 0.0);
    for (size_t i = 0; i < len; ++i) { const long j = centre - (long)(len / 8) + (long)i; if (j >= 0 && j < (long)d.g.size()) o[i] = d.g[(size_t)j]; }
    return o;
}

// ------------------------------------------------------------------------------------------------- coherent tone analysis (harmonics, THD)
struct ToneSpectrum {
    double f0 = 0, fundamental = 0, rms = 0, peak = 0, thd = 0;           // fundamental amplitude (peak), THD as a fraction (H2..Hn)
    std::vector<double> amp;                                               // amp[k-1] = peak amplitude of harmonic k
    std::vector<double> dbc;                                               // dBc re the fundamental, dbc[0] = 0
    size_t window = 0;
};
// analyse the LAST n samples of y at exact multiples of f0.  When n * f0 / sr is a whole number the analysis is leakage free.
inline ToneSpectrum analyseTone(const Vec& y, double sr, double f0, size_t n, int nHarm = 10)
{
    ToneSpectrum t; t.f0 = f0; t.window = std::min(n, y.size());
    const size_t N = t.window, start = y.size() - N;
    t.amp.assign((size_t)nHarm, 0.0); t.dbc.assign((size_t)nHarm, 0.0);
    for (int k = 1; k <= nHarm; ++k) {
        if (f0 * k >= 0.5 * sr) break;
        double c = 0, s = 0;
        const double w = 2.0 * kPi * f0 * k / sr;
        // recursive oscillator would be faster; the windows are short enough for direct evaluation
        double cr = 1.0, sr_ = 0.0; const double cw = std::cos(w), sw = std::sin(w);
        for (size_t i = 0; i < N; ++i) { c += y[start + i] * cr; s += y[start + i] * sr_; const double ncr = cr * cw - sr_ * sw; sr_ = sr_ * cw + cr * sw; cr = ncr; }
        t.amp[(size_t)k - 1] = std::sqrt(c * c + s * s) * 2.0 / (double)N;
    }
    t.fundamental = t.amp[0];
    double hh = 0; for (int k = 2; k <= nHarm; ++k) hh += t.amp[(size_t)k - 1] * t.amp[(size_t)k - 1];
    t.thd = t.fundamental > 0 ? std::sqrt(hh) / t.fundamental : 0.0;
    for (int k = 1; k <= nHarm; ++k) t.dbc[(size_t)k - 1] = db20(t.amp[(size_t)k - 1] / std::max(t.fundamental, 1e-300));
    double e = 0, pk = 0;
    for (size_t i = 0; i < N; ++i) { e += y[start + i] * y[start + i]; pk = std::max(pk, std::fabs(y[start + i])); }
    t.rms = std::sqrt(e / (double)N); t.peak = pk;
    return t;
}

struct HarmonicComparison { double maxAbsDb = 0; int worstOrder = 0; int compared = 0; double fundamentalErrDb = 0; };
// Sec. 26: "Within 1 dB for components above -60 dBc when measurable": only harmonics whose REFERENCE level exceeds floorDbc are compared.
inline HarmonicComparison compareHarmonics(const ToneSpectrum& ref, const ToneSpectrum& test, double floorDbc = -60.0)
{
    HarmonicComparison h;
    h.fundamentalErrDb = db20(test.fundamental / std::max(ref.fundamental, 1e-300));
    for (size_t k = 1; k < ref.dbc.size() && k < test.dbc.size(); ++k) {
        if (ref.dbc[k] < floorDbc) continue;
        const double d = std::fabs(test.dbc[k] - ref.dbc[k]);
        ++h.compared;
        if (d > h.maxAbsDb) { h.maxAbsDb = d; h.worstOrder = (int)k + 1; }
    }
    return h;
}

// ------------------------------------------------------------------------------------------------- clipping onset (Sec. 26: fixed stated definition)
struct LevelPoint { double inPeak = 0, fundOut = 0, thd = 0; };       // one stepped-sine measurement
struct Onset {
    double thd1Input = std::numeric_limits<double>::quiet_NaN();      // definition A: input peak where THD reaches thdTarget
    double comp1dBInput = std::numeric_limits<double>::quiet_NaN();   // definition B: input peak where the fundamental gain is 1 dB below small-signal
    double smallSignalGain = 0;
};
// points must be ordered by rising input level; interpolation is linear in log(level) / log(thd) (A) and in log(level) / dB (B)
inline Onset findOnset(const std::vector<LevelPoint>& p, double thdTarget = 0.01, double compressionDb = 1.0)
{
    Onset o;
    if (p.size() < 2) return o;
    o.smallSignalGain = p.front().fundOut / std::max(p.front().inPeak, 1e-300);
    for (size_t i = 1; i < p.size(); ++i) {
        if (std::isnan(o.thd1Input) && p[i - 1].thd < thdTarget && p[i].thd >= thdTarget && p[i - 1].thd > 0) {
            const double a = (std::log(thdTarget) - std::log(p[i - 1].thd)) / (std::log(p[i].thd) - std::log(p[i - 1].thd));
            o.thd1Input = std::exp(std::log(p[i - 1].inPeak) + a * (std::log(p[i].inPeak) - std::log(p[i - 1].inPeak)));
        }
        const double g0 = db20(p[i - 1].fundOut / p[i - 1].inPeak / o.smallSignalGain), g1 = db20(p[i].fundOut / p[i].inPeak / o.smallSignalGain);
        if (std::isnan(o.comp1dBInput) && g0 > -compressionDb && g1 <= -compressionDb) {
            const double a = (-compressionDb - g0) / (g1 - g0);
            o.comp1dBInput = std::exp(std::log(p[i - 1].inPeak) + a * (std::log(p[i].inPeak) - std::log(p[i - 1].inPeak)));
        }
    }
    return o;
}
inline double onsetErrorDb(double refInput, double testInput) { return db20(testInput / refInput); }

// ------------------------------------------------------------------------------------------------- intermodulation (Sec. 26: "specified stimuli and levels")
struct ImdProduct { int a, b; double hz, dbc; };
struct ImdResult { std::vector<ImdProduct> products; double a1 = 0, a2 = 0; double totalDb = 0; };
// y: steady state; f1, f2 must be exact bins of the n-sample window (coherentFrequency).  Products |a f1 + b f2|, 2 <= |a|+|b| <= 5, positive frequency, away from the tones.
inline ImdResult twoToneIMD(const Vec& y, double sr, double f1, double f2, size_t n)
{
    ImdResult r;
    auto amp = [&](double f) {
        const size_t N = std::min(n, y.size()), start = y.size() - N;
        double c = 0, s = 0; const double w = 2.0 * kPi * f / sr;
        double cr = 1.0, sr_ = 0.0; const double cw = std::cos(w), sw = std::sin(w);
        for (size_t i = 0; i < N; ++i) { c += y[start + i] * cr; s += y[start + i] * sr_; const double ncr = cr * cw - sr_ * sw; sr_ = sr_ * cw + cr * sw; cr = ncr; }
        return std::sqrt(c * c + s * s) * 2.0 / (double)N;
    };
    r.a1 = amp(f1); r.a2 = amp(f2);
    const double ref = std::max(r.a1, r.a2);
    double sum = 0;
    for (int a = -5; a <= 5; ++a) for (int b = -5; b <= 5; ++b) {
        const int ord = std::abs(a) + std::abs(b);
        if (ord < 2 || ord > 5 || (a == 0 && b == 0)) continue;
        const double f = a * f1 + b * f2;
        if (f <= 0.0 || f >= 0.5 * sr) continue;
        if (std::fabs(f - f1) < 1e-6 || std::fabs(f - f2) < 1e-6) continue;
        bool dup = false; for (auto& q : r.products) if (std::fabs(q.hz - f) < 1e-6) dup = true;
        if (dup) continue;
        const double A = amp(f);
        r.products.push_back({a, b, f, db20(A / std::max(ref, 1e-300))});
        sum += A * A;
    }
    r.totalDb = db20(std::sqrt(sum) / std::max(ref, 1e-300));
    return r;
}
// compare product by product: only products whose reference level exceeds floorDbc count (Sec. 26: within 2 dB)
inline double compareImd(const ImdResult& ref, const ImdResult& test, double floorDbc = -60.0)
{
    double worst = 0;
    for (size_t i = 0; i < ref.products.size() && i < test.products.size(); ++i)
        if (ref.products[i].dbc > floorDbc) worst = std::max(worst, std::fabs(test.products[i].dbc - ref.products[i].dbc));
    return worst;
}

// ------------------------------------------------------------------------------------------------- burst envelope and recovery (Sec. 26)
struct Envelope { Vec t, a; double hop = 0; };
// lock-in amplitude at f0 over a sliding window of `cycles` whole cycles (so a stationary tone gives a flat envelope)
inline Envelope lockinEnvelope(const Vec& y, double sr, double f0, int cycles = 4, double hopSeconds = 0.001)
{
    Envelope e; e.hop = hopSeconds;
    const size_t W = (size_t)std::llround((double)cycles * sr / f0);
    const size_t hop = std::max<size_t>(1, (size_t)std::llround(hopSeconds * sr));
    const double w = 2.0 * kPi * f0 / sr;
    // cumulative sums of y cos / y sin make every window O(1)
    std::vector<double> cc(y.size() + 1, 0.0), cs(y.size() + 1, 0.0);
    for (size_t i = 0; i < y.size(); ++i) { cc[i + 1] = cc[i] + y[i] * std::cos(w * (double)i); cs[i + 1] = cs[i] + y[i] * std::sin(w * (double)i); }
    for (size_t s = 0; s + W <= y.size(); s += hop) {
        const double c = cc[s + W] - cc[s], q = cs[s + W] - cs[s];
        e.t.push_back(((double)s + 0.5 * (double)W) / sr);
        e.a.push_back(std::sqrt(c * c + q * q) * 2.0 / (double)W);
    }
    return e;
}
// largest relative deviation |test - ref| / max(ref, floor) over [t0, t1]  (Sec. 26: within 5 % over declared attack/recovery windows)
inline double envelopeDeviation(const Envelope& ref, const Envelope& test, double t0, double t1, double floorFrac = 0.01)
{
    double mx = 0; for (double v : ref.a) mx = std::max(mx, v);
    double worst = 0;
    for (size_t i = 0; i < ref.a.size() && i < test.a.size(); ++i) {
        if (ref.t[i] < t0 || ref.t[i] > t1) continue;
        worst = std::max(worst, std::fabs(test.a[i] - ref.a[i]) / std::max(ref.a[i], floorFrac * mx));
    }
    return worst;
}
struct Recovery {
    double settled = 0;            // mean envelope over the final `settleFrac` of the record
    double minRel = 1.0;           // deepest dip relative to settled (after tAfter)
    double tWithin10 = std::numeric_limits<double>::quiet_NaN(), tWithin5 = std::numeric_limits<double>::quiet_NaN();   // seconds after tAfter (last exit from the band)
    double tau = std::numeric_limits<double>::quiet_NaN();      // exponential time constant fitted to |env - settled| (log-linear least squares); NaN if excursion < 3 % or the record did not settle
    double excursion = 0;          // largest |env - settled| / settled after tAfter
    double settleResidual = 0;     // |mean(previous settle window) - settled| / largest excursion: how far the record is from settled (tau reported only if <= 3 %)
};
inline Recovery recoveryLandmarks(const Envelope& e, double tAfter, double settleFrac = 0.1)
{
    Recovery r;
    if (e.a.empty()) return r;
    const size_t n = e.a.size(), s0 = (size_t)((double)n * (1.0 - settleFrac));
    double acc = 0; for (size_t i = s0; i < n; ++i) acc += e.a[i];
    r.settled = acc / (double)std::max<size_t>(1, n - s0);
    auto lastExit = [&](double tol) {
        double t = std::numeric_limits<double>::quiet_NaN(); bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (e.t[i] < tAfter) continue;
            if (std::fabs(e.a[i] / r.settled - 1.0) > tol) { any = true; t = e.t[i] - tAfter; }
        }
        return any ? t + e.hop : 0.0;
    };
    for (size_t i = 0; i < n; ++i) if (e.t[i] >= tAfter) r.minRel = std::min(r.minRel, e.a[i] / r.settled);
    r.tWithin10 = lastExit(0.10); r.tWithin5 = lastExit(0.05);
    // tau: points whose deviation is between 5 % and 60 % of the largest deviation, regression of ln|dev| on t
    double dmax = 0; for (size_t i = 0; i < n; ++i) if (e.t[i] >= tAfter) dmax = std::max(dmax, std::fabs(e.a[i] - r.settled));
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
    for (size_t i = 0; i < n; ++i) {
        if (e.t[i] < tAfter) continue;
        const double d = std::fabs(e.a[i] - r.settled);
        if (d < 0.05 * dmax || d > 0.6 * dmax) continue;
        const double x = e.t[i] - tAfter, yv = std::log(d);
        sx += x; sy += yv; sxx += x * x; sxy += x * yv; ++m;
    }
    // a time constant is only meaningful when there is a measurable excursion to fit: below 3 % of the settled level the "recovery" is
    // numerical noise and tau is left as NaN (never reported as a number)
    r.excursion = r.settled > 0 ? dmax / r.settled : 0.0;
    if (r.excursion >= 0.03 && m >= 5) { const double den = m * sxx - sx * sx; if (den > 0) { const double slope = (m * sxy - sx * sy) / den; if (slope < 0) r.tau = -1.0 / slope; } }
    // A time constant is only determined if the record actually SETTLED: the mean of the last settle-window must equal the mean of the one before it
    // to within 3 % of the excursion.  (A recovery slower than the record leaves the "settled" level still moving; the fit then reports a
    // spuriously short tau, so the criterion has to look at the envelope itself, not at the fitted value.)
    {
        const size_t w = n - s0, p0 = s0 >= w ? s0 - w : 0;
        double prev = 0; for (size_t i = p0; i < s0; ++i) prev += e.a[i];
        prev /= (double)std::max<size_t>(1, s0 - p0);
        r.settleResidual = dmax > 0 ? std::fabs(prev - r.settled) / dmax : 0.0;
        if (std::isfinite(r.tau) && r.settleResidual > 0.03) r.tau = std::numeric_limits<double>::quiet_NaN();
    }
    return r;
}

} // namespace cd57::metrics
