// Acoustic.h -- PROVISIONAL acoustic operator: cone velocity -> microphone signal.
//
// Dossier Sec. 17-19 / 27 "Motion route": the amplifier is solved with the electromechanical speaker inside the circuit,
// so this operator takes CONE VELOCITY v [m/s] (the circuit's coneVelocity() port) -- never terminal voltage. Feeding it
// terminal voltage, or feeding cone motion through a full voltage-to-mic IR, would count the cone response twice
// (see AcousticFactor.h for the coherent hybrid that does use a measured IR).
//
//   p_mic = R * U_cone ,  U_cone = Sd * v.
//   Piston in an infinite baffle, far field:  p = rho0 * Sd * dv/dt / (2 pi r) * D(theta)   (acceleration-proportional)
//   Open back, Sec. 19 ideal dipole:          H_dipole(f) = 1 - exp(-j 2 pi f dt),  dt = path difference / c
//   (equal opposite-polarity sources with a relative delay; the rear wave arrives later and SUBTRACTS).
//   Here:  H = 1 - kappa * exp(-j 2 pi f dt)  with  kappa = rearLevel * r / (r + path difference):
//   the rear source is further from the microphone by the path difference, so by 1/r spreading its amplitude relative
//   to the front wave is r/(r + dx); rearLevel (<= 1) lumps baffle/panel obstruction. kappa = 1 recovers Sec. 19.
//   Sec. 19 limits stay in force: no finite-baffle diffraction, no cone breakup, no room, equal-source dipole only.
//
// Output units: the operator is NORMALISED, not calibrated in pascals: calibrate1k() sets the gain so that a 1 kHz sine
// of amplitude Vt at the speaker terminals produces a sine of amplitude Vt at the output (no absolute SPL is claimed).
// The propagation delay front-source -> microphone is NOT applied (arrival time = 0 at the cone), the opposite choice to
// a measured IR, which carries its propagation delay (see AcousticFactor.h).
//
// EVERYTHING below the first line of AcousticParams is PROVISIONAL (no measured IR / polar data exists in the public
// record, dossier Sec. 19/28). A measured voltage-to-mic IR may replace this block (plugin "User IR" or the coherent
// "hybrid" mode) -- never both.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace cd57 {

