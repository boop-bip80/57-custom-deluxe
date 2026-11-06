// AmpCircuit.h -- the '57 Custom Deluxe circuit: netlist + physical controls + I/O ports.
//
// Everything electrical lives in data/netlist/cd57_nominal.net.  This class only
//   * binds named netlist elements to physical controls (pot fractions, jack switches, speaker, mains),
//   * injects the source and the mains as Norton currents,
//   * exposes the model boundary of dossier Sec. 05/20:  jack EMF  ->  speaker-terminal voltage/current,
//     plus cone velocity for the acoustic stage.
// Physical units are volts/amps/seconds inside (dossier Sec. 24).
#pragma once
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <string>

#include "Network.h"

namespace cd57 {

constexpr double kPi = 3.14159265358979323846;

enum class PotTaper { Linear, Audio15, Audio10, Custom };

struct PotMap {
    PotTaper taper = PotTaper::Audio15;      // ASSUMED: modern taper numerics UNKNOWN (dossier Sec. 08/28)
    std::array<double, 11> custom{};         // alpha at knob 0,0.1,...,1.0 when taper == Custom (measured map)
    double endpointResistance = 20.0;        // documented small contact resistance (dossier Sec. 08)

    // alpha(x): fraction of track from the grounded/low end to the wiper, x = knob rotation 0..1
    double alpha(double x) const
    {
        x = std::min(1.0, std::max(0.0, x));
        switch (taper) {
            case PotTaper::Linear: return x;
            case PotTaper::Audio15: return (std::exp(3.4657 * x) - 1.0) / (std::exp(3.4657) - 1.0);   // alpha(0.5)=0.15
            case PotTaper::Audio10: return (std::exp(4.3944 * x) - 1.0) / (std::exp(4.3944) - 1.0);   // alpha(0.5)=0.10
            case PotTaper::Custom: {
                const double p = x * 10.0; int i = std::min(9, (int)p); const double f = p - i;
                return custom[i] * (1 - f) + custom[i + 1] * f;
            }
        }
        return x;
    }
};

struct SpeakerTS {                           // ASSUMED generic 12" alnico guitar driver (PN 7708599000 params UNKNOWN)
    double Re = 6.4, Le = 0.7e-3;
    double fs = 98.0, Qms = 5.5, Qes = 1.0, Mms = 0.0195;   // kg
    double Sd = 0.050;                                       // m^2
    double alphaT = 0.00393, Cth = 8.0, Rth = 6.0;           // copper tempco, J/K, K/W (thermal compartment, PROPOSED)
    double Bl() const { const double w0 = 2 * kPi * fs; return std::sqrt(w0 * Mms * Re / Qes); }
    double Rms() const { const double w0 = 2 * kPi * fs; return w0 * Mms / Qms; }
    double Cms() const { const double w0 = 2 * kPi * fs; return 1.0 / (w0 * w0 * Mms); }
};

enum class LoadMode { Speaker = 0, SpeakerPlusExt8 = 1, Dummy8 = 2, Dummy4 = 3, Dummy16 = 4 };

struct AmpConfig {
    // mains / PT (all ASSUMED, dossier Sec. 28: "Supply transformer parameters" UNKNOWN)
    double mainsHz = 60.0;
    double ptHalfVrms = 377.4;             // HV half-winding RMS at nominal line: CALIBRATED (cd57_validate --calibrate-pt) to the documented 12.5 W @ 5% THD; manifest interval 330-400 V
    double ptRs = 90.0;                      // per-half series resistance
    double lineScale = 1.0;                  // line voltage relative to nominal (Variac)
    RectifierType rectifier = RectifierType::Tube5Y3;
    std::string v1Type = "12AY7", v2Type = "12AX7";
    bool otSaturation = false;               // optional lab feature: ASSUMED output-transformer saturation law (default OFF, dossier Sec. 12/29)
    // output transformer, driven by the parameter manifest (dossier Sec. 30: the manifest is the machine-readable source of truth).  NaN = keep the
    // netlist value.  All are referred as the manifest entry names say; the netlist split between windings is kept in proportion.
    double otRatioFull = std::nan("");       // full-primary turns ratio n = N_full / N_secondary (= sqrt(Zp / Zs))
    double otLprimFull = std::nan("");       // H, small-signal magnetising inductance of the full primary, secondary open:  N_full^2 (P0 + Pair)
    double otLeakFullRef = std::nan("");     // H, total leakage referred to the full primary:  Lla + Llb + n^2 Lls
    double otDcrPrimHalf = std::nan("");     // ohm, DC resistance of ONE primary half
    double otDcrSec = std::nan("");          // ohm
    double otSatFlux = std::nan("");         // V*s per secondary turn (used only when the optional saturation law is switched on)
    double potTotal = 1.0e6;
    PotMap volumeMap, toneMap;
    // A MEASURED pot map from the manifest (Sec. 08) overrides the assumed taper selection of the plugin.  The Instrument and Mic volumes can be
    // measured separately (micVolumeMap is used only when micMapMeasured is set, otherwise both volumes share volumeMap).
    PotMap micVolumeMap;
    bool volumeMapMeasured = false, micMapMeasured = false, toneMapMeasured = false;
    SpeakerTS speaker;
    // source model
    bool pickupModel = false;
    double srcR = 100.0, srcL = 1e-6, cableC = 20e-12;
    double pickupR = 6000.0, pickupL = 2.5, pickupCable = 500e-12;
    // optional creative variation (default off => nominal netlist values)
    double variationAmount = 0.0;
    uint32_t variationSeed = 1;
    // optional noise/hum (deterministic hum is separate from seeded stochastic noise; dossier Sec. 13)
    double humCurrent = 0.0;                 // A peak injected at V1 cathode (heater-cathode leakage proxy)
    double noiseScale = 0.0;                 // 0 = noise-free circuit (independently testable)
    uint32_t noiseSeed = 1;
};

struct AmpControls {
    double instVol = 0.7, micVol = 0.0, tone = 0.7;   // knob rotation 0..1
    int guitarJack = 1;                      // 0 none, 1 Instrument1, 2 Instrument2, 3 Mic1, 4 Mic2
    int guitarJack2 = 0;                     // SECOND plug (a second guitar, or a cable terminated in the source impedance): 0 none, 1..4 as above.  Same jack as the
                                             // first plug is ignored (one jack holds one plug).
    int patch = 0;                           // 0 none, 1 I1-I2, 2 M1-M2, 3 I1-M1, 4 I1-M2, 5 I2-M1, 6 I2-M2
    LoadMode load = LoadMode::Speaker;
    bool power = true;
};

class AmpCircuit {
public:
    AmpConfig cfg;

