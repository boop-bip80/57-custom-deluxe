// validate_acoustic -- tests of the acoustic boundary and the measured-IR path (dossier Sec. 17-19, 27 experiments 4 and 7).
//
//   validate_acoustic            runs everything, exit code 0 = all checks passed
//
// WHAT THIS DOES AND DOES NOT ESTABLISH.  No measured voltage-to-microphone transfer H exists in the public record.  Every
// H used below is SYNTHETIC (built here from a known G and a known R_true).  These tests therefore validate the algebra and
// its implementation (factorisation, regularisation, units, polarity, gain, arrival time, the wiring through the circuit's
// real speaker port).  They say NOTHING about the acoustics of the real cabinet, speaker or microphone: that needs the
// measurements listed in the dossier (Sec. 25, 28).  Result vocabulary: PASS / FAIL / INFO (a reported number).
#include <cmath>
#include <complex>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "Acoustic.h"
#include "AcousticFactor.h"
#include "CD57Engine.h"
#include "EmbeddedData.h"

using namespace cd57;

static int nPass = 0, nFail = 0;
static void section(const std::string& t) { std::cout << "\n== " << t << "\n"; }
static void check(bool ok, const std::string& what, const std::string& detail = "")
{
    (ok ? nPass : nFail)++;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << (detail.empty() ? "" : "   [" + detail + "]") << "\n";
}
static void info(const std::string& what) { std::cout << "  INFO  " << what << "\n"; }
static std::string f(double v, int p = 3) { std::ostringstream o; o << std::fixed << std::setprecision(p) << v; return o.str(); }
static double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-300)); }

// complex amplitude of the component of y at frequency f0 (lock-in over a whole number of cycles at the end of y);
// fs/f0 must be an integer number of samples so the window holds exactly `cycles` periods of f0
static cplx lockIn(const std::vector<double>& y, double fs, double f0, int cycles)
{
    const int per = (int)std::round(fs / f0);
    const size_t n = std::min<size_t>(y.size(), (size_t)per * (size_t)cycles);
    cplx a(0, 0);
    for (size_t i = y.size() - n; i < y.size(); ++i) a += y[i] * std::polar(1.0, -2.0 * M_PI * f0 * (double)i / fs);
    return a * (2.0 / (double)n);
}

// ---------------------------------------------------------------------------------------------------- A. provisional operator
static AcousticOperator makeOp(double fs, const AcousticParams& p, double vPerV1k)
{
    AcousticOperator op; op.prepare(fs); op.set(p); op.calibrate1k(vPerV1k); return op;
}

static double pistonDirectivity(double x)    // 2 J1(x)/x via the power series of J1
{
    double j1 = 0, term = x / 2.0;
    for (int k = 0; k < 40; ++k) { j1 += term; term *= -(x * x / 4.0) / ((k + 1.0) * (k + 2.0)); }
    return x < 1e-9 ? 1.0 : 2.0 * j1 / x;
}

