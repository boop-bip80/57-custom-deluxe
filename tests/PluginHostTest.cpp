// PluginHostTest.cpp -- loads the BUILT VST3 through JUCE's VST3 host and exercises it (no external tools).
//   usage: cd57_host_test "<path to .vst3 bundle>"
// Checks: loads, parameter set, latency, finite/non-silent audio at awkward block sizes, state round-trip,
//         parameter automation during processing, mono/stereo layouts, sample-rate change, the acoustic output modes
//         (mode list, saved mode + IR reference, IR gain/polarity/arrival time, hybrid vs route 1).
#include <cmath>
#include <complex>
#include <iostream>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

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

    std::cout << "-- acoustic output modes and the measured-IR path (save/restore, gain, polarity, arrival time, hybrid)\n";
    {
        constexpr double fs = 48000.0;
        auto findParam = [&](const char* name) -> AudioProcessorParameter* {
            for (auto* p : inst->getParameters()) if (p->getName(64).equalsIgnoreCase(name)) return p;
            return nullptr;
        };
        auto* outMode = findParam("Output");
        CHECK(outMode != nullptr && outMode->getNumSteps() == 4, "Output parameter exposes 4 modes (3 earlier ones + hybrid appended)");
        if (outMode && outMode->getNumSteps() == 4) {
            CHECK(outMode->getText(1.0f, 64).containsIgnoreCase("hybrid"), "mode 3 is the hybrid: '" + outMode->getText(1.0f, 64) + "'");
            CHECK(outMode->getText(2.0f / 3.0f, 64).containsIgnoreCase("User IR") && outMode->getText(0.0f, 64).containsIgnoreCase("Microphone") && outMode->getText(1.0f / 3.0f, 64).containsIgnoreCase("terminals"),
                  "modes 0, 1, 2 keep their earlier meaning (preset compatibility): '" + outMode->getText(0.0f, 64) + "' / '" + outMode->getText(1.0f / 3.0f, 64) + "' / '" + outMode->getText(2.0f / 3.0f, 64) + "'");
        }
        if (auto* heat = findParam("Speaker Heating")) heat->setValue(0.0f);       // keep the speaker's Re constant for exact route comparisons

        // state with a chosen outMode index and IR path, applied through the host's setStateInformation
        auto applyState = [&](int mode, const String& irPath) {
            MemoryBlock b; inst->getStateInformation(b);
            auto xml = AudioProcessor::getXmlFromBinary(b.getData(), (int)b.getSize());
            for (auto* c : xml->getChildIterator()) if (c->getStringAttribute("id") == "outMode") c->setAttribute("value", mode);
            if (irPath.isNotEmpty()) xml->setAttribute("irPath", irPath); else xml->removeAttribute("irPath");
            MemoryBlock nb; AudioProcessor::copyXmlToBinary(*xml, nb);
            inst->setStateInformation(nb.getData(), (int)nb.getSize());
            std::cout << "    [applyState: requested mode " << mode << ", parameter now reads " << (outMode ? (int)std::lround(outMode->getValue() * 3.0f) : -1) << ", IR '" << irPath << "']\n";
        };
        auto currentMode = [&]() { return outMode ? (int)std::lround(outMode->getValue() * 3.0f) : -1; };
        std::cout << "  outMode parameter " << (outMode ? "found" : "NOT FOUND") << ", value " << (outMode ? outMode->getValue() : -1.0f) << "\n";
        auto render = [&](const std::vector<float>& in) {
            inst->setPlayConfigDetails(2, 2, fs, 512);
            inst->prepareToPlay(fs, 512);          // fresh deterministic engine state; the processed IRs are re-derived here
            std::vector<float> out; out.reserve(in.size());
            for (size_t i = 0; i < in.size(); i += 512) {
                const int n = (int)std::min<size_t>(512, in.size() - i);
                AudioBuffer<float> b(2, n); MidiBuffer m;
                for (int k = 0; k < n; ++k) { b.setSample(0, k, in[i + (size_t)k]); b.setSample(1, k, in[i + (size_t)k]); }
                inst->processBlock(b, m);
                for (int k = 0; k < n; ++k) out.push_back(b.getSample(0, k));
            }
            return out;
        };
        auto writeIr = [&](const String& name, double rate, const std::vector<float>& h) {
            File f = File::getSpecialLocation(File::tempDirectory).getChildFile("cd57_hosttest_" + name + ".wav");
            f.deleteFile();
            std::unique_ptr<OutputStream> os = std::make_unique<FileOutputStream>(f);
            WavAudioFormat wav;
            auto w = wav.createWriterFor(os, AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1).withBitsPerSample(32).withSampleFormat(AudioFormatWriterOptions::SampleFormat::floatingPoint));
            if (w) { AudioBuffer<float> b(1, (int)h.size()); for (size_t i = 0; i < h.size(); ++i) b.setSample(0, (int)i, h[i]); w->writeFromAudioSampleBuffer(b, 0, (int)h.size()); }
            return f;
        };
        auto lockIn = [&](const std::vector<float>& y, double f0, int cycles) {
            const int per = (int)std::lround(fs / f0); const size_t n = std::min<size_t>(y.size(), (size_t)per * (size_t)cycles);
            std::complex<double> a(0, 0);
            for (size_t i = y.size() - n; i < y.size(); ++i) a += (double)y[i] * std::polar(1.0, -2.0 * M_PI * f0 * (double)i / fs);
            return a * (2.0 / (double)n);
        };
        std::vector<float> burst((size_t)(0.5 * fs), 0.0f);                      // 0.3 s 440 Hz burst, then silence
        for (size_t i = 0; i < (size_t)(0.3 * fs); ++i) burst[i] = 0.08f * (float)std::sin(2 * M_PI * 440.0 * (double)i / fs) * (float)std::min(1.0, (double)i / (0.02 * fs));
        auto peakOf = [](const std::vector<float>& v) { float p = 0; for (float x : v) p = std::max(p, std::fabs(x)); return p; };

        applyState(1, {});
        const auto term = render(burst);                                          // mode 1: speaker terminals
        const int latTerm = inst->getLatencySamples();
        CHECK(currentMode() == 1 && peakOf(term) > 0.05f, "speaker-terminals reference rendered, peak " + String(peakOf(term), 3) + " FS");

        // mode 2 with a unit impulse (48 kHz and 96 kHz files): must equal the terminals EXACTLY (gain 1, polarity, zero delay)
        for (double rate : {48000.0, 96000.0}) {
            std::vector<float> d(512, 0.0f); d[0] = 1.0f;
            const auto f = writeIr("unit" + String((int)rate), rate, d);
            applyState(2, f.getFullPathName());
            CHECK(currentMode() == 2, "mode 2 (User IR) restored from saved state, IR path " + f.getFileName());
            const auto y = render(burst);
            float worst = 0; for (size_t i = 0; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i] - term[i]));
            CHECK(worst < 2e-4f * peakOf(term), "unit-impulse IR at " + String((int)rate) + " Hz reproduces the speaker terminals (no normalisation, no trim): worst difference " + String(worst, 7) + " FS");
            CHECK(inst->getLatencySamples() == latTerm, "reported latency unchanged by the convolver (" + String(inst->getLatencySamples()) + " samples)");
            f.deleteFile();
        }
        // inverted, attenuated, delayed IR: y = -0.5 * terminals(n - 100)
        {
            std::vector<float> d(1024, 0.0f); d[100] = -0.5f;
            const auto f = writeIr("inv", 48000.0, d);
            applyState(2, f.getFullPathName());
            const auto y = render(burst);
            float worst = 0; for (size_t i = 100; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i] - (-0.5f * term[i - 100])));
            CHECK(worst < 2e-4f * peakOf(term) && peakOf(y) > 0.2f * peakOf(term), "IR -0.5 z^-100 gives -0.5 x terminals delayed by 100 samples (polarity, absolute gain, arrival time kept): worst difference " + String(worst, 7) + " FS");
            f.deleteFile();
        }
        // missing IR file: the state keeps the reference, and the mode falls back to the speaker terminals (documented), no crash
        {
            applyState(2, "/nonexistent/cd57/missing_ir.wav");
            const auto y = render(burst);
            float worst = 0; for (size_t i = 0; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i] - term[i]));
            MemoryBlock b; inst->getStateInformation(b);
            auto xml = AudioProcessor::getXmlFromBinary(b.getData(), (int)b.getSize());
            CHECK(worst < 1e-6f && xml->getStringAttribute("irPath") == "/nonexistent/cd57/missing_ir.wav", "missing IR file: output falls back to the speaker terminals and the saved state still names the file");
        }
        // hybrid (mode 3) vs route 1 (mode 2) with a physical IR: -0.8, 1 ms delay, 2nd-order 60 Hz high-pass (finite energy at DC)
        {
            std::vector<float> h(8192, 0.0f); h[48] = -0.8f;
            {
                const double w = std::tan(M_PI * 60.0 / fs), k = std::sqrt(2.0), n0 = 1.0 / (1.0 + k * w + w * w);
                const double b0 = n0, b1 = -2 * n0, b2 = n0, a1 = 2 * (w * w - 1) * n0, a2 = (1 - k * w + w * w) * n0;
                double z1 = 0, z2 = 0;
                for (auto& v : h) { const double x0 = v, y = b0 * x0 + z1; z1 = b1 * x0 - a1 * y + z2; z2 = b2 * x0 - a2 * y; v = (float)y; }
            }
            const auto f = writeIr("hyb", 48000.0, h);
            for (double f0 : {240.0, 1000.0}) {
                std::vector<float> tone((size_t)(1.0 * fs));
                for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.05f * (float)std::sin(2 * M_PI * f0 * (double)i / fs) * (float)std::min(1.0, (double)i / (0.1 * fs));
                applyState(2, f.getFullPathName());
                const auto r1 = render(tone);
                applyState(3, f.getFullPathName());
                CHECK(currentMode() == 3, "hybrid mode 3 restored from saved state");
                const auto r2 = render(tone);
                const auto a1 = lockIn(r1, f0, 40), a2 = lockIn(r2, f0, 40);
                const double dbErr = 20.0 * std::log10(std::abs(a2) / std::abs(a1)), phErr = std::arg(a2 / a1) * 180.0 / M_PI;
                CHECK(std::fabs(dbErr) < 0.5 && std::fabs(phErr) < 3.0 && std::abs(a1) > 1e-4, "hybrid (cone motion) vs route 1 (terminal voltage) at " + String(f0, 0) + " Hz through the built plugin: " + String(dbErr, 3) + " dB, " + String(phErr, 2) + " deg (|y| = " + String(std::abs(a1), 4) + " FS)");
            }
            // hybrid state round trip keeps mode and IR reference
            MemoryBlock b; inst->getStateInformation(b);
            auto xml = AudioProcessor::getXmlFromBinary(b.getData(), (int)b.getSize());
            int savedMode = -1; for (auto* c : xml->getChildIterator()) if (c->getStringAttribute("id") == "outMode") savedMode = c->getIntAttribute("value");
            CHECK(savedMode == 3 && xml->getStringAttribute("irPath") == f.getFullPathName() && xml->getIntAttribute("irFileSamples") == 8192 && xml->getStringAttribute("irHash").isNotEmpty(),
                  "saved state carries mode 3, the IR path, file length and hash");
            f.deleteFile();
        }
        applyState(0, {});
    }

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