    void loadNetlist(const std::string& text)
    {
        net_ = std::make_unique<Network>();
        net_->parse(text);
        bind();
    }

    // Prepare for internal sample rate (Hz) and integration method (theta 0.5 = trapezoidal, 1 = backward Euler)
    void prepare(double internalRate, double theta = 0.5)
    {
        fsInt_ = internalRate;
        theta_ = theta;
        net_->useKernel = useKernel_;
        net_->build(1.0 / internalRate, theta);
        applyConfig();
        mainsGain_ = mainsTarget_ = target_.power ? 1.0 : 0.0;
        emTube_ = emRect_ = target_.power ? 1.0 : 0.0;
        smoothCoef_ = 1.0 - std::exp(-1.0 / (0.020 * fsInt_ / 64.0));     // ~20 ms @ one call per 64 internal samples
        if (smoothCoef_ <= 0) smoothCoef_ = 0.2;
        applyControls(true);
        net_->refreshLinear();
    }

    // Solve the quiescent state: DC point with a DC-equivalent rectifier, then short explicit transients that
    // re-centre the rectifier equivalent on the true mean B+1 (so every config starts near its steady state),
    // then a final warm-up so the explicit rectifier/ripple settles (dossier Sec. 11 "controlled startup
    // trajectory": the initialisation is NOT an audible power-on event).
    bool initialiseQuiescent(double warmupSeconds = 0.15)
    {
        Network& n = *net_;
        double vdc = dcGuessFrac * std::sqrt(2.0) * cfg.ptHalfVrms * cfg.lineScale;
        bool ok = true;
        const int b1 = n.nodeId("b1");
        auto dcSolve = [&](double v) {
            std::fill(n.injVec().begin(), n.injVec().end(), 0.0);
            n.inj(ra_) = v / cfg.ptRs; n.inj(rb_) = v / cfg.ptRs;
            for (int g = 0; g < 3; ++g) n.setEmission(g, 1.0);
            bool r = n.solveDC();
            std::fill(n.injVec().begin(), n.injVec().end(), 0.0);
            return r;
        };
        auto runFor = [&](double sec, double* meanB1) {
            const int steps = std::max(1, (int)(sec * fsInt_));
            double acc = 0; int cnt = 0; bool r = true;
            for (int i = 0; i < steps; ++i) {
                r = tick(0.0) && r;
                if (meanB1 && i >= steps * 2 / 3) { acc += n.V(b1); ++cnt; }
            }
            if (meanB1) *meanB1 = cnt ? acc / cnt : 0.0;
            return r;
        };
        for (int pass = 0; pass < 2; ++pass) {
            ok = dcSolve(vdc) && ok;
            const double bdc = n.V(b1);
            phase_ = 0.0;
            double bm = 0.0;
            runFor(0.09, &bm);
            vdc += (bm - bdc);                       // B1 ~ Vdc - const : re-centre
        }
        ok = dcSolve(vdc) && ok;
        phase_ = 0.0;
        ok = runFor(warmupSeconds, nullptr) && ok;
        return ok;
    }

