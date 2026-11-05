// AcousticFactor.h -- measured impulse-response (IR) path: preparation conventions, and the coherent
// "cone-motion" factorisation of a measured terminal-voltage-to-microphone IR (dossier Sec. 18, 19, 27).  JUCE-free.
//
// ------------------------------------------------------------------------------------------------------------------
// The two coherent routes (Sec. 27 "The linear acoustic identity, explicitly")
//
//   G(f) = U_cone(f)/V_s(f)   [(m^3/s)/V]   U_cone = Sd * v  (volume velocity), V_s = speaker-terminal voltage
//   H(f) = P_mic(f)/V_s(f)    measured voltage-to-microphone transfer
//
//   Route 1 (electrical load + IR):  I_s = Y_sp V_s  (the circuit's complex load) ;  P_mic = H V_s .
//   Route 2 (motion):                P_mic = R U_cone ,   R ~ H G* / (|G|^2 + lambda(f)).
//
// In the usable linear band both give the same microphone signal.  Feeding U_cone through the FULL H instead gives
// G*H -- the cone response counted twice, in the wrong units (Sec. 27).  Route 2 lets a measured H coexist with the
// model's own cone motion and voice-coil heating (the nonlinear / thermal behaviour lives in U_cone, not in H).
//
// IR FILE CONVENTION (what the user must provide)
//   * Content: the impulse response h[n] of the system "voltage at the speaker terminals -> microphone signal", sampled at
//     the file's rate, NOT normalised.  Polarity is preserved (a positive terminal-voltage step must give the sign you
//     want in the microphone channel).
//   * Gain: dimensionless, in output-full-scale per terminal-full-scale.  The plugin's terminal output is
//     x = V_s / 28.28 V (times the Output Level control); route 1 is  y = h (*) x .  A unit impulse is therefore an exact
//     0 dB, zero-delay, polarity-preserving "Speaker terminals" path.  To convert a physical measurement: if the
//     microphone channel reads m[n] digital full-scale units when the terminals see a voltage impulse response in
//     volts, then  h_file[n] = m[n] * 28.28  (28.28 V peak = the plugin's 0 dBFS terminal reference).
//   * Arrival time: sample 0 of the file is the instant the voltage is applied to the terminals.  The microphone
//     propagation delay is INCLUDED (do not trim leading silence); the plugin never trims.  The same time origin holds for
//     route 2: the residual is built so G*R reproduces H with no added delay, which requires the IR to carry roughly
//     0.3 ms (>= ~14 samples at 48 kHz) of arrival delay -- true for any microphone >= 10 cm from the cone -- because the
//     regularised inverse is not strictly causal (see FactorResult::nonCausalDb).  The measured value is reported.
//   * Channels: the first channel is used; supply mono.  Length: up to maxSeconds (default 2 s).  A longer file is cut and a
//     10 ms raised-cosine fade is applied to the last 10 ms ONLY in that case (no other windowing is ever applied).
//   * Sample-rate conversion: Kaiser-windowed-sinc resampling (beta 10, 32 zero-crossings each side, anti-alias cutoff at the
//     lower Nyquist) with output sample m taken at exactly t = m/fs_out (no added delay) and the samples scaled by
//     fs_in/fs_out so that the TRANSFER FUNCTION (in-band gain, DC gain) is preserved.  This replaces JUCE's convolution
//     loader resampler (Lagrange, no anti-alias filter).
//   * Raw captures: the plugin stores the original file path (and its rate/length/hash) in its state and re-derives the
//     processed IR from it; the raw file is never modified.
// ------------------------------------------------------------------------------------------------------------------
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "AmpCircuit.h"

namespace cd57 {

using cplx = std::complex<double>;

inline std::size_t nextPow2(std::size_t n) { std::size_t p = 1; while (p < n) p <<= 1; return p; }

// in-place radix-2 FFT (forward: e^{-j...}); inverse is unnormalised-conjugate scaled by 1/N
inline void fftInPlace(std::vector<cplx>& a, bool inverse = false)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * M_PI / (double)len;
        const cplx wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            cplx w(1);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const cplx u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl;
            }
        }
    }
    if (inverse) for (auto& v : a) v /= (double)n;
}

