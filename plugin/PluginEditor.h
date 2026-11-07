// PluginEditor.h -- front panel in the spirit of a 1950s lacquered-tweed amplifier seen from above: tweed cabinet,
// brown chassis well, chrome control plate with cream pointer ("chicken-head") knobs, jewel pilot light and input
// jacks; every secondary setting lives in a tabbed lower strip.  All artwork is drawn procedurally in this file.
// The editor only attaches to existing parameters: it never adds, removes, renames or reorders any.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class CD57LookAndFeel : public juce::LookAndFeel_V4
{
public:
    CD57LookAndFeel();
    float scale = 1.0f;   // editor scale (1 = 1100 px wide)
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool, int, int, int, int, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool, bool) override;
};

// cream pointer knob on the chrome plate; value bubble on hover/drag, double-click resets to the parameter default
class PointerKnob : public juce::Slider
{
public:
    PointerKnob() : juce::Slider(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox) {}
    void mouseEnter(const juce::MouseEvent& e) override { juce::Slider::mouseEnter(e); repaint(); }
    void mouseExit(const juce::MouseEvent& e) override { juce::Slider::mouseExit(e); repaint(); }
};

class JackComponent : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;
    juce::String label;
    bool selected = false;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { if (onClick) onClick(); }
};

class ToggleSwitch : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void(bool)> onChange;
    juce::String label{"POWER"}, onText{"ON"}, offText{"OFF"};
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

    static constexpr int kBaseW = 1100, kBaseH = 680;

private:
    void timerCallback() override;
    void renderBackground();
    void showProvenance();
    void showTab(int);
    struct KnobBox { PointerKnob s; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> a; };
    struct ComboBoxRow { juce::ComboBox c; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> a; };
    struct SliderRow { juce::Slider s; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> a; };
    void knob(KnobBox&, const char* id, const char* text);
    void combo(ComboBoxRow&, const char* id, const char* text, int tab);
    void slider(SliderRow&, const char* id, const char* text, int tab);
    void setChoice(const char* id, int idx);
    int S(float v) const { return juce::roundToInt(v * sc_); }
    juce::Rectangle<int> R(float x, float y, float w, float h) const { return {S(x), S(y), S(w), S(h)}; }

    CD57AudioProcessor& proc_;
    CD57LookAndFeel laf_;
    float sc_ = 1.0f;
    juce::Image background_;
    KnobBox kInst_, kMic_, kTone_;
    JackComponent jacks_[4];
    ToggleSwitch powerSw_, standbySw_;
    std::unique_ptr<juce::ParameterAttachment> jackAtt_, powerAtt_, standbyAtt_;
    ComboBoxRow cPlug2_, cPlug2Src_, cPatch_, cLoad_, cExternalLoad_, cOut_, cMarket_, cV1_, cV2_, cRect_, cOS_, cSource_, cTaper_, cInCh_;
    SliderRow sMicPos_, sMicDist_, sTrim_, sOutLevel_, sInCal_, sLine_, sSrcR_, sVar_, sSeed_, sHum_, sNoise_;
    juce::ToggleButton heatBtn_{"Speaker heating"}, bypassBtn_{"Bypass"}, otSatBtn_{"OT saturation (assumed)"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> heatAtt_, bypassAtt_, otSatAtt_;
    juce::TextButton irBtn_{"Load IR..."}, aboutBtn_{"Provenance / About"};
    juce::TextButton tabBtn_[3];
    std::vector<std::pair<juce::Component*, int>> tabbed_;   // component -> tab index
    int tab_ = 0;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::TooltipWindow tooltips_{this, 600};
    juce::Rectangle<int> well_, plate_, strip_, content_, status_, scopeR_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CD57Editor)
};
