#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace juce;
namespace pid = cd57ui::id;
namespace om = cd57ui::outmode;

// ---------------------------------------------------------------------------------------------------------------
// Background rebuild of the engine (oversampling change): never allocate on the audio thread.
class CD57AudioProcessor::Rebuilder : public Thread
{
public:
    explicit Rebuilder(CD57AudioProcessor& o) : Thread("CD57 rebuild"), owner(o) { startThread(Priority::normal); }
    ~Rebuilder() override { signalThreadShouldExit(); wake.signal(); stopThread(3000); }
    void request() { wake.signal(); }
    void run() override
    {
        while (!threadShouldExit()) {
            wake.wait(200);
            if (threadShouldExit()) break;
            {   // retire the previous engine here (off the audio thread)
                std::unique_ptr<cd57::CD57Engine> old;
                { const SpinLock::ScopedLockType l(owner.swapLock_); if (retired) old = std::move(retired); }
            }
            const int want = owner.requestedOS_.load();
            if (want == owner.engineOS_.load()) continue;
            auto e = std::make_unique<cd57::CD57Engine>();
            e->prepare(owner.sampleRate_, owner.maxBlock_, owner.netlist_, owner.gatherParams(), owner.manifestText_);
            {
                const SpinLock::ScopedLockType l(owner.swapLock_);
                owner.pending_ = std::move(e);
            }
            pendingOS = want;
        }
    }
    CD57AudioProcessor& owner;
    WaitableEvent wake;
    std::unique_ptr<cd57::CD57Engine> retired;
    int pendingOS = 1;
};

