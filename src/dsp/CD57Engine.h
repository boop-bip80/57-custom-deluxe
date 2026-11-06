// CD57Engine.h -- plugin-agnostic engine: calibrated I/O around the circuit-level amplifier.
//
// Boundary (dossier Sec. 01/05/20/24): digital input -> jack EMF [V] (K_in) -> circuit -> speaker-terminal voltage [V]
// (calibrated by K_out) and, separately, a provisional acoustic operator driven by CONE VELOCITY.
// Physical state (capacitor charge, rails, flux, speaker heat) persists across blocks (Sec. 23).
#pragma once
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "Acoustic.h"
#include "AmpCircuit.h"
#include "Manifest.h"
#include "Oversampler.h"

namespace cd57 {

struct MarketVariant { const char* name; double volts, hz; };
// dossier Sec. 02 replacement/market table (the published document's set, not a promise each is sold)
inline const MarketVariant* marketVariants()
{
    static const MarketVariant v[] = {
        {"US 120 V / 60 Hz (8150500100)", 120.0, 60.0}, {"Taiwan 110 V / 60 Hz (8150501100)", 110.0, 60.0},
        {"Australia 240 V / 50 Hz (8150503100)", 240.0, 50.0}, {"UK/EU 230 V / 50 Hz (8150504100/-06100)", 230.0, 50.0},
        {"Argentina 220 V / 50 Hz (8150505100)", 220.0, 50.0}, {"Japan 100 V / 50 Hz (8150507100)", 100.0, 50.0},
        {"Japan 100 V / 60 Hz (8150507100)", 100.0, 60.0}, {"China 220 V / 50 Hz (8150508100)", 220.0, 50.0},
        {"Korea 220 V / 60 Hz (8150509100)", 220.0, 60.0}};
    return v;
}
constexpr int kMarketCount = 9;

struct EngineParams {
    // front panel (knob rotation 0..1)
    float instVol = 0.55f, micVol = 0.0f, tone = 0.65f;
    int jack = 1, patch = 0, load = 0;
    int jack2 = 0;                       // second plug: 0 none, 1 Instrument 1, 2 Instrument 2, 3 Mic 1, 4 Mic 2 (driven by process()'s second input, or terminated)
    bool power = true;
    bool standbyOn = true;                 // TRUE = play; FALSE = standby with heaters powered
    // plugin I/O calibration
    float inputTrimDb = 0.0f, outputDb = 0.0f;
    float inputCalVolts = 1.0f;          // peak volts at the jack for a 0 dBFS sample
    float outputRefVolts = 28.28f;       // peak speaker-terminal volts that reads 0 dBFS (rated 12.5 W sine = -6 dBFS)
    // supply / valves (stock: 5Y3, 12AY7, 12AX7)
    int market = 0;
    float linePercent = 100.0f;
    int v1 = 0, v2 = 0, rectifier = 0;
    int oversampleLog2 = 1;
    // source
    int sourceMode = 0;                  // 0 = line/DI (ideal EMF + R), 1 = passive pickup + cable
    float sourceR = 100.0f;
    // pot law
    int taper = 0;                       // 0 audio-15, 1 audio-10, 2 linear
    // character (all 0 = nominal circuit)
    float variation = 0.0f; int variationSeed = 1;
    float humDb = -200.0f;               // heater/ripple hum injection, dB re 1 uA (very low = off)
    float noise = 0.0f; int noiseSeed = 1;
    bool speakerHeating = true;
    bool otSaturation = false;           // optional lab feature: ASSUMED output-transformer saturation law (default OFF: linear core)
    // acoustic
    int outputMode = 1;                  // plugin output selector (0 provisional mic, 1 terminals, 2 user IR, 3 user IR on cone motion); unused by the engine
    float micRadial = 0.15f, micDistance = 0.10f;
};

// Largest peak voltage at the instrument jack the solver is validated for (a pickup gives well under 2 V; a hot line-level reamp
// signal a few volts).  Beyond ~30 V the stage-1 grid network is driven so far into conduction that unrecovered solver steps appear.
constexpr double kMaxJackVolts = 30.0;

struct EngineDiag {
    uint64_t steps = 0, iterations = 0, failures = 0, substepped = 0, panics = 0;
    uint64_t nonFiniteInputs = 0, clampedInputs = 0;   // host samples that were NaN/Inf (treated as 0) or beyond the supported jack voltage
    double maxResidual = 0.0;
    double meanIterations() const { return steps ? double(iterations) / double(steps) : 0.0; }
    float coilTempRise = 0.f, b1 = 0.f, b2 = 0.f, b3 = 0.f, cathode = 0.f;
};

class CD57Engine {
public:
    // manifestText: parameter manifest JSON (dossier Sec. 30).  Returns via manifestError() if it was rejected;
    // a rejected manifest falls back to the compiled-in defaults and the failure is reported, never ignored.
    void prepare(double sampleRate, int maxBlock, const std::string& netlistText, const EngineParams& p, const std::string& manifestText = std::string())
    {
        manifestError_.clear(); manifestOk_ = false;
        if (!manifestText.empty()) { manifestError_ = manifest_.load(manifestText); manifestOk_ = manifestError_.empty(); }
        // manifest-derived circuit values are resolved ONCE here (string-keyed lookups allocate); makeConfig() on the audio thread
        // then only copies this and applies the plugin controls
        baseCfg_ = AmpConfig{};
        if (manifestOk_) {
            // a measured pot map that is not a valid map is a manifest error, reported exactly like a rejected manifest
            const std::string applyErr = manifest_.apply(baseCfg_);
            if (!applyErr.empty()) { manifestError_ = applyErr; manifestOk_ = false; baseCfg_ = AmpConfig{}; }
        }
        sr_ = sampleRate;
        maxBlock_ = maxBlock;
        params_ = p;
        netlist_ = netlistText;
        os_.prepare(p.oversampleLog2, sr_); osCone_.prepare(p.oversampleLog2, sr_); osCur_.prepare(p.oversampleLog2, sr_); os2_.prepare(p.oversampleLog2, sr_);
        const int F = os_.factor();
        inD_.assign(maxBlock, 0.0); upD_.assign((size_t)maxBlock * F, 0.0); in2D_.assign(maxBlock, 0.0); up2D_.assign((size_t)maxBlock * F, 0.0);
        vt_.assign((size_t)maxBlock * F, 0.0); vc_.assign((size_t)maxBlock * F, 0.0); ic_.assign((size_t)maxBlock * F, 0.0);
        vtB_.assign(maxBlock, 0.0); vcB_.assign(maxBlock, 0.0); icB_.assign(maxBlock, 0.0);
        build(p);
    }

