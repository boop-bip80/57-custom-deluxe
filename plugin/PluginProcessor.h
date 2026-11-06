// PluginProcessor.h -- VST3 / Standalone wrapper around the circuit-level engine.
#pragma once
#include <atomic>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "AcousticFactor.h"
#include "CD57Engine.h"
#include "EmbeddedData.h"

namespace cd57ui {
// parameter IDs (stable: they are saved in presets and exposed to host automation)
namespace id {
inline constexpr const char* instVol = "instVol";
inline constexpr const char* micVol = "micVol";
inline constexpr const char* tone = "tone";
inline constexpr const char* jack = "jack";
inline constexpr const char* patch = "patch";
inline constexpr const char* plug2 = "plug2";          // second plug: none / Inst 1 / Inst 2 / Mic 1 / Mic 2
inline constexpr const char* plug2Src = "plug2src";    // second plug source: terminated cable / right input channel
inline constexpr const char* power = "power";
inline constexpr const char* bypass = "bypass";
inline constexpr const char* load = "load";
inline constexpr const char* outMode = "outMode";
inline constexpr const char* micPos = "micPos";
inline constexpr const char* micDist = "micDist";
inline constexpr const char* inTrim = "inTrim";
inline constexpr const char* outLevel = "outLevel";
inline constexpr const char* inCal = "inCal";
inline constexpr const char* market = "market";
inline constexpr const char* linePct = "linePct";
inline constexpr const char* v1 = "v1";
inline constexpr const char* v2 = "v2";
inline constexpr const char* rect = "rect";
inline constexpr const char* oversample = "oversample";
inline constexpr const char* source = "source";
inline constexpr const char* srcR = "srcR";
inline constexpr const char* taper = "taper";
inline constexpr const char* variation = "variation";
inline constexpr const char* seed = "seed";
inline constexpr const char* hum = "hum";
inline constexpr const char* noise = "noise";
inline constexpr const char* heating = "heating";
inline constexpr const char* otSat = "otSat";
inline constexpr const char* inputCh = "inputCh";
} // namespace id
// values of the "outMode" choice parameter (saved in presets by index: keep the order, only ever append)
namespace outmode {
inline constexpr int provisionalMic = 0;     // PROVISIONAL cone-velocity -> mic operator (Acoustic.h)
inline constexpr int speakerTerminals = 1;   // calibrated speaker-terminal volts
inline constexpr int userIr = 2;             // route 1: measured voltage-to-mic IR applied to the terminal voltage
inline constexpr int userIrCone = 3;         // route 2 / hybrid: the same IR factored onto the model's cone motion (AcousticFactor.h)
} // namespace outmode
} // namespace cd57ui

class CD57AudioProcessor : public juce::AudioProcessor, private juce::AudioProcessorValueTreeState::Listener, private juce::AsyncUpdater
{
public:
    CD57AudioProcessor();
    ~CD57AudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "57 Custom Deluxe"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return std::max(1.0, irTailSeconds_.load()); }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int size) override;

    // Measured impulse response: voltage at the speaker terminals -> microphone signal (format and conventions: AcousticFactor.h).
    // It replaces the provisional acoustic stage and is never added to it:
    //   "User IR"           route 1 -- convolved with the terminal voltage;
    //   "User IR on cone motion (hybrid)"  route 2 -- the same IR factored into a residual that is convolved with the model's cone
    //                       velocity, so cone motion and voice-coil heating of the model stay in the loop without counting the cone twice.
    // Not called from the audio thread (reads the file, resamples, factors and hands the result to the convolvers).
    void loadImpulseResponse(const juce::File& f);
    juce::String irName() const { const juce::ScopedLock l(irLock_); return irName_; }
    juce::String irStatus() const { const juce::ScopedLock l(irLock_); return irStatus_; }

    juce::AudioProcessorValueTreeState apvts;

    // diagnostics published for the UI (lock-free)
    struct Diag {
        std::atomic<float> b1{0}, b2{0}, b3{0}, cathode{0}, coilTemp{0}, itPerStep{0}, pilot{1}, peak{0}, cpu{0};
        std::atomic<uint64_t> fallbacks{0}, failures{0}, steps{0}, panics{0};
        std::atomic<int> latency{0}, internalRateHz{0};
    } diag;
    static constexpr int kScope = 4096;
    float scope[kScope] = {};
    std::atomic<int> scopePos{0};
    std::string manifestSummary() const { return manifestSummary_; }
    std::string manifestError() const { return manifestError_; }

