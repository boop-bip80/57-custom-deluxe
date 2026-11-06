// PluginEditor.h -- front panel (tweed / chrome) plus a service panel exposing the circuit state.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class CD57LookAndFeel : public juce::LookAndFeel_V4
{
public:
    CD57LookAndFeel();
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool, int, int, int, int, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getLabelFont(juce::Label&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool, bool) override;
};

class JackComponent : public juce::Component
{
public:
    std::function<void()> onClick;
    juce::String label;
    bool selected = false;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { if (onClick) onClick(); }
};

class PowerSwitch : public juce::Component
{
public:
    std::function<void(bool)> onChange;
    bool on = true;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { on = !on; if (onChange) onChange(on); repaint(); }
};

class CD57Editor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit CD57Editor(CD57AudioProcessor&);
    ~CD57Editor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintTweed();
    void showProvenance();
    struct KnobBox { juce::Slider s; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> a; };
    struct ComboBoxRow { juce::ComboBox c; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> a; };
    struct SliderRow { juce::Slider s; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> a; };
    void knob(KnobBox&, const char* id, const char* text);
    void combo(ComboBoxRow&, const char* id, const char* text);
    void slider(SliderRow&, const char* id, const char* text);
    void setChoice(const char* id, int idx);

    CD57AudioProcessor& proc_;
    CD57LookAndFeel laf_;
    juce::Image tweed_;
    KnobBox kInst_, kMic_, kTone_;
    JackComponent jacks_[4];
    PowerSwitch powerSw_;
    std::unique_ptr<juce::ParameterAttachment> jackAtt_, powerAtt_;
    ComboBoxRow cPlug2_, cPlug2Src_, cPatch_, cLoad_, cOut_, cMarket_, cV1_, cV2_, cRect_, cOS_, cSource_, cTaper_, cInCh_;
    SliderRow sMicPos_, sMicDist_, sTrim_, sOutLevel_, sInCal_, sLine_, sSrcR_, sVar_, sSeed_, sHum_, sNoise_;
    juce::ToggleButton heatBtn_{"Speaker heating"}, bypassBtn_{"Bypass"}, otSatBtn_{"OT saturation (assumed)"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> heatAtt_, bypassAtt_, otSatAtt_;
    juce::TextButton irBtn_{"Load IR..."}, aboutBtn_{"Provenance / About"};
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Rectangle<int> plate_, service_, lab_, scopeR_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CD57Editor)
};