    // rebuild the circuit (sample-rate / oversampling / structural change).  Resets physical state to quiescent.
    void build(const EngineParams& p)
    {
        params_ = p;
        os_.prepare(p.oversampleLog2, sr_); osCone_.prepare(p.oversampleLog2, sr_); osCur_.prepare(p.oversampleLog2, sr_); os2_.prepare(p.oversampleLog2, sr_);
        amp_.cfg = makeConfig(p);
        amp_.loadNetlist(netlist_);
        const double fsInt = sr_ * os_.factor();
        amp_.setControls(makeControls(p));
        amp_.prepare(fsInt, 0.5);
        amp_.initialiseQuiescent(0.15);
        acoustic_.prepare(sr_);
        acoustic_.set(makeAcoustic(p));
        calibrateAcoustic();
        coilDeltaT_ = 0.0;
        coilAcc_ = 0.0; coilCnt_ = 0;
        ctr_ = 0;
        diagBase_ = amp_.network().stats;
        applied_ = p;
        const int F = os_.factor();
        if ((size_t)maxBlock_ * F != upD_.size()) {
            upD_.assign((size_t)maxBlock_ * F, 0.0); vt_.assign((size_t)maxBlock_ * F, 0.0); vc_.assign((size_t)maxBlock_ * F, 0.0); ic_.assign((size_t)maxBlock_ * F, 0.0); up2D_.assign((size_t)maxBlock_ * F, 0.0);
        }
    }

    // Carry the physical state (rails, capacitor charges, transformer flux, voice-coil temperature, mains phase) over from a running
    // engine of the same sample rate but possibly another oversampling factor, so an oversampling change does not reset the amplifier
    // to its quiescent state.  The resampler filters and the acoustic operator restart (a glitch of about one filter latency).
    // Real-time safe: no allocation.  Returns false (nothing transferred) if the engines are not compatible.
    bool adoptStateFrom(const CD57Engine& o)
    {
        if (std::fabs(o.sr_ - sr_) > 1e-9 || !amp_.adoptStateFrom(o.amp_)) return false;
        coilDeltaT_ = o.coilDeltaT_;
        amp_.setVoiceCoilResistance(amp_.cfg.speaker.Re * (1.0 + amp_.cfg.speaker.alphaT * coilDeltaT_));
        coilAcc_ = 0.0; coilCnt_ = 0;
        return true;
    }