static void testProvisionalOperator()
{
    section("A. Provisional operator (Acoustic.h): units, 1 kHz calibration, dipole (Sec. 19), mic-position model");
    const double fs = 48000.0;
    const SpeakerTS sp;
    const double A1k = std::abs(speakerVelocityPerVolt(sp, 1000.0));

    // A1. input is cone velocity [m/s]: response scales linearly with the input and is acceleration-proportional (+90 deg, +6 dB/oct)
    {
        AcousticParams p; p.openBack = false; p.coneRolloffHz = 1e5; p.presenceDb = 0.0; p.micRadial = 0.0; p.micDistance = 0.0;
        auto op = makeOp(fs, p, A1k);
        const cplx r1 = op.response(300.0), r2 = op.response(600.0);
        const double slope = dB(std::abs(r2) / std::abs(r1));
        const double phase = std::arg(r1) * 180.0 / M_PI;
        check(std::fabs(slope - 6.02) < 0.15, "closed-back, flat-chain operator rises 6.02 dB/oct on cone velocity (acceleration-proportional)", "slope " + f(slope, 3) + " dB/oct");
        check(std::fabs(phase - 90.0) < 6.0, "...with +90 deg phase (dv/dt; the 20 Hz high-pass adds ~+5 deg, half-sample and low-pass lags ~-2 deg)", "phase at 300 Hz " + f(phase, 2) + " deg");
        std::vector<double> y1, y2;
        auto o1 = makeOp(fs, p, A1k), o2 = makeOp(fs, p, A1k);
        for (int i = 0; i < 4800; ++i) { const double v = 0.01 * std::sin(2 * M_PI * 440 * i / fs); y1.push_back(o1.process(v)); y2.push_back(o2.process(2.0 * v)); }
        double e = 0; for (size_t i = 0; i < y1.size(); ++i) e = std::max(e, std::fabs(y2[i] - 2.0 * y1[i]));
        check(e < 1e-12, "operator is linear in cone velocity", "max |op(2v) - 2 op(v)| = " + f(e, 3));
    }

    // A2. analytic response() equals what process() actually does (default parameters, fractional-delay rear tap included)
    {
        const AcousticParams p;
        double worst = 0;
        for (double f0 : {60.0, 100.0, 300.0, 1000.0, 3000.0, 8000.0}) {
            auto op = makeOp(fs, p, A1k);
            std::vector<double> y;
            for (int i = 0; i < (int)(0.6 * fs); ++i) y.push_back(op.process(0.001 * std::sin(2 * M_PI * f0 * i / fs)));
            const cplx m = lockIn(y, fs, f0, 20) / 0.001;
            const cplx a = op.response(f0);
            worst = std::max(worst, std::fabs(dB(std::abs(m) / std::abs(a))));
        }
        check(worst < 0.01, "analytic response() reproduces process() to < 0.01 dB (6 test frequencies)", "worst " + f(worst, 4) + " dB");
    }

    // A3. 1 kHz calibration: a 1 kHz sine of amplitude Vt at the terminals -> amplitude Vt at the output, for every mic setting
    {
        AcousticParams settings[4];
        settings[1].micRadial = 0.9; settings[2].micDistance = 1.0; settings[3].pathDifference_m = 0.55; settings[3].openBack = true;
        double worst = 0;
        for (const auto& p : settings) {
            auto op = makeOp(fs, p, A1k);
            const double Vt = 2.0;
            std::vector<double> y;
            for (int i = 0; i < (int)(0.3 * fs); ++i) y.push_back(op.process(Vt * A1k * std::sin(2 * M_PI * 1000.0 * i / fs)));
            const double amp = std::abs(lockIn(y, fs, 1000.0, 50));
            worst = std::max(worst, std::fabs(dB(amp / Vt)));
        }
        check(worst < 0.01, "1 kHz terminal-volt sine -> unit gain at the output for 4 microphone/path settings", "worst deviation " + f(worst, 4) + " dB");
        // changing the microphone at run time keeps the calibration (set() after calibrate1k)
        AcousticOperator op; op.prepare(fs); op.set(AcousticParams{}); op.calibrate1k(A1k);
        AcousticParams q; q.micRadial = 0.8; q.micDistance = 0.9; op.set(q);
        const double g = dB(std::abs(op.response(1000.0)) * A1k);
        check(std::fabs(g) < 1e-9, "calibration survives a run-time set() of the microphone position", "gain at 1 kHz " + f(g, 6) + " dB");
    }

    // A4. open-back dipole against Sec. 19: H = 1 - kappa exp(-j 2 pi f dt); polarity, low-frequency limit, comb positions
    {
        AcousticParams po, pc; pc.openBack = false;
        // calibrate1k(0) leaves the gain at 1, so open-back/closed-back ratios are the pure rear-wave factor
        auto open = makeOp(fs, po, 0.0), closed = makeOp(fs, pc, 0.0);
        // integer-sample path so the rear tap is exact: dt = 60 samples
        AcousticParams pi_ = po; pi_.pathDifference_m = 60.0 / fs * kSoundSpeed;
        auto openI = makeOp(fs, pi_, 0.0);
        const double kI = AcousticOperator::rearAmplitude(pi_), dtI = 60.0 / fs;
        double worstExact = 0;
        for (double f0 : {30.0, 200.0, 800.0, 2000.0, 5000.0}) {
            const cplx ratio = openI.response(f0) / closed.response(f0);
            const cplx expect = 1.0 - kI * std::polar(1.0, -2.0 * M_PI * f0 * dtI);
            worstExact = std::max(worstExact, std::abs(ratio - expect));
        }
        check(worstExact < 1e-9, "integer-delay open-back/closed-back ratio equals 1 - kappa exp(-j2 pi f dt) exactly (Sec. 19)", "max |diff| " + f(worstExact, 12));
        // low-frequency limit and polarity: ratio -> (1-kappa) + j kappa w dt  (+90 deg lead, front wave dominant, positive)
        const double f0 = 20.0;
        const cplx ratio = openI.response(f0) / closed.response(f0);
        check(ratio.real() > 0 && ratio.imag() > 0, "low-frequency limit: real part (1 - kappa) > 0 and imaginary part +kappa w dt > 0 (front adds, rear subtracts, delayed)", "ratio at 20 Hz = " + f(ratio.real(), 4) + " + j" + f(ratio.imag(), 4));
        check(std::fabs(ratio.imag() / (kI * 2 * M_PI * f0 * dtI) - 1.0) < 0.02, "slope of the low-frequency imaginary part is kappa*2 pi f dt (dipole 6 dB/oct)", "measured/expected " + f(ratio.imag() / (kI * 2 * M_PI * f0 * dtI), 4));
        // comb: first maximum (1+kappa) at f = 1/(2 dt), first re-minimum (1-kappa) at f = 1/dt
        const double fMax = 1.0 / (2 * dtI), fMin = 1.0 / dtI;
        const double gMax = std::abs(openI.response(fMax) / closed.response(fMax)), gMin = std::abs(openI.response(fMin) / closed.response(fMin));
        check(std::fabs(gMax - (1 + kI)) < 1e-6 && std::fabs(gMin - (1 - kI)) < 1e-6, "comb extremes are 1+kappa at 1/(2 dt) and 1-kappa at 1/dt", "|H| " + f(gMax, 4) + " / " + f(gMin, 4));
        // polarity in the time domain: (open - closed) is a NEGATIVE, delayed copy of the closed-back output, scaled by kappa
        {
            auto o = makeOp(fs, pi_, 0.0), c = makeOp(fs, pc, 0.0);
            std::vector<double> yo, yc;
            for (int i = 0; i < 2400; ++i) {
                const double t = (i - 200) / fs, v = 0.01 * std::exp(-0.5 * std::pow(t / 0.00012, 2));      // smooth velocity pulse (band-limited)
                yo.push_back(o.process(v)); yc.push_back(c.process(v));
            }
            double num = 0, den = 0;
            for (int i = 100; i < 2000; ++i) { const double d = yo[(size_t)i] - yc[(size_t)i], ref = yc[(size_t)i - 60]; num += d * ref; den += ref * ref; }
            const double coef = num / den;
            check(coef < 0 && std::fabs(coef + kI) < 0.01 * kI + 1e-9, "time-domain polarity: open-back minus closed-back = -kappa x (closed-back output delayed by dt)", "regression coefficient " + f(coef, 4) + ", kappa " + f(kI, 4));
        }
        // default (fractional-delay) tap vs the ideal formula: Catmull-Rom interpolation error
        const double dt = po.pathDifference_m / kSoundSpeed, k = AcousticOperator::rearAmplitude(po);
        for (double f1 : {500.0, 2000.0, 4000.0}) {
            const cplx ratio = open.response(f1) / closed.response(f1);
            const cplx expect = 1.0 - k * std::polar(1.0, -2.0 * M_PI * f1 * dt);
            info("default fractional tap vs ideal 1 - kappa e^{-jwdt} at " + f(f1, 0) + " Hz: |H| error " + f(dB(std::abs(ratio) / std::abs(expect)), 3) + " dB, phase error " + f((std::arg(ratio) - std::arg(expect)) * 180 / M_PI, 2) + " deg");
        }
    }

    // A5. microphone-position model
    {
        const double x = kPistonMinus3dB;
        check(std::fabs(pistonDirectivity(x) - 1.0 / std::sqrt(2.0)) < 1e-3, "piston directivity 2J1(x)/x is 1/sqrt2 (-3.01 dB) at the constant used for the mic-position corner", "D(" + f(x, 4) + ") = " + f(pistonDirectivity(x), 5));
        check(std::fabs(pistonDirectivity(3.8317)) < 1e-3, "...and the first null is at x = 3.832 (3.83 is the null; the corner uses the -3 dB point)", "D(3.8317) = " + f(pistonDirectivity(3.8317), 5));
        AcousticParams a, b;
        a.micRadial = 0.2; b.micRadial = 0.8;
        check(AcousticOperator::directivityCornerHz(b) <= AcousticOperator::directivityCornerHz(a), "off-axis microphone (larger radial offset) never raises the HF corner", f(AcousticOperator::directivityCornerHz(a), 0) + " -> " + f(AcousticOperator::directivityCornerHz(b), 0) + " Hz");
        AcousticParams c = a, d = a; c.micDistance = 0.0; d.micDistance = 1.0;
        check(AcousticOperator::directivityCornerHz(c) <= AcousticOperator::directivityCornerHz(d), "a closer microphone at the same radial offset sees a larger off-axis angle (lower corner)", f(AcousticOperator::directivityCornerHz(c), 0) + " vs " + f(AcousticOperator::directivityCornerHz(d), 0) + " Hz");
        const double rNear = AcousticOperator::rearAmplitude(c), rFar = AcousticOperator::rearAmplitude(d);
        const double expectNear = c.rearLevel * 0.025 / (0.025 + c.pathDifference_m), expectFar = d.rearLevel * 0.6 / (0.6 + d.pathDifference_m);
        check(std::fabs(rNear - expectNear) < 1e-12 && std::fabs(rFar - expectFar) < 1e-12 && rNear < rFar, "rear-wave amplitude follows 1/r spreading: kappa = rearLevel * r/(r + dx)", "near " + f(rNear, 3) + " (2.5 cm), far " + f(rFar, 3) + " (60 cm)");
        AcousticParams def; const double dd = AcousticOperator::axialDistance_m(def);
        info("default mic (radial " + f(def.micRadial, 2) + ", distance knob " + f(def.micDistance, 2) + "): axial " + f(dd * 100, 1) + " cm, directivity corner " + f(AcousticOperator::directivityCornerHz(def), 0) + " Hz, rear amplitude kappa " + f(AcousticOperator::rearAmplitude(def), 3));
        info("ALL microphone-position behaviour is a PROVISIONAL geometric model; no measured positions exist (dossier Sec. 19: interpolation needs phase-aware calibrated captures)");
    }
}

