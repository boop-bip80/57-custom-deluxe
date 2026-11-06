// Oversampler.h -- linear-phase half-band cascade (dossier Sec. 23).
//
// Specification (measured by tools/validate_numerics.cpp, section B, and bounded by its tests):
//   * every 2x stage is a Kaiser-windowed half-band FIR designed for the HOST rate: passband 0 .. kPassbandHz,
//     stop-band starting at (stage-rate/2 - kPassbandHz), so that everything that can fold or image into
//     0 .. kPassbandHz is suppressed by at least kStopbandDb (design value; the test bound is lower);
//   * passband ripple is the Kaiser value 10^(-kStopbandDb/20);
//   * linear phase: the up and the down path each have a constant group delay.  Stage s runs at 2^(s+1) x fs and
//     contributes (L_s-1)/2 samples at that rate; an integer delay line at the internal rate pads the down path so
//     that latency() is an exact whole number of host samples at every factor (the host is told this number);
//   * the odd polyphase branch of a half-band is a pure delay, so each stage costs about L/2 multiplies per output
//     pair; up() and down() are allocation-free (scratch is sized in prepare()).
// Physical time constants stay in the circuit; only the integration step changes with the internal rate.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace cd57 {

constexpr double kPassbandHz = 20000.0;      // protected audio band
constexpr double kStopbandDb = 112.0;        // design stop-band attenuation of every stage (Kaiser estimate)

// One 2x half-band stage.  Taps h[0..L-1], L = 4M+3, centre c = 2M+1 (odd), h[c] = 1/2 exactly and
// h[c +- 2j] = 0 for j >= 1 is NOT the case: the non-zero taps are the centre and the even indices k = c +- odd.
class HalfbandStage {
public:
    // fs = host rate; stage s runs at rate 2^(s+1) * fs.  The transition band is made as wide as the protected band allows.
    void design(double hostRate, int stage, double stopbandDb)
    {
        const double r = hostRate * double(1 << (stage + 1));
        const double width = std::max(1000.0, r / 2.0 - 2.0 * kPassbandHz);         // Hz, symmetric about r/4 (host rates below 41 kHz are not supported)
        const double dw = 2.0 * M_PI * width / r;
        int L = (int)std::ceil((stopbandDb - 8.0) / (2.285 * dw)) + 1;
        L = std::max(L, 7);
        while (L % 4 != 3) ++L;
        L_ = L; c_ = (L_ - 1) / 2;
        const double beta = 0.1102 * (stopbandDb - 8.7);
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 80; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        std::vector<double> h(L_, 0.0);
        double others = 0.0;
        for (int k = 0; k < L_; ++k) {
            const int n = k - c_;
            if (n == 0 || n % 2 == 0) continue;
            const double r2 = 2.0 * k / (L_ - 1) - 1.0;
            const double w = i0(beta * std::sqrt(std::max(0.0, 1.0 - r2 * r2))) / i0(beta);
            h[k] = std::sin(M_PI * n / 2.0) / (M_PI * n) * w;
            others += h[k];
        }
        for (int k = 0; k < L_; ++k) if (k != c_) h[k] *= 0.5 / others;               // unity DC gain with an exact 1/2 centre tap
        h[c_] = 0.5;
        // even-index taps (pair branch) in polyphase order, doubled for the zero-stuffing gain of 2
        he_.clear();
        for (int k = 0; k < L_; k += 2) he_.push_back(2.0 * h[k]);
        hd_.clear();
        for (int k = 0; k < L_; k += 2) hd_.push_back(h[k]);
        m_ = (c_ - 1) / 2;                                                            // odd branch: pure delay of M samples
        const int hist = (int)he_.size();
        upHist_.assign(2 * (size_t)hist, 0.0); upPos_ = 0;
        dnE_.assign(2 * (size_t)hist, 0.0); dnO_.assign(2 * (size_t)(m_ + 2), 0.0); dnPos_ = dnPosO_ = 0;
        upM_.assign(2 * (size_t)(m_ + 1), 0.0); upMPos_ = 0;
    }
    int taps() const { return L_; }
    int centre() const { return c_; }           // group delay of one pass, in samples at the stage's output (up) / input (down) rate
    const std::vector<double>& evenTaps() const { return hd_; }

    void reset()
    {
        std::fill(upHist_.begin(), upHist_.end(), 0.0); std::fill(upM_.begin(), upM_.end(), 0.0);
        std::fill(dnE_.begin(), dnE_.end(), 0.0); std::fill(dnO_.begin(), dnO_.end(), 0.0);
        upPos_ = upMPos_ = dnPos_ = dnPosO_ = 0;
    }