    // Warm the up-sampler's filter history with the most recent host-rate input (oldest sample first), without touching the circuit, so a
    // freshly built engine that takes over a running one does not see the input fall to zero for one filter length.  Real-time safe.
    void primeInput(const float* in, int n)
    {
        const double kIn = (double)applied_.inputCalVolts * std::pow(10.0, (double)applied_.inputTrimDb / 20.0);
        for (int pos = 0; pos < n; pos += maxBlock_) {
            const int m = std::min(maxBlock_, n - pos);
            for (int i = 0; i < m; ++i) { const double x = (double)in[pos + i]; inD_[(size_t)i] = std::isfinite(x) ? x * kIn : 0.0; }
            os_.up(inD_.data(), m, upD_.data());
        }
    }

    void setParams(const EngineParams& p) { pending_ = p; hasPending_ = true; }
    int latencySamples() const { return (int)std::lround(os_.latency()); }          // exact: the cascade pads its delay to a whole number of samples
    double sampleRate() const { return sr_; }
    const Manifest& manifest() const { return manifest_; }
    const std::string& manifestError() const { return manifestError_; }
    AmpCircuit& amp() { return amp_; }
    const EngineParams& params() const { return applied_; }
    // Last process() block at the host rate (first n entries): cone velocity [m/s] for the hybrid acoustic route (dossier
    // Sec. 27), and the output scale kOut that maps terminal volts to output units (identical to the one process() applies).
    const std::vector<double>& coneVelocityBlock() const { return vcB_; }
    // Speaker-terminal CURRENT [A] of the last process() block at the host rate (dossier Sec. 05/25/31: expose terminal voltage AND current).
    // Total current into the load at the terminals (voice coil + dummy + extension), decimated by its own resampler.  Off by default so the
    // plugin pays nothing; the offline tools switch it on.  Same latency as the voltage output.
    void setCaptureTerminalCurrent(bool on) { captureCurrent_ = on; }
    const std::vector<double>& terminalCurrentBlock() const { return icB_; }
    double outputScale() const { return std::pow(10.0, (double)applied_.outputDb / 20.0) / (double)applied_.outputRefVolts; }

    EngineDiag diag() const
    {
        EngineDiag d;
        const auto& s = amp_.network().stats;
        d.steps = s.steps - diagBase_.steps; d.iterations = s.iterations - diagBase_.iterations;
        d.failures = s.failures - diagBase_.failures; d.substepped = s.substepped - diagBase_.substepped;
        d.panics = panics_; d.nonFiniteInputs = nonFinite_; d.clampedInputs = clamped_; d.maxResidual = s.maxResidual; d.coilTempRise = (float)coilDeltaT_;
        d.b1 = (float)amp_.nodeV("b1"); d.b2 = (float)amp_.nodeV("b2"); d.b3 = (float)amp_.nodeV("b3"); d.cathode = (float)amp_.nodeV("k34");
        return d;
    }

    void reset()
    {
        os_.reset(); osCone_.reset(); osCur_.reset(); os2_.reset(); acoustic_.reset();
        amp_.initialiseQuiescent(0.1);
        coilDeltaT_ = 0.0; amp_.setVoiceCoilResistance(params_.speakerHeating ? amp_.cfg.speaker.Re : amp_.cfg.speaker.Re);
    }

    // mono in -> terminal-referred output and provisional-acoustic output (both calibrated, n <= maxBlock)
    void process(const float* in, float* outTerm, float* outAcoustic, int n) { process(in, nullptr, outTerm, outAcoustic, n); }