// ---------------------------------------------------------------------------------------------------- speaker G(f)
// Closed-form terminal-voltage -> cone-velocity transfer of the model's linear speaker (dossier Sec. 17 equations with
// F_acoustic = 0, exactly what the netlist's Re, Le, Res||Cmes||Lces branch implements):
//   u = Re i + Le di/dt + Bl v ;   Mms dv/dt + Rms v + x/Cms = Bl i
//   =>   v/u = Bl / [ (Re + j w Le) (Rms + j w Mms + 1/(j w Cms)) + Bl^2 ]            [(m/s)/V]
// Re may be overridden (hot voice coil).  At f <= 0 the cone velocity per volt is 0 (stiffness).
inline cplx speakerVelocityPerVolt(const SpeakerTS& s, double f, double Re = -1.0)
{
    if (f <= 0.0) return cplx(0, 0);
    const double w = 2.0 * M_PI * f, Bl = s.Bl();
    const cplx Ze(Re > 0 ? Re : s.Re, w * s.Le);
    const cplx Zmech(s.Rms(), w * s.Mms - 1.0 / (w * s.Cms()));
    return Bl / (Ze * Zmech + Bl * Bl);
}
// G(f) of Sec. 27: volume velocity per terminal volt [(m^3/s)/V]
inline cplx speakerVolumeVelocityPerVolt(const SpeakerTS& s, double f, double Re = -1.0) { return s.Sd * speakerVelocityPerVolt(s, f, Re); }

// ---------------------------------------------------------------------------------------------------- IR preparation
inline double besselI0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 80; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-18 * s) break; }
    return s;
}

// Sample-rate conversion that preserves the transfer function and the time origin (see the convention above).
inline std::vector<double> resampleKeepTransfer(const std::vector<double>& x, double fsIn, double fsOut)
{
    if (x.empty() || fsIn <= 0 || fsOut <= 0) return {};
    if (fsIn == fsOut) return x;
    const double ratio = fsIn / fsOut;                    // input samples per output sample
    const double g = std::min(1.0, fsOut / fsIn);         // anti-alias cutoff relative to the input Nyquist
    const double halfSpan = 32.0 / g;                     // kernel half-width in input samples
    const double beta = 10.0, i0b = besselI0(beta);
    constexpr int kWinN = 8192;                           // Kaiser window table, linear interpolation (error < 1e-6)
    std::vector<double> winTab((std::size_t)kWinN + 2, 0.0);
    for (int i = 0; i <= kWinN; ++i) { const double r = (double)i / kWinN; winTab[(std::size_t)i] = besselI0(beta * std::sqrt(1.0 - r * r)) / i0b; }
    const std::size_t M = (std::size_t)std::ceil((double)x.size() / ratio);
    std::vector<double> y(M, 0.0);
    for (std::size_t m = 0; m < M; ++m) {
        const double u = (double)m * ratio;
        const long k0 = std::max(0L, (long)std::ceil(u - halfSpan)), k1 = std::min((long)x.size() - 1, (long)std::floor(u + halfSpan));
        double acc = 0.0;
        for (long k = k0; k <= k1; ++k) {
            const double d = u - (double)k, a = M_PI * g * d;
            const double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
            const double r = std::fabs(d) / halfSpan;
            double win = 0.0;
            if (r < 1.0) { const double t = r * kWinN; const std::size_t i = (std::size_t)t; win = winTab[i] + (t - (double)i) * (winTab[i + 1] - winTab[i]); }
            acc += x[(std::size_t)k] * g * sinc * win;
        }
        y[m] = ratio * acc;
    }
    return y;
}

struct PreparedIr {
    std::vector<double> h;        // at the target rate; polarity, gain and arrival time preserved
    double fsFile = 0, fsTarget = 0;
    std::size_t fileSamples = 0;  // length of the raw capture
    bool cutAtMaxLength = false;  // longer than maxSeconds: cut + 10 ms fade (the only windowing ever applied)
    double peakAbs = 0.0;
    double onsetMs = 0.0;         // first sample with |h| >= 1 % of the peak (-40 dB), for information only
    double dcGain = 0.0;          // sum of the processed samples
};

inline PreparedIr prepareImpulseResponse(const std::vector<double>& raw, double fsFile, double fsTarget, double maxSeconds = 2.0)
{
    PreparedIr p;
    p.fsFile = fsFile; p.fsTarget = fsTarget; p.fileSamples = raw.size();
    p.h = resampleKeepTransfer(raw, fsFile, fsTarget);
    const std::size_t maxLen = (std::size_t)std::max(16.0, maxSeconds * fsTarget);
    if (p.h.size() > maxLen) {
        p.h.resize(maxLen);
        const std::size_t fade = std::min(maxLen / 2, (std::size_t)(0.010 * fsTarget));
        for (std::size_t i = 0; i < fade; ++i) p.h[maxLen - fade + i] *= 0.5 * (1.0 + std::cos(M_PI * (double)(i + 1) / (double)(fade + 1)));
        p.cutAtMaxLength = true;
    }
    for (double v : p.h) { p.peakAbs = std::max(p.peakAbs, std::fabs(v)); p.dcGain += v; }
    for (std::size_t i = 0; i < p.h.size(); ++i) if (std::fabs(p.h[i]) >= 0.01 * p.peakAbs && p.peakAbs > 0) { p.onsetMs = 1000.0 * (double)i / fsTarget; break; }
    return p;
}