private:
    class Rebuilder;
    void parameterChanged(const juce::String& id, float) override;
    // latency changes are applied on the message thread through an AsyncUpdater: its destructor cancels any pending
    // update, so no callback can run after the processor is destroyed (a lambda capturing `this` could).
    void handleAsyncUpdate() override { setLatencySamples(latencyToApply_.load()); }
    std::atomic<int> latencyToApply_{0};
    cd57::EngineParams gatherParams() const;
    void buildEngineBlocking(int oversampleLog2);
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::unique_ptr<cd57::CD57Engine> engine_, pending_;
    // continuity across an engine swap (oversampling change): the new engine adopts the old one's circuit state and is primed with the
    // recent input; for kFade samples the retiring engine keeps running on the same input and the outputs are cross-faded
    static constexpr int kInHist = 512, kFade = 512;
    std::unique_ptr<cd57::CD57Engine> fadeEngine_;
    std::vector<float> inHist_, primeBuf_, fadeTerm_, fadeAc_, fadeW_;
    int inHistPos_ = 0, fadePos_ = 0;
    juce::SpinLock swapLock_;
    std::unique_ptr<Rebuilder> rebuilder_;
    std::atomic<int> requestedOS_{1}, engineOS_{1};
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    std::string netlist_, manifestText_, manifestSummary_, manifestError_;

    std::vector<float> monoIn_, monoIn2_, outTerm_, outAc_, irBuf_, coneBuf_;
    // Two zero-latency partitioned convolvers sharing one background queue: route 1 (terminal voltage * IR) and route 2
    // (cone volume velocity * residual).  Only the active one is run; it is reset when its mode is selected.
    juce::dsp::ConvolutionMessageQueue irQueue_;
    juce::dsp::Convolution convolution_{juce::dsp::Convolution::Latency{0}, irQueue_};
    juce::dsp::Convolution convolutionCone_{juce::dsp::Convolution::Latency{0}, irQueue_};
    juce::dsp::ProcessSpec spec_{};
    void rebuildImpulseResponsesLocked();          // irLock_ held; message/prepare thread only
    int totalLatency(int engineLatency) const { return engineLatency + convLatency_.load(); }
    mutable juce::CriticalSection irLock_;
    juce::String irName_, irPath_, irStatus_;
    std::vector<float> irRaw_;                     // raw capture (channel 0, file rate), never modified
    double irFileRate_ = 0.0;
    int irFileChannels_ = 0;
    juce::String irHash_;                          // hash of the raw samples, saved in the state
    cd57::SpeakerTS speaker_;                      // the model's speaker (manifest applied), source of G(f)
    std::atomic<bool> irLoaded_{false};
    std::atomic<double> coneInputScale_{0.0};      // Sd / gRef : cone velocity [m/s] -> residual input (volts-equivalent), see AcousticFactor.h
    std::atomic<double> irTailSeconds_{0.0};
    std::atomic<int> convLatency_{0};
    int lastMode_ = -1;
    int blockCounter_ = 0;
    std::atomic<float>* pBypass_ = nullptr;
    std::atomic<float>* pInputCh_ = nullptr;
    std::atomic<float>* pPlug2_ = nullptr;
    std::atomic<float>* pPlug2Src_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CD57AudioProcessor)
};
