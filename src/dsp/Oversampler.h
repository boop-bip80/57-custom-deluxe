// Oversampler.h -- linear-phase half-band cascade (dossier Sec. 23).
//
// Stage 1 (fs -> 2fs) uses a sharp Kaiser half-band (~80 dB stop-band, 0.45..0.55 fs transition);
// further 2x stages see a signal already band-limited to fs/2, so a short, wide-transition filter suffices.
// Latency is reported exactly (dossier: "Report their latency and phase behavior").
// Physical time constants stay in the circuit; only the integration step changes with the internal rate.
#pragma once
#include <cmath>
#include <vector>

namespace cd57 {

class HalfbandStage {
public:
    // taps must be of the form 4M+3.
    void design(int taps, double stopbandDb)
    {
        L_ = taps;
        c_ = (L_ - 1) / 2;
        double beta;
        const double A = stopbandDb;
        if (A > 50) beta = 0.1102 * (A - 8.7);
        else if (A >= 21) beta = 0.5842 * std::pow(A - 21, 0.4) + 0.07886 * (A - 21);
        else beta = 0.0;
        h_.assign(L_, 0.0);
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 60; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        double sum = 0.0;
        for (int k = 0; k < L_; ++k) {
            const double n = k - c_;
            const double sinc = (n == 0) ? 1.0 : std::sin(M_PI * n / 2.0) / (M_PI * n / 2.0);
            const double r = 2.0 * k / (L_ - 1) - 1.0;
            const double w = i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta);
            h_[k] = 0.5 * sinc * w;
            sum += h_[k];
        }
        for (auto& v : h_) v /= sum;              // unity DC gain; interpolation applies the x2 for zero-stuffing
        hist_.assign(L_, 0.0); pos_ = 0;
        histD_.assign(L_, 0.0); posD_ = 0;
        // polyphase split
        he_.clear(); ho_.clear();
        for (int k = 0; k < L_; ++k) (k % 2 == 0 ? he_ : ho_).push_back(2.0 * h_[k]);
    }
    int taps() const { return L_; }
    int delayAtHighRate() const { return c_; }       // each of up/down

    void reset() { std::fill(hist_.begin(), hist_.end(), 0.0); std::fill(histD_.begin(), histD_.end(), 0.0); pos_ = posD_ = 0; }

    // one input sample -> two output samples (interpolation by 2, unity passband gain)
    void up(double x, double& y0, double& y1)
    {
        // history holds the last L_/2+1 inputs
        pos_ = (pos_ + 1) % L_;
        hist_[pos_] = x;
        double a = 0.0, b = 0.0;
        for (size_t j = 0; j < he_.size(); ++j) a += he_[j] * hist_[(pos_ - (int)j + L_) % L_];
        for (size_t j = 0; j < ho_.size(); ++j) b += ho_[j] * hist_[(pos_ - (int)j + L_) % L_];
        y0 = a; y1 = b;
    }
    // two input samples (x0 then x1) -> one output sample (decimation by 2)
    double down(double x0, double x1)
    {
        posD_ = (posD_ + 2) % L_;
        histD_[(posD_ - 1 + L_) % L_] = x0;
        histD_[posD_] = x1;
        double y = 0.0;
        for (int k = 0; k < L_; ++k) y += h_[k] * histD_[(posD_ - k + L_) % L_];
        return y;
    }

private:
    int L_ = 0, c_ = 0, pos_ = 0, posD_ = 0;
    std::vector<double> h_, hist_, histD_, he_, ho_;
};

class Oversampler {
public:
    void prepare(int log2Factor)
    {
        n_ = log2Factor;
        stages_.assign(n_, HalfbandStage{});
        stagesDown_.assign(n_, HalfbandStage{});
        for (int s = 0; s < n_; ++s) {
            const int taps = (s == 0) ? 63 : 23;
            const double A = (s == 0) ? 80.0 : 60.0;
            stages_[s].design(taps, A);
            stagesDown_[s].design(taps, A);
        }
        buf_[0].clear(); buf_[1].clear();
    }
    int factor() const { return 1 << n_; }
    // exact group delay in base-rate samples (up + down)
    double latency() const
    {
        // stage s runs at rate 2^(s+1)*fs: up delay (L-1)/2 and down delay (L-1)/2 samples at that rate
        double d = 0.0;
        for (int s = 0; s < n_; ++s) d += double(stages_[s].taps() - 2) / double(1 << (s + 1));   // decimator aligns to the newer sample of each pair
        return d;
    }
    void reset() { for (auto& s : stages_) s.reset(); for (auto& s : stagesDown_) s.reset(); }

    // upsample n base samples into n*factor samples
    void up(const double* in, int n, double* out)
    {
        const int F = factor();
        if (n_ == 0) { for (int i = 0; i < n; ++i) out[i] = in[i]; return; }
        tmpA_.assign((size_t)n * F, 0.0); tmpB_.assign((size_t)n * F, 0.0);
        std::vector<double>* src = &tmpA_; std::vector<double>* dst = &tmpB_;
        for (int i = 0; i < n; ++i) (*src)[i] = in[i];
        int len = n;
        for (int s = 0; s < n_; ++s) {
            for (int i = 0; i < len; ++i) { double a, b; stages_[s].up((*src)[i], a, b); (*dst)[2 * i] = a; (*dst)[2 * i + 1] = b; }
            len *= 2; std::swap(src, dst);
        }
        for (int i = 0; i < len; ++i) out[i] = (*src)[i];
    }
    // downsample n*factor samples into n base samples
    void down(const double* in, int n, double* out)
    {
        const int F = factor();
        if (n_ == 0) { for (int i = 0; i < n; ++i) out[i] = in[i]; return; }
        tmpA_.assign((size_t)n * F, 0.0); tmpB_.assign((size_t)n * F, 0.0);
        std::vector<double>* src = &tmpA_; std::vector<double>* dst = &tmpB_;
        for (int i = 0; i < n * F; ++i) (*src)[i] = in[i];
        int len = n * F;
        for (int s = n_ - 1; s >= 0; --s) {
            for (int i = 0; i < len / 2; ++i) (*dst)[i] = stagesDown_[s].down((*src)[2 * i], (*src)[2 * i + 1]);
            len /= 2; std::swap(src, dst);
        }
        for (int i = 0; i < n; ++i) out[i] = (*src)[i];
    }

private:
    int n_ = 0;
    std::vector<HalfbandStage> stages_, stagesDown_;
    std::vector<double> buf_[2], tmpA_, tmpB_;
};

} // namespace cd57