    // advance one internal step.  eEmf: source EMF in volts.  Returns false if the solver failed this step.
    bool tick(double eEmf, bool allowWarm = false) { (void)allowWarm; return tick2(eEmf, 0.0); }

    // advance one internal step with a SECOND source: eEmf2 drives the second plug (0 = a terminated cable: the plug and its source impedance are present,
    // the signal is not)
    bool tick2(double eEmf, double eEmf2)
    {
        Network& n = *net_;
        phase_ += 2.0 * kPi * cfg.mainsHz / fsInt_;
        if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
        const double e = mainsGain_ * std::sqrt(2.0) * cfg.ptHalfVrms * cfg.lineScale * std::sin(phase_);
        n.inj(ra_) = e / cfg.ptRs;
        n.inj(rb_) = -e / cfg.ptRs;
        double vin = eEmf;
        if (cfg.noiseScale > 0.0) vin += cfg.noiseScale * noiseDensityV_ * gauss_();
        n.inj(pu_) = vin * srcG_;
        n.inj(pu2_) = eEmf2 * srcG2_;
        if (cfg.humCurrent != 0.0) n.inj(k1_) = cfg.humCurrent * std::sin(phase_ * 1.0);
        updatePowerEnvelope();
        return n.step();
    }

    // ---- ports ---------------------------------------------------------------------------------
    double vTerminal() const { return net_->V(sp_); }
    // current into the voice coil (zero when the coil is disconnected, i.e. in the resistive dummy-load modes)
    double speakerCurrent() const { return (net_->V(sp_) - net_->V(sx_)) / rRe_; }
    // total current delivered into the load at the speaker terminals (dossier Sec. 05: expose terminal voltage AND current)
    double terminalCurrent() const { return speakerCurrent() + net_->V(sp_) / rDum_ + net_->V(sp_) / rExt_; }
    double coneVelocity() const { return net_->V(sy_) / Bl_; }
    double nodeV(const char* name) const { return net_->V(net_->nodeId(name)); }
    Network& network() { return *net_; }
    const Network& network() const { return *net_; }
    double bl() const { return Bl_; }

    // ---- control application ---------------------------------------------------------------------
    // Sets the *targets*; call applyControls() at control rate.  Pots are smoothed in the physical
    // fraction domain (dossier Sec. 23: "Smooth a control's physical parameter").
    void setControls(const AmpControls& c) { target_ = c; }

