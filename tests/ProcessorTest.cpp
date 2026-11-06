// ProcessorTest.cpp -- exercises the plugin's AudioProcessor DIRECTLY (no VST3 host in between), so the raw saved state is visible.
//   usage: cd57_processor_test
// Checks: latency reporting per host rate; the input-channel choice; the acoustic output modes (mode list, saved mode + IR reference,
//         IR gain/polarity/arrival time, hybrid vs route 1); circuit-state continuity across an oversampling change.
#include <cmath>
#include <complex>
#include <iostream>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "CD57Engine.h"
#include "EmbeddedData.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

using namespace juce;
static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++failures; std::cout << "  FAIL: " << msg << "\n"; } else std::cout << "  ok:   " << msg << "\n"; } while (0)

static AudioProcessorParameter* findParam(AudioProcessor& p, const char* name)
{
    for (auto* q : p.getParameters()) if (q->getName(64).equalsIgnoreCase(name)) return q;
    return nullptr;
}

int main()
{
    ScopedJuceInitialiser_GUI init;
    std::unique_ptr<AudioProcessor> inst(createPluginFilter());
    CHECK(inst != nullptr && inst->getParameters().size() >= 27, "processor created directly, parameters exposed: " + String(inst ? inst->getParameters().size() : 0));
    if (!inst) return 1;

    std::cout << "-- reported latency equals the engine's exact, rate-dependent value\n";
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        inst->setPlayConfigDetails(2, 2, sr, 1024);
        inst->prepareToPlay(sr, 1024);
        cd57::EngineParams ep;                                             // the plugin default oversampling is 2x (choice index 1)
        ep.oversampleLog2 = 1;
        cd57::CD57Engine e; e.prepare(sr, 1024, cd57::embeddedNetlist(), ep, cd57::embeddedManifest());
        CHECK(inst->getLatencySamples() == e.latencySamples() && e.latencySamples() > 0, "host rate " + String((int)sr) + " Hz: reported latency " + String(inst->getLatencySamples()) + " samples = engine " + String(e.latencySamples()));
    }

    std::cout << "-- acoustic output modes and the measured-IR path (save/restore, gain, polarity, arrival time, hybrid)\n";
    {
        constexpr double fs = 48000.0;
        auto* outMode = findParam(*inst, "Output");
        CHECK(outMode != nullptr && outMode->getNumSteps() == 4, "Output parameter exposes 4 modes (3 earlier ones + hybrid appended)");
        if (outMode && outMode->getNumSteps() == 4) {
            CHECK(outMode->getText(1.0f, 64).containsIgnoreCase("hybrid"), "mode 3 is the hybrid: '" + outMode->getText(1.0f, 64) + "'");
            CHECK(outMode->getText(2.0f / 3.0f, 64).containsIgnoreCase("User IR") && outMode->getText(0.0f, 64).containsIgnoreCase("Microphone") && outMode->getText(1.0f / 3.0f, 64).containsIgnoreCase("terminals"),
                  "modes 0, 1, 2 keep their earlier meaning (preset compatibility): '" + outMode->getText(0.0f, 64) + "' / '" + outMode->getText(1.0f / 3.0f, 64) + "' / '" + outMode->getText(2.0f / 3.0f, 64) + "'");
        }
        if (auto* heat = findParam(*inst, "Speaker Heating")) heat->setValue(0.0f);       // keep the speaker's Re constant for exact route comparisons

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


    std::cout << "-- second plug (dossier Sec. 06): a second guitar on the right input, or a terminated cable\n";
    {
        constexpr double fs = 48000.0;
        auto* outMode = findParam(*inst, "Output");
        auto* plug2 = findParam(*inst, "Second Plug");
        auto* src2 = findParam(*inst, "Second Plug Source");
        auto* inCh = findParam(*inst, "Input channel");
        CHECK(plug2 != nullptr && plug2->getNumSteps() == 5 && src2 != nullptr && src2->getNumSteps() == 2, "Second Plug (none / Inst 1 / Inst 2 / Mic 1 / Mic 2) and Second Plug Source (terminated / right input) are exposed");
        if (outMode && plug2 && src2 && inCh) {
            outMode->setValueNotifyingHost(1.0f / 3.0f);
            std::vector<float> x((size_t)(0.3 * fs));
            for (size_t i = 0; i < x.size(); ++i) x[i] = 0.0008f * (float)std::sin(2 * M_PI * 1300.0 * (double)i / fs) * (float)std::min(1.0, (double)i / (0.03 * fs));
            const std::vector<float> zero(x.size(), 0.0f);
            auto render = [&](const std::vector<float>& l, const std::vector<float>& r, float plug, float source) {
                plug2->setValueNotifyingHost(plug); src2->setValueNotifyingHost(source); inCh->setValueNotifyingHost(0.5f);       // first source = left only
                inst->setPlayConfigDetails(2, 2, fs, 512); inst->prepareToPlay(fs, 512);
                std::vector<float> out;
                for (size_t i = 0; i < l.size(); i += 512) {
                    const int n = (int)std::min<size_t>(512, l.size() - i);
                    AudioBuffer<float> b(2, n); MidiBuffer m;
                    for (int k = 0; k < n; ++k) { b.setSample(0, k, l[i + (size_t)k]); b.setSample(1, k, r[i + (size_t)k]); }
                    inst->processBlock(b, m);
                    for (int k = 0; k < n; ++k) out.push_back(b.getSample(0, k));
                }
                return out;
            };
            auto line = [&](const std::vector<float>& v, double f) { const size_t n = (size_t)(0.2 * fs), s0 = v.size() - n; double c = 0, q = 0; for (size_t i = 0; i < n; ++i) { const double w = 2 * M_PI * f * (double)i / fs; c += v[s0 + i] * std::cos(w); q += v[s0 + i] * std::sin(w); } return std::sqrt(c * c + q * q) * 2.0 / (double)n; };
            // the right channel carries a 1.3 kHz tone; the left channel is silent.  Plug 2 = Instrument 2 (index 2 of 5 -> 0.5)
            const auto none = render(zero, x, 0.0f, 1.0f), terminated = render(zero, x, 0.5f, 0.0f), driven = render(zero, x, 0.5f, 1.0f);
            const double lNone = line(none, 1300.0), lTerm = line(terminated, 1300.0), lDriven = line(driven, 1300.0);
            CHECK(lDriven > 100.0 * lNone && lDriven > 100.0 * lTerm, "the right-channel signal reaches the amplifier only through a DRIVEN second plug (1.3 kHz line " + String(lDriven, 5) + " FS vs " + String(lTerm, 7) + " terminated, " + String(lNone, 7) + " no plug)");
            plug2->setValueNotifyingHost(0.0f); src2->setValueNotifyingHost(0.0f); inCh->setValueNotifyingHost(0.0f);
        }
    }

    std::cout << "-- input channel choice (dual-mono source stays calibrated; a one-sided source is selectable)\n";
    {
        constexpr double fs = 48000.0;
        auto* outMode = findParam(*inst, "Output");
        auto* inCh = findParam(*inst, "Input channel");
        CHECK(inCh != nullptr && inCh->getNumSteps() == 3, "Input channel parameter exposes 3 choices (mean / left / right)");
        if (outMode && inCh && inCh->getNumSteps() == 3) {
            outMode->setValueNotifyingHost(1.0f / 3.0f);                      // speaker terminals
            std::vector<float> x((size_t)(0.3 * fs));
            for (size_t i = 0; i < x.size(); ++i) x[i] = 0.05f * (float)std::sin(2 * M_PI * 330.0 * (double)i / fs) * (float)std::min(1.0, (double)i / (0.03 * fs));
            auto render = [&](const std::vector<float>& l, const std::vector<float>& r, float choice) {
                inCh->setValueNotifyingHost(choice);
                inst->setPlayConfigDetails(2, 2, fs, 512);
                inst->prepareToPlay(fs, 512);
                std::vector<float> out;
                for (size_t i = 0; i < l.size(); i += 512) {
                    const int n = (int)std::min<size_t>(512, l.size() - i);
                    AudioBuffer<float> b(2, n); MidiBuffer m;
                    for (int k = 0; k < n; ++k) { b.setSample(0, k, l[i + (size_t)k]); b.setSample(1, k, r[i + (size_t)k]); }
                    inst->processBlock(b, m);
                    for (int k = 0; k < n; ++k) out.push_back(b.getSample(0, k));
                }
                return out;
            };
            auto rms = [](const std::vector<float>& v) { double s = 0; for (size_t i = v.size() / 2; i < v.size(); ++i) s += (double)v[i] * v[i]; return std::sqrt(s / (double)(v.size() - v.size() / 2)); };
            const std::vector<float> zero(x.size(), 0.0f);
            const auto dualMean = render(x, x, 0.0f), leftOnly = render(x, zero, 0.5f), rightOnly = render(zero, x, 1.0f), meanOneSided = render(x, zero, 0.0f);
            float worstL = 0, worstR = 0;
            for (size_t i = 0; i < dualMean.size(); ++i) { worstL = std::max(worstL, std::fabs(dualMean[i] - leftOnly[i])); worstR = std::max(worstR, std::fabs(dualMean[i] - rightOnly[i])); }
            CHECK(worstL < 1e-6f && worstR < 1e-6f, "a dual-mono source gives the identical level with 'Mean', 'Left only' and 'Right only' (worst differences " + String(worstL, 8) + " / " + String(worstR, 8) + " FS)");
            CHECK(rms(meanOneSided) < 0.9 * rms(dualMean), "'Mean' on a one-sided source is lower (it averages with the silent channel), so 'Left only' / 'Right only' exist: rms " + String(rms(meanOneSided), 4) + " vs " + String(rms(dualMean), 4) + " FS");
            inCh->setValueNotifyingHost(0.0f);
        }
    }

    std::cout << "-- an oversampling change keeps the circuit state (rails, capacitor charge, flux): no pop, no offset\n";
    {
        constexpr double fs = 48000.0;
        auto* os = findParam(*inst, "Oversampling");
        auto* outMode = findParam(*inst, "Output");
        CHECK(os != nullptr && os->getNumSteps() == 4, "Oversampling parameter exposes 4 factors");
        if (os && outMode) {
            outMode->setValueNotifyingHost(1.0f / 3.0f);
            os->setValueNotifyingHost(1.0f / 3.0f);                           // 2x
            inst->setPlayConfigDetails(2, 2, fs, 512);
            inst->prepareToPlay(fs, 512);
            long t = 0;
            auto block = [&](std::vector<float>& out) {
                AudioBuffer<float> b(2, 512); MidiBuffer m;
                for (int k = 0; k < 512; ++k) { const float v = 0.05f * (float)std::sin(2 * M_PI * 330.0 * (double)(t + k) / fs); b.setSample(0, k, v); b.setSample(1, k, v); }
                t += 512;
                inst->processBlock(b, m);
                out.assign(b.getReadPointer(0), b.getReadPointer(0) + 512);
            };
            std::vector<float> blk;
            double preRms = 0, prePk = 0; int cnt = 0;
            for (int i = 0; i < 160; ++i) { block(blk); if (i >= 120) { for (float v : blk) { preRms += (double)v * v; prePk = std::max(prePk, (double)std::fabs(v)); } ++cnt; } }
            preRms = std::sqrt(preRms / (cnt * 512.0));
            os->setValueNotifyingHost(2.0f / 3.0f);                           // 4x: background rebuild, then a lock-free swap with state transfer
            double worstPk = 0, dc = 0, postRms = 0; int pc = 0;
            for (int i = 0; i < 400; ++i) {
                block(blk); Thread::sleep(2);
                const bool settled = i >= 60;                                 // the new engine appears after the rebuild thread finishes; allow for it
                for (float v : blk) { if (i >= 20) worstPk = std::max(worstPk, (double)std::fabs(v)); if (settled && i >= 300) { dc += v; postRms += (double)v * v; } }
                if (settled && i >= 300) ++pc;
            }
            dc /= pc * 512.0; postRms = std::sqrt(postRms / (pc * 512.0));
            std::cout << "    latency after the swap: " << inst->getLatencySamples() << " samples; pre-swap rms " << preRms << " peak " << prePk << "; post-swap rms " << postRms << " peak " << worstPk << " dc " << dc << "\n";
            CHECK(worstPk < 1.15 * prePk && std::fabs(dc) < 0.01 * prePk + 1e-4 && std::fabs(postRms / preRms - 1.0) < 0.1, "after the swap the signal continues at the same level with no pop or offset (peak <= 1.15x, |dc| ~ 0, rms within 10 %)");
        }
    }

    std::cout << (failures ? "RESULT: FAILED\n" : "RESULT: ALL PASSED\n");
    return failures ? 1 : 0;
}
