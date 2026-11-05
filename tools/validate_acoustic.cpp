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

int main()
{
    std::cout << "validate_acoustic -- acoustic boundary and impulse-response path\n"
                 "NOTE: no measured voltage-to-microphone H exists. Everything below validates the ALGEBRA and the IMPLEMENTATION with SYNTHETIC H,\n"
                 "      not the acoustics of the real cabinet, speaker or microphone (dossier Sec. 19, 27 experiment 4/7 need measurements).\n";
    gNetlist = embeddedNetlist(); gManifest = embeddedManifest();
    testProvisionalOperator();
    testSpeakerTransfer();
    testIrPreparation();
    std::cout << "\n" << nPass << " passed, " << nFail << " failed\n";
    return nFail ? 1 : 0;
}