// ---------------------------------------------------------------------------------------------------- B. speaker G(f)
static std::string gNetlist, gManifest;

static void testSpeakerTransfer()
{
    section("B. Closed-form G(f) = U_cone/V_s against the circuit's own speaker port");
    EngineParams p; p.oversampleLog2 = 1; p.load = 0; p.speakerHeating = false; p.instVol = 0.5f; p.tone = 0.7f;
    CD57Engine e; e.prepare(48000.0, 512, gNetlist, p, gManifest);
    const SpeakerTS& sp = e.amp().cfg.speaker;
    const double freqs[] = {20, 40, 60, 98, 150, 300, 600, 1000, 2000, 3000, 5000, 8000};

    double worstEng = 0;
    for (double f0 : freqs) {
        const cplx a = speakerVelocityPerVolt(sp, f0), b = CD57Engine::coneVelocityPerVolt(sp, f0);
        worstEng = std::max(worstEng, std::abs(a / b - 1.0));
    }
    check(worstEng < 1e-9, "closed form Bl/(Ze Zmech + Bl^2) equals the equivalent-circuit form used by CD57Engine::coneVelocityPerVolt", "worst relative difference " + f(worstEng, 12));

    // netlist AC solve (continuous-time, linearised about the operating point): inject at the jack, compare v_cone / V_terminal
    Network& net = e.amp().network();
    const int pu = net.nodeId("pu"), nsp = net.nodeId("sp"), nsy = net.nodeId("sy");
    double worstMag = 0, worstPh = 0;
    std::ostringstream tab;
    for (double f0 : freqs) {
        const auto v = net.acSolve(f0, pu);
        const cplx gNet = (v[(size_t)nsy] / e.amp().bl()) / v[(size_t)nsp];
        const cplx gCl = speakerVelocityPerVolt(sp, f0);
        worstMag = std::max(worstMag, std::fabs(dB(std::abs(gNet) / std::abs(gCl))));
        worstPh = std::max(worstPh, std::fabs(std::arg(gNet / gCl)) * 180 / M_PI);
        tab << f(f0, 0) << "Hz:" << f(dB(std::abs(gNet) / std::abs(gCl)), 4) << "dB ";
    }
    check(worstMag < 0.01 && worstPh < 0.1, "netlist AC response v_cone/V_terminal equals the closed form at 12 frequencies 20 Hz-8 kHz", "worst " + f(worstMag, 5) + " dB, " + f(worstPh, 4) + " deg");

    // transient: the engine's coneVelocity() port vs terminal voltage under a real (small-signal) drive through the whole amplifier
    {
        const double fs = 48000.0;
        double worstT = 0, worstTp = 0;
        std::ostringstream t2;
        for (double f0 : {60.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 4000.0}) {
            EngineParams q = p; q.inputCalVolts = 1.0f;
            CD57Engine eng; eng.prepare(fs, 512, gNetlist, q, gManifest);
            const int total = (int)(1.2 * fs);
            std::vector<double> vt, vc;
            std::vector<float> in(512), ot(512), oa(512);
            const double amp = 0.02;                                       // volts at the jack: small signal
            for (int i0 = 0; i0 < total; i0 += 512) {
                const int n = std::min(512, total - i0);
                for (int k = 0; k < n; ++k) { const double t = (i0 + k) / fs; in[(size_t)k] = (float)(amp * std::sin(2 * M_PI * f0 * t) * std::min(1.0, t / 0.2)); }
                eng.process(in.data(), ot.data(), oa.data(), n);
                for (int k = 0; k < n; ++k) { vt.push_back((double)ot[(size_t)k] / eng.outputScale()); vc.push_back(eng.coneVelocityBlock()[(size_t)k]); }
            }
            const cplx Vt = lockIn(vt, fs, f0, 30), Vc = lockIn(vc, fs, f0, 30);
            const cplx gm = Vc / Vt, gc = speakerVelocityPerVolt(eng.amp().cfg.speaker, f0);
            worstT = std::max(worstT, std::fabs(dB(std::abs(gm) / std::abs(gc))));
            worstTp = std::max(worstTp, std::fabs(std::arg(gm / gc)) * 180 / M_PI);
            t2 << f(f0, 0) << "Hz:" << f(dB(std::abs(gm) / std::abs(gc)), 3) << "dB/" << f(std::arg(gm / gc) * 180 / M_PI, 2) << "deg(Vt=" << f(std::abs(Vt), 2) << "V) ";
        }
        info("engine cone-velocity port vs closed form (magnitude/phase error, terminal amplitude): " + t2.str());
        check(worstT < 0.2 && worstTp < 1.5, "engine coneVelocity()/vTerminal() transfer from a transient solve through the amplifier equals the closed form (7 frequencies 60 Hz-4 kHz)", "worst " + f(worstT, 3) + " dB, " + f(worstTp, 2) + " deg");
    }

    // hot voice coil: Re(T) enters the cone velocity exactly as the closed form says (route 2 sees heating, a fixed IR cannot)
    {
        const double fs = 48000.0, f0 = 400.0, hotRe = sp.Re * 1.5;
        EngineParams q = p;
        CD57Engine eng; eng.prepare(fs, 512, gNetlist, q, gManifest);
        eng.amp().setVoiceCoilResistance(hotRe);
        std::vector<double> vt, vc; std::vector<float> in(512), ot(512), oa(512);
        const int total = (int)(1.2 * fs);
        for (int i0 = 0; i0 < total; i0 += 512) {
            const int n = std::min(512, total - i0);
            for (int k = 0; k < n; ++k) { const double t = (i0 + k) / fs; in[(size_t)k] = (float)(0.02 * std::sin(2 * M_PI * f0 * t) * std::min(1.0, t / 0.2)); }
            eng.process(in.data(), ot.data(), oa.data(), n);
            for (int k = 0; k < n; ++k) { vt.push_back((double)ot[(size_t)k] / eng.outputScale()); vc.push_back(eng.coneVelocityBlock()[(size_t)k]); }
        }
        const cplx gm = lockIn(vc, fs, f0, 30) / lockIn(vt, fs, f0, 30);
        const cplx hot = speakerVelocityPerVolt(sp, f0, hotRe), cold = speakerVelocityPerVolt(sp, f0);
        check(std::fabs(dB(std::abs(gm) / std::abs(hot))) < 0.2 && dB(std::abs(hot) / std::abs(cold)) < -0.5,
              "with Re x1.5 (hot coil) the engine's cone velocity per volt follows the Re-overridden closed form, "
              "i.e. the hybrid route sees voice-coil heating",
              "measured " + f(dB(std::abs(gm)), 3) + " dB re 1 m/s/V, closed form hot " + f(dB(std::abs(hot)), 3) + ", cold " + f(dB(std::abs(cold)), 3));
    }
}

