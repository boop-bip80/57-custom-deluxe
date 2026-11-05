// PluginProcessor.h -- VST3 / Standalone wrapper around the circuit-level engine.
#pragma once
#include <atomic>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

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
} // namespace id
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
    double getTailLengthSeconds() const override { return 1.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int size) override;

    // user impulse response (voltage -> microphone capture; replaces the provisional acoustic stage, never added to it)
    void loadImpulseResponse(const juce::File& f);
    juce::String irName() const { const juce::ScopedLock l(irLock_); return irName_; }

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
    juce::SpinLock swapLock_;
    std::unique_ptr<Rebuilder> rebuilder_;
    std::atomic<int> requestedOS_{1}, engineOS_{1};
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    std::string netlist_, manifestText_, manifestSummary_, manifestError_;

    std::vector<float> monoIn_, outTerm_, outAc_, irBuf_;
    juce::dsp::Convolution convolution_;
    juce::dsp::ProcessSpec spec_{};
    mutable juce::CriticalSection irLock_;
    juce::String irName_, irPath_;
    std::atomic<bool> irLoaded_{false};
    int blockCounter_ = 0;
    std::atomic<float>* pBypass_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CD57AudioProcessor)
};