// 64-bit FNV-1a over the raw float32 bit patterns: identifies "the same capture" in saved state
inline std::uint64_t hashSamples(const std::vector<float>& s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (float v : s) { std::uint32_t b; static_assert(sizeof b == sizeof v, ""); std::memcpy(&b, &v, sizeof b); for (int k = 0; k < 4; ++k) { h ^= (b >> (8 * k)) & 0xffu; h *= 1099511628211ull; } }
    return h;
}

// ---------------------------------------------------------------------------------------------------- factorisation
struct FactorSettings {
    double sampleRate = 48000.0;
    // identifiable band: where the measurement is trusted and where G is used as the reference level.
    // PROVISIONAL defaults: dossier Sec. 26 states the clean-response band 80 Hz-8 kHz; the low edge is widened to 50 Hz.
    double bandLoHz = 50.0, bandHiHz = 8000.0;
    // lambda floor, PROPOSED by this implementation: sqrt(lambda0) = 10^(floorDb/20) * max_band|G|.
    // -60 dB keeps the Tikhonov weighting |G|^2/(|G|^2+lambda) within 0.5 dB up to ~7.7 kHz for the stock speaker model (whose |G|
    // is 52 dB below its peak at 8 kHz) and bounds the residual gain to 1/(2*10^(-3)) = +54 dB re 1/max|G| (FactorResult::gainBoundDb).
    // A higher floor trades usable bandwidth for a smaller worst-case gain and a shorter non-causal pre-ringing.
    double floorDb = -60.0;
    // lambda grows above bandHiHz by this many dB (power) per octave: R rolls off beyond the identifiable band, no brick wall.
    double outOfBandDbPerOct = 12.0;
    double toleranceDb = 0.5;     // Sec. 26 clean-magnitude target, used to report the regularisation-limited band
    std::size_t firLength = 0;    // 0 = same as the IR length
};

struct FactorResult {
    std::vector<double> residual;     // causal FIR r[n]: P_mic = r (*) (U_cone / gRef), in the units of h (U_cone/gRef is "volts equivalent")
    double gRef = 0.0;                // max over the band of |G|  [(m^3/s)/V]
    double gainBoundDb = 0.0;         // guaranteed:  |R(f)| * gRef / |H(f)| <= 1/(2*10^(floorDb/20)) = this many dB (frequency-independent bound)
    // regularisation-limited band: contiguous around the |G| peak where the Tikhonov weighting |G|^2/(|G|^2+lambda) is within toleranceDb
    double regLimitLoHz = 0.0, regLimitHiHz = 0.0;
    // REALISED usable band: contiguous around the |G| peak where the actual FIR still satisfies |G R - H| <= eps |H|_smooth,
    // eps = |10^(toleranceDb/20) - 1| (i.e. the complex error of a toleranceDb magnitude error), |H|_smooth = 1/3-octave power mean.
    // Includes the error from the dropped non-causal part and from the FIR length, which the analytic limit above does not.
    double usableLoHz = 0.0, usableHiHz = 0.0;
    double nonCausalDb = -300.0;      // energy of R at negative time (dropped) relative to total, dB
    double truncationDb = -300.0;     // energy of R beyond the FIR length (dropped) relative to total, dB
    std::size_t fftSize = 0;
};