// ---------------------------------------------------------------------------------------------------- C. IR preparation
static cplx freqResp(const std::vector<double>& h, double fs, double f0)
{
    cplx a(0, 0);
    for (size_t n = 0; n < h.size(); ++n) a += h[n] * std::polar(1.0, -2.0 * M_PI * f0 * (double)n / fs);
    return a;
}

static void testIrPreparation()
{
    section("C. IR preparation: gain, polarity and arrival time survive sample-rate conversion; no trimming, no normalisation");
    // a known transfer function: delay of 0.75 ms, -1.8 gain (inverted), plus a 2-pole resonance at 1.2 kHz
    auto make = [](double fs) {
        const double tau = 0.00075, w0 = 2 * M_PI * 1200.0, zeta = 0.15;
        std::vector<double> h((size_t)(0.12 * fs), 0.0);
        const double wd = w0 * std::sqrt(1 - zeta * zeta);
        for (size_t n = 0; n < h.size(); ++n) {
            const double t = (double)n / fs - tau;
            if (t >= 0) h[n] = -1.8 * (w0 * w0 / wd) * std::exp(-zeta * w0 * t) * std::sin(wd * t) / fs;   // per-sample IR of H(s) = w0^2/(s^2 + 2 zeta w0 s + w0^2)
        }
        return h;
    };
    struct Case { double in, out; };
    for (Case c : {Case{44100, 48000}, Case{96000, 48000}, Case{48000, 44100}, Case{192000, 48000}, Case{48000, 96000}}) {
        const auto raw = make(c.in);
        const auto p = prepareImpulseResponse(raw, c.in, c.out, 2.0);
        double worstMag = 0, worstDelay = 0;
        for (double f0 : {100.0, 400.0, 1000.0, 1500.0, 3000.0, 6000.0, 15000.0}) {
            if (f0 > 0.4 * std::min(c.in, c.out)) continue;
            const cplx a = freqResp(p.h, c.out, f0), b = freqResp(raw, c.in, f0);        // the transfer function must be unchanged
            worstMag = std::max(worstMag, std::fabs(dB(std::abs(a) / std::abs(b))));
            worstDelay = std::max(worstDelay, std::fabs(std::arg(a / b)) / (2 * M_PI * f0) * 1e6);   // equivalent time error, us
        }
        const cplx dc = freqResp(p.h, c.out, 20.0), dcRef = freqResp(raw, c.in, 20.0);
        check(worstMag < 0.02 && worstDelay < 3.0 && std::real(dc / dcRef) > 0.99 && std::real(dc) < 0.0,
              "resample " + f(c.in / 1000, 1) + " -> " + f(c.out / 1000, 1) + " kHz preserves gain, phase/arrival time and polarity (H(20 Hz) stays negative)",
              "worst |H| error " + f(worstMag, 4) + " dB, worst time error " + f(worstDelay, 2) + " us");
    }
    {   // identity: same rate returns the file untouched (no normalisation, no trim, polarity kept); leading silence kept
        std::vector<double> raw(5000, 0.0); raw[100] = -0.37; raw[400] = 0.1;
        const auto p = prepareImpulseResponse(raw, 48000, 48000, 2.0);
        check(p.h == raw && !p.cutAtMaxLength && std::fabs(p.onsetMs - 100.0 / 48.0) < 1e-9, "same-rate IR is bit-identical: not normalised, not trimmed, leading 100 samples kept, polarity kept", "onset " + f(p.onsetMs, 3) + " ms, peak " + f(p.peakAbs, 3));
    }
    {   // over-length: cut at 2 s with a 10 ms fade on the last 10 ms only
        std::vector<double> raw((size_t)(3.0 * 48000), 1.0);
        const auto p = prepareImpulseResponse(raw, 48000, 48000, 2.0);
        bool headUntouched = true; for (size_t i = 0; i < p.h.size() - 480 - 1; ++i) if (p.h[i] != 1.0) headUntouched = false;
        check(p.cutAtMaxLength && p.h.size() == 96000 && headUntouched && p.h.back() < 0.01, "over-length IR is cut at 2 s; only the last 10 ms are faded", "length " + std::to_string(p.h.size()) + ", last sample " + f(p.h.back(), 4));
    }
}