// ---------------------------------------------------------------------------------------------------------------
AudioProcessorValueTreeState::ParameterLayout CD57AudioProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto F = [](float lo, float hi, float step = 0.0f, float skew = 1.0f) { NormalisableRange<float> r(lo, hi, step); r.setSkewForCentre(skew); return r; };
    auto lin = [](float lo, float hi, float step = 0.0f) { return NormalisableRange<float>(lo, hi, step); };
    auto fl = [&](const char* id, const char* name, NormalisableRange<float> r, float def, const char* unit = "") {
        p.push_back(std::make_unique<AudioParameterFloat>(ParameterID{id, 1}, name, r, def, AudioParameterFloatAttributes().withLabel(unit)));
    };
    auto ch = [&](const char* id, const char* name, StringArray items, int def) {
        p.push_back(std::make_unique<AudioParameterChoice>(ParameterID{id, 1}, name, items, def));
    };
    auto bo = [&](const char* id, const char* name, bool def) { p.push_back(std::make_unique<AudioParameterBool>(ParameterID{id, 1}, name, def)); };

    // front panel (knob scale 0..12 as printed on the amplifier face -- scale assumed)
    fl(pid::instVol, "Instrument Volume", lin(0.0f, 12.0f, 0.01f), 6.0f);
    fl(pid::micVol, "Mic Volume", lin(0.0f, 12.0f, 0.01f), 0.0f);
    fl(pid::tone, "Tone", lin(0.0f, 12.0f, 0.01f), 7.0f);
    ch(pid::jack, "Guitar Jack", {"Instrument 1", "Instrument 2", "Mic 1", "Mic 2"}, 0);
    ch(pid::plug2, "Second Plug", {"None", "Instrument 1", "Instrument 2", "Mic 1", "Mic 2"}, 0);
    ch(pid::plug2Src, "Second Plug Source", {"Terminated cable (guitar at rest)", "Right input (second guitar)"}, 0);
    ch(pid::patch, "Jumper Cable", {"None", "Inst 1 - Inst 2", "Mic 1 - Mic 2", "Inst 1 - Mic 1", "Inst 1 - Mic 2", "Inst 2 - Mic 1", "Inst 2 - Mic 2"}, 0);
    bo(pid::power, "Power", true);
    bo(pid::bypass, "Bypass", false);
    // load / output
    // IMPORTANT: the FIVE original choices must remain FIVE in the same order. Enlarging this
    // AudioParameterChoice would re-scale its normalised automation points in old DAW sessions.
    // New external arrangements live in a separate, default-off parameter appended BELOW.
    ch(pid::load, "Speaker Load", {
        "Stock: internal 8 ohm (assumed Eminence)",
        "Experimental: internal + ext 8 ohm (Fender does NOT recommend)",
        "Bench: dummy 8 ohm (not a speaker)",
        "Bench: dummy 4 ohm (not a speaker)",
        "Bench: dummy 16 ohm (not a speaker)"}, 0);
    // saved by index: the first three entries are unchanged from earlier versions, the hybrid mode was appended
    ch(pid::outMode, "Output", {"Microphone (provisional model)", "Speaker terminals", "User IR (voltage to mic)", "User IR on cone motion (hybrid)"}, 0);
    // geometric proxies of the PROVISIONAL microphone model only (ignored by both IR modes); no measured positions exist
    fl(pid::micPos, "Mic Position (provisional model)", lin(0.0f, 1.0f, 0.001f), 0.15f);
    fl(pid::micDist, "Mic Distance (provisional model)", lin(0.0f, 1.0f, 0.001f), 0.10f);
    fl(pid::inTrim, "Input Trim", lin(-12.0f, 12.0f, 0.1f), 0.0f, " dB");        // trim x calibration (<= 5 V) stays inside the 30 V validated at the jack
    fl(pid::outLevel, "Output Level", lin(-30.0f, 12.0f, 0.1f), 0.0f, " dB");
    // service / lab
    fl(pid::inCal, "Input Calibration (V peak @ 0 dBFS)", F(0.1f, 5.0f, 0.01f, 1.0f), 1.0f, " V");
    ch(pid::market, "Market Variant", {"US 120 V / 60 Hz", "Taiwan 110 V / 60 Hz", "Australia 240 V / 50 Hz", "UK/EU 230 V / 50 Hz", "Argentina 220 V / 50 Hz", "Japan 100 V / 50 Hz", "Japan 100 V / 60 Hz", "China 220 V / 50 Hz", "Korea 220 V / 60 Hz"}, 0);
    fl(pid::linePct, "Line Voltage", lin(70.0f, 110.0f, 0.5f), 100.0f, " %");
    ch(pid::v1, "V1 Tube", {"12AY7 (stock)", "12AX7"}, 0);
    ch(pid::v2, "V2 Tube", {"12AX7 (stock)", "12AY7"}, 0);
    ch(pid::rect, "Rectifier", {"5Y3 (stock)", "Silicon diodes"}, 0);
    ch(pid::oversample, "Oversampling", {"1x", "2x (default)", "4x", "8x (offline: not real-time)"}, 1);
    ch(pid::source, "Source Model", {"Line / DI", "Passive pickup + cable"}, 0);
    fl(pid::srcR, "Source Resistance", F(1.0f, 20000.0f, 1.0f, 500.0f), 100.0f, " ohm");
    ch(pid::taper, "Volume Taper (assumed)", {"Audio 15%", "Audio 10%", "Linear"}, 0);
    fl(pid::variation, "Specimen Variation", lin(0.0f, 1.0f, 0.01f), 0.0f);
    p.push_back(std::make_unique<AudioParameterInt>(ParameterID{pid::seed, 1}, "Variation Seed", 1, 999, 1));
    fl(pid::hum, "Hum (dB re 1 uA)", lin(-100.0f, 0.0f, 0.5f), -100.0f, " dB");
    fl(pid::noise, "Noise", lin(0.0f, 1.0f, 0.01f), 0.0f);
    bo(pid::heating, "Speaker Heating", true);
    bo(pid::otSat, "OT core saturation (assumed law)", false);
    ch(pid::inputCh, "Input channel", {"Mean of L + R", "Left only", "Right only"}, 0);
    // APPEND after every existing parameter. This protects both index-based host parameter order
    // and the old speaker-load parameter's normalised automation. True = switch ON/PLAY.
    bo(pid::standbyOn, "Standby (ON = play)", true);
    ch(pid::externalLoad, "External Speaker Setup", {
        "None (use original load selection)",
        "Single external 8 ohm (provisional load proxy)",
        "Two external 16 ohm (provisional load proxies)"}, 0);
    return {p.begin(), p.end()};
}

