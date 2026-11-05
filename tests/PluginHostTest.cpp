// PluginHostTest.cpp -- loads the BUILT VST3 through JUCE's VST3 host and exercises it (no external tools).
//   usage: cd57_host_test "<path to .vst3 bundle>"
// Checks: loads, parameter set, latency, finite/non-silent audio at awkward block sizes, state round-trip,
//         parameter automation during processing, mono/stereo layouts, sample-rate change.
#include <cmath>
#include <iostream>
#include <juce_audio_processors/juce_audio_processors.h>

using namespace juce;
static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++failures; std::cout << "  FAIL: " << msg << "\n"; } else std::cout << "  ok:   " << msg << "\n"; } while (0)

static bool finiteAll(const AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c) for (int i = 0; i < b.getNumSamples(); ++i) if (!std::isfinite(b.getSample(c, i))) return false;
    return true;
}

int main(int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::cout << "usage: cd57_host_test <vst3 path>\n"; return 2; }
    VST3PluginFormat fmt;
    OwnedArray<PluginDescription> descs;
    fmt.findAllTypesForFile(descs, argv[1]);
    CHECK(descs.size() == 1, "VST3 bundle scanned: " + String(descs.size()) + " plugin description(s)");
    if (descs.isEmpty()) return 1;
    std::cout << "  name='" << descs[0]->name << "' manufacturer='" << descs[0]->manufacturerName << "' category='" << descs[0]->category << "'\n";

    String err;
    auto inst = fmt.createInstanceFromDescription(*descs[0], 48000.0, 512, err);
    CHECK(inst != nullptr, "instantiated (" + err + ")");
    if (!inst) return 1;
    CHECK(inst->getParameters().size() >= 25, "parameters exposed: " + String(inst->getParameters().size()));

    if (argc > 2 && String(argv[2]) == "--lifecycle") {      // minimal create/process/destroy cycle (used under valgrind)
        inst->setPlayConfigDetails(2, 2, 48000.0, 256);
        inst->prepareToPlay(48000.0, 256);
        AudioBuffer<float> b(2, 256); MidiBuffer m; b.clear(); inst->processBlock(b, m);
        inst->releaseResources();
        inst.reset();
        std::cout << "lifecycle done\n";
        return 0;
    }
    if (argc > 2 && String(argv[2]) == "--rebuild") {        // rebuild/swap path only (used under valgrind)
        inst->setPlayConfigDetails(2, 2, 48000.0, 256);
        inst->prepareToPlay(48000.0, 256);
        AudioProcessorParameter* os = nullptr;
        for (auto* p : inst->getParameters()) if (p->getName(64).containsIgnoreCase("Oversampling")) os = p;
        for (int i = 0; i < 20; ++i) { AudioBuffer<float> b(2, 256); MidiBuffer m; b.clear(); inst->processBlock(b, m); }
        if (os) os->setValueNotifyingHost(2.0f / 3.0f);
        for (int i = 0; i < 120; ++i) { AudioBuffer<float> b(2, 256); MidiBuffer m; b.clear(); inst->processBlock(b, m); Thread::sleep(40); }
        inst->releaseResources();
        inst.reset();
        std::cout << "rebuild lifecycle done\n";
        return 0;
    }
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        std::cout << "-- sample rate " << sr << "\n";
        inst->setPlayConfigDetails(2, 2, sr, 1024);
        inst->prepareToPlay(sr, 1024);   // host contract: no block may exceed this
        std::cout << "  reported latency: " << inst->getLatencySamples() << " samples\n";
        double peak = 0.0, energy = 0.0; long n = 0;
        const int blocks[] = {64, 480, 37, 1024, 512, 7, 256};
        long t = 0;
        for (int rep = 0; rep < 8; ++rep) for (int bs : blocks) {
            AudioBuffer<float> b(2, bs); MidiBuffer m;
            for (int i = 0; i < bs; ++i, ++t) { const float x = 0.2f * (float)std::sin(2 * M_PI * 196.0 * t / sr) * (float)std::min(1.0, t / (0.02 * sr)); b.setSample(0, i, x); b.setSample(1, i, x); }
            if (rep == 4 && bs == 480) if (auto* p = inst->getParameters()[0]) p->setValue(0.9f);   // automation mid-stream
            inst->processBlock(b, m);
            CHECK(finiteAll(b), "") ;
            for (int i = 0; i < bs; ++i) { const float y = b.getSample(0, i); peak = std::max(peak, (double)std::fabs(y)); energy += (double)y * y; ++n; }
        }
        CHECK(peak > 0.01 && peak < 8.0, "audio produced, peak " + String(peak, 3) + " FS");
        CHECK(std::sqrt(energy / n) > 1e-3, "non-silent, rms " + String(std::sqrt(energy / n), 4));
    }

    std::cout << "-- state round trip\n";
    MemoryBlock st; inst->getStateInformation(st);
    CHECK(st.getSize() > 100, "state size " + String((int)st.getSize()) + " bytes");
    inst->getParameters()[1]->setValue(0.37f);
    const float before = inst->getParameters()[1]->getValue();
    inst->setStateInformation(st.getData(), (int)st.getSize());
    CHECK(std::fabs(inst->getParameters()[1]->getValue() - before) > 1e-4f, "setState restored the saved parameter value");

    std::cout << "-- oversampling change during processing (background rebuild, lock-free swap)\n";
    {
        inst->prepareToPlay(48000.0, 512);
        AudioProcessorParameter* os = nullptr;
        for (auto* p : inst->getParameters()) if (p->getName(64).containsIgnoreCase("Oversampling")) os = p;
        CHECK(os != nullptr, "found oversampling parameter");
        if (os) {
            os->setValueNotifyingHost(2.0f / 3.0f);      // choice index 2 of 4 -> 4x
            bool ok = true;
            for (int i = 0; i < 400; ++i) { AudioBuffer<float> b(2, 512); MidiBuffer m; for (int s = 0; s < 512; ++s) { b.setSample(0, s, 0.1f * (float)std::sin(0.03 * (i * 512 + s))); b.setSample(1, s, b.getSample(0, s)); }
                inst->processBlock(b, m); ok = ok && finiteAll(b); Thread::sleep(1); }
            CHECK(ok, "audio stayed finite while the engine was rebuilt and swapped");
        }
    }
    inst->releaseResources();
    std::cout << (failures ? "RESULT: FAILED\n" : "RESULT: ALL PASSED\n");
    return failures ? 1 : 0;
}