// ---------------------------------------------------------------------------------------------------- D. factorisation identity
static std::vector<cplx> spectrum(const std::vector<double>& x, size_t N)
{
    std::vector<cplx> a(N, cplx(0, 0));
    for (size_t i = 0; i < x.size() && i < N; ++i) a[i] = x[i];
    fftInPlace(a);
    return a;
}
static std::vector<double> convolveFFT(const std::vector<double>& x, const std::vector<double>& h)
{
    const size_t n = x.size() + h.size() - 1, N = nextPow2(n);
    auto X = spectrum(x, N), Hh = spectrum(h, N);
    for (size_t k = 0; k < N; ++k) X[k] *= Hh[k];
    fftInPlace(X, true);
    std::vector<double> y(n);
    for (size_t i = 0; i < n; ++i) y[i] = X[i].real();
    return y;
}
// energy ratio of the error spectrum to the reference spectrum in [lo, hi] Hz (ESR, dossier Sec. 26), in dB
static double bandESR(const std::vector<double>& ref, const std::vector<double>& test, double fs, double lo, double hi)
{
    const size_t N = nextPow2(std::max(ref.size(), test.size()));
    const auto A = spectrum(ref, N), B = spectrum(test, N);
    double e = 0, r = 0;
    for (size_t k = 1; k < N / 2; ++k) {
        const double f0 = (double)k * fs / (double)N;
        if (f0 < lo || f0 > hi) continue;
        e += std::norm(B[k] - A[k]); r += std::norm(A[k]);
    }
    return 10.0 * std::log10(std::max(e / std::max(r, 1e-300), 1e-30));
}

// synthetic "true" residual R_true(f): gain, arrival delay, a finite-depth notch, a resonance and a mild roll-off.
// NOT a measurement: it only has to be known exactly.
struct RTrue {
    double gain = 0.7, delaySec = 0.5e-3, notchHz = 2500.0, notchDepthDb = -30.0, resHz = 3200.0, resDb = 9.0, resQ = 3.0, lpHz = 7000.0;
    cplx operator()(double f) const
    {
        const cplx s(0.0, 2.0 * M_PI * f);
        const double wn = 2 * M_PI * notchHz, zp = 0.15, zz = zp * std::pow(10.0, notchDepthDb / 20.0);
        const cplx notch = (s * s + 2.0 * zz * wn * s + wn * wn) / (s * s + 2.0 * zp * wn * s + wn * wn);
        const double wr = 2 * M_PI * resHz, zr = 0.5 / resQ, A = std::pow(10.0, resDb / 20.0);
        const cplx res = 1.0 + (A - 1.0) * (2.0 * zr * wr * s) / (s * s + 2.0 * zr * wr * s + wr * wr);
        const cplx lp = 1.0 / (1.0 + s / (2 * M_PI * lpHz));
        return gain * std::polar(1.0, -2.0 * M_PI * f * delaySec) * notch * res * lp;
    }
};

// IR whose spectrum is Spec(f) on an N-point grid (conjugate-symmetric), truncated to L samples
template <class SpecFn>
static std::vector<double> irFromSpectrum(SpecFn&& Spec, double fs, size_t N, size_t L)
{
    std::vector<cplx> a(N, cplx(0, 0));
    for (size_t k = 0; k <= N / 2; ++k) {
        cplx v = Spec((double)k * fs / (double)N);
        if (k == 0 || k == N / 2) v = cplx(v.real(), 0.0);
        a[k] = v; if (k > 0 && k < N / 2) a[N - k] = std::conj(v);
    }
    fftInPlace(a, true);
    std::vector<double> h(L);
    for (size_t i = 0; i < L; ++i) h[i] = a[i].real();
    return h;
}

// zero-phase band-pass by FFT masking (analysis helper for the time-domain comparisons)
static std::vector<double> bandLimit(const std::vector<double>& x, size_t N, double fs, double lo, double hi)
{
    auto X = spectrum(x, N);
    for (size_t k = 0; k <= N / 2; ++k) {
        const double f0 = (double)k * fs / (double)N;
        const bool in = f0 >= lo && f0 <= hi;
        if (!in) { X[k] = 0; if (k > 0 && k < N / 2) X[N - k] = 0; }
    }
    fftInPlace(X, true);
    std::vector<double> y(N); for (size_t i = 0; i < N; ++i) y[i] = X[i].real();
    return y;
}