CD57AudioProcessor::CD57AudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo(), true).withOutput("Output", AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "CD57", createLayout())
{
    netlist_ = cd57::embeddedNetlist();
    manifestText_ = cd57::embeddedManifest();
    {   // the speaker the engine will use (manifest applied exactly as CD57Engine::makeConfig does): source of G(f) for the hybrid route
        cd57::Manifest man;
        cd57::AmpConfig cfg;
        if (man.load(manifestText_).empty()) {
            const std::string error = man.apply(cfg);
            if (error.empty()) {
                const int electricalMeasured = (int)man.isMeasured("speaker_Re") + (int)man.isMeasured("speaker_Le")
                    + (int)man.isMeasured("speaker_fs") + (int)man.isMeasured("speaker_Qms") + (int)man.isMeasured("speaker_Qes");
                calibrationStatus_ = "OEM speaker electrical: " + std::to_string(electricalMeasured)
                    + "/5 specimen measurements (remaining properties assumed); Instrument taper: "
                    + (cfg.volumeMapMeasured ? "MEASURED" : "ASSUMED") + "; Mic taper: "
                    + (cfg.micMapMeasured ? "MEASURED" : "ASSUMED");
            }
        }
        speaker_ = cfg.speaker;
    }
    if (calibrationStatus_.empty()) calibrationStatus_ = "OEM speaker and modern volume tapers: ASSUMED (no valid calibrated profile)";
    irStatus_ = "No IR loaded: the IR output modes pass the speaker terminals through unchanged";
    pBypass_ = apvts.getRawParameterValue(pid::bypass);
    pInputCh_ = apvts.getRawParameterValue(pid::inputCh);
    pPlug2_ = apvts.getRawParameterValue(pid::plug2); pPlug2Src_ = apvts.getRawParameterValue(pid::plug2Src);
    apvts.addParameterListener(pid::oversample, this);
    rebuilder_ = std::make_unique<Rebuilder>(*this);
}

CD57AudioProcessor::~CD57AudioProcessor()
{
    cancelPendingUpdate();
    apvts.removeParameterListener(pid::oversample, this);
    rebuilder_.reset();
}

bool CD57AudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto& o = l.getMainOutputChannelSet();
    const auto& i = l.getMainInputChannelSet();
    if (o != AudioChannelSet::mono() && o != AudioChannelSet::stereo()) return false;
    return i == AudioChannelSet::mono() || i == AudioChannelSet::stereo();
}

void CD57AudioProcessor::parameterChanged(const String& pidStr, float v)
{
    if (pidStr == pid::oversample) {
        requestedOS_.store((int)std::lround(v));
        if (rebuilder_) rebuilder_->request();
    }
}