    void applyControls(bool immediate = false)
    {
        if (immediate) cur_ = target_;
        Network& n = *net_;
        const double a = immediate ? 1.0 : smoothCoef_;
        cur_.instVol += a * (target_.instVol - cur_.instVol);
        cur_.micVol += a * (target_.micVol - cur_.micVol);
        cur_.tone += a * (target_.tone - cur_.tone);
        auto snap = [](double& c, double t) { if (std::fabs(c - t) < 1e-5) c = t; };
        snap(cur_.instVol, target_.instVol); snap(cur_.micVol, target_.micVol); snap(cur_.tone, target_.tone);
        cur_.guitarJack = target_.guitarJack; cur_.guitarJack2 = target_.guitarJack2; cur_.patch = target_.patch; cur_.load = target_.load; cur_.power = target_.power;
        const double RT = cfg.potTotal;
        const double Rc = cfg.volumeMap.endpointResistance;
        auto setPot = [&](int lo, int hi, double alpha) {
            n.setR(lo, std::max(alpha * RT, Rc));
            n.setR(hi, std::max((1.0 - alpha) * RT, Rc));
        };
        setPot(R_vb_lo_, R_vb_hi_, cfg.volumeMap.alpha(cur_.instVol));
        setPot(R_vn_lo_, R_vn_hi_, (cfg.micMapMeasured ? cfg.micVolumeMap : cfg.volumeMap).alpha(cur_.micVol));
        // tone: beta = fraction from the 5 nF end to the wiper
        const double beta = cfg.toneMap.alpha(cur_.tone);
        setPot(R_t_b_, R_t_a_, beta);

        // jack states ------------------------------------------------------------------------
        const double ON = 0.02, OFF = 1e11;
        bool plugged[4] = {false, false, false, false};
        if (cur_.guitarJack >= 1 && cur_.guitarJack <= 4) plugged[cur_.guitarJack - 1] = true;
        const int j2 = (cur_.guitarJack2 >= 1 && cur_.guitarJack2 <= 4 && cur_.guitarJack2 != cur_.guitarJack) ? cur_.guitarJack2 : 0;     // one jack holds one plug
        if (j2) plugged[j2 - 1] = true;
        static const int pairA[7] = {0, 0, 2, 0, 0, 1, 1}, pairB[7] = {0, 1, 3, 2, 3, 2, 3};
        int patch = cur_.patch;
        if (patch > 0) {
            const int ja = pairA[patch], jb = pairB[patch];
            if (plugged[ja] || plugged[jb]) patch = 0;     // an end would collide with the guitar plug: ignored
            else { plugged[ja] = true; plugged[jb] = true; }
        }
        n.setR(R_sw_b1_, plugged[0] ? OFF : ON); n.setR(R_sw_b2_, plugged[1] ? OFF : ON);
        n.setR(R_sw_n1_, plugged[2] ? OFF : ON); n.setR(R_sw_n2_, plugged[3] ? OFF : ON);
        for (int j = 0; j < 4; ++j) { n.setR(R_sel_[j], (cur_.guitarJack == j + 1) ? ON : OFF); n.setR(R_sel2_[j], (j2 == j + 1) ? ON : OFF); }
        static const int ptIdx[7] = {-1, 0, 1, 2, 3, 4, 5};
        for (int p = 0; p < 6; ++p) n.setR(R_pt_[p], (patch > 0 && ptIdx[patch] == p) ? ON : OFF);

        // load ---------------------------------------------------------------------------------
        const double Rvc = voiceCoilR_;
        double rdum = OFF, rext = OFF, re = Rvc;
        switch (cur_.load) {
            case LoadMode::Speaker: break;
            case LoadMode::SpeakerPlusExt8: rext = 8.0; break;
            case LoadMode::Dummy8: rdum = 8.0; re = OFF; break;
            case LoadMode::Dummy4: rdum = 4.0; re = OFF; break;
            case LoadMode::Dummy16: rdum = 16.0; re = OFF; break;
        }
        n.setR(R_re_, re); n.setR(R_dum_, rdum); n.setR(R_ext_, rext);
        rRe_ = re; rDum_ = rdum; rExt_ = rext;
        load_g_ = 1.0 / (rdum) + 1.0 / rext;
        mainsTarget_ = cur_.power ? 1.0 : 0.0;
        n.refreshIfDirty();
    }

    // Re-apply cfg (tube types, rectifier, source chain, speaker, variation, noise) without disturbing the
    // physical state: capacitor charge and rail voltages carry over (dossier Sec. 23: state persistence).
    void reconfigure() { applyConfig(); applyControls(false); net_->refreshLinear(); }

    // Update voice-coil resistance from the thermal state (control-rate)
    void setVoiceCoilResistance(double Re)
    {
        voiceCoilR_ = Re;
        if (cur_.load == LoadMode::Speaker || cur_.load == LoadMode::SpeakerPlusExt8) { net_->setR(R_re_, Re); rRe_ = Re; net_->refreshLinear(); }
    }