// Tikhonov-regularised factorisation  R = H G* / (|G|^2 + lambda(f)),  H = FFT of the IR, G = callable returning the model's
// U_cone/V_s at frequency f (Hz).  Computed in units of gRef (G_n = G/gRef): R_n = H G_n* / (|G_n|^2 + lambda_n).
// Cost: two FFTs of nextPow2(max(2*len, 0.5 s)) points -- prepare OFF the audio thread.
template <class GFunc>
FactorResult factorResidual(const std::vector<double>& h, GFunc&& G, const FactorSettings& s)
{
    FactorResult out;
    const std::size_t L = h.size();
    if (L == 0) return out;
    const std::size_t N = nextPow2(std::max<std::size_t>(2 * L, (std::size_t)(0.5 * s.sampleRate)));
    out.fftSize = N;
    const double df = s.sampleRate / (double)N, hi = std::min(s.bandHiHz, 0.45 * s.sampleRate);

    std::vector<cplx> Gn(N / 2 + 1);
    for (std::size_t k = 0; k <= N / 2; ++k) Gn[k] = G((double)k * df);
    for (std::size_t k = 0; k <= N / 2; ++k) { const double f = (double)k * df; if (f >= s.bandLoHz && f <= hi) out.gRef = std::max(out.gRef, std::abs(Gn[k])); }
    if (!(out.gRef > 0.0)) return out;
    for (auto& g : Gn) g /= out.gRef;

    const double lam0 = std::pow(10.0, s.floorDb / 10.0);
    out.gainBoundDb = -s.floorDb - 6.0206;
    auto lambda = [&](double f) { return f > hi ? lam0 * std::pow(10.0, s.outOfBandDbPerOct / 10.0 * std::log2(f / hi)) : lam0; };

    std::vector<cplx> A(N, cplx(0, 0));
    for (std::size_t i = 0; i < L; ++i) A[i] = h[i];
    fftInPlace(A);
    const std::vector<cplx> Hs(A.begin(), A.begin() + (std::ptrdiff_t)(N / 2 + 1));
    for (std::size_t k = 0; k <= N / 2; ++k) {
        const double f = (double)k * df;
        cplx r = A[k] * std::conj(Gn[k]) / (std::norm(Gn[k]) + lambda(f));
        if (k == 0 || k == N / 2) r = cplx(r.real(), 0.0);
        A[k] = r;
        if (k > 0 && k < N / 2) A[N - k] = std::conj(r);
    }
    std::size_t kp = 1;                                  // bin of the |G| peak inside the identifiable band
    {
        double best = 0;
        for (std::size_t k = 1; k <= N / 2; ++k) { const double f = (double)k * df; if (f >= s.bandLoHz && f <= hi && std::abs(Gn[k]) > best) { best = std::abs(Gn[k]); kp = k; } }
        const double tol = std::pow(10.0, -s.toleranceDb / 20.0);
        auto W = [&](std::size_t k) { const double g2 = std::norm(Gn[k]); return g2 / (g2 + lambda((double)k * df)); };
        std::size_t klo = kp, khi = kp;
        while (klo > 1 && W(klo - 1) >= tol) --klo;
        while (khi < N / 2 && W(khi + 1) >= tol) ++khi;
        out.regLimitLoHz = (double)klo * df; out.regLimitHiHz = (double)khi * df;
    }
    fftInPlace(A, true);
    double eTot = 0, eNeg = 0, eTail = 0;
    const std::size_t Lr = s.firLength ? s.firLength : L;
    for (std::size_t n = 0; n < N; ++n) {
        const double e = A[n].real() * A[n].real();
        eTot += e;
        if (n >= N / 2) eNeg += e; else if (n >= Lr) eTail += e;
    }
    if (eTot > 0) {
        out.nonCausalDb = 10.0 * std::log10(std::max(eNeg / eTot, 1e-30));
        out.truncationDb = 10.0 * std::log10(std::max(eTail / eTot, 1e-30));
    }
    out.residual.resize(Lr);
    for (std::size_t n = 0; n < Lr && n < N / 2; ++n) out.residual[n] = A[n].real();

    // realised usable band from the FIR actually delivered
    {
        std::vector<cplx> B(N, cplx(0, 0));
        for (std::size_t n = 0; n < Lr && n < N / 2; ++n) B[n] = out.residual[n];
        fftInPlace(B);
        std::vector<double> cum(N / 2 + 2, 0.0);          // prefix sums of |H|^2 for the 1/3-octave power mean
        for (std::size_t k = 0; k <= N / 2; ++k) cum[k + 1] = cum[k] + std::norm(Hs[k]);
        const double eps = std::fabs(std::pow(10.0, s.toleranceDb / 20.0) - 1.0);
        auto ok = [&](std::size_t k) {
            const std::size_t hw = std::max<std::size_t>(2, (std::size_t)std::lround(0.12 * (double)k));
            const std::size_t a = k > hw ? k - hw : 0, b = std::min(N / 2, k + hw);
            const double smooth = (cum[b + 1] - cum[a]) / (double)(b - a + 1);
            return std::norm(Gn[k] * B[k] - Hs[k]) <= eps * eps * smooth;
        };
        std::size_t klo = kp, khi = kp;
        while (klo > 1 && ok(klo - 1)) --klo;
        while (khi < N / 2 && ok(khi + 1)) ++khi;
        out.usableLoHz = (double)klo * df; out.usableHiHz = (double)khi * df;
    }
    return out;
}

} // namespace cd57