cd57::EngineParams CD57AudioProcessor::gatherParams() const
{
    auto g = [&](const char* id) { return apvts.getRawParameterValue(id)->load(); };
    cd57::EngineParams e;
    e.instVol = g(pid::instVol) / 12.0f; e.micVol = g(pid::micVol) / 12.0f; e.tone = g(pid::tone) / 12.0f;
    e.jack = (int)g(pid::jack) + 1; e.jack2 = (int)g(pid::plug2); e.patch = (int)g(pid::patch); e.load = (int)g(pid::load);
    // Separate override: legacy `load` remains a five-choice, automation-compatible parameter.
    // 1 -> new LoadMode::SingleExternal8 (5), 2 -> new LoadMode::DualExternal16 (6).
    const int external = (int)g(pid::externalLoad);
    if (external > 0) e.load = 4 + external;
    e.power = g(pid::power) > 0.5f;
    e.standbyOn = g(pid::standbyOn) > 0.5f;
    e.inputTrimDb = g(pid::inTrim); e.outputDb = g(pid::outLevel); e.inputCalVolts = g(pid::inCal);
    e.market = (int)g(pid::market); e.linePercent = g(pid::linePct);
    e.v1 = (int)g(pid::v1); e.v2 = (int)g(pid::v2); e.rectifier = (int)g(pid::rect);
    e.oversampleLog2 = (int)g(pid::oversample);
    e.sourceMode = (int)g(pid::source); e.sourceR = g(pid::srcR);
    e.taper = (int)g(pid::taper);
    e.variation = g(pid::variation); e.variationSeed = (int)g(pid::seed);
    e.humDb = g(pid::hum) <= -99.5f ? -200.0f : g(pid::hum);
    e.noise = g(pid::noise); e.noiseSeed = (int)g(pid::seed);
    e.speakerHeating = g(pid::heating) > 0.5f;
    e.otSaturation = g(pid::otSat) > 0.5f;
    e.outputMode = (int)g(pid::outMode);
    e.micRadial = g(pid::micPos); e.micDistance = g(pid::micDist);
    return e;
}

void CD57AudioProcessor::buildEngineBlocking(int oversampleLog2)
{
    auto e = std::make_unique<cd57::CD57Engine>();
    auto p = gatherParams();
    p.oversampleLog2 = oversampleLog2;
    e->prepare(sampleRate_, maxBlock_, netlist_, p, manifestText_);
    manifestError_ = e->manifestError();
    manifestSummary_ = e->manifest().summary();
    {
        const SpinLock::ScopedLockType l(swapLock_);
        engine_ = std::move(e);
        pending_.reset();
    }
    engineOS_.store(oversampleLog2);
    requestedOS_.store(oversampleLog2);
    setLatencySamples(totalLatency(engine_->latencySamples()));
    diag.latency.store(totalLatency(engine_->latencySamples()));
    diag.internalRateHz.store((int)(sampleRate_ * (1 << oversampleLog2)));
}

void CD57AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(64, std::min(samplesPerBlock, 2048));
    monoIn_.assign((size_t)maxBlock_, 0.0f); monoIn2_.assign((size_t)maxBlock_, 0.0f); outTerm_.assign((size_t)maxBlock_, 0.0f); outAc_.assign((size_t)maxBlock_, 0.0f);
    irBuf_.assign((size_t)maxBlock_, 0.0f); coneBuf_.assign((size_t)maxBlock_, 0.0f);
    inHist_.assign((size_t)kInHist, 0.0f); primeBuf_.assign((size_t)kInHist, 0.0f); inHistPos_ = 0;
    fadeTerm_.assign((size_t)maxBlock_, 0.0f); fadeAc_.assign((size_t)maxBlock_, 0.0f); fadeW_.assign((size_t)maxBlock_, 1.0f); fadeEngine_.reset(); fadePos_ = 0;
    spec_ = {sampleRate, (uint32)maxBlock_, 1};
    {   // re-derive the processed IR(s) at the new rate from the raw capture, THEN prepare (JUCE: load before prepare)
        const ScopedLock l(irLock_);
        rebuildImpulseResponsesLocked();
    }
    convolution_.prepare(spec_);
    convolutionCone_.prepare(spec_);
    convolution_.reset();
    convolutionCone_.reset();
    convLatency_.store(std::max(convolution_.getLatency(), convolutionCone_.getLatency()));
    lastMode_ = -1;
    buildEngineBlocking((int)apvts.getRawParameterValue(pid::oversample)->load());
}