constexpr double kSoundSpeed = 343.0;                 // m/s, 20 degC air (external physical constant, not in dossier)
constexpr double kPistonMinus3dB = 1.6163;            // x at which 2 J1(x)/x = 1/sqrt(2) (solved numerically; checked in validate_acoustic)

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void reset() { z1 = z2 = 0; }
    std::complex<double> response(double fs, double f) const
    {
        const std::complex<double> zi = std::polar(1.0, -2.0 * M_PI * f / fs);
        return (b0 + (b1 + b2 * zi) * zi) / (1.0 + (a1 + a2 * zi) * zi);
    }
    void lowpass(double fs, double f0, double q)
    {
        f0 = std::min(f0, 0.45 * fs);
        const double w = 2 * M_PI * f0 / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
        const double a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    void peaking(double fs, double f0, double q, double gainDb)
    {
        f0 = std::min(f0, 0.45 * fs);
        const double A = std::pow(10.0, gainDb / 40.0), w = 2 * M_PI * f0 / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
        const double a0 = 1 + al / A;
        b0 = (1 + al * A) / a0; b1 = -2 * c / a0; b2 = (1 - al * A) / a0; a1 = -2 * c / a0; a2 = (1 - al / A) / a0;
    }
    void highpass(double fs, double f0, double q)
    {
        const double w = 2 * M_PI * f0 / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
        const double a0 = 1 + al;
        b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
};

struct AcousticParams {
    double micRadial = 0.15;        // 0 = dust-cap centre ... 1 = cone edge       (PROVISIONAL: geometry proxy, not a measured position)
    double micDistance = 0.10;      // 0 = ~2.5 cm ... 1 = ~60 cm                    (PROVISIONAL: geometry proxy)
    bool openBack = true;           // stock cabinet is open-back (dossier Sec. 02/19)
    double pathDifference_m = 0.42; // effective front-to-rear path difference      (PROVISIONAL, not measured; manifest interval 0.25-0.7)
    double rearLevel = 0.80;        // rear-wave level at equal distance incl. baffle/panel obstruction (PROVISIONAL; interval 0.5-1.0)
    double coneRolloffHz = 5200.0;  // PROVISIONAL generic 12" guitar driver
    double presenceHz = 2900.0, presenceDb = 3.0, presenceQ = 1.3;   // PROVISIONAL cone-mode proxy
    double coneRadius_m = 0.12;     // PROVISIONAL effective piston radius of a 12" driver (external estimate, not in dossier)
};

class AcousticOperator {
public:
    void prepare(double fs)
    {
        fs_ = fs;
        const int len = (int)std::ceil(0.004 * fs) + 8;   // up to 4 ms delay line
        dl_.assign((size_t)len, 0.0);
        wp_ = 0;
        reset();
        update();
    }
    void set(const AcousticParams& p) { P_ = p; update(); }
    const AcousticParams& params() const { return P_; }
    void reset() { std::fill(dl_.begin(), dl_.end(), 0.0); vprev_ = 0; lp1_ = 0; for (auto* b : {&cone_, &pres_, &dcb_}) b->reset(); }

    // Normalisation: unit gain at 1 kHz from terminal volts to output, where `velocityPerTerminalVolt1k` is |G_v(1 kHz)|,
    // the speaker's cone-velocity per terminal volt.  Evaluated analytically from the operator's own transfer function
    // (response()), and re-applied whenever set() changes the microphone geometry, so the 1 kHz level is the same for every
    // microphone position and path difference.
    void calibrate1k(double velocityPerTerminalVolt1k)
    {
        vPerV1k_ = velocityPerTerminalVolt1k;
        recalibrate();
    }

    // Exact linear response of process() at frequency f [Hz]: output per unit cone velocity (includes the 1 kHz calibration).
    std::complex<double> response(double f) const { return gain_ * chain(f); }

    // microphone axial distance [m] and off-axis geometry used by the model (public for tests / UI text)
    static double axialDistance_m(const AcousticParams& p) { return 0.025 + 0.575 * p.micDistance; }
    // far-field piston directivity -3 dB frequency at the mic's off-axis angle, clamped to a usable range
    static double directivityCornerHz(const AcousticParams& p)
    {
        const double dist = axialDistance_m(p);
        const double sinT = std::max(1e-3, std::sin(std::atan2(p.micRadial * p.coneRadius_m, dist)));
        return std::min(14000.0, std::max(2500.0, kPistonMinus3dB * kSoundSpeed / (2 * M_PI * p.coneRadius_m * sinT)));
    }
    static double rearAmplitude(const AcousticParams& p)
    {
        if (!p.openBack) return 0.0;
        const double r = axialDistance_m(p);
        return p.rearLevel * r / (r + p.pathDifference_m);
    }

    double process(double v)
    {
        const double a = (v - vprev_) * fs_;     // cone acceleration (backward difference)
        vprev_ = v;
        double p = a;
        const int N = (int)dl_.size();
        if (P_.openBack) {
            dl_[(size_t)wp_] = a;
            double rd = (double)wp_ - delaySamples_;
            while (rd < 0) rd += (double)N;
            const int i1 = (int)rd; const double fr = rd - i1;
            const double ym1 = dl_[(size_t)((i1 - 1 + N) % N)], y0 = dl_[(size_t)(i1 % N)], y1 = dl_[(size_t)((i1 + 1) % N)], y2 = dl_[(size_t)((i1 + 2) % N)];
            // 4-point cubic Hermite (Catmull-Rom) fractional-delay read
            const double c0 = y0, c1 = 0.5 * (y1 - ym1), c2 = ym1 - 2.5 * y0 + 2 * y1 - 0.5 * y2, c3 = 0.5 * (y2 - ym1) + 1.5 * (y0 - y1);
            const double rear = ((c3 * fr + c2) * fr + c1) * fr + c0;
            p = a - rearGain_ * rear;            // rear wave: opposite polarity, delayed (Sec. 19)
            wp_ = (wp_ + 1) % N;
        }
        p = dcb_.process(p);
        p = cone_.process(p);
        p = pres_.process(p);
        lp1_ += lpCoef_ * (p - lp1_);            // mic position (off-axis) loss
        return gain_ * lp1_;
    }

private:
    // linear response of the processing chain WITHOUT the calibration gain
    std::complex<double> chain(double f) const
    {
        using C = std::complex<double>;
        if (fs_ <= 0) return C(0, 0);
        const double w = 2 * M_PI * f / fs_;
        const C zi = std::polar(1.0, -w);
        C h = (1.0 - zi) * fs_;                                  // backward-difference acceleration
        if (P_.openBack) {
            // rear tap: read at lag ceil(delay) - j for Catmull-Rom weights w_j(fr), fr = ceil(delay) - delay
            const double dCeil = std::ceil(delaySamples_), fr = dCeil - delaySamples_;
            const double wm1 = ((-0.5 * fr + 1.0) * fr - 0.5) * fr, w0 = (1.5 * fr - 2.5) * fr * fr + 1.0,
                         w1 = ((-1.5 * fr + 2.0) * fr + 0.5) * fr, w2 = (0.5 * fr - 0.5) * fr * fr;
            C rear(0, 0);
            const double wj[4] = {wm1, w0, w1, w2};
            for (int j = -1; j <= 2; ++j) rear += wj[j + 1] * std::polar(1.0, -w * (dCeil - j));
            h *= 1.0 - rearGain_ * rear;
        }
        h *= dcb_.response(fs_, f) * cone_.response(fs_, f) * pres_.response(fs_, f);
        h *= lpCoef_ / (1.0 - (1.0 - lpCoef_) * zi);
        return h;
    }

    void recalibrate()
    {
        const double m = vPerV1k_ > 0 ? std::abs(chain(1000.0)) * vPerV1k_ : 0.0;
        gain_ = m > 1e-30 ? 1.0 / m : 1.0;
    }

    void update()
    {
        if (fs_ <= 0) return;
        // >= 2 samples: the 4-point read must not touch not-yet-written ring entries
        delaySamples_ = std::min(std::max(P_.pathDifference_m / kSoundSpeed * fs_, 2.0), (double)dl_.size() - 4.0);
        rearGain_ = rearAmplitude(P_);
        cone_.lowpass(fs_, P_.coneRolloffHz, 0.75);
        pres_.peaking(fs_, P_.presenceHz, P_.presenceQ, P_.presenceDb);
        dcb_.highpass(fs_, 20.0, 0.707);
        lpCoef_ = 1.0 - std::exp(-2 * M_PI * std::min(directivityCornerHz(P_), 0.45 * fs_) / fs_);
        recalibrate();
    }

    double fs_ = 0, vprev_ = 0, lp1_ = 0, gain_ = 1.0, vPerV1k_ = 0.0, lpCoef_ = 1.0, delaySamples_ = 20.0, rearGain_ = 0.0;
    int wp_ = 0;
    std::vector<double> dl_;
    Biquad cone_, pres_, dcb_;
    AcousticParams P_;
};

} // namespace cd57