static void testFactorisation()
{
    section("D. Linear acoustic identity (Sec. 27 experiment 4) with a SYNTHETIC H = G R_true: route 1 vs route 2");
    info("NO MEASURED H EXISTS: this validates the algebra and its implementation, not the acoustics of the real cabinet");
    const double fs = 48000.0;
    const SpeakerTS sp;
    auto G = [&](double f0) { return speakerVolumeVelocityPerVolt(sp, f0); };
    const RTrue rt;
    const size_t L = 8192, NG = 1 << 16;
    const auto h = irFromSpectrum([&](double f0) { return G(f0) * rt(f0); }, fs, NG, L);
    FactorSettings fset; fset.sampleRate = fs;
    const auto fr = factorResidual(h, G, fset);
    info("factorisation: gRef " + f(fr.gRef * 1e3, 3) + " (L/s)/V, lambda floor " + f(fset.floorDb, 0) + " dB re max|G| in [" + f(fset.bandLoHz, 0) + ", " + f(fset.bandHiHz, 0) + "] Hz, +" + f(fset.outOfBandDbPerOct, 0) + " dB/oct above; FFT " + std::to_string(fr.fftSize));
    info("guaranteed gain bound |R| gRef/|H| <= +" + f(fr.gainBoundDb, 1) + " dB; regularisation-limited band (0.5 dB): " + f(fr.regLimitLoHz, 0) + " - " + f(fr.regLimitHiHz, 0) + " Hz; REALISED usable band of the delivered FIR: " + f(fr.usableLoHz, 0) + " - " + f(fr.usableHiHz, 0) + " Hz");
    info("residual FIR: " + std::to_string(fr.residual.size()) + " taps; non-causal energy dropped " + f(fr.nonCausalDb, 1) + " dB, tail beyond FIR dropped " + f(fr.truncationDb, 1) + " dB (re total)");

    // D1: frequency-domain reconstruction G_n * FFT(r)  vs  H
    const auto Hf = spectrum(h, NG);
    const auto Rf = spectrum(fr.residual, NG);
    const std::pair<double, double> bands[] = {{80, 300}, {300, 1000}, {1000, 2000}, {2000, 4000}, {4000, 6000}, {6000, 8000}, {8000, 12000}};
    double worstMag = 0;
    std::ostringstream tab;
    for (auto b : bands) {
        double e = 0, rr = 0, maxMag = 0;
        for (size_t k = 1; k < NG / 2; ++k) {
            const double f0 = (double)k * fs / (double)NG;
            if (f0 < b.first || f0 >= b.second) continue;
            const cplx Hhat = (G(f0) / fr.gRef) * Rf[k];
            e += std::norm(Hhat - Hf[k]); rr += std::norm(Hf[k]);
            if (std::fabs(f0 - rt.notchHz) > 0.15 * rt.notchHz) maxMag = std::max(maxMag, std::fabs(dB(std::abs(Hhat) / std::abs(Hf[k]))));
        }
        tab << f(b.first, 0) << "-" << f(b.second, 0) << "Hz: ESR " << f(10 * std::log10(e / rr), 1) << " dB, max |H| err " << f(maxMag, 3) << " dB\n          ";
        if (b.second <= fr.usableHiHz) worstMag = std::max(worstMag, maxMag);
    }
    info("frequency-domain identity G*R vs H by band (notch +-15 % excluded from the magnitude column):\n          " + tab.str());
    check(fr.usableLoHz <= 80.0 && fr.usableHiHz >= 1000.0, "the realised usable band covers at least 80 Hz - 1 kHz for this synthetic H (dossier Sec. 26 band is 80 Hz - 8 kHz; the reached top edge is reported above)", f(fr.usableLoHz, 0) + " - " + f(fr.usableHiHz, 0) + " Hz");
    check(worstMag <= 0.5, "independent check: inside the realised usable band G R reproduces |H| within the 0.5 dB tolerance the band was declared with", "worst |H| error " + f(worstMag, 3) + " dB");

    // D2: time-domain identity with a noise drive: route 1 y1 = h (*) x ; route 2 y2 = r (*) (U_cone/gRef), U_cone = g (*) x
    {
        const auto g = irFromSpectrum(G, fs, NG, NG / 2);
        std::mt19937 rng(20240229); std::normal_distribution<double> nd(0.0, 1.0);
        std::vector<double> x((size_t)(1.5 * fs));
        for (auto& v : x) v = nd(rng);
        const auto u = convolveFFT(x, g);
        std::vector<double> c(u.size()); for (size_t i = 0; i < u.size(); ++i) c[i] = u[i] / fr.gRef;
        const auto y1 = convolveFFT(x, h), y2 = convolveFFT(c, fr.residual);
        std::ostringstream t2; double worst = -300;
        for (auto b : {std::pair<double, double>{80, 300}, {300, 1000}, {1000, 2000}, {2000, 4000}, {4000, 6000}}) {
            const double e = bandESR(y1, y2, fs, b.first, b.second);
            t2 << f(b.first, 0) << "-" << f(b.second, 0) << ": " << f(e, 1) << " dB  ";
            if (b.second <= fr.usableHiHz) worst = std::max(worst, e);
        }
        info("route 2 (cone volume velocity -> residual FIR) vs route 1 (terminal voltage -> H), noise drive, ESR by band: " + t2.str());
        check(worst < -26.02, "route 1 and route 2 agree over the realised usable band: band ESR below the dossier's NRMSE 5 % (-26.02 dB)", "worst band ESR " + f(worst, 1) + " dB");

        // D4: the double-count trap: pushing U_cone (as volts-equivalent) through the FULL H instead of the residual
        const auto yw = convolveFFT(c, h);
        std::ostringstream t4; double bestWrong = 300;
        for (auto b : {std::pair<double, double>{80, 300}, {300, 1000}, {1000, 2000}, {2000, 4000}}) {
            const double e = bandESR(y1, yw, fs, b.first, b.second);
            t4 << f(b.first, 0) << "-" << f(b.second, 0) << ": " << f(e, 1) << " dB  ";
            if (b.first >= 300) bestWrong = std::min(bestWrong, e);
        }
        info("DOUBLE-COUNT TRAP: cone motion through the full H gives G_n*H; ESR vs the correct route 1 by band: " + t4.str());
        std::ostringstream t5;
        for (double f0 : {100.0, 300.0, 1000.0, 3000.0}) t5 << f(f0, 0) << " Hz: " << f(dB(std::abs(G(f0)) / fr.gRef), 1) << " dB  ";
        info("the wrong transfer is |G_n| x |H|, i.e. wrong by 20 log10|G_n| = " + t5.str() + " (only at the cone's own resonance peak, ~100 Hz, does |G_n| = 1)");
        check(bestWrong > -3.0, "applying the FULL H to cone motion is grossly wrong wherever the cone response is not at its peak (300 Hz - 4 kHz: ESR worse than -3 dB), while the residual route is below -26 dB", "best wrong-route ESR in 300 Hz-4 kHz " + f(bestWrong, 1) + " dB");

        // D6: arrival time and polarity in the time domain: zero-phase band-limit both outputs to 200-1500 Hz, cross-correlate
        const size_t Nc = nextPow2(std::max(y1.size(), y2.size()));
        const auto b1 = bandLimit(y1, Nc, fs, 200.0, 1500.0), b2 = bandLimit(y2, Nc, fs, 200.0, 1500.0);
        auto A1 = spectrum(b1, Nc); const auto A2 = spectrum(b2, Nc);
        for (size_t k = 0; k < Nc; ++k) A1[k] = std::conj(A1[k]) * A2[k];
        fftInPlace(A1, true);                                      // r12[m] = sum b1[n] b2[n+m]
        size_t mp = 0; for (size_t m = 0; m < Nc; ++m) if (A1[m].real() > A1[mp].real()) mp = m;
        const double ym1 = A1[(mp + Nc - 1) % Nc].real(), y0 = A1[mp].real(), yp1 = A1[(mp + 1) % Nc].real();
        const double lag = ((mp > Nc / 2) ? (double)mp - (double)Nc : (double)mp) + 0.5 * (ym1 - yp1) / (ym1 - 2 * y0 + yp1);
        double e11 = 0, e12 = 0; for (size_t i = 0; i < Nc; ++i) { e11 += b1[i] * b1[i]; e12 += b1[i] * b2[i]; }
        check(std::fabs(lag) < 0.1 && e12 > 0 && std::fabs(dB(e12 / e11)) < 0.5, "arrival time, polarity, gain (200 Hz-1.5 kHz, noise drive): cross-correlation lag of route 2 vs route 1 within 0.1 sample, same sign, level within 0.5 dB",
              "lag " + f(lag, 4) + " samples, regression gain " + f(dB(e12 / e11), 3) + " dB");
    }

    // D3: regularisation at deep nulls: the bound holds, naive division does not
    {
        std::ostringstream t3;
        double prevNaive = 0; bool monotone = true, boundHolds = true, naiveBreaks = false;
        for (double nullDb : {-60.0, -90.0, -120.0}) {
            auto Gn = [&](double f0) {
                const cplx s(0.0, 2.0 * M_PI * f0);
                const double wn = 2 * M_PI * 1500.0, zp = 0.02, zz = zp * std::pow(10.0, nullDb / 20.0);       // ~60 Hz wide null at 1.5 kHz
                return G(f0) * (s * s + 2.0 * zz * wn * s + wn * wn) / (s * s + 2.0 * zp * wn * s + wn * wn);
            };
            auto h0 = irFromSpectrum([&](double f0) { return Gn(f0) * rt(f0); }, fs, NG, L);
            const auto H0 = spectrum(h0, NG);
            double hpk = 0; for (size_t k = 1; k < NG / 2; ++k) hpk = std::max(hpk, std::abs(H0[k]));
            // white measurement floor: -60 dB per spectral bin relative to the largest bin of |H|
            std::mt19937 rng(7); std::normal_distribution<double> nd(0.0, 1.0);
            const double sigma = hpk * std::pow(10.0, -60.0 / 20.0) / std::sqrt((double)L);
            auto hm = h0; for (auto& v : hm) v += sigma * nd(rng);
            const auto frN = factorResidual(hm, Gn, fset);
            const auto Hm = spectrum(hm, NG), Rr = spectrum(frN.residual, NG);
            hpk = 0; for (size_t k = 1; k < NG / 2; ++k) hpk = std::max(hpk, std::abs(Hm[k]));
            double naiveMax = 0, regMax = 0;
            for (size_t k = 1; k < NG / 2; ++k) {
                const double f0 = (double)k * fs / (double)NG;
                naiveMax = std::max(naiveMax, std::abs(Hm[k]) / std::abs(Gn(f0)) * frN.gRef);       // |R_naive| gRef  (H/G, in the residual's units)
                regMax = std::max(regMax, std::abs(Rr[k]));
            }
            const double bound = hpk / (2.0 * std::pow(10.0, fset.floorDb / 20.0));
            t3 << "null " << f(nullDb, 0) << " dB: naive " << f(dB(naiveMax / hpk), 1) << " dB, regularised " << f(dB(regMax / hpk), 1) << " dB re max|H| (bound +" << f(frN.gainBoundDb, 1) << ");  ";
            boundHolds = boundHolds && regMax <= bound;
            monotone = monotone && naiveMax > prevNaive; prevNaive = naiveMax;
            if (nullDb <= -120.0) naiveBreaks = naiveMax > bound;
        }
        info("deep-null test (G with a notch of the stated depth at 1.5 kHz; H carries a -60 dB white measurement floor), peak |R| gRef re max|H|: " + t3.str());
        check(boundHolds && monotone && naiveBreaks, "regularisation bound |R| gRef <= max|H|/(2 * 10^(floor/20)) holds for nulls of -60/-90/-120 dB; naive H/G grows with null depth and exceeds the bound at -120 dB", "see INFO line");
    }

    // D5: polarity and absolute gain with a physical IR (finite energy at DC): H = -2.5 (inverted, +8 dB), 1 ms delay, 2nd-order 60 Hz high-pass
    {
        const double gainWanted = -2.5; const size_t dly = 48;
        std::vector<double> hp(8192, 0.0); hp[dly] = gainWanted;
        {   // 2nd-order Butterworth high-pass at 60 Hz (bilinear)
            const double w = std::tan(M_PI * 60.0 / fs), k = std::sqrt(2.0), n = 1.0 / (1.0 + k * w + w * w);
            const double b0 = n, b1 = -2 * n, b2 = n, a1 = 2 * (w * w - 1) * n, a2 = (1 - k * w + w * w) * n;
            double z1 = 0, z2 = 0;
            for (auto& v : hp) { const double x0 = v, y = b0 * x0 + z1; z1 = b1 * x0 - a1 * y + z2; z2 = b2 * x0 - a2 * y; v = y; }
        }
        const auto frp = factorResidual(hp, G, fset);
        const auto Hp = spectrum(hp, NG), Rp = spectrum(frp.residual, NG);
        double worstDb = 0, worstPh = 0, minReal = 1e9;
        for (double f0 : {100.0, 150.0, 300.0, 700.0, 1000.0, 1800.0, 3000.0}) {
            const size_t k = (size_t)std::round(f0 * (double)NG / fs);
            const double fk = (double)k * fs / (double)NG;
            const cplx hat = (G(fk) / frp.gRef) * Rp[k], ref = Hp[k];
            worstDb = std::max(worstDb, std::fabs(dB(std::abs(hat) / std::abs(ref))));
            worstPh = std::max(worstPh, std::fabs(std::arg(hat / ref)) * 180 / M_PI);
            minReal = std::min(minReal, std::real(hat / ref));
        }
        auto gd = [&](const std::vector<cplx>& S, bool withG) {
            const size_t k1 = (size_t)std::round(950.0 * (double)NG / fs), k2 = (size_t)std::round(1050.0 * (double)NG / fs);
            auto at = [&](size_t k) { const double fk = (double)k * fs / (double)NG; return withG ? (G(fk) / frp.gRef) * S[k] : S[k]; };
            return -std::arg(at(k2) / at(k1)) / (2 * M_PI * ((double)(k2 - k1) * fs / (double)NG));
        };
        const double gdRef = gd(Hp, false), gdHat = gd(Rp, true);
        const size_t kk = (size_t)std::round(1000.0 * (double)NG / fs);
        check(worstDb < 0.5 && worstPh < 3.0 && minReal > 0.9 && std::real(Hp[kk]) < 0,
              "polarity and absolute gain: G*R reproduces H = -2.5 z^-48 (inverted, 2nd-order 60 Hz HP) with the same sign within 0.5 dB / 3 deg over 100 Hz - 3 kHz",
              "worst " + f(worstDb, 3) + " dB, " + f(worstPh, 2) + " deg, min Re(G*R/H) " + f(minReal, 3) + ", H(1 kHz) = " + f(std::real(Hp[kk]), 3));
        check(std::fabs(gdRef - (double)dly / fs) < 20e-6 && std::fabs(gdHat - gdRef) < 20e-6, "arrival time: group delay of G*R at 1 kHz equals that of H (1 ms delay + the 60 Hz high-pass, 13.6 us at 1 kHz) within 20 us", "H " + f(gdRef * 1e3, 4) + " ms, G*R " + f(gdHat * 1e3, 4) + " ms");
    }
}