void CD57AudioProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int total = buffer.getNumSamples();
    const int inCh = getTotalNumInputChannels(), outCh = getTotalNumOutputChannels();
    for (int c = inCh; c < outCh; ++c) buffer.clear(c, 0, total);
    if (pBypass_ && pBypass_->load() > 0.5f) return;

    // swap in a freshly built engine if one is ready (no allocation, no blocking)
    if (swapLock_.tryEnter()) {
        if (pending_ && rebuilder_ && !rebuilder_->retired && !fadeEngine_) {
            if (engine_) {
                // carry rails, capacitor charge, flux and coil temperature over (no reset to quiescent) and prime the new up-sampler
                for (int k = 0; k < kInHist; ++k) primeBuf_[(size_t)k] = inHist_[(size_t)((inHistPos_ + k) % kInHist)];
                pending_->primeInput(primeBuf_.data(), kInHist);
                pending_->adoptStateFrom(*engine_);
            }
            std::swap(engine_, pending_);
            fadeEngine_ = std::move(pending_);                    // the retiring engine keeps running for kFade samples (cross-fade)
            fadePos_ = 0;
            engineOS_.store(engine_->params().oversampleLog2);
            const int lat = totalLatency(engine_->latencySamples());
            diag.latency.store(lat);
            diag.internalRateHz.store((int)(sampleRate_ * (1 << engine_->params().oversampleLog2)));
            latencyToApply_.store(lat);
            triggerAsyncUpdate();
        }
        swapLock_.exit();
    }
    if (!engine_) { buffer.clear(); return; }

    const auto t0 = Time::getHighResolutionTicks();
    auto params = gatherParams();
    // the oversampling factor changes ONLY through the background rebuild + swap above (never an in-place rebuild here)
    params.oversampleLog2 = engine_->params().oversampleLog2;
    engine_->setParams(params);
    const int mode = params.outputMode;
    const bool irReady = irLoaded_.load(std::memory_order_acquire);
    const bool useIR = mode == om::userIr && irReady;
    const bool useCone = mode == om::userIrCone && irReady;
    if (mode != lastMode_) {                       // a convolver that was idle holds stale history: restart it when its mode is selected
        lastMode_ = mode;
        if (useIR) convolution_.reset();
        if (useCone) convolutionCone_.reset();
    }

    const int inputChoice = (int)pInputCh_->load();
    // a second plug fed from the RIGHT input channel: the first source is then the LEFT channel alone (they are two different guitars, not one mixed signal)
    const bool dual = (int)pPlug2_->load() > 0 && (int)pPlug2Src_->load() == 1 && inCh > 1;
    int done = 0;
    float peak = 0.0f;
    while (done < total) {
        const int n = std::min(total - done, maxBlock_);
        // mono guitar signal: sum of the available input channels
        for (int i = 0; i < n; ++i) {
            float s = buffer.getReadPointer(0)[done + i];
            if (inCh > 1) {
                const float r = buffer.getReadPointer(1)[done + i];
                if (dual) monoIn2_[(size_t)i] = r;
                else s = inputChoice == 1 ? s : (inputChoice == 2 ? r : 0.5f * (s + r));    // mean keeps a dual-mono source at its calibrated level
            }
            monoIn_[(size_t)i] = s;
            inHist_[(size_t)inHistPos_] = s; inHistPos_ = (inHistPos_ + 1) % kInHist;
        }
        engine_->process(monoIn_.data(), dual ? monoIn2_.data() : nullptr, outTerm_.data(), outAc_.data(), n);
        const bool fading = fadeEngine_ != nullptr;
        if (fading) {
            auto fp = params; fp.oversampleLog2 = fadeEngine_->params().oversampleLog2;
            fadeEngine_->setParams(fp);
            fadeEngine_->process(monoIn_.data(), dual ? monoIn2_.data() : nullptr, fadeTerm_.data(), fadeAc_.data(), n);
            for (int i = 0; i < n; ++i) {
                const float w = std::min(1.0f, (float)(fadePos_ + i + 1) / (float)kFade);
                fadeW_[(size_t)i] = w;
                outTerm_[(size_t)i] = w * outTerm_[(size_t)i] + (1.0f - w) * fadeTerm_[(size_t)i];
                outAc_[(size_t)i] = w * outAc_[(size_t)i] + (1.0f - w) * fadeAc_[(size_t)i];
            }
        }
        const float* res = outAc_.data();
        if (mode == om::speakerTerminals) res = outTerm_.data();
        else if (useIR) {
            // Route 1, "User IR": fed with the speaker-terminal voltage (output units); the provisional acoustic model is bypassed.
            for (int i = 0; i < n; ++i) irBuf_[(size_t)i] = outTerm_[(size_t)i];
            float* ch[1] = {irBuf_.data()};
            AudioBuffer<float> ab(ch, 1, n);
            dsp::AudioBlock<float> blk(ab);
            dsp::ProcessContextReplacing<float> ctx(blk);
            convolution_.process(ctx);
            res = irBuf_.data();
        } else if (useCone) {
            // Route 2, hybrid: the model's cone velocity [m/s] -> volume velocity Sd*v, expressed in volts-equivalent (/gRef) and in the
            // same output units as route 1 (kOut), then the residual R = H G*/(|G|^2+lambda) (AcousticFactor.h).  Same arrival-time origin.
            const double scale = engine_->outputScale() * coneInputScale_.load(std::memory_order_relaxed);
            const auto& vc = engine_->coneVelocityBlock();
            if (fading) {
                const auto& vo = fadeEngine_->coneVelocityBlock();
                for (int i = 0; i < n; ++i) coneBuf_[(size_t)i] = (float)(scale * (fadeW_[(size_t)i] * vc[(size_t)i] + (1.0f - fadeW_[(size_t)i]) * vo[(size_t)i]));
            } else for (int i = 0; i < n; ++i) coneBuf_[(size_t)i] = (float)(scale * vc[(size_t)i]);
            float* ch[1] = {coneBuf_.data()};
            AudioBuffer<float> ab(ch, 1, n);
            dsp::AudioBlock<float> blk(ab);
            dsp::ProcessContextReplacing<float> ctx(blk);
            convolutionCone_.process(ctx);
            res = coneBuf_.data();
        } else if (mode == om::userIr || mode == om::userIrCone) res = outTerm_.data();    // no IR loaded yet: speaker terminals
        for (int c = 0; c < outCh; ++c) {
            float* dst = buffer.getWritePointer(c) + done;
            for (int i = 0; i < n; ++i) dst[i] = res[i];
        }
        for (int i = 0; i < n; ++i) {
            peak = std::max(peak, std::fabs(res[i]));
            const int sp = scopePos.load(std::memory_order_relaxed);
            scope[sp] = res[i];
            scopePos.store((sp + 1) % kScope, std::memory_order_relaxed);
        }
        done += n;
        if (fading) {
            fadePos_ += n;
            if (fadePos_ >= kFade && swapLock_.tryEnter()) {          // hand the retired engine to the rebuild thread (it frees it off the audio thread)
                if (rebuilder_ && !rebuilder_->retired) { rebuilder_->retired = std::move(fadeEngine_); fadePos_ = 0; }
                swapLock_.exit();
            }
        }
    }
    // publish diagnostics (cheap; ~every 8th block)
    if ((++blockCounter_ & 7) == 0) {
        const auto d = engine_->diag();
        diag.b1 = d.b1; diag.b2 = d.b2; diag.b3 = d.b3; diag.cathode = d.cathode; diag.coilTemp = d.coilTempRise;
        diag.itPerStep = (float)d.meanIterations(); diag.fallbacks = d.substepped; diag.failures = d.failures; diag.steps = d.steps; diag.panics = d.panics;
        diag.pilot = (float)engine_->amp().powerEnvelope();
    }
    diag.peak = std::max(diag.peak.load() * 0.9f, peak);
    const double el = Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks() - t0);
    diag.cpu = (float)(0.9 * diag.cpu.load() + 0.1 * (el / (total / sampleRate_)));
}