    // Take over the running state of another circuit built from the same netlist (possibly at another internal rate): circuit state,
    // mains phase, power-switch / heater-emission envelopes, smoothed control positions and the voice-coil resistance.  The next
    // applyControls() call moves the pots from the carried positions toward this circuit's targets.
    bool adoptStateFrom(const AmpCircuit& o)
    {
        if (!net_ || !o.net_ || !net_->copyStateFrom(*o.net_)) return false;
        phase_ = o.phase_; mainsGain_ = o.mainsGain_; mainsTarget_ = o.mainsTarget_; emTube_ = o.emTube_; emRect_ = o.emRect_;
        cur_ = o.cur_;
        setVoiceCoilResistance(o.voiceCoilR_);
        applyControls(false);
        return true;
    }

    void setSmoothing(double coefPerCall) { smoothCoef_ = coefPerCall; }
    double powerEnvelope() const { return mainsGain_; }

    double dcGuessFrac = 0.86;
    void setUseKernel(bool b) { useKernel_ = b; }          // validation: compare the generated LU kernel with the generic path

private:
    static int idxOf(const std::vector<std::string>& v, const char* nm) { for (size_t i = 0; i < v.size(); ++i) if (v[i] == nm) return (int)i; return -1; }

    void bind()
    {
        Network& n = *net_;
        pu_ = n.nodeId("pu"); pu2_ = n.nodeId("pu2"); sp_ = n.nodeId("sp"); sx_ = n.nodeId("sx"); sy_ = n.nodeId("sy");
        ra_ = n.nodeId("ra"); rb_ = n.nodeId("rb"); k1_ = n.nodeId("k1");
        R_vb_lo_ = n.resId("Rvb_lo"); R_vb_hi_ = n.resId("Rvb_hi");
        R_vn_lo_ = n.resId("Rvn_lo"); R_vn_hi_ = n.resId("Rvn_hi");
        R_t_a_ = n.resId("Rt_a"); R_t_b_ = n.resId("Rt_b");
        R_sw_b1_ = n.resId("Sw_b1"); R_sw_b2_ = n.resId("Sw_b2"); R_sw_n1_ = n.resId("Sw_n1"); R_sw_n2_ = n.resId("Sw_n2");
        R_sel_[0] = n.resId("Rsel_b1"); R_sel_[1] = n.resId("Rsel_b2"); R_sel_[2] = n.resId("Rsel_n1"); R_sel_[3] = n.resId("Rsel_n2");
        const char* pt[6] = {"Rpt_b1b2", "Rpt_n1n2", "Rpt_b1n1", "Rpt_b1n2", "Rpt_b2n1", "Rpt_b2n2"};
        for (int i = 0; i < 6; ++i) R_pt_[i] = n.resId(pt[i]);
        R_src_ = n.resId("Rsrc"); L_pu_ = n.indId("Lpu"); C_cab_ = n.capId("Ccab");
        R_src2_ = n.resId("Rsrc2"); L_pu2_ = n.indId("Lpu2"); C_cab2_ = n.capId("Ccab2");
        { const char* s2[4] = {"Rsel2_b1", "Rsel2_b2", "Rsel2_n1", "Rsel2_n2"}; for (int i = 0; i < 4; ++i) R_sel2_[i] = n.resId(s2[i]); }
        R_re_ = n.resId("Re"); R_dum_ = n.resId("Rdum"); R_ext_ = n.resId("Rext");
        L_le_ = n.indId("Le"); R_es_ = n.resId("Res"); C_mes_ = n.capId("Cmes"); L_ces_ = n.indId("Lces");
        R_k1_ = n.resId("Rk1");
        if (n.hasXfm()) xfmNominal_ = n.xfm();
    }