    // Same, with a SECOND source (in2, may be null) feeding the second plug selected by EngineParams::jack2.  With jack2 == 0 in2 is ignored; with jack2 set and
    // in2 null the plug is a cable terminated in the source impedance (a guitar at rest).  Both inputs are calibrated by the same K_in.
    void process(const float* in, const float* in2, float* outTerm, float* outAcoustic, int n)
    {
        if (hasPending_) { applyPending(); hasPending_ = false; }
        const int F = os_.factor();
        const double kIn = (double)applied_.inputCalVolts * std::pow(10.0, (double)applied_.inputTrimDb / 20.0);
        const double kOut = std::pow(10.0, (double)applied_.outputDb / 20.0) / (double)applied_.outputRefVolts;
        for (int i = 0; i < n; ++i) {
            // boundary protection: a NaN/Inf host sample must not poison the circuit state (it would trigger an emergency
            // re-initialisation on the audio thread), and the solver is only validated up to kMaxJackVolts at the jack
            double x = (double)in[i];
            if (!std::isfinite(x)) { x = 0.0; ++nonFinite_; }
            x *= kIn;
            if (x > kMaxJackVolts) { x = kMaxJackVolts; ++clamped_; } else if (x < -kMaxJackVolts) { x = -kMaxJackVolts; ++clamped_; }
            inD_[i] = x;
        }
        os_.up(inD_.data(), n, upD_.data());
        const bool two = in2 != nullptr && applied_.jack2 >= 1;
        if (two) {
            for (int i = 0; i < n; ++i) {
                double x = (double)in2[i];
                if (!std::isfinite(x)) { x = 0.0; ++nonFinite_; }
                x *= kIn;
                if (x > kMaxJackVolts) { x = kMaxJackVolts; ++clamped_; } else if (x < -kMaxJackVolts) { x = -kMaxJackVolts; ++clamped_; }
                in2D_[i] = x;
            }
            os2_.up(in2D_.data(), n, up2D_.data());
        }
        const int N = n * F;
        const double hInt = 1.0 / (sr_ * F);
        coilInterval_ = (int64_t)(256) * F;           // fixed interval: output is independent of the host block size
        bool bad = false;
        for (int i = 0; i < N; ++i) {
            if ((ctr_++ & 63) == 0) { amp_.applyControls(false); }
            amp_.tick2(upD_[i], two ? up2D_[i] : 0.0);
            const double vt = amp_.vTerminal();
            vt_[i] = vt; vc_[i] = amp_.coneVelocity();
            if (captureCurrent_) ic_[i] = amp_.terminalCurrent();
            const double ic = amp_.speakerCurrent();
            coilAcc_ += ic * ic * hInt; coilCnt_ += 1;
            if (coilCnt_ >= coilInterval_) stepThermal(hInt);
            if (!(std::fabs(vt) < 5e3)) { bad = true; break; }
        }
        if (bad) { recover(); for (int i = 0; i < n; ++i) { outTerm[i] = 0.f; outAcoustic[i] = 0.f; } return; }
        os_.down(vt_.data(), n, vtB_.data());
        osCone_.down(vc_.data(), n, vcB_.data());       // separate decimator: filter history is per-signal
        if (captureCurrent_) osCur_.down(ic_.data(), n, icB_.data());
        for (int i = 0; i < n; ++i) {
            outTerm[i] = (float)(kOut * vtB_[i]);
            outAcoustic[i] = (float)(kOut * acoustic_.process(vcB_[i]));
        }
        if (!applied_.speakerHeating && coilDeltaT_ != 0.0) { coilDeltaT_ = 0.0; amp_.setVoiceCoilResistance(amp_.cfg.speaker.Re); }
    }

    // linear cone-velocity response per terminal volt at frequency f (speaker electromechanical model)
    static std::complex<double> coneVelocityPerVolt(const SpeakerTS& s, double f)
    {
        using C = std::complex<double>;
        const double w = 2 * M_PI * f, Bl = s.Bl();
        const double Res = Bl * Bl / s.Rms(), Cm = s.Mms / (Bl * Bl), Lc = s.Cms() * Bl * Bl;
        const C Y = C(1.0 / Res, 0) + C(0, w * Cm) + C(0, -1.0 / (w * Lc));
        const C Zm = 1.0 / Y, Ze = C(s.Re, w * s.Le);
        return (Zm / (Ze + Zm)) / Bl;
    }

private:
    AmpConfig makeConfig(const EngineParams& p) const
    {
        AmpConfig c = baseCfg_;
        const MarketVariant& m = marketVariants()[std::min(std::max(p.market, 0), kMarketCount - 1)];
        c.mainsHz = m.hz;
        c.lineScale = (double)p.linePercent / 100.0;
        c.v1Type = p.v1 == 0 ? "12AY7" : "12AX7";
        c.v2Type = p.v2 == 0 ? "12AX7" : "12AY7";
        c.rectifier = p.rectifier == 0 ? RectifierType::Tube5Y3 : RectifierType::Silicon;
        c.pickupModel = p.sourceMode == 1;
        c.srcR = p.sourceR;
        // a MEASURED map from the manifest wins over the plugin's assumed taper choice (dossier Sec. 08)
        if (!c.volumeMapMeasured) c.volumeMap.taper = p.taper == 0 ? PotTaper::Audio15 : (p.taper == 1 ? PotTaper::Audio10 : PotTaper::Linear);
        if (!c.toneMapMeasured) c.toneMap.taper = PotTaper::Linear;
        c.variationAmount = p.variation; c.variationSeed = (uint32_t)p.variationSeed;
        c.humCurrent = p.humDb > -150.0f ? 1e-6 * std::pow(10.0, (double)p.humDb / 20.0) : 0.0;
        c.noiseScale = p.noise; c.noiseSeed = (uint32_t)p.noiseSeed;
        c.otSaturation = p.otSaturation;
        return c;
    }
    static AmpControls makeControls(const EngineParams& p)
    {
        AmpControls c;
        c.instVol = p.instVol; c.micVol = p.micVol; c.tone = p.tone;
        c.guitarJack = p.jack; c.guitarJack2 = p.jack2; c.patch = p.patch; c.load = (LoadMode)p.load; c.power = p.power; c.standbyOn = p.standbyOn;
        return c;
    }
    static AcousticParams makeAcoustic(const EngineParams& p)
    {
        AcousticParams a; a.micRadial = p.micRadial; a.micDistance = p.micDistance; return a;
    }
    void calibrateAcoustic()
    {
        const auto g = coneVelocityPerVolt(amp_.cfg.speaker, 1000.0);
        acoustic_.calibrate1k(std::abs(g));
    }

