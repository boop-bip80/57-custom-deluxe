// Acoustic.h -- PROVISIONAL acoustic operator: cone velocity -> microphone pressure.
//
// Dossier Sec. 17-19 / 27: the electrical amplifier is solved with the *identified reactive load* (the speaker's
// electromechanical equivalent sits inside the circuit), so the acoustic operator must take CONE MOTION as its
// input -- feeding it terminal voltage and a full voltage-to-mic IR would count the cone response twice.
//
//   p_mic = R * U_cone ,  U_cone = Sd * v.
//   Piston radiation at distance r:  p ~ rho0 * Sd * dv/dt / (2 pi r)          (acceleration-proportional)
//   Open back (ideal dipole, Linkwitz):  H = 1 - k exp(-j 2 pi f dt), dt = path difference / c.
//   Mic position: piston directivity -> off-axis high-frequency loss; cone mass/breakup roll-off + presence peak.
//
// EVERYTHING beyond the first two lines is PROVISIONAL (no measured IR / polar data exists in the public record).
// A measured voltage-to-mic IR may replace this block (plugin: "User IR" mode) -- never both.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace cd57 {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void reset() { z1 = z2 = 0; }
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
    double micRadial = 0.15;        // 0 = dust-cap centre ... 1 = cone edge       (PROVISIONAL)
    double micDistance = 0.10;      // 0 = ~2.5 cm ... 1 = ~60 cm                    (PROVISIONAL)
    bool openBack = true;           // stock cabinet is open-back
    double pathDifference_m = 0.42; // effective front-to-rear path difference      (PROVISIONAL, not measured)
    double rearLevel = 0.80;        // rear-wave level incl. baffle/panel obstruction (PROVISIONAL)
    double coneRolloffHz = 5200.0;  // PROVISIONAL generic 12" guitar driver
    double presenceHz = 2900.0, presenceDb = 3.0, presenceQ = 1.3;   // PROVISIONAL cone-mode proxy
};

class AcousticOperator {
public:
    void prepare(double fs)
    {
        fs_ = fs;
        const int len = (int)std::ceil(0.004 * fs) + 8;   // up to 4 ms delay line
        dl_.assign(len, 0.0);
        wp_ = 0;
        reset();
        update();
    }
    void set(const AcousticParams& p) { P_ = p; update(); }
    void reset() { std::fill(dl_.begin(), dl_.end(), 0.0); vprev_ = 0; lp1_ = 0; for (auto* b : {&cone_, &pres_, &dcb_}) b->reset(); }

    // normalisation: gain such that a 1 kHz tone at the terminals (given |v/Vt| at 1 kHz) has unit amplitude at the output
    void calibrate1k(double velocityPerTerminalVolt1k)
    {
        reset();
        gain_ = 1.0;
        const double f = 1000.0, A = velocityPerTerminalVolt1k;
        const int n = (int)(0.08 * fs_);
        double peak = 0.0;
        for (int i = 0; i < n; ++i) {
            const double y = process(A * std::sin(2 * M_PI * f * i / fs_));
            if (i > n / 2) peak = std::max(peak, std::fabs(y));
        }
        gain_ = peak > 1e-30 ? 1.0 / peak : 1.0;
        reset();
    }

    double process(double v)
    {
        double a = (v - vprev_) * fs_;       // cone acceleration (backward difference)
        vprev_ = v;
        double p = a;
        if (P_.openBack) {
            dl_[wp_] = a;
            double rd = (double)wp_ - delaySamples_;
            while (rd < 0) rd += (double)dl_.size();
            const int i1 = (int)rd; const double fr = rd - i1;
            const int N = (int)dl_.size();
            const double ym1 = dl_[(i1 - 1 + N) % N], y0 = dl_[i1 % N], y1 = dl_[(i1 + 1) % N], y2 = dl_[(i1 + 2) % N];
            // 4-point cubic Lagrange interpolation
            const double c0 = y0, c1 = 0.5 * (y1 - ym1), c2 = ym1 - 2.5 * y0 + 2 * y1 - 0.5 * y2, c3 = 0.5 * (y2 - ym1) + 1.5 * (y0 - y1);
            const double rear = ((c3 * fr + c2) * fr + c1) * fr + c0;
            p = a - P_.rearLevel * rear;
            wp_ = (wp_ + 1) % N;
        }
        p = dcb_.process(p);
        p = cone_.process(p);
        p = pres_.process(p);
        lp1_ += lpCoef_ * (p - lp1_);        // mic position (off-axis) loss
        return gain_ * distGain_ * lp1_;
    }

private:
    void update()
    {
        if (fs_ <= 0) return;
        delaySamples_ = std::min(P_.pathDifference_m / 343.0 * fs_, (double)dl_.size() - 4.0);
        cone_.lowpass(fs_, P_.coneRolloffHz, 0.75);
        pres_.peaking(fs_, P_.presenceHz, P_.presenceQ, P_.presenceDb);
        dcb_.highpass(fs_, 20.0, 0.707);
        // mic position: off-axis angle -> piston-directivity first null -> high-frequency corner
        const double a_cone = 0.12, dist = 0.025 + 0.575 * P_.micDistance;
        const double sinT = std::max(1e-3, std::sin(std::atan2(P_.micRadial * a_cone, dist)));
        const double fnull = 3.83 * 343.0 / (2 * M_PI * a_cone * sinT);
        const double fc = std::min(14000.0, std::max(2500.0, 1.6 * fnull));
        lpCoef_ = 1.0 - std::exp(-2 * M_PI * std::min(fc, 0.45 * fs_) / fs_);
        // far mic loses a little level (1/r relative to the 2.5 cm reference is NOT applied: loudness-normalised)
        distGain_ = 1.0;
    }

    double fs_ = 0, vprev_ = 0, lp1_ = 0, gain_ = 1.0, distGain_ = 1.0, lpCoef_ = 1.0, delaySamples_ = 20.0;
    int wp_ = 0;
    std::vector<double> dl_;
    Biquad cone_, pres_, dcb_;
    AcousticParams P_;
};

} // namespace cd57