    // one input sample -> two output samples (interpolation by 2, unity passband gain)
    void up(double x, double& y0, double& y1)
    {
        const int J = (int)he_.size();
        upPos_ = (upPos_ + 1) % J;
        upHist_[upPos_] = upHist_[upPos_ + J] = x;
        const double* p = &upHist_[upPos_ + J];                 // p[-j] = x[i-j]
        double a = 0.0;
        for (int j = 0; j < J; ++j) a += he_[j] * p[-j];
        const int K = m_ + 1;
        upMPos_ = (upMPos_ + 1) % K;
        upM_[upMPos_] = upM_[upMPos_ + K] = x;
        y0 = a;
        y1 = upM_[upMPos_ + K - m_];                            // x[i - M]
    }
    // two input samples (x0 then x1) -> one output sample aligned with x0 (decimation by 2)
    double down(double x0, double x1)
    {
        const int J = (int)hd_.size();
        dnPos_ = (dnPos_ + 1) % J;
        dnE_[dnPos_] = dnE_[dnPos_ + J] = x0;
        const int K = m_ + 2;
        dnPosO_ = (dnPosO_ + 1) % K;
        dnO_[dnPosO_] = dnO_[dnPosO_ + K] = x1;
        const double* p = &dnE_[dnPos_ + J];                    // p[-j] = E[m-j]
        double y = 0.0;
        for (int j = 0; j < J; ++j) y += hd_[j] * p[-j];
        y += 0.5 * dnO_[dnPosO_ + K - (m_ + 1)];                // O[m-M-1] * h[c]
        return y;
    }

private:
    int L_ = 0, c_ = 0, m_ = 0, upPos_ = 0, upMPos_ = 0, dnPos_ = 0, dnPosO_ = 0;
    std::vector<double> he_, hd_, upHist_, upM_, dnE_, dnO_;
};

class Oversampler {
public:
    // log2Factor: 0..5.  hostRate: the filters are designed for it (the transition band depends on the rate).
    void prepare(int log2Factor, double hostRate)
    {
        n_ = log2Factor;
        up_.assign(n_, HalfbandStage{});
        dn_.assign(n_, HalfbandStage{});
        double twiceUp = 0.0;                                 // 2 x (up delay) = sum over stages of (L_s-1)/2^(s+1), host samples
        for (int s = 0; s < n_; ++s) {
            up_[s].design(hostRate, s, kStopbandDb);
            dn_[s].design(hostRate, s, kStopbandDb);
            twiceUp += double(up_[s].taps() - 1) / double(1 << (s + 1));
        }
        const double total = std::ceil(twiceUp - 1e-9);
        padInternal_ = (int)std::lround((total - twiceUp) * double(1 << n_));            // whole internal-rate samples
        upDelay_ = 0.5 * twiceUp;
        downDelay_ = total - upDelay_;
        pad_.assign((size_t)padInternal_ + 1, 0.0); padPos_ = 0;
        const size_t chunk = (size_t)kChunk * (size_t)(1 << n_);
        bufA_.assign(chunk, 0.0); bufB_.assign(chunk, 0.0);
    }
    int factor() const { return 1 << n_; }
    // exact delays in host-rate samples; latency() is a whole number (the value reported to the host)
    double latencyUp() const { return upDelay_; }
    double latencyDown() const { return downDelay_; }
    double latency() const { return upDelay_ + downDelay_; }
    void reset()
    {
        for (auto& s : up_) s.reset();
        for (auto& s : dn_) s.reset();
        std::fill(pad_.begin(), pad_.end(), 0.0); padPos_ = 0;
    }

    // upsample n host samples into n*factor samples (any n; processed in chunks, no allocation)
    void up(const double* in, int n, double* out)
    {
        const int F = factor();
        if (n_ == 0) { std::copy(in, in + n, out); return; }
        for (int i0 = 0; i0 < n; i0 += kChunk) {
            const int m = std::min(kChunk, n - i0);
            double* a = bufA_.data(); double* b = bufB_.data();
            std::copy(in + i0, in + i0 + m, a);
            int len = m;
            for (int s = 0; s < n_; ++s) {
                for (int i = 0; i < len; ++i) up_[s].up(a[i], b[2 * i], b[2 * i + 1]);
                len *= 2; std::swap(a, b);
            }
            std::copy(a, a + (size_t)m * F, out + (size_t)i0 * F);
        }
    }
    // downsample n*factor samples into n host samples
    void down(const double* in, int n, double* out)
    {
        const int F = factor();
        if (n_ == 0) { std::copy(in, in + n, out); return; }
        for (int i0 = 0; i0 < n; i0 += kChunk) {
            const int m = std::min(kChunk, n - i0);
            double* a = bufA_.data(); double* b = bufB_.data();
            const double* src = in + (size_t)i0 * F;
            const int len0 = m * F;
            if (padInternal_ > 0) {
                const int P = padInternal_ + 1;
                for (int i = 0; i < len0; ++i) { pad_[padPos_] = src[i]; padPos_ = (padPos_ + 1) % P; a[i] = pad_[padPos_]; }
            } else {
                std::copy(src, src + len0, a);
            }
            int len = len0;
            for (int s = n_ - 1; s >= 0; --s) {
                for (int i = 0; i < len / 2; ++i) b[i] = dn_[s].down(a[2 * i], a[2 * i + 1]);
                len /= 2; std::swap(a, b);
            }
            std::copy(a, a + m, out + i0);
        }
    }

private:
    static constexpr int kChunk = 256;
    int n_ = 0, padInternal_ = 0, padPos_ = 0;
    double upDelay_ = 0.0, downDelay_ = 0.0;
    std::vector<HalfbandStage> up_, dn_;
    std::vector<double> pad_, bufA_, bufB_;
};

} // namespace cd57