    void applyConfig()
    {
        Network& n = *net_;
        // tubes
        for (auto& t : n.tri()) {
            if (t.m.name == "V1A" || t.m.name == "V1B") { t.type = cfg.v1Type; t.par = triodeParams(cfg.v1Type); }
            else { t.type = cfg.v2Type; t.par = triodeParams(cfg.v2Type); }
        }
        for (auto& d : n.dio()) {
            d.par = DiodeParams{};
            d.par.type = cfg.rectifier;
        }
        // source chain
        if (cfg.pickupModel) {
            n.setR(R_src_, cfg.pickupR); n.setL(L_pu_, cfg.pickupL); n.setC(C_cab_, cfg.pickupCable);
        } else {
            n.setR(R_src_, std::max(cfg.srcR, 1.0)); n.setL(L_pu_, std::max(cfg.srcL, 1e-9)); n.setC(C_cab_, cfg.cableC);
        }
        srcG_ = 1.0 / n.getR(R_src_);
        // the second plug carries the same kind of source (line/DI or pickup + cable)
        n.setR(R_src2_, n.getR(R_src_)); n.setL(L_pu2_, cfg.pickupModel ? cfg.pickupL : std::max(cfg.srcL, 1e-9)); n.setC(C_cab2_, cfg.pickupModel ? cfg.pickupCable : cfg.cableC);
        srcG2_ = 1.0 / n.getR(R_src2_);
        n.setR(n.resId("Rs_a"), cfg.ptRs); n.setR(n.resId("Rs_b"), cfg.ptRs);
        n.setCoreSaturation(cfg.otSaturation);
        applyTransformerConfig();
        if (cfg.mainsHz < 40.0) cfg.mainsHz = 50.0;
        // speaker electrical equivalent (dossier Sec. 17)
        const SpeakerTS& s = cfg.speaker;
        Bl_ = s.Bl();
        const double Res = Bl_ * Bl_ / s.Rms();
        const double Cmes = s.Mms / (Bl_ * Bl_);
        const double Lces = s.Cms() * Bl_ * Bl_;
        voiceCoilR_ = s.Re;
        n.setR(R_re_, s.Re); n.setL(L_le_, s.Le); n.setR(R_es_, Res); n.setC(C_mes_, Cmes); n.setL(L_ces_, Lces);
        // optional specimen variation (creative feature, separate from the nominal model)
        if (cfg.variationAmount > 0.0) {
            std::mt19937 rng(cfg.variationSeed);
            std::normal_distribution<double> nd(0.0, 1.0);
            for (auto& r : n.res()) if (!r.m.novar && !r.m.dyn && r.m.tol > 0) r.var = 1.0 + cfg.variationAmount * r.m.tol * nd(rng);
            for (auto& c : n.cap()) if (!c.m.novar && !c.m.dyn && c.m.tol > 0) c.var = 1.0 + cfg.variationAmount * c.m.tol * nd(rng);
            for (auto& t : n.tri()) t.scale = 1.0 + cfg.variationAmount * 0.08 * nd(rng);
            for (auto& p : n.pen()) p.scale = 1.0 + cfg.variationAmount * 0.06 * nd(rng);
        } else {
            for (auto& r : n.res()) r.var = 1.0;
            for (auto& c : n.cap()) c.var = 1.0;
            for (auto& t : n.tri()) t.scale = 1.0;
            for (auto& p : n.pen()) p.scale = 1.0;
        }
        // Johnson noise density of the first-stage input network (equivalent ~34k || tube Req) [V/sqrt(Hz)] -> per-sample sigma
        const double kT4 = 4.0 * 1.380649e-23 * 300.0;
        const double Req = 20e3;                      // PROP: effective input-referred resistance
        noiseDensityV_ = std::sqrt(kT4 * Req * 0.5 * fsInt_);
        noiseRng_.seed(cfg.noiseSeed);
    }