// ---------------------------------------------------------------------------------------------------------------
namespace {
// Reads channel 0 of an audio file as float samples, UNCHANGED (no normalisation, no trimming).  Integer PCM maps to +-1 full scale,
// so a gain above 0 dBFS needs a 32-bit float file.  At most 10 s are read.
bool readIrFile(const File& f, std::vector<float>& samples, double& rate, int& channels, bool& longer, String& err)
{
    AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r(fm.createReaderFor(f));
    if (!r) { err = "not a readable audio file"; return false; }
    if (r->lengthInSamples <= 0 || r->sampleRate <= 0.0 || r->numChannels == 0) { err = "empty audio file"; return false; }
    const int n = (int)std::min<int64>(r->lengthInSamples, (int64)(10.0 * r->sampleRate));
    longer = r->lengthInSamples > n;
    AudioBuffer<float> b((int)r->numChannels, n);
    if (!r->read(&b, 0, n, 0, true, true)) { err = "read failed"; return false; }
    samples.assign(b.getReadPointer(0), b.getReadPointer(0) + n);
    rate = r->sampleRate; channels = (int)r->numChannels;
    return true;
}
} // namespace

void CD57AudioProcessor::loadImpulseResponse(const File& f)
{
    std::vector<float> samples; double rate = 0.0; int channels = 0; bool longer = false; String err;
    const bool ok = f.existsAsFile() && readIrFile(f, samples, rate, channels, longer, err);
    const ScopedLock l(irLock_);
    irPath_ = f.getFullPathName();                // remembered even if the file cannot be read now, so a saved state keeps its reference
    if (!ok) {
        // A state that names an unreadable file must not silently render with whichever IR happened to be loaded before (a preset has
        // to sound the same every time): no IR is active, the output falls back to the speaker terminals, and the status says so.
        irRaw_.clear(); irName_.clear(); irHash_.clear(); irFileRate_ = 0.0; irFileChannels_ = 0;
        rebuildImpulseResponsesLocked();
        irStatus_ = "IR NOT LOADED (" + (f.existsAsFile() ? err : String("file not found")) + "): " + f.getFullPathName() + " -- no IR is active; the output falls back to the speaker terminals";
        return;
    }
    irName_ = f.getFileName();
    irRaw_ = std::move(samples);
    irFileRate_ = rate; irFileChannels_ = channels;
    irHash_ = String::toHexString((int64)cd57::hashSamples(irRaw_));
    rebuildImpulseResponsesLocked();
    if (channels > 1) irStatus_ += "  [channel 1 of " + String(channels) + " used]";
    if (longer) irStatus_ += "  [file longer than 10 s: only the first 10 s were read]";
}

