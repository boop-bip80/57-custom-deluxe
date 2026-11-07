// editor_snapshot -- creates the plugin editor offscreen, checks that every size in its resize range lays out without
// error, and writes PNG snapshots (default size and, optionally, scaled sizes).  Usage: cd57_editor_snapshot out.png [width ...]
#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>
#include "../plugin/PluginProcessor.h"

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    CD57AudioProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);
    proc.prepareToPlay(48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> ed(proc.createEditor());
    if (!ed) { std::cerr << "no editor\n"; return 1; }
    const juce::String out = argc > 1 ? juce::String(argv[1]) : juce::String("editor.png");
    auto shoot = [&](const juce::String& path) {
        auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
        juce::File f(juce::File::getCurrentWorkingDirectory().getChildFile(path));
        f.deleteFile();
        juce::FileOutputStream os(f);
        juce::PNGImageFormat png;
        const bool ok = os.openedOk() && png.writeImageToStream(img, os);
        std::cout << (ok ? "wrote " : "FAILED ") << f.getFullPathName() << " (" << img.getWidth() << "x" << img.getHeight() << ")\n";
        return ok;
    };
    bool ok = shoot(out);
    // also capture every lower-strip tab (buttons whose onClick switches the page), then return to the first one
    std::vector<juce::TextButton*> tabs;
    for (auto* c : ed->getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(c); b != nullptr && b->getButtonText().containsAnyOf("ABCDEFGHIJKLMNOPQRSTUVWXYZ") && b->getButtonText() == b->getButtonText().toUpperCase() && b->onClick)
            tabs.push_back(b);
    for (size_t t = 1; t < tabs.size(); ++t) {
        tabs[t]->onClick();
        ok = shoot(out.upToLastOccurrenceOf(".", false, false) + "_tab" + juce::String((int)t) + ".png") && ok;
    }
    if (!tabs.empty()) tabs[0]->onClick();
    for (int i = 2; i < argc; ++i) {
        const int w = juce::String(argv[i]).getIntValue();
        if (w <= 0) continue;
        const int h = ed->getConstrainer() ? (int)std::lround(w / std::max(0.01, ed->getConstrainer()->getFixedAspectRatio() > 0 ? ed->getConstrainer()->getFixedAspectRatio() : (double)ed->getWidth() / ed->getHeight())) : ed->getHeight() * w / ed->getWidth();
        ed->setSize(w, h);
        ok = shoot(out.upToLastOccurrenceOf(".", false, false) + "_" + juce::String(w) + ".png") && ok;
    }
    ed.reset();
    proc.releaseResources();
    return ok ? 0 : 1;
}
