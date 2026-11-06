// validate_metrics -- self-test of src/analysis/Metrics.h against ANALYTIC answers (dossier Sec. 26 metric definitions).
//
// Every test builds a signal whose answer is known in closed form (or from an independent brute-force evaluation) and checks the metric
// against it with a tolerance fixed in advance, so a defect in a metric makes a test FAIL instead of silently shifting every comparison.
// No amplifier model is involved: this verifies the ruler, not the object measured.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Metrics.h"

using namespace cd57::metrics;

static int nPass = 0, nFail = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "")
{
    (ok ? nPass : nFail)++;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "   [", detail.empty() ? "" : (detail + "]").c_str());
}
static std::string f(double v, int p = 4) { char b[64]; std::snprintf(b, sizeof b, "%.*f", p, v); return b; }

// deterministic white noise in [-1, 1]
static Vec noise(size_t n, uint32_t seed)
{
    Vec x(n); uint32_t s = seed;
    for (auto& v : x) { s = s * 1664525u + 1013904223u; v = (double)(int32_t)s / 2147483648.0; }
    return x;
}

// RBJ low-pass biquad
struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    Biquad(double fc, double Q, double sr)
    {
        const double w = 2 * kPi * fc / sr, al = std::sin(w) / (2 * Q), c = std::cos(w), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    double tick(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    cplx H(double f, double sr) const
    {
        const cplx z1i = std::polar(1.0, -2 * kPi * f / sr), z2i = z1i * z1i;
        return (b0 + b1 * z1i + b2 * z2i) / (1.0 + a1 * z1i + a2 * z2i);
    }
};

int main()
{
    const double sr = 48000.0;

    std::printf("\n== 1. waveform metrics (NRMSE / ESR)\n");
    {
        Vec y(48000), yh(48000);
        for (size_t i = 0; i < y.size(); ++i) y[i] = 0.7 * std::sin(2 * kPi * 440.0 * (double)i / sr);
        Vec nz = noise(y.size(), 7); const double nr = rms(nz), yr = rms(y);
        for (size_t i = 0; i < y.size(); ++i) yh[i] = y[i] + 0.05 * yr / nr * nz[i];
        const auto w = waveformError(y, yh);
        check(std::fabs(w.nrmse - 0.05) < 1e-9, "NRMSE of a 5 % rms error is 0.05", "nrmse " + f(w.nrmse, 8));
        check(std::fabs(w.esrDb - (-26.0206)) < 1e-3, "NRMSE 0.05 <-> ESR -26.02 dB (dossier Sec. 26)", "esr " + f(w.esrDb) + " dB");
        const auto z = waveformError(y, y);
        check(z.nrmse == 0.0 && z.absRmsError == 0.0, "identical signals give exactly zero error");
        // DC handling is a declared option, not an accident
        Vec yd = yh; for (auto& v : yd) v += 0.1;
        const auto keep = waveformError(y, yd), rem = [&] { WaveformOptions o; o.removeDc = true; return waveformError(y, yd, o); }();
        check(keep.nrmse > rem.nrmse * 1.5 && std::fabs(rem.nrmse - 0.05) < 1e-3, "a DC offset counts as error unless DC removal is declared",
              "kept " + f(keep.nrmse) + ", removed " + f(rem.nrmse));
        // near-silence is flagged, never reported as a meaningless ratio
        Vec quiet(1000, 1e-12), q2(1000, 2e-12);
        check(waveformError(quiet, q2).illConditioned, "a near-silent reference is flagged ill-conditioned (Sec. 26: use an absolute criterion)");
        // band limiting is applied identically to both signals: an error outside the band is ignored
        Vec ye = y; Vec hf(y.size()); for (size_t i = 0; i < y.size(); ++i) hf[i] = 0.2 * std::sin(2 * kPi * 15000.0 * (double)i / sr);
        for (size_t i = 0; i < y.size(); ++i) ye[i] += hf[i];
        WaveformOptions bl; bl.bandLo = 80; bl.bandHi = 8000; bl.sampleRate = sr;
        check(waveformError(y, ye, bl).nrmse < 1e-3 && waveformError(y, ye).nrmse > 0.2, "out-of-band error is removed by the declared band limit and counted without it",
              "in-band " + f(waveformError(y, ye, bl).nrmse, 6) + ", full " + f(waveformError(y, ye).nrmse));
    }

    std::printf("\n== 2. fixed-delay estimation (one integer delay per configuration, never per clip)\n");
    {
        Vec x = noise(20000, 11), t(x.size(), 0.0);
        const int d = 37;
        for (size_t i = 0; i + d < x.size(); ++i) t[i + d] = x[i];
        Vec nz = noise(t.size(), 5); for (size_t i = 0; i < t.size(); ++i) t[i] += 0.01 * nz[i];
        const auto e = estimateFixedDelay(x, t, 200);
        check(e.samples == d, "a 37-sample delay is recovered exactly", "estimate " + std::to_string(e.samples) + ", peak corr " + f(e.peakCorrelation, 4));
        // removeDelay zero-fills the last d samples, so the analysis window is declared explicitly (Sec. 26: report alignment and windowing)
        WaveformOptions wo; wo.count = x.size() - (size_t)e.samples;
        const auto w = waveformError(x, removeDelay(t, e.samples), wo);
        check(std::fabs(w.nrmse - 0.01) < 0.0015, "after removing the declared delay the waveforms agree to the added noise (1 % rms)", "nrmse " + f(w.nrmse, 5));
        Vec t2(x.size(), 0.0); for (size_t i = 0; i + 5 < x.size(); ++i) t2[i] = x[i + 5];      // test LEADS by 5
        check(estimateFixedDelay(x, t2, 50).samples == -5, "a leading test signal gives a negative delay");
    }

    std::printf("\n== 3. transfer function, phase and group delay from a log sweep\n");
    {
        const Sweep sw = makeLogSweep(20.0, 20000.0, 4.0, sr, 0.5);
        Biquad bq(3000.0, 0.8, sr);
        Vec y(sw.x.size() + 4096, 0.0);
        for (size_t i = 0; i < y.size(); ++i) y[i] = bq.tick(i < sw.x.size() ? sw.x[i] : 0.0);
        Vec xp = sw.x; xp.resize(y.size(), 0.0);
        const Response r = transferFunction(xp, y, sr, 80.0, 8000.0);
        double wm = 0, wp = 0, wg = 0;
        Biquad ref(3000.0, 0.8, sr);
        for (size_t i = 0; i < r.f.size(); ++i) {
            const cplx h = ref.H(r.f[i], sr);
            wm = std::max(wm, std::fabs(r.magDb[i] - db20(std::abs(h))));
            wp = std::max(wp, std::fabs(std::remainder(r.phaseRad[i] - std::arg(h), 2 * kPi)) * 180.0 / kPi);
        }
        // analytic group delay by centred difference of the analytic unwrapped phase
        for (size_t i = 1; i + 1 < r.f.size(); ++i) {
            const double f0 = r.f[i - 1], f1 = r.f[i + 1];
            double p0 = std::arg(ref.H(f0, sr)), p1 = std::arg(ref.H(f1, sr)); p1 = p0 + std::remainder(p1 - p0, 2 * kPi);
            const double gd = -(p1 - p0) / (2 * kPi * (f1 - f0));
            wg = std::max(wg, std::fabs(r.groupDelay[i] - gd) * 1e6);
        }
        check(wm < 0.05, "measured magnitude matches the analytic biquad within 0.05 dB over 80 Hz-8 kHz", "worst " + f(wm, 4) + " dB");
        check(wp < 1.0, "measured phase matches the analytic phase within 1 degree", "worst " + f(wp, 3) + " deg");
        check(wg < 15.0, "measured group delay matches the analytic value within 15 us", "worst " + f(wg, 2) + " us");
        // fixed-delay removal: a pure 25-sample delay is invisible after the declared removal, and is a pure phase error without it
        Vec yd(y.size(), 0.0); for (size_t i = 25; i < y.size(); ++i) yd[i] = y[i - 25];
        const Response rr = transferFunction(xp, yd, sr, 80.0, 8000.0, 25);
        const auto c = compareResponses(r, rr);
        check(c.maxAbsDb < 0.01 && c.maxPhaseDeg < 0.5, "a declared 25-sample fixed delay is removed from phase and group delay", "mag " + f(c.maxAbsDb, 4) + " dB, phase " + f(c.maxPhaseDeg, 3) + " deg");
        const Response rn = transferFunction(xp, yd, sr, 80.0, 8000.0, 0);
        const auto c2 = compareResponses(r, rn);
        check(c2.maxPhaseDeg > 20.0 && c2.maxAbsDb < 0.01, "without removal the same delay shows as a phase/group-delay error and no magnitude error", "phase " + f(c2.maxPhaseDeg, 1) + " deg, gd " + f(c2.maxGroupDelayErrUs, 0) + " us");
        // a +0.5 dB gain error is reported as 0.5 dB
        Vec yg = y; for (auto& v : yg) v *= std::pow(10.0, 0.5 / 20.0);
        const auto c3 = compareResponses(r, transferFunction(xp, yg, sr, 80.0, 8000.0));
        check(std::fabs(c3.maxAbsDb - 0.5) < 0.01 && c3.maxPhaseDeg < 0.1, "a 0.5 dB gain error reads 0.5 dB (the Sec. 26 clean-response limit)", "reads " + f(c3.maxAbsDb, 4) + " dB");
    }

    std::printf("\n== 4. harmonic levels (coherent lock-in)\n");
    {
        const size_t n = 48000;                              // 1 s window
        const double f0 = coherentFrequency(sr, n, 1000.0);
        auto synth = [&](double d2, double d3, double d5, double ph) {
            Vec y(n);
            for (size_t i = 0; i < n; ++i) {
                const double w = 2 * kPi * f0 * (double)i / sr;
                y[i] = std::sin(w) + std::pow(10.0, d2 / 20) * std::sin(2 * w + ph) + std::pow(10.0, d3 / 20) * std::sin(3 * w - 0.4) + std::pow(10.0, d5 / 20) * std::sin(5 * w);
            }
            return y;
        };
        const auto a = analyseTone(synth(-40, -50, -65, 0.3), sr, f0, n);
        check(std::fabs(a.dbc[1] + 40) < 1e-6 && std::fabs(a.dbc[2] + 50) < 1e-6 && std::fabs(a.dbc[4] + 65) < 1e-6, "H2/H3/H5 levels recovered to 1 micro-dB",
              f(a.dbc[1], 6) + " / " + f(a.dbc[2], 6) + " / " + f(a.dbc[4], 6));
        const double thdExp = std::sqrt(std::pow(10.0, -4.0) + std::pow(10.0, -5.0) + std::pow(10.0, -6.5));
        check(std::fabs(a.thd - thdExp) < 1e-7, "THD equals the rss of the harmonics", "thd " + f(a.thd, 7) + " vs " + f(thdExp, 7));
        const auto b = analyseTone(synth(-40, -50 + 0.7, -65 + 3.0, 1.1), sr, f0, n);
        const auto cmp = compareHarmonics(a, b);
        check(cmp.compared == 2 && std::fabs(cmp.maxAbsDb - 0.7) < 1e-4 && cmp.worstOrder == 3,
              "only harmonics above -60 dBc are compared (H5 at -65 dBc is excluded), worst = H3 at 0.7 dB",
              "compared " + std::to_string(cmp.compared) + ", worst H" + std::to_string(cmp.worstOrder) + " " + f(cmp.maxAbsDb, 4) + " dB");
    }

    std::printf("\n== 5. clipping onset (two fixed definitions)\n");
    {
        // y = tanh(g x): THD and fundamental from an independent 2^16-point Fourier integral
        auto truth = [](double A, double& fund, double& thd) {
            const int N = 1 << 16; double c1 = 0, hh = 0;
            std::vector<double> c(10, 0.0);
            for (int k = 1; k <= 9; k += 2) { double s = 0; for (int i = 0; i < N; ++i) { const double th = 2 * kPi * (i + 0.5) / N; s += std::tanh(A * std::sin(th)) * std::sin(k * th); } c[(size_t)k] = 2.0 * s / N; }
            c1 = c[1]; for (int k = 3; k <= 9; k += 2) hh += c[(size_t)k] * c[(size_t)k];
            fund = c1; thd = std::sqrt(hh) / c1;
        };
        std::vector<LevelPoint> pts;
        for (double dbv = -40; dbv <= 20; dbv += 0.5) {
            const double A = std::pow(10.0, dbv / 20.0); double fund, thd; truth(A, fund, thd);
            // the metric under test sees only these two numbers per level, as it would from a measurement
            pts.push_back({A, fund, thd});
        }
        const Onset o = findOnset(pts, 0.01, 1.0);
        // bisection on the independent truth
        auto solve = [&](auto pred) { double lo = 1e-3, hi = 10; for (int i = 0; i < 80; ++i) { const double m = std::sqrt(lo * hi); (pred(m) ? hi : lo) = m; } return std::sqrt(lo * hi); };
        const double a1 = solve([&](double A) { double fu, th; truth(A, fu, th); return th >= 0.01; });
        const double a2 = solve([&](double A) { double fu, th; truth(A, fu, th); return db20(fu / A) <= -1.0; });
        check(std::fabs(onsetErrorDb(a1, o.thd1Input)) < 0.02, "onset definition A (THD = 1 %) agrees with the brute-force answer to 0.02 dB", "metric " + f(o.thd1Input, 5) + " vs " + f(a1, 5) + " (" + f(onsetErrorDb(a1, o.thd1Input), 4) + " dB)");
        check(std::fabs(onsetErrorDb(a2, o.comp1dBInput)) < 0.05, "onset definition B (1 dB gain compression) agrees to 0.05 dB", "metric " + f(o.comp1dBInput, 5) + " vs " + f(a2, 5) + " (" + f(onsetErrorDb(a2, o.comp1dBInput), 4) + " dB)");
        check(std::fabs(onsetErrorDb(1.0, std::pow(10.0, 0.5 / 20.0)) - 0.5) < 1e-9 && std::fabs(onsetErrorDb(2.0, 1.0) + 6.0206) < 1e-3,
              "onset error is a level ratio in dB (x 1.05925 = +0.5 dB; half the level = -6.02 dB)");
    }

    std::printf("\n== 6. intermodulation products\n");
    {
        const size_t n = 96000;
        const double f1 = coherentFrequency(sr, n, 700.0), f2 = coherentFrequency(sr, n, 1000.0);
        const double a = 0.01, A = 0.5;
        Vec y(n);
        for (size_t i = 0; i < n; ++i) { const double t = (double)i / sr, x = A * (std::cos(2 * kPi * f1 * t) + std::cos(2 * kPi * f2 * t)); y[i] = x + a * x * x * x; }
        const auto r = twoToneIMD(y, sr, f1, f2, n);
        const double fund = A + 2.25 * a * A * A * A, im3 = 0.75 * a * A * A * A;
        double got = 1e9, got2 = 1e9, got3 = 1e9;
        for (auto& p : r.products) {
            if (std::fabs(p.hz - (2 * f1 - f2)) < 1e-6) got = p.dbc;
            if (std::fabs(p.hz - (2 * f1 + f2)) < 1e-6) got2 = p.dbc;
            if (std::fabs(p.hz - 3 * f1) < 1e-6) got3 = p.dbc;
        }
        check(std::fabs(got - db20(im3 / fund)) < 1e-4, "2f1 - f2 level equals (3/4) a A^3 re the fundamental", f(got, 5) + " vs " + f(db20(im3 / fund), 5) + " dBc");
        check(std::fabs(got2 - db20(im3 / fund)) < 1e-4, "2f1 + f2 level equals (3/4) a A^3 as well", f(got2, 5) + " dBc");
        check(std::fabs(got3 - db20(0.25 * a * A * A * A / fund)) < 1e-4, "3 f1 (harmonic) level equals (1/4) a A^3", f(got3, 5) + " dBc");
        const auto r2 = twoToneIMD([&] { Vec z(n); for (size_t i = 0; i < n; ++i) { const double t = (double)i / sr, x = A * (std::cos(2 * kPi * f1 * t) + std::cos(2 * kPi * f2 * t)); z[i] = x + 1.12 * a * x * x * x; } return z; }(), sr, f1, f2, n);
        check(compareImd(r, r2) > 0.9 && compareImd(r, r2) < 1.2, "a 12 % change of the cubic coefficient reads about 1 dB of IMD error", f(compareImd(r, r2), 3) + " dB");
    }

    std::printf("\n== 7. burst envelope and recovery landmarks\n");
    {
        const double f0 = 1000.0, tau = 0.040;
        Vec y((size_t)(0.8 * sr));
        for (size_t i = 0; i < y.size(); ++i) { const double t = (double)i / sr; y[i] = (1.0 - 0.8 * std::exp(-t / tau)) * std::sin(2 * kPi * f0 * t); }
        const Envelope e = lockinEnvelope(y, sr, f0, 4, 0.001);
        const Recovery r = recoveryLandmarks(e, 0.0);
        check(std::fabs(r.tau - tau) / tau < 0.06, "fitted recovery time constant equals the synthetic 40 ms within 6 %", f(r.tau * 1e3, 2) + " ms");
        check(std::fabs(r.tWithin10 - tau * std::log(8.0)) < 0.008, "time to stay within 10 % equals tau ln 8 (83.2 ms) within 8 ms", f(r.tWithin10 * 1e3, 1) + " ms");
        check(std::fabs(r.tWithin5 - tau * std::log(16.0)) < 0.008, "time to stay within 5 % equals tau ln 16 (110.9 ms) within 8 ms", f(r.tWithin5 * 1e3, 1) + " ms");
        check(r.minRel > 0.19 && r.minRel < 0.30, "deepest dip is about 0.2 of the settled amplitude", f(r.minRel, 3));
        // no excursion => no time constant: a flat envelope (numerical noise only) must not produce a tau
        Vec flat(y.size()); for (size_t i = 0; i < flat.size(); ++i) { const double t = (double)i / sr; flat[i] = (1.0 - 0.004 * std::exp(-t / 0.02)) * std::sin(2 * kPi * f0 * t); }
        const Recovery rf = recoveryLandmarks(lockinEnvelope(flat, sr, f0, 4, 0.001), 0.006);
        check(std::isnan(rf.tau) && rf.tWithin10 == 0.0 && rf.excursion < 0.03, "a flat envelope (0.4 % excursion) reports no time constant and no recovery time", "tau " + f(rf.tau, 3) + ", t10 " + f(rf.tWithin10, 4) + ", excursion " + f(rf.excursion, 4));
        // a recovery slower than the record can resolve (tau 2 s in a 0.8 s record) must not be reported as a number
        Vec slow(y.size()); for (size_t i = 0; i < slow.size(); ++i) { const double t = (double)i / sr; slow[i] = (1.0 - 0.3 * std::exp(-t / 2.0)) * std::sin(2 * kPi * f0 * t); }
        const Recovery rs = recoveryLandmarks(lockinEnvelope(slow, sr, f0, 4, 0.001), 0.006);
        check(std::isnan(rs.tau), "a recovery slower than the record can resolve (tau 2 s in 0.8 s) reports no time constant", "tau " + f(rs.tau, 3));
        // envelope deviation: a 4 % amplitude error reads 4 %, a 7 % error is above the 5 % criterion
        Vec y4 = y, y7 = y; for (auto& v : y4) v *= 1.04; for (auto& v : y7) v *= 1.07;
        const double d4 = envelopeDeviation(e, lockinEnvelope(y4, sr, f0, 4, 0.001), 0.0, 0.8), d7 = envelopeDeviation(e, lockinEnvelope(y7, sr, f0, 4, 0.001), 0.0, 0.8);
        check(std::fabs(d4 - 0.04) < 0.002 && std::fabs(d7 - 0.07) < 0.002 && d4 < 0.05 && d7 > 0.05, "envelope deviation reads 4 % and 7 % correctly (5 % criterion discriminates)", f(d4, 4) + ", " + f(d7, 4));
    }

    std::printf("\n== 8. Farina separation of harmonic orders from one sweep\n");
    {
        const Sweep sw = makeLogSweep(20.0, 20000.0, 6.0, sr, 0.5);
        const double a = 0.2 / (0.5 * 0.5);          // a A^2 = 0.2 at the sweep amplitude
        const int latency = 100;
        Vec y(sw.x.size() + 8192, 0.0);
        for (size_t i = 0; i < sw.x.size(); ++i) { const double x = sw.x[i]; y[i + latency] = x + a * x * x * x; }
        const SweepDeconvolution d = deconvolveSweep(sw, y);
        const Vec h1 = harmonicIR(sw, d, 1, latency, 16384), h3 = harmonicIR(sw, d, 3, latency, 16384);
        const size_t N = nextPow2(16384);
        auto S1 = toSpectrum(h1, N), S3 = toSpectrum(h3, N);
        double worst1 = 0, worst3 = 0;
        for (double f0 : {300.0, 500.0, 1000.0, 2000.0}) {
            const size_t k1 = (size_t)std::llround(f0 * (double)N / sr), k3 = (size_t)std::llround(3 * f0 * (double)N / sr);
            const double lin = std::abs(S1[k1]), h3m = std::abs(S3[k3]);
            const double linExp = 1.0 + 0.75 * 0.2, h3Exp = 0.25 * 0.2;      // amplitude relative to A
            worst1 = std::max(worst1, std::fabs(db20(lin / linExp)));
            worst3 = std::max(worst3, std::fabs(db20(h3m / lin) - db20(h3Exp / linExp)));
        }
        check(worst1 < 0.3, "linear IR of a cubic nonlinearity has gain 1 + (3/4) a A^2", "worst " + f(worst1, 3) + " dB");
        check(worst3 < 0.3, "third-harmonic IR / linear IR equals (a A^2 / 4) / (1 + 3 a A^2 / 4) = -27.2 dB", "worst " + f(worst3, 3) + " dB");
    }

    std::printf("\n%d passed, %d failed\n", nPass, nFail);
    return nFail ? 1 : 0;
}