// Derives the processed IR (route 1) and the cone-motion residual (route 2) from the raw capture at the current host rate and hands both
// to the convolvers.  All allocation happens here, on the calling (message / prepare) thread.
void CD57AudioProcessor::rebuildImpulseResponsesLocked()
{
    if (irRaw_.empty() || irFileRate_ <= 0.0) { irLoaded_.store(false, std::memory_order_release); return; }
    const std::vector<double> raw(irRaw_.begin(), irRaw_.end());
    const auto ir = cd57::prepareImpulseResponse(raw, irFileRate_, sampleRate_, 2.0);
    cd57::FactorSettings fset;
    fset.sampleRate = sampleRate_;
    const cd57::SpeakerTS spk = speaker_;
    const auto fr = cd57::factorResidual(ir.h, [&spk](double hz) { return cd57::speakerVolumeVelocityPerVolt(spk, hz); }, fset);
    if (fr.residual.empty() || !(fr.gRef > 0.0)) {
        irLoaded_.store(false, std::memory_order_release);
        irStatus_ = "IR NOT LOADED (factorisation failed): " + irPath_;
        return;
    }
    auto toBuffer = [](const std::vector<double>& v) {
        AudioBuffer<float> b(1, (int)v.size());
        for (size_t i = 0; i < v.size(); ++i) b.setSample(0, (int)i, (float)v[i]);
        return b;
    };
    // Normalise::no and Trim::no: gain, polarity and arrival time are the file's.  The buffer is already at the host rate, so JUCE
    // applies no resampling (and its fs_in/fs_out gain factor is exactly 1).
    convolution_.loadImpulseResponse(toBuffer(ir.h), sampleRate_, dsp::Convolution::Stereo::no, dsp::Convolution::Trim::no, dsp::Convolution::Normalise::no);
    convolutionCone_.loadImpulseResponse(toBuffer(fr.residual), sampleRate_, dsp::Convolution::Stereo::no, dsp::Convolution::Trim::no, dsp::Convolution::Normalise::no);
    coneInputScale_.store(spk.Sd / fr.gRef);
    irTailSeconds_.store((double)ir.h.size() / sampleRate_);
    irLoaded_.store(true, std::memory_order_release);

    String st;
    st << "IR " << irName_ << ": " << String(irFileRate_, 0) << " Hz, " << (int)ir.fileSamples << " samples -> " << (int)ir.h.size() << " at " << String(sampleRate_, 0)
       << " Hz, onset " << String(ir.onsetMs, 2) << " ms, peak " << String(ir.peakAbs, 3) << (ir.cutAtMaxLength ? ", CUT at 2 s (10 ms fade)" : "")
       << ". Hybrid exact over " << String(fr.usableLoHz, 0) << "-" << String(fr.usableHiHz, 0) << " Hz (0.5 dB), residual gain bound +" << String(fr.gainBoundDb, 0) << " dB";
    if (fr.nonCausalDb > -30.0) st << ". WARNING: no arrival delay in the IR (non-causal part " << String(fr.nonCausalDb, 0) << " dB dropped): hybrid accuracy is limited";
    irStatus_ = st;
}

