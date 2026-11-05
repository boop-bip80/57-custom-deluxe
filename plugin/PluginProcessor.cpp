#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;
namespace pid = cd57ui::id;

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
    ch(pid::patch, "Jumper Cable", {"None", "Inst 1 - Inst 2", "Mic 1 - Mic 2", "Inst 1 - Mic 1", "Inst 1 - Mic 2", "Inst 2 - Mic 1", "Inst 2 - Mic 2"}, 0);
    bo(pid::power, "Power", true);
    bo(pid::bypass, "Bypass", false);
    // load / output
    ch(pid::load, "Speaker Load", {"Internal speaker 8 ohm", "Internal + extension 8 ohm", "Dummy load 8 ohm", "Dummy load 4 ohm", "Dummy load 16 ohm"}, 0);
    ch(pid::outMode, "Output", {"Microphone (provisional model)", "Speaker terminals", "User IR (voltage to mic)"}, 0);
    fl(pid::micPos, "Mic Position", lin(0.0f, 1.0f, 0.001f), 0.15f);
    fl(pid::micDist, "Mic Distance", lin(0.0f, 1.0f, 0.001f), 0.10f);
    fl(pid::inTrim, "Input Trim", lin(-24.0f, 24.0f, 0.1f), 0.0f, " dB");
    fl(pid::outLevel, "Output Level", lin(-30.0f, 12.0f, 0.1f), 0.0f, " dB");
    // service / lab
    fl(pid::inCal, "Input Calibration (V peak @ 0 dBFS)", F(0.1f, 5.0f, 0.01f, 1.0f), 1.0f, " V");
    ch(pid::market, "Market Variant", {"US 120 V / 60 Hz", "Taiwan 110 V / 60 Hz", "Australia 240 V / 50 Hz", "UK/EU 230 V / 50 Hz", "Argentina 220 V / 50 Hz", "Japan 100 V / 50 Hz", "Japan 100 V / 60 Hz", "China 220 V / 50 Hz", "Korea 220 V / 60 Hz"}, 0);
    fl(pid::linePct, "Line Voltage", lin(70.0f, 110.0f, 0.5f), 100.0f, " %");
    ch(pid::v1, "V1 Tube", {"12AY7 (stock)", "12AX7"}, 0);
    ch(pid::v2, "V2 Tube", {"12AX7 (stock)", "12AY7"}, 0);
    ch(pid::rect, "Rectifier", {"5Y3 (stock)", "Silicon diodes"}, 0);
    ch(pid::oversample, "Oversampling", {"1x", "2x", "4x", "8x"}, 1);
    ch(pid::source, "Source Model", {"Line / DI", "Passive pickup + cable"}, 0);
    fl(pid::srcR, "Source Resistance", F(1.0f, 20000.0f, 1.0f, 500.0f), 100.0f, " ohm");
    ch(pid::taper, "Volume Taper (assumed)", {"Audio 15%", "Audio 10%", "Linear"}, 0);
    fl(pid::variation, "Specimen Variation", lin(0.0f, 1.0f, 0.01f), 0.0f);
    p.push_back(std::make_unique<AudioParameterInt>(ParameterID{pid::seed, 1}, "Variation Seed", 1, 999, 1));
    fl(pid::hum, "Hum (dB re 1 uA)", lin(-100.0f, 0.0f, 0.5f), -100.0f, " dB");
    fl(pid::noise, "Noise", lin(0.0f, 1.0f, 0.01f), 0.0f);
    bo(pid::heating, "Speaker Heating", true);
    return {p.begin(), p.end()};
}

CD57AudioProcessor::CD57AudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo(), true).withOutput("Output", AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "CD57", createLayout())
{
    netlist_ = cd57::embeddedNetlist();
    manifestText_ = cd57::embeddedManifest();
    pBypass_ = apvts.getRawParameterValue(pid::bypass);
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
    e.jack = (int)g(pid::jack) + 1; e.patch = (int)g(pid::patch); e.load = (int)g(pid::load);
    e.power = g(pid::power) > 0.5f;
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
    setLatencySamples(engine_->latencySamples());
    diag.latency.store(engine_->latencySamples());
    diag.internalRateHz.store((int)(sampleRate_ * (1 << oversampleLog2)));
}

void CD57AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(64, std::min(samplesPerBlock, 2048));
    monoIn_.assign((size_t)maxBlock_, 0.0f); outTerm_.assign((size_t)maxBlock_, 0.0f); outAc_.assign((size_t)maxBlock_, 0.0f); irBuf_.assign((size_t)maxBlock_, 0.0f);
    buildEngineBlocking((int)apvts.getRawParameterValue(pid::oversample)->load());
    spec_ = {sampleRate, (uint32)maxBlock_, 1};
    convolution_.prepare(spec_);
    convolution_.reset();
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
        if (pending_ && rebuilder_ && !rebuilder_->retired) {
            std::swap(engine_, pending_);
            if (rebuilder_) rebuilder_->retired = std::move(pending_);
            engineOS_.store(engine_->params().oversampleLog2);
            const int lat = engine_->latencySamples();
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
    const bool useIR = mode == 2 && irLoaded_.load();

    int done = 0;
    float peak = 0.0f;
    while (done < total) {
        const int n = std::min(total - done, maxBlock_);
        // mono guitar signal: sum of the available input channels
        for (int i = 0; i < n; ++i) {
            float s = buffer.getReadPointer(0)[done + i];
            if (inCh > 1) s = 0.5f * (s + buffer.getReadPointer(1)[done + i]);
            monoIn_[(size_t)i] = s;
        }
        engine_->process(monoIn_.data(), outTerm_.data(), outAc_.data(), n);
        const float* res = outAc_.data();
        if (mode == 1) res = outTerm_.data();
        else if (useIR) {
            // User IR (voltage -> mic): fed with the speaker-terminal voltage; the provisional acoustic model is bypassed.
            for (int i = 0; i < n; ++i) irBuf_[(size_t)i] = outTerm_[(size_t)i];
            float* ch[1] = {irBuf_.data()};
            AudioBuffer<float> ab(ch, 1, n);
            dsp::AudioBlock<float> blk(ab);
            dsp::ProcessContextReplacing<float> ctx(blk);
            convolution_.process(ctx);
            res = irBuf_.data();
        } else if (mode == 2) res = outTerm_.data();
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
void CD57AudioProcessor::loadImpulseResponse(const File& f)
{
    if (!f.existsAsFile()) return;
    convolution_.loadImpulseResponse(f, dsp::Convolution::Stereo::no, dsp::Convolution::Trim::yes, 0, dsp::Convolution::Normalise::yes);
    const ScopedLock l(irLock_);
    irName_ = f.getFileName(); irPath_ = f.getFullPathName();
    irLoaded_.store(true);
}

AudioProcessorEditor* CD57AudioProcessor::createEditor() { return new CD57Editor(*this); }

void CD57AudioProcessor::getStateInformation(MemoryBlock& dest)
{
    auto state = apvts.copyState();
    { const ScopedLock l(irLock_); state.setProperty("irPath", irPath_, nullptr); }
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void CD57AudioProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size)) {
        auto tree = ValueTree::fromXml(*xml);
        if (tree.isValid()) {
            const String irp = tree.getProperty("irPath").toString();
            apvts.replaceState(tree);
            if (irp.isNotEmpty()) loadImpulseResponse(File(irp));
        }
    }
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CD57AudioProcessor(); }