// ---------------------------------------------------------------------------------------------------- E. through the real engine
struct EngineRun { std::vector<double> xt, vc; double vtRms = 0; };
// drive the complete engine with jack volts, return the terminal output in OUTPUT UNITS (what route 1 convolves) and the
// engine's cone velocity [m/s] (what route 2 needs), both at the host rate
static EngineRun runEngine(CD57Engine& e, const std::vector<double>& jackVolts)
{
    EngineRun r;
    std::vector<float> in(512), ot(512), oa(512);
    const double cal = (double)e.params().inputCalVolts;
    double acc = 0;
    for (size_t i0 = 0; i0 < jackVolts.size(); i0 += 512) {
        const int n = (int)std::min<size_t>(512, jackVolts.size() - i0);
        for (int k = 0; k < n; ++k) in[(size_t)k] = (float)(jackVolts[i0 + (size_t)k] / cal);
        e.process(in.data(), ot.data(), oa.data(), n);
        for (int k = 0; k < n; ++k) {
            r.xt.push_back((double)ot[(size_t)k]); r.vc.push_back(e.coneVelocityBlock()[(size_t)k]);
            acc += std::pow((double)ot[(size_t)k] / e.outputScale(), 2);
        }
    }
    r.vtRms = std::sqrt(acc / (double)jackVolts.size());
    return r;
}