    // Output-transformer parameters from the manifest.  Always recomputed from the NOMINAL netlist record, so repeated reconfiguration never compounds.
    // Order matters: ratio first (it changes N_full), then magnetising inductance (P0 = L / N_full^2 - Pair), leakage (all three windings scaled in
    // proportion so that Lla + Llb + n^2 Lls equals the target), then DC resistances and the saturation flux.
    void applyTransformerConfig()
    {
        Network& n = *net_;
        if (!n.hasXfm()) return;
        Network::Xfm& x = n.xfm();
        const Network::Xfm& nom = xfmNominal_;
        x.N[0] = nom.N[0]; x.N[1] = nom.N[1]; x.N[2] = nom.N[2]; x.P0 = nom.P0; x.Pair = nom.Pair; x.phiSat = nom.phiSat;
        for (int k = 0; k < 3; ++k) { x.Ll[k] = nom.Ll[k]; x.R[k] = nom.R[k]; }
        if (std::isfinite(cfg.otRatioFull) && cfg.otRatioFull > 0.0) { x.N[0] = x.N[1] = 0.5 * cfg.otRatioFull * x.N[2]; }
        const double Nfull = x.N[0] + x.N[1], nn = Nfull / x.N[2];
        if (std::isfinite(cfg.otLprimFull) && cfg.otLprimFull > 0.0) x.P0 = std::max(cfg.otLprimFull / (Nfull * Nfull) - x.Pair, 1e-9);
        if (std::isfinite(cfg.otLeakFullRef) && cfg.otLeakFullRef > 0.0) {
            const double cur = x.Ll[0] + x.Ll[1] + nn * nn * x.Ll[2], f = cfg.otLeakFullRef / cur;
            for (int k = 0; k < 3; ++k) x.Ll[k] *= f;
        }
        if (std::isfinite(cfg.otDcrPrimHalf) && cfg.otDcrPrimHalf > 0.0) x.R[0] = x.R[1] = cfg.otDcrPrimHalf;
        if (std::isfinite(cfg.otDcrSec) && cfg.otDcrSec > 0.0) x.R[2] = cfg.otDcrSec;
        if (std::isfinite(cfg.otSatFlux) && cfg.otSatFlux > 0.0) x.phiSat = cfg.otSatFlux;
        n.markDirty();
    }

    void updatePowerEnvelope()
    {
        // switch contact: ~10 ms ramp ; heater emission follows with its own time constants (PROPOSED)
        const double up = 1.0 / (0.010 * fsInt_);
        if (mainsGain_ < mainsTarget_) mainsGain_ = std::min(mainsTarget_, mainsGain_ + up);
        else if (mainsGain_ > mainsTarget_) mainsGain_ = std::max(mainsTarget_, mainsGain_ - up);
        const double on = mainsTarget_ > 0.5 ? 1.0 : 0.0;
        const double tauTube = on ? 5.0 : 2.0, tauRect = on ? 1.5 : 1.0;
        emTube_ += (on - emTube_) * (1.0 - std::exp(-1.0 / (tauTube * fsInt_)));
        emRect_ += (on - emRect_) * (1.0 - std::exp(-1.0 / (tauRect * fsInt_)));
        net_->setEmission(0, std::max(0.0, emTube_)); net_->setEmission(1, std::max(0.0, emTube_)); net_->setEmission(2, std::max(0.0, emRect_));
    }

    double gauss_() { return nd_(noiseRng_); }

    std::unique_ptr<Network> net_;
    Network::Xfm xfmNominal_{};
    bool useKernel_ = true;
    double fsInt_ = 96000, theta_ = 0.5, phase_ = 0.0;
    AmpControls target_, cur_;
    double smoothCoef_ = 0.2, mainsGain_ = 1.0, mainsTarget_ = 1.0, emTube_ = 1.0, emRect_ = 1.0;
    double Bl_ = 1.0, voiceCoilR_ = 6.4, load_g_ = 0.0, srcG_ = 0.01, srcG2_ = 0.01, noiseDensityV_ = 0.0;
    double rRe_ = 6.4, rDum_ = 1e11, rExt_ = 1e11;     // resistances presently set on the voice-coil / dummy / extension branches
    int pu_, pu2_, sp_, sx_, sy_, ra_, rb_, k1_;
    int R_vb_lo_, R_vb_hi_, R_vn_lo_, R_vn_hi_, R_t_a_, R_t_b_, R_sw_b1_, R_sw_b2_, R_sw_n1_, R_sw_n2_;
    int R_sel2_[4], R_src2_, L_pu2_, C_cab2_;
    int R_sel_[4], R_pt_[6], R_src_, L_pu_, C_cab_, R_re_, R_dum_, R_ext_, L_le_, R_es_, C_mes_, L_ces_, R_k1_;
    int R_rs_a_ = -1, R_rs_b_ = -1;
    std::mt19937 noiseRng_{1};
    std::normal_distribution<double> nd_{0.0, 1.0};
};

} // namespace cd57