    void applyPending()
    {
        const EngineParams& n = pending_;
        const EngineParams& o = applied_;
        const bool structural = n.oversampleLog2 != o.oversampleLog2;
        if (structural) { build(n); return; }
        const bool cfgChanged = n.market != o.market || n.linePercent != o.linePercent || n.v1 != o.v1 || n.v2 != o.v2 ||
            n.rectifier != o.rectifier || n.sourceMode != o.sourceMode || n.sourceR != o.sourceR || n.taper != o.taper ||
            n.variation != o.variation || n.variationSeed != o.variationSeed || n.humDb != o.humDb || n.noise != o.noise ||
            n.noiseSeed != o.noiseSeed || n.otSaturation != o.otSaturation;
        applied_ = n;
        amp_.setControls(makeControls(n));
        if (cfgChanged) { amp_.cfg = makeConfig(n); amp_.reconfigure(); }
        if (n.micRadial != o.micRadial || n.micDistance != o.micDistance) { acoustic_.set(makeAcoustic(n)); }
    }

    // speaker heating: C_th dT/dt = i^2 R_e(T) - T/R_th  (single compartment, dossier Sec. 18, PROPOSED)
    void stepThermal(double hInt)
    {
        const SpeakerTS& s = amp_.cfg.speaker;
        const double dt = coilCnt_ * hInt;
        if (applied_.speakerHeating && dt > 0.0) {
            const double re = s.Re * (1.0 + s.alphaT * coilDeltaT_);
            const double i2 = coilAcc_ / dt;
            coilDeltaT_ += dt / s.Cth * (i2 * re - coilDeltaT_ / s.Rth);
            coilDeltaT_ = std::min(std::max(coilDeltaT_, 0.0), 250.0);
            amp_.setVoiceCoilResistance(s.Re * (1.0 + s.alphaT * coilDeltaT_));
        }
        coilAcc_ = 0.0; coilCnt_ = 0;
    }

    void recover()
    {
        ++panics_;
        os_.reset(); osCone_.reset(); osCur_.reset(); os2_.reset(); acoustic_.reset();
        amp_.initialiseQuiescent(0.1);
    }

    double sr_ = 48000; int maxBlock_ = 0;
    std::string netlist_;
    Manifest manifest_; std::string manifestError_; bool manifestOk_ = false;
    AmpConfig baseCfg_;
    EngineParams params_, applied_, pending_;
    bool hasPending_ = false;
    AmpCircuit amp_;
    Oversampler os_, osCone_, osCur_, os2_;
    AcousticOperator acoustic_;
    std::vector<double> inD_, upD_, vt_, vc_, vtB_, vcB_, ic_, icB_, in2D_, up2D_;
    bool captureCurrent_ = false;
    uint64_t ctr_ = 0, panics_ = 0, nonFinite_ = 0, clamped_ = 0;
    double coilDeltaT_ = 0.0, coilAcc_ = 0.0; int64_t coilCnt_ = 0, coilInterval_ = 256;
    Network::Stats diagBase_;
};

} // namespace cd57