static void testThroughEngine()
{
    section("E. Route 2 reproduces route 1 through the ACTUAL engine speaker port (full amplifier + oversampler in the loop)");
    info("synthetic H (known G*R_true built from the engine's own speaker parameters); the amplifier is real, the acoustics are not");
    const double fs = 48000.0;
    EngineParams p; p.oversampleLog2 = 1; p.load = 0; p.speakerHeating = false; p.instVol = 0.5f; p.tone = 0.7f; p.inputCalVolts = 1.0f;
    CD57Engine e; e.prepare(fs, 512, gNetlist, p, gManifest);
    const SpeakerTS& sp = e.amp().cfg.speaker;
    auto G = [&](double f0) { return speakerVolumeVelocityPerVolt(sp, f0); };
    const RTrue rt;
    const size_t L = 8192, NG = 1 << 16;
    const auto h = irFromSpectrum([&](double f0) { return G(f0) * rt(f0); }, fs, NG, L);
    FactorSettings fset; fset.sampleRate = fs;
    const auto fr = factorResidual(h, G, fset);

    std::mt19937 rng(99); std::normal_distribution<double> nd(0.0, 1.0);
    // 2 s of 1 mV rms white noise at the jack (small signal), then 0.5 s of silence so every ring-down (cone, transformer) completes
    // inside the record: both routes are compared on COMPLETE responses, with no truncation edge in either one
    std::vector<double> jack((size_t)(2.5 * fs), 0.0);
    for (size_t i = 0; i < (size_t)(2.0 * fs); ++i) jack[i] = 0.001 * nd(rng);
    const auto run = runEngine(e, jack);
    info("drive: 1 mV rms white noise at the jack (rms over the 2.5 s record incl. silence) -> " + f(run.vtRms, 3) + " V rms at the speaker terminals (rated 12.5 W = 10 V rms), jack-to-terminal path is the full nonlinear circuit");

    // route 1: y1 = h (*) x_t        route 2: y2 = r (*) c,  c = kOut * Sd * v / gRef   (the plugin's hybrid input)
    const double kOut = e.outputScale();
    std::vector<double> c(run.vc.size());
    for (size_t i = 0; i < c.size(); ++i) c[i] = kOut * sp.Sd * run.vc[i] / fr.gRef;
    const auto y1 = convolveFFT(run.xt, h), y2 = convolveFFT(c, fr.residual);
    std::ostringstream t; double worst = -300;
    for (auto b : {std::pair<double, double>{100, 300}, {300, 1000}, {1000, 2000}, {2000, 4000}, {4000, 6000}}) {
        const double es = bandESR(y1, y2, fs, b.first, b.second);
        t << f(b.first, 0) << "-" << f(b.second, 0) << ": " << f(es, 1) << " dB  ";
        if (b.second <= std::min(fr.usableHiHz, 4000.0)) worst = std::max(worst, es);
    }
    info("route 2 (engine cone velocity -> residual) vs route 1 (engine terminal voltage -> H), ESR by band: " + t.str());
    check(worst < -26.02, "through the real engine: route 1 and route 2 agree to better than the dossier's NRMSE 5 % (-26.02 dB) in 100 Hz - min(4 kHz, realised usable band)", "worst band ESR " + f(worst, 1) + " dB; realised usable band " + f(fr.usableLoHz, 0) + "-" + f(fr.usableHiHz, 0) + " Hz");

    // no speaker connected (dummy load): no cone motion, so the hybrid is silent while a fixed voltage-to-mic IR would still "radiate"
    {
        EngineParams q = p; q.load = 2;
        CD57Engine d; d.prepare(fs, 512, gNetlist, q, gManifest);
        std::vector<double> j((size_t)(0.3 * fs)); for (auto& v : j) v = 0.01 * nd(rng);
        const auto rd = runEngine(d, j);
        double vmax = 0, xmax = 0; for (size_t i = 0; i < rd.vc.size(); ++i) { vmax = std::max(vmax, std::fabs(rd.vc[i])); xmax = std::max(xmax, std::fabs(rd.xt[i])); }
        check(vmax < 1e-6 && xmax > 1e-3, "dummy-load output: terminal voltage present, cone velocity ~ 0, so the hybrid route is silent (no cone, no sound)", "max |v| " + f(vmax, 9) + " m/s, max terminal output " + f(xmax, 4));
    }
}

int main()
{
    std::cout << "validate_acoustic -- acoustic boundary and impulse-response path\n"
                 "NOTE: no measured voltage-to-microphone H exists. Everything below validates the ALGEBRA and the IMPLEMENTATION with SYNTHETIC H,\n"
                 "      not the acoustics of the real cabinet, speaker or microphone (dossier Sec. 19, 27 experiment 4/7 need measurements).\n";
    gNetlist = embeddedNetlist(); gManifest = embeddedManifest();
    testProvisionalOperator();
    testSpeakerTransfer();
    testIrPreparation();
    testFactorisation();
    testThroughEngine();
    std::cout << "\n" << nPass << " passed, " << nFail << " failed\n";
    return nFail ? 1 : 0;
}