AudioProcessorEditor* CD57AudioProcessor::createEditor() { return new CD57Editor(*this); }

void CD57AudioProcessor::getStateInformation(MemoryBlock& dest)
{
    auto state = apvts.copyState();
    {   // reference to the raw capture + enough metadata to detect that the file changed; the processed IRs are re-derived on load
        const ScopedLock l(irLock_);
        state.setProperty("irPath", irPath_, nullptr);
        state.setProperty("irFileRate", irFileRate_, nullptr);
        state.setProperty("irFileSamples", (int)irRaw_.size(), nullptr);
        state.setProperty("irFileChannels", irFileChannels_, nullptr);
        state.setProperty("irHash", irHash_, nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void CD57AudioProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size)) {
        auto tree = ValueTree::fromXml(*xml);
        if (tree.isValid()) {
            const String irp = tree.getProperty("irPath").toString(), savedHash = tree.getProperty("irHash").toString();
            // A legacy session has neither new parameter. Force the defaults even if the user
            // loads it after engaging standby or selecting an external override in this instance.
            auto appendMissingControl = [&](const char* id, float value) {
                if (tree.getChildWithProperty("id", id).isValid()) return;
                ValueTree newControl("PARAM");
                newControl.setProperty("id", id, nullptr);
                newControl.setProperty("value", value, nullptr);
                tree.addChild(newControl, -1, nullptr);
            };
            appendMissingControl(pid::standbyOn, 1.0f);
            appendMissingControl(pid::externalLoad, 0.0f);
            apvts.replaceState(tree);
            if (irp.isNotEmpty()) {
                loadImpulseResponse(File(irp));
                const ScopedLock l(irLock_);
                if (savedHash.isNotEmpty() && irHash_.isNotEmpty() && savedHash != irHash_) irStatus_ += "  WARNING: this file differs from the capture saved in the state";
            }
        }
    }
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CD57AudioProcessor(); }
