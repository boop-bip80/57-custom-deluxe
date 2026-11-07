#include "PluginEditor.h"

using namespace juce;
namespace pid = cd57ui::id;

namespace {
// palette: lacquered tweed, brown chassis, chrome plate, cream knobs (all colours chosen here; no external artwork)
const Colour kTweed(0xffcf9f52), kTweedLight(0xffe6c27a), kTweedDark(0xff8f6326), kLacquer(0xff6b3f12),
             kChrome(0xffe4e6e8), kChromeMid(0xffb9bcbf), kChromeDark(0xff7d8184), kInk(0xff2a1d14), kEngrave(0xff3b3027),
             kBrown(0xff4a2615), kWell(0xff24170e), kStrip(0xff1b1410), kStripLine(0xff5d4630), kCream(0xfff1e4c3),
             kCreamDark(0xffc9b68c), kAmber(0xffffc04d), kJewel(0xffff3324);
Font F(float h, bool bold = false) { return Font(FontOptions(h).withStyle(bold ? "Bold" : "Regular")); }
} // namespace

// ---------------------------------------------------------------------------------------------------- look & feel
CD57LookAndFeel::CD57LookAndFeel()
{
    setColour(Label::textColourId, kCream);
    setColour(ComboBox::backgroundColourId, Colour(0xff2c2018));
    setColour(ComboBox::textColourId, kCream);
    setColour(ComboBox::outlineColourId, kStripLine);
    setColour(ComboBox::arrowColourId, kCream);
    setColour(PopupMenu::backgroundColourId, Colour(0xff2c2018));
    setColour(PopupMenu::textColourId, kCream);
    setColour(PopupMenu::highlightedBackgroundColourId, kBrown);
    setColour(TextButton::buttonColourId, Colour(0xff3a2b1f));
    setColour(TextButton::buttonOnColourId, kCream);
    setColour(TextButton::textColourOffId, kCream);
    setColour(TextButton::textColourOnId, kInk);
    setColour(ToggleButton::textColourId, kCream);
    setColour(Slider::textBoxTextColourId, kCream);
    setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour(Slider::textBoxBackgroundColourId, Colour(0xff2c2018));
    setColour(TooltipWindow::backgroundColourId, Colour(0xf02c2018));
    setColour(TooltipWindow::textColourId, kCream);
    setColour(TooltipWindow::outlineColourId, kStripLine);
}

Font CD57LookAndFeel::getComboBoxFont(ComboBox&) { return F(12.5f * scale); }
Font CD57LookAndFeel::getPopupMenuFont() { return F(13.5f * scale); }
Font CD57LookAndFeel::getLabelFont(Label& l) { return l.getFont(); }
Font CD57LookAndFeel::getTextButtonFont(TextButton&, int h) { return F(jmin(13.0f * scale, h * 0.55f), true); }

void CD57LookAndFeel::drawButtonBackground(Graphics& g, Button& b, const Colour&, bool over, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    const bool on = b.getToggleState();
    g.setColour(on ? kCream : Colour(0xff3a2b1f).brighter(over ? 0.15f : 0.0f).darker(down ? 0.2f : 0.0f));
    g.fillRoundedRectangle(r, 4.0f * scale);
    g.setColour(on ? kCreamDark : kStripLine);
    g.drawRoundedRectangle(r, 4.0f * scale, 1.0f);
}

void CD57LookAndFeel::drawRotarySlider(Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider& s)
{
    const auto bounds = Rectangle<float>((float)x, (float)y, (float)w, (float)h);
    const float bubbleH = 20.0f * scale;
    const auto area = bounds.withTrimmedBottom(bubbleH + 2.0f * scale);
    const float R = jmin(area.getWidth(), area.getHeight()) * 0.5f;      // radius of the printed scale's numbers
    const auto ctr = area.getCentre();
    const float kr = R * 0.56f;                                          // skirt radius
    // printed scale 1..12 on the plate (engraved: dark fill with a light lower edge)
    for (int i = 1; i <= 12; ++i) {
        const float a = a0 + (a1 - a0) * (float)(i - 1) / 11.0f;
        const float sn = std::sin(a), cs = -std::cos(a);
        const float r0 = kr + 4.0f * scale, r1 = kr + 9.0f * scale;
        g.setColour(kEngrave);
        g.drawLine(ctr.x + sn * r0, ctr.y + cs * r0, ctr.x + sn * r1, ctr.y + cs * r1, 1.4f * scale);
        const float rt = kr + 19.0f * scale;
        const auto tr = Rectangle<float>(ctr.x + sn * rt - 10.0f * scale, ctr.y + cs * rt - 7.0f * scale, 20.0f * scale, 14.0f * scale);
        g.setFont(F(11.5f * scale, true));
        g.setColour(Colours::white.withAlpha(0.55f)); g.drawText(String(i), tr.translated(0, 0.8f * scale), Justification::centred);
        g.setColour(kEngrave); g.drawText(String(i), tr, Justification::centred);
    }
    // drop shadow
    g.setColour(Colours::black.withAlpha(0.28f));
    g.fillEllipse(Rectangle<float>(kr * 2.1f, kr * 2.1f).withCentre(ctr.translated(2.5f * scale, 4.0f * scale)));
    // skirt: cream disc with knurled rim and a soft dome
    const auto sk = Rectangle<float>(kr * 2.0f, kr * 2.0f).withCentre(ctr);
    g.setGradientFill(ColourGradient(kCream.brighter(0.15f), sk.getX(), sk.getY(), kCreamDark.darker(0.15f), sk.getRight(), sk.getBottom(), false));
    g.fillEllipse(sk);
    g.setColour(kCreamDark.darker(0.35f));
    for (int i = 0; i < 36; ++i) {
        const float a = MathConstants<float>::twoPi * (float)i / 36.0f;
        g.drawLine(ctr.x + std::sin(a) * kr * 0.86f, ctr.y - std::cos(a) * kr * 0.86f, ctr.x + std::sin(a) * kr * 0.98f, ctr.y - std::cos(a) * kr * 0.98f, 0.9f * scale);
    }
    g.setColour(kInk.withAlpha(0.45f)); g.drawEllipse(sk, 1.0f * scale);
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(0.35f), ctr.x - kr * 0.4f, ctr.y - kr * 0.5f, Colours::transparentWhite, ctr.x + kr * 0.3f, ctr.y + kr * 0.4f, true));
    g.fillEllipse(sk.reduced(kr * 0.2f));
    // pointer ("chicken head"): long tapered nose, short rounded tail
    const float ang = a0 + pos * (a1 - a0);
    Path ptr;
    const float nose = kr * 1.22f, tail = kr * 0.62f, half = kr * 0.36f;
    ptr.startNewSubPath(0.0f, -nose);
    ptr.cubicTo(half * 0.35f, -nose * 0.75f, half, -kr * 0.25f, half, 0.0f);
    ptr.cubicTo(half, tail * 0.75f, half * 0.55f, tail, 0.0f, tail);
    ptr.cubicTo(-half * 0.55f, tail, -half, tail * 0.75f, -half, 0.0f);
    ptr.cubicTo(-half, -kr * 0.25f, -half * 0.35f, -nose * 0.75f, 0.0f, -nose);
    ptr.closeSubPath();
    const auto xf = AffineTransform::rotation(ang).translated(ctr.x, ctr.y);
    g.setColour(Colours::black.withAlpha(0.30f));
    g.fillPath(ptr, xf.translated(2.0f * scale, 3.0f * scale));
    g.setGradientFill(ColourGradient(kCream.brighter(0.2f), ctr.x - kr, ctr.y - kr, kCreamDark, ctr.x + kr, ctr.y + kr, false));
    g.fillPath(ptr, xf);
    g.setColour(kInk.withAlpha(0.5f));
    g.strokePath(ptr, PathStrokeType(1.0f * scale), xf);
    Path line; line.startNewSubPath(0.0f, -nose * 0.92f); line.lineTo(0.0f, -kr * 0.25f);
    g.setColour(kBrown); g.strokePath(line, PathStrokeType(2.0f * scale, PathStrokeType::curved, PathStrokeType::rounded), xf);
    g.setColour(kCreamDark.darker(0.2f)); g.fillEllipse(Rectangle<float>(kr * 0.22f, kr * 0.22f).withCentre(ctr));   // set-screw
    // value readout while hovering or dragging
    if (s.isMouseOverOrDragging()) {
        const String txt = s.getTextFromValue(s.getValue());
        g.setFont(F(12.0f * scale, true));
        const float tw = jmax(48.0f * scale, GlyphArrangement::getStringWidth(g.getCurrentFont(), txt) + 16.0f * scale);
        const auto pill = Rectangle<float>(tw, bubbleH).withCentre({ctr.x, bounds.getBottom() - bubbleH * 0.5f - 1.0f});
        g.setColour(kStrip.withAlpha(0.92f)); g.fillRoundedRectangle(pill, bubbleH * 0.5f);
        g.setColour(kAmber); g.drawRoundedRectangle(pill, bubbleH * 0.5f, 1.0f);
        g.setColour(kCream); g.drawText(txt, pill, Justification::centred);
    }
}

void CD57LookAndFeel::drawLinearSlider(Graphics& g, int x, int y, int w, int h, float pos, float, float, Slider::SliderStyle, Slider& s)
{
    const float cy = (float)y + h * 0.5f, th = 4.0f * scale, kr = 6.5f * scale;
    g.setColour(Colour(0xff0f0b08));
    g.fillRoundedRectangle((float)x, cy - th * 0.5f, (float)w, th, th * 0.5f);
    g.setColour(kAmber.withAlpha(0.65f));
    g.fillRoundedRectangle((float)x, cy - th * 0.5f, jmax(0.0f, pos - (float)x), th, th * 0.5f);
    g.setGradientFill(ColourGradient(kCream.brighter(0.2f), pos, cy - kr, kCreamDark, pos, cy + kr, false));
    g.fillEllipse(pos - kr, cy - kr, kr * 2, kr * 2);
    g.setColour(s.isMouseOverOrDragging() ? kAmber : kInk.withAlpha(0.6f));
    g.drawEllipse(pos - kr, cy - kr, kr * 2, kr * 2, 1.0f);
}

void CD57LookAndFeel::drawComboBox(Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& b)
{
    const auto r = Rectangle<float>(0, 0, (float)w, (float)h).reduced(0.5f);
    g.setColour(b.findColour(ComboBox::backgroundColourId).brighter(b.isMouseOver() ? 0.12f : 0.0f));
    g.fillRoundedRectangle(r, 4.0f * scale);
    g.setColour(b.isMouseOver() ? kAmber.withAlpha(0.7f) : b.findColour(ComboBox::outlineColourId));
    g.drawRoundedRectangle(r, 4.0f * scale, 1.0f);
    const float ax = (float)w - 12.0f * scale, ay = (float)h * 0.5f;
    Path a; a.addTriangle(ax - 4.0f * scale, ay - 2.0f * scale, ax + 4.0f * scale, ay - 2.0f * scale, ax, ay + 3.0f * scale);
    g.setColour(kCream); g.fillPath(a);
}

void CD57LookAndFeel::drawToggleButton(Graphics& g, ToggleButton& b, bool over, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    const float s = 15.0f * scale;
    const auto box = Rectangle<float>(2.0f, (r.getHeight() - s) * 0.5f, s, s);
    g.setColour(Colour(0xff0f0b08)); g.fillRoundedRectangle(box, 3.0f * scale);
    g.setColour(over ? kAmber : kStripLine); g.drawRoundedRectangle(box, 3.0f * scale, 1.0f);
    if (b.getToggleState()) { g.setColour(kAmber); g.fillRoundedRectangle(box.reduced(3.5f * scale), 2.0f); }
    g.setColour(kCream); g.setFont(F(12.5f * scale));
    g.drawText(b.getButtonText(), (int)(box.getRight() + 6 * scale), 0, b.getWidth() - (int)box.getRight() - 6, b.getHeight(), Justification::centredLeft);
}

// ---------------------------------------------------------------------------------------------------- small components
void JackComponent::paint(Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const float d = jmin(r.getWidth(), r.getHeight() - r.getHeight() * 0.22f) * 0.72f;
    const auto c = Rectangle<float>(d, d).withCentre({r.getCentreX(), r.getY() + d * 0.5f + r.getHeight() * 0.06f});
    // chrome hex nut + bushing
    Path nut; nut.addPolygon(c.getCentre(), 6, d * 0.5f, MathConstants<float>::pi / 6.0f);
    g.setColour(Colours::black.withAlpha(0.25f)); g.fillPath(nut, AffineTransform::translation(1.5f, 2.5f));
    g.setGradientFill(ColourGradient(kChrome, c.getX(), c.getY(), kChromeDark, c.getRight(), c.getBottom(), false));
    g.fillPath(nut);
    g.setColour(kInk.withAlpha(0.35f)); g.strokePath(nut, PathStrokeType(1.0f));
    g.setColour(Colour(0xff0b0907)); g.fillEllipse(c.reduced(d * 0.24f));
    g.setColour(kChromeDark); g.drawEllipse(c.reduced(d * 0.24f), 1.4f);
    if (selected) {   // plug inserted: nickel sleeve and a cable leaving downward
        g.setGradientFill(ColourGradient(Colour(0xfff0e2b0), c.getX(), c.getY(), Colour(0xff8e7a3a), c.getRight(), c.getBottom(), false));
        g.fillEllipse(c.reduced(d * 0.30f));
        g.setColour(Colour(0xff15110d)); g.fillEllipse(c.reduced(d * 0.40f));
    }
    g.setFont(F(r.getHeight() * 0.15f, true));
    const auto lr = r.withTop(r.getBottom() - r.getHeight() * 0.22f);
    g.setColour(Colours::white.withAlpha(0.5f)); g.drawText(label, lr.translated(0, 0.8f), Justification::centred);
    g.setColour(kEngrave); g.drawText(label, lr, Justification::centred);
}

void ToggleSwitch::paint(Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const float u = r.getWidth() / 80.0f;
    const auto txtH = 14.0f * u;
    g.setFont(F(11.0f * u, true));
    g.setColour(on ? kEngrave : kEngrave.withAlpha(0.45f)); g.drawText(onText, r.withHeight(txtH), Justification::centred);
    g.setColour(!on ? kEngrave : kEngrave.withAlpha(0.45f)); g.drawText(offText, r.withTrimmedBottom(txtH).removeFromBottom(txtH), Justification::centred);
    const float cx = r.getCentreX(), cy = r.getCentreY() - 2.0f * u;
    // chrome nut and bat lever (seen from above: the lever leans towards the active legend)
    Path nut; nut.addPolygon({cx, cy}, 6, 14.0f * u, 0.0f);
    g.setGradientFill(ColourGradient(kChrome, cx - 14 * u, cy - 14 * u, kChromeDark, cx + 14 * u, cy + 14 * u, false));
    g.fillPath(nut);
    g.setColour(kInk.withAlpha(0.35f)); g.strokePath(nut, PathStrokeType(1.0f));
    const float dir = on ? -1.0f : 1.0f;
    Path bat; bat.addRoundedRectangle(cx - 4.5f * u, jmin(cy, cy + dir * 26.0f * u), 9.0f * u, 26.0f * u, 4.5f * u);
    g.setColour(Colours::black.withAlpha(0.3f)); g.fillPath(bat, AffineTransform::translation(2.0f * u, 3.0f * u));
    g.setGradientFill(ColourGradient(Colours::white, cx - 4 * u, cy, kChromeDark, cx + 5 * u, cy, false));
    g.fillPath(bat);
    g.setColour(kChromeMid); g.fillEllipse(cx - 5.5f * u, cy + dir * 26.0f * u - 5.5f * u, 11.0f * u, 11.0f * u);
    g.setFont(F(11.5f * u, true));
    const auto lr = r.withTop(r.getBottom() - txtH);
    g.setColour(Colours::white.withAlpha(0.5f)); g.drawText(label, lr.translated(0, 0.8f), Justification::centred);
    g.setColour(kEngrave); g.drawText(label, lr, Justification::centred);
}

// ---------------------------------------------------------------------------------------------------- editor
CD57Editor::CD57Editor(CD57AudioProcessor& p) : AudioProcessorEditor(&p), proc_(p)
{
    setLookAndFeel(&laf_);

    knob(kInst_, pid::instVol, "INSTRUMENT");
    knob(kMic_, pid::micVol, "MIC");
    knob(kTone_, pid::tone, "TONE");

    static const char* jn[4] = {"INST 1", "INST 2", "MIC 1", "MIC 2"};
    for (int i = 0; i < 4; ++i) {
        jacks_[i].label = jn[i];
        jacks_[i].setTooltip("Click to plug the guitar into this jack");
        jacks_[i].onClick = [this, i] { setChoice(pid::jack, i); };
        addAndMakeVisible(jacks_[i]);
    }
    jackAtt_ = std::make_unique<ParameterAttachment>(*proc_.apvts.getParameter(pid::jack), [this](float v) {
        const int sel = (int)std::lround(v);
        for (int i = 0; i < 4; ++i) { jacks_[i].selected = (i == sel); jacks_[i].repaint(); }
    });
    jackAtt_->sendInitialUpdate();

    addAndMakeVisible(powerSw_);
    powerSw_.setTooltip("Mains power");
    powerSw_.onChange = [this](bool on) { powerAtt_->setValueAsCompleteGesture(on ? 1.0f : 0.0f); };
    powerAtt_ = std::make_unique<ParameterAttachment>(*proc_.apvts.getParameter(pid::power), [this](float v) { powerSw_.on = v > 0.5f; powerSw_.repaint(); });
    powerAtt_->sendInitialUpdate();
    standbySw_.label = "STANDBY";
    standbySw_.onText = "PLAY";
    standbySw_.offText = "STBY";
    standbySw_.setTooltip("PLAY: audible operation with POWER on. STBY: HV supply interrupted while heaters stay warm (physical switch topology provisional).");
    addAndMakeVisible(standbySw_);
    standbySw_.onChange = [this](bool on) { standbyAtt_->setValueAsCompleteGesture(on ? 1.0f : 0.0f); };
    standbyAtt_ = std::make_unique<ParameterAttachment>(*proc_.apvts.getParameter(pid::standbyOn), [this](float v) { standbySw_.on = v > 0.5f; standbySw_.repaint(); });
    standbyAtt_->sendInitialUpdate();

    // lower strip, tab 0: amp setup
    combo(cOut_, pid::outMode, "Output", 0);
    combo(cLoad_, pid::load, "Original/internal load", 0);
    cLoad_.c.setTooltip("Original 5-mode selector retained exactly for host automation compatibility. Internal + extension is NOT recommended by Fender; dummy loads are bench modes.");
    combo(cExternalLoad_, pid::externalLoad, "External setup (override)", 0);
    cExternalLoad_.c.setTooltip("Default NONE uses the original load selector. Fender-supported: one external 8 ohm at INTERNAL jack, or two external 16 ohm speakers at their specified jacks. Both use provisional resistive electrical proxies, with NO external cabinet acoustics.");
    combo(cOS_, pid::oversample, "Oversampling", 0);
    combo(cMarket_, pid::market, "Market variant", 0);
    combo(cPatch_, pid::patch, "Jumper cable", 0);
    combo(cInCh_, pid::inputCh, "Input channel", 0);
    combo(cSource_, pid::source, "Source", 0);
    combo(cV1_, pid::v1, "V1 tube", 0);
    combo(cV2_, pid::v2, "V2 tube", 0);
    combo(cRect_, pid::rect, "Rectifier", 0);
    combo(cTaper_, pid::taper, "Volume taper (assumed)", 0);
    combo(cPlug2_, pid::plug2, "Second plug", 0);
    combo(cPlug2Src_, pid::plug2Src, "Second plug source", 0);
    for (auto* b : {&otSatBtn_, &heatBtn_, &bypassBtn_}) { addAndMakeVisible(*b); tabbed_.push_back({b, 0}); }
    otSatAtt_ = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(proc_.apvts, pid::otSat, otSatBtn_);
    heatAtt_ = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(proc_.apvts, pid::heating, heatBtn_);
    bypassAtt_ = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(proc_.apvts, pid::bypass, bypassBtn_);
    for (auto* b : {&irBtn_, &aboutBtn_}) { addAndMakeVisible(*b); tabbed_.push_back({b, 0}); }
    irBtn_.onClick = [this] {
        chooser_ = std::make_unique<FileChooser>("Select a speaker-terminal-voltage to microphone IR (untrimmed, not normalised; see Provenance)", File(), "*.wav;*.aif;*.aiff;*.flac");
        chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this](const FileChooser& fc) {
            const auto f = fc.getResult();
            if (f.existsAsFile()) { proc_.loadImpulseResponse(f); setChoice(pid::outMode, cd57ui::outmode::userIr); }
        });
    };
    aboutBtn_.onClick = [this] { showProvenance(); };

    // tab 1: calibration / lab
    slider(sTrim_, pid::inTrim, "Input trim", 1);
    slider(sOutLevel_, pid::outLevel, "Output level", 1);
    slider(sInCal_, pid::inCal, "Input cal (V pk @ FS)", 1);
    slider(sLine_, pid::linePct, "Line voltage", 1);
    slider(sSrcR_, pid::srcR, "Source R", 1);
    slider(sMicPos_, pid::micPos, "Mic position (provisional model)", 1);
    slider(sMicDist_, pid::micDist, "Mic distance (provisional model)", 1);
    slider(sVar_, pid::variation, "Specimen variation", 1);
    slider(sSeed_, pid::seed, "Variation seed", 1);
    slider(sHum_, pid::hum, "Hum", 1);
    slider(sNoise_, pid::noise, "Noise", 1);

    static const char* tn[3] = {"AMP SETUP", "CALIBRATION / LAB", "SERVICE"};
    for (int i = 0; i < 3; ++i) {
        tabBtn_[i].setButtonText(tn[i]);
        tabBtn_[i].setClickingTogglesState(false);
        tabBtn_[i].onClick = [this, i] { showTab(i); };
        addAndMakeVisible(tabBtn_[i]);
    }
    showTab(0);

    setResizable(true, true);
    setResizeLimits(kBaseW * 7 / 10, kBaseH * 7 / 10, kBaseW * 3 / 2, kBaseH * 3 / 2);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio((double)kBaseW / (double)kBaseH);
    setSize(kBaseW, kBaseH);
    startTimerHz(20);
}

CD57Editor::~CD57Editor() { stopTimer(); setLookAndFeel(nullptr); }

void CD57Editor::knob(KnobBox& k, const char* id, const char* text)
{
    k.s.setRotaryParameters(degreesToRadians(-150.0f), degreesToRadians(150.0f), true);
    k.s.setNumDecimalPlacesToDisplay(1);
    k.s.setMouseDragSensitivity(220);
    k.s.setPopupDisplayEnabled(false, false, nullptr);
    k.s.setDoubleClickReturnValue(true, proc_.apvts.getParameter(id)->convertFrom0to1(proc_.apvts.getParameter(id)->getDefaultValue()));
    k.s.setTooltip(String(text) + " (drag; double-click resets)");
    k.l.setText(text, dontSendNotification); k.l.setJustificationType(Justification::centred);
    k.l.setColour(Label::textColourId, kEngrave);
    k.l.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(k.s); addAndMakeVisible(k.l);
    k.a = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(proc_.apvts, id, k.s);
}
void CD57Editor::combo(ComboBoxRow& c, const char* id, const char* text, int tab)
{
    c.l.setText(text, dontSendNotification);
    c.l.setColour(Label::textColourId, kCreamDark);
    if (auto* ch = dynamic_cast<AudioParameterChoice*>(proc_.apvts.getParameter(id))) c.c.addItemList(ch->choices, 1);
    addAndMakeVisible(c.c); addAndMakeVisible(c.l);
    tabbed_.push_back({&c.c, tab}); tabbed_.push_back({&c.l, tab});
    c.a = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment>(proc_.apvts, id, c.c);
}
void CD57Editor::slider(SliderRow& s, const char* id, const char* text, int tab)
{
    s.s.setSliderStyle(Slider::LinearHorizontal);
    s.s.setTextBoxStyle(Slider::TextBoxRight, false, 70, 18);
    s.s.setDoubleClickReturnValue(true, proc_.apvts.getParameter(id)->convertFrom0to1(proc_.apvts.getParameter(id)->getDefaultValue()));
    s.s.setTooltip(String(text) + " (double-click resets)");
    s.l.setText(text, dontSendNotification);
    s.l.setColour(Label::textColourId, kCreamDark);
    addAndMakeVisible(s.s); addAndMakeVisible(s.l);
    tabbed_.push_back({&s.s, tab}); tabbed_.push_back({&s.l, tab});
    s.a = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(proc_.apvts, id, s.s);
}
void CD57Editor::setChoice(const char* id, int idx)
{
    if (auto* p = proc_.apvts.getParameter(id)) { p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1((float)idx)); p->endChangeGesture(); }
}
void CD57Editor::showTab(int t)
{
    tab_ = t;
    for (int i = 0; i < 3; ++i) tabBtn_[i].setToggleState(i == t, dontSendNotification);
    for (auto& [c, i] : tabbed_) c->setVisible(i == t);
    repaint();
}

// static artwork, rendered once per size: tweed cabinet, chassis well, brushed chrome plate, strip background
void CD57Editor::renderBackground()
{
    const int W = getWidth(), H = getHeight();
    if (W <= 0 || H <= 0) return;
    background_ = Image(Image::RGB, W, H, true);
    Graphics g(background_);
    const float s = sc_;
    // tweed: diagonal twill in two thread colours plus a cross-hatch, then fibre noise and an amber lacquer vignette
    g.fillAll(kTweed);
    const float step = 4.0f * s;
    for (float i = (float)-H; i < (float)W; i += step) {
        g.setColour(kTweedDark.withAlpha(0.30f)); g.drawLine(i, 0.0f, i + (float)H, (float)H, 1.5f * s);
        g.setColour(kTweedLight.withAlpha(0.35f)); g.drawLine(i + step * 0.5f, 0.0f, i + step * 0.5f + (float)H, (float)H, 0.9f * s);
    }
    for (float i = 0; i < (float)(W + H); i += step * 3.0f) {
        g.setColour(kTweedDark.withAlpha(0.10f)); g.drawLine(i, 0.0f, i - (float)H, (float)H, 0.8f * s);
    }
    Random rnd(1957);
    for (int i = 0; i < W * H / 60; ++i) {
        g.setColour((rnd.nextBool() ? Colours::black : Colours::white).withAlpha(0.05f));
        g.fillRect(rnd.nextInt(W), rnd.nextInt(H), 1 + rnd.nextInt(3), 1);
    }
    g.setGradientFill(ColourGradient(Colours::transparentBlack, W * 0.5f, H * 0.3f, kLacquer.withAlpha(0.55f), 0.0f, 0.0f, true));
    g.fillAll();
    g.setGradientFill(ColourGradient(Colour(0x30ffcc66), 0, 0, Colours::transparentBlack, 0, (float)strip_.getY(), false));
    g.fillRect(0, 0, W, strip_.getY());

    // chassis well (the opening in the cabinet top) with an inner shadow
    const auto wf = well_.toFloat();
    g.setColour(Colours::black.withAlpha(0.35f)); g.fillRoundedRectangle(wf.expanded(3.0f * s).translated(0, 2.0f * s), 10.0f * s);
    g.setColour(kWell); g.fillRoundedRectangle(wf, 9.0f * s);
    g.setGradientFill(ColourGradient(Colours::black.withAlpha(0.6f), 0, wf.getY(), Colours::transparentBlack, 0, wf.getY() + 18.0f * s, false));
    g.fillRoundedRectangle(wf, 9.0f * s);

    // brushed chrome plate
    const auto pf = plate_.toFloat();
    g.setGradientFill(ColourGradient(kChrome, 0, pf.getY(), kChromeDark, 0, pf.getBottom(), false));
    g.fillRoundedRectangle(pf, 5.0f * s);
    Random br(7);
    for (int yy = plate_.getY(); yy < plate_.getBottom(); ++yy) {
        g.setColour((br.nextBool() ? Colours::white : Colours::black).withAlpha(0.025f + 0.03f * br.nextFloat()));
        g.drawHorizontalLine(yy, pf.getX() + 2.0f, pf.getRight() - 2.0f);
    }
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(0.35f), 0, pf.getY(), Colours::transparentWhite, 0, pf.getY() + pf.getHeight() * 0.35f, false));
    g.fillRoundedRectangle(pf, 5.0f * s);
    g.setColour(Colours::black.withAlpha(0.45f)); g.drawRoundedRectangle(pf, 5.0f * s, 1.2f * s);
    // plate screws
    for (float sx : {pf.getX() + 12.0f * s, pf.getRight() - 12.0f * s})
        for (float sy : {pf.getY() + 12.0f * s, pf.getBottom() - 12.0f * s}) {
            const auto c = Rectangle<float>(9.0f * s, 9.0f * s).withCentre({sx, sy});
            g.setGradientFill(ColourGradient(kChrome, c.getX(), c.getY(), kChromeDark, c.getRight(), c.getBottom(), false)); g.fillEllipse(c);
            g.setColour(kInk.withAlpha(0.6f)); g.drawLine(c.getX() + 1.5f * s, c.getCentreY(), c.getRight() - 1.5f * s, c.getCentreY(), 1.2f * s);
        }
    // engraved section rules
    auto rule = [&](float x) { g.setColour(kEngrave.withAlpha(0.55f)); g.drawVerticalLine(S(x), pf.getY() + 18 * s, pf.getBottom() - 18 * s);
                               g.setColour(Colours::white.withAlpha(0.5f)); g.drawVerticalLine(S(x) + 1, pf.getY() + 18 * s, pf.getBottom() - 18 * s); };
    rule(248.0f); rule(762.0f);

    // name plate on the tweed (plain type; no manufacturer marks)
    const auto np = R(400, 16, 300, 44).toFloat();
    g.setColour(Colours::black.withAlpha(0.3f)); g.fillRoundedRectangle(np.translated(0, 2.0f * s), 6.0f * s);
    g.setGradientFill(ColourGradient(kBrown.brighter(0.25f), 0, np.getY(), kBrown.darker(0.3f), 0, np.getBottom(), false));
    g.fillRoundedRectangle(np, 6.0f * s);
    g.setColour(kCreamDark); g.drawRoundedRectangle(np.reduced(3.0f * s), 4.0f * s, 1.0f * s);
    g.setColour(kCream); g.setFont(F(21.0f * s, true));
    g.drawText("'57 CUSTOM DELUXE", np.withTrimmedBottom(13.0f * s), Justification::centred);
    g.setFont(F(9.5f * s)); g.setColour(kCreamDark);
    g.drawText("CIRCUIT-LEVEL MODEL  -  DOCUMENTED APPROXIMATION", np.withTrimmedTop(26.0f * s).withTrimmedBottom(4.0f * s), Justification::centred);
    // hint on the tweed
    g.setColour(kInk.withAlpha(0.7f)); g.setFont(F(11.0f * s));
    g.drawText("drag a knob or hover for its value  -  double-click resets  -  click a jack to plug in  -  drag the corner to resize",
               R(28, 326, 1044, 18), Justification::centred);

    // lower strip
    const auto sf = strip_.toFloat();
    g.setGradientFill(ColourGradient(kStrip.brighter(0.08f), 0, sf.getY(), kStrip.darker(0.3f), 0, sf.getBottom(), false));
    g.fillRect(sf);
    g.setColour(Colours::black.withAlpha(0.6f)); g.fillRect(sf.withHeight(3.0f * s));
    g.setColour(kStripLine); g.drawHorizontalLine(strip_.getY() + S(3), 0.0f, (float)W);
    g.setColour(kStripLine.withAlpha(0.6f)); g.drawHorizontalLine(status_.getY() - S(4), sf.getX() + 20 * s, sf.getRight() - 20 * s);
}

void CD57Editor::paint(Graphics& g)
{
    if (background_.getWidth() != getWidth() || background_.getHeight() != getHeight()) renderBackground();
    g.drawImageAt(background_, 0, 0);
    const float s = sc_;

    // jewel pilot light: faceted red lens in a chrome bezel; brightness follows the modelled pilot lamp
    const float pil = jlimit(0.0f, 1.0f, proc_.diag.pilot.load());
    const auto jc = Point<float>((float)S(800), (float)S(176));
    const float jr = 19.0f * s;
    if (pil > 0.05f) {
        g.setGradientFill(ColourGradient(kJewel.withAlpha(0.45f * pil), jc.x, jc.y, Colours::transparentBlack, jc.x + jr * 2.6f, jc.y, true));
        g.fillEllipse(Rectangle<float>(jr * 5.2f, jr * 5.2f).withCentre(jc));
    }
    g.setGradientFill(ColourGradient(kChrome, jc.x - jr * 1.3f, jc.y - jr * 1.3f, kChromeDark, jc.x + jr * 1.3f, jc.y + jr * 1.3f, false));
    g.fillEllipse(Rectangle<float>(jr * 2.6f, jr * 2.6f).withCentre(jc));
    g.setColour(kInk.withAlpha(0.4f)); g.drawEllipse(Rectangle<float>(jr * 2.6f, jr * 2.6f).withCentre(jc), 1.0f);
    const Colour lens = Colour(0xff3a0806).interpolatedWith(kJewel, pil);
    g.setGradientFill(ColourGradient(lens.brighter(0.4f), jc.x - jr * 0.4f, jc.y - jr * 0.4f, lens.darker(0.6f), jc.x + jr, jc.y + jr, true));
    g.fillEllipse(Rectangle<float>(jr * 2.0f, jr * 2.0f).withCentre(jc));
    for (int i = 0; i < 8; ++i) {   // facets
        const float a = MathConstants<float>::twoPi * (float)i / 8.0f;
        g.setColour(Colours::black.withAlpha(0.18f));
        g.drawLine(jc.x + std::cos(a) * jr * 0.45f, jc.y + std::sin(a) * jr * 0.45f, jc.x + std::cos(a) * jr * 0.98f, jc.y + std::sin(a) * jr * 0.98f, 1.0f * s);
    }
    g.setColour(Colours::white.withAlpha(0.15f + 0.45f * pil)); g.fillEllipse(jc.x - jr * 0.55f, jc.y - jr * 0.65f, jr * 0.5f, jr * 0.35f);
    g.setFont(F(11.5f * s, true));
    g.setColour(Colours::white.withAlpha(0.5f)); g.drawText("PILOT", R(760, 212, 80, 16).translated(0, 1), Justification::centred);
    g.setColour(kEngrave); g.drawText("PILOT", R(760, 212, 80, 16), Justification::centred);

    // strip content painted directly
    if (tab_ == 2) {
        const auto& d = proc_.diag;
        const auto sv = content_.withWidth(S(430));
        g.setColour(kAmber); g.setFont(F(12.5f * s, true));
        g.drawText("LIVE CIRCUIT STATE", sv.getX(), sv.getY(), sv.getWidth(), S(16), Justification::left);
        g.setFont(Font(FontOptions(Font::getDefaultMonospacedFontName(), 12.5f * s, Font::plain)));
        g.setColour(kCream);
        int ty = sv.getY() + S(22);
        auto line = [&](const String& t) { g.drawText(t, sv.getX(), ty, sv.getWidth(), S(16), Justification::left); ty += S(17); };
        line(String::formatted("B+1 (reservoir) %7.1f V", d.b1.load()));
        line(String::formatted("B+2 (screens)   %7.1f V", d.b2.load()));
        line(String::formatted("B+3 (preamp)    %7.1f V", d.b3.load()));
        line(String::formatted("6V6 cathode     %7.2f V", d.cathode.load()));
        line(String::formatted("voice-coil rise %7.1f K", d.coilTemp.load()));
        line(String::formatted("solver %.2f it/step, %llu fallbacks", d.itPerStep.load(), (unsigned long long)d.fallbacks.load()));
        line(String::formatted("unrecovered %llu, guard resets %llu", (unsigned long long)d.failures.load(), (unsigned long long)d.panics.load()));
        line(String::formatted("internal %d Hz, latency %d smp", d.internalRateHz.load(), d.latency.load()));
        const float cpu = d.cpu.load() * 100.0f;
        g.setColour(cpu > 80 ? Colour(0xffff6a50) : kCream);
        line(String::formatted("CPU (this instance) %.0f %%", cpu));

        g.setColour(Colour(0xff0d1a10)); g.fillRoundedRectangle(scopeR_.toFloat(), 6.0f * s);
        g.setColour(Colour(0xff1d4a2a));
        for (int i = 1; i < 4; ++i) g.drawHorizontalLine(scopeR_.getY() + i * scopeR_.getHeight() / 4, (float)scopeR_.getX(), (float)scopeR_.getRight());
        Path pth; const int N = 1024; const int pos = proc_.scopePos.load();
        for (int i = 0; i < N; ++i) {
            const float v = jlimit(-1.0f, 1.0f, proc_.scope[(pos - N + i + CD57AudioProcessor::kScope * 2) % CD57AudioProcessor::kScope]);
            const float px = scopeR_.getX() + scopeR_.getWidth() * (float)i / (float)(N - 1), py = scopeR_.getCentreY() - v * scopeR_.getHeight() * 0.48f;
            if (i == 0) pth.startNewSubPath(px, py); else pth.lineTo(px, py);
        }
        g.setColour(Colour(0xff52ff7a)); g.strokePath(pth, PathStrokeType(1.3f * s));
        g.setColour(Colour(0xff7fd89a)); g.setFont(F(10.5f * s)); g.drawText("OUTPUT", scopeR_.getX() + S(6), scopeR_.getY() + S(3), S(80), S(12), Justification::left);
    } else if (tab_ == 1) {
        g.setColour(kCreamDark.withAlpha(0.8f)); g.setFont(F(11.0f * s));
        g.drawText("Nominal values; changes carry circuit state. Mic position/distance move only the provisional acoustic model (see Provenance).",
                   content_.getX(), content_.getBottom() - S(16), content_.getWidth(), S(14), Justification::left);
    }

    // status lines (always visible)
    const int externalChoice = (int)std::lround(proc_.apvts.getRawParameterValue(pid::externalLoad)->load());
    // The external selector takes precedence without altering the original five-choice load parameter.
    const int loadChoice = externalChoice > 0 ? 4 + externalChoice : (int)std::lround(proc_.apvts.getRawParameterValue(pid::load)->load());
    String loadGuidance;
    switch (loadChoice) {
        case 0: loadGuidance = "Stock wiring: internal 8 ohm Eminence; electrical/acoustic model remains ASSUMED."; break;
        case 1: loadGuidance = "NOT FENDER-RECOMMENDED: internal + 8 ohm extension. Experimental legacy mode only."; break;
        case 2: loadGuidance = "BENCH ONLY: 8 ohm resistive dummy load; internal speaker disconnected."; break;
        case 3: loadGuidance = "BENCH ONLY: 4 ohm resistive dummy load; internal speaker disconnected."; break;
        case 4: loadGuidance = "BENCH ONLY: 16 ohm resistive dummy load; internal speaker disconnected."; break;
        case 5: loadGuidance = "Fender-supported jack arrangement: single external 8 ohm; electrical load PROXY, no external acoustics."; break;
        case 6: loadGuidance = "Fender-supported jack arrangement: two external 16 ohm; electrical load PROXY, no external acoustics."; break;
        default: loadGuidance = "Unknown load configuration: provisional behavior."; break;
    }
    g.setFont(F(10.5f * s));
    const int lh = S(14);
    g.setColour(loadChoice == 1 ? Colour(0xffff7a5c) : kCreamDark);
    g.drawText(loadGuidance, status_.getX(), status_.getY(), status_.getWidth(), lh, Justification::left);
    g.setColour(kCreamDark);
    g.drawText(String(proc_.calibrationStatus()), status_.getX(), status_.getY() + lh, status_.getWidth(), lh, Justification::left);
    g.drawText(proc_.irStatus(), status_.getX(), status_.getY() + 2 * lh, status_.getWidth(), lh, Justification::left);
}

void CD57Editor::resized()
{
    sc_ = (float)getWidth() / (float)kBaseW;
    laf_.scale = sc_;
    well_ = R(28, 70, 1044, 250);
    plate_ = R(44, 86, 1012, 218);
    strip_ = {0, S(352), getWidth(), getHeight() - S(352)};
    content_ = R(24, 396, 1052, 210);
    status_ = R(24, 618, 1052, 3 * 14);
    scopeR_ = R(480, 396, 596, 210);

    // jacks: two columns (MIC left, INST right), rows 1 and 2.  jacks_[] order: INST 1, INST 2, MIC 1, MIC 2
    for (int i = 0; i < 4; ++i) jacks_[i].setBounds(R(i < 2 ? 160.0f : 72.0f, 104.0f + (float)(i % 2) * 96.0f, 78, 92));
    auto kb = [&](KnobBox& k, float cx) {
        k.s.setBounds(R(cx - 82, 96, 164, 186));
        k.l.setBounds(R(cx - 80, 280, 160, 18));
        k.l.setFont(F(13.0f * sc_, true));
    };
    kb(kInst_, 333); kb(kMic_, 505); kb(kTone_, 677);
    standbySw_.setBounds(R(856, 116, 80, 128));
    powerSw_.setBounds(R(952, 116, 80, 128));

    for (int i = 0; i < 3; ++i) tabBtn_[i].setBounds(R(24.0f + i * 170.0f, 362, 160, 26));
    irBtn_.changeWidthToFitText(); aboutBtn_.changeWidthToFitText();

    // amp setup: 4 columns x 5 rows
    const float cw = 1052.0f / 4.0f;
    auto cell = [&](int col, int row) { return Rectangle<float>(24.0f + col * cw, 396.0f + row * 42.0f, cw - 14.0f, 40.0f); };
    auto placeCombo = [&](ComboBoxRow& c, int col, int row) {
        const auto r = cell(col, row);
        c.l.setBounds(R(r.getX(), r.getY(), r.getWidth(), 15)); c.l.setFont(F(11.0f * sc_));
        c.c.setBounds(R(r.getX(), r.getY() + 15, r.getWidth(), 22));
    };
    placeCombo(cOut_, 0, 0); placeCombo(cLoad_, 1, 0); placeCombo(cExternalLoad_, 2, 0); placeCombo(cOS_, 3, 0);
    placeCombo(cMarket_, 0, 1); placeCombo(cPatch_, 1, 1); placeCombo(cInCh_, 2, 1); placeCombo(cSource_, 3, 1);
    placeCombo(cV1_, 0, 2); placeCombo(cV2_, 1, 2); placeCombo(cRect_, 2, 2); placeCombo(cTaper_, 3, 2);
    placeCombo(cPlug2_, 0, 3); placeCombo(cPlug2Src_, 1, 3);
    auto placeBtn = [&](Component& b, int col, int row, float w) { const auto r = cell(col, row); b.setBounds(R(r.getX(), r.getY() + 15, w, 22)); };
    placeBtn(otSatBtn_, 2, 3, cw - 14.0f); placeBtn(heatBtn_, 3, 3, cw - 14.0f);
    placeBtn(bypassBtn_, 0, 4, cw - 14.0f); placeBtn(irBtn_, 1, 4, 140.0f); placeBtn(aboutBtn_, 2, 4, 180.0f);

    // calibration / lab: 4 columns x 3 rows of sliders
    auto placeSlider = [&](SliderRow& sl, int col, int row) {
        const auto r = Rectangle<float>(24.0f + col * cw, 396.0f + row * 60.0f, cw - 18.0f, 52.0f);
        sl.l.setBounds(R(r.getX(), r.getY(), r.getWidth(), 16)); sl.l.setFont(F(11.0f * sc_));
        sl.s.setBounds(R(r.getX(), r.getY() + 18, r.getWidth(), 24));
        sl.s.setTextBoxStyle(Slider::TextBoxRight, false, S(72), S(20));
    };
    placeSlider(sTrim_, 0, 0); placeSlider(sOutLevel_, 1, 0); placeSlider(sInCal_, 2, 0); placeSlider(sLine_, 3, 0);
    placeSlider(sSrcR_, 0, 1); placeSlider(sMicPos_, 1, 1); placeSlider(sMicDist_, 2, 1);
    placeSlider(sVar_, 0, 2); placeSlider(sSeed_, 1, 2); placeSlider(sHum_, 2, 2); placeSlider(sNoise_, 3, 2);

    renderBackground();
}

void CD57Editor::timerCallback()
{
    // the jewel and the status lines change continuously; the service tab also shows live values and the scope
    repaint(tab_ == 2 ? getLocalBounds() : getLocalBounds().withTop(status_.getY() - S(2)));
    repaint(R(740, 130, 120, 100));
}

void CD57Editor::showProvenance()
{
    String msg;
    msg << "This plugin is a documented approximation of the modern '57 Custom Deluxe built from the public-source engineering dossier.\n"
        << "No physical amplifier, speaker, transformer or tube was measured. It is NOT a measured one-to-one model.\n\n"
        << "Evidence in the loaded manifest:\n" << String(proc_.manifestSummary()) << "\n\n";
    if (proc_.manifestError().size()) msg << "MANIFEST WARNING: " << String(proc_.manifestError()) << "\n\n";
    msg << "Circuit: historical 5E3-family netlist (data/netlist/cd57_nominal.net) solved as a full nonlinear nodal system:\n"
        << "jack network, interacting volumes/tone, shared-cathode 12AY7, 12AX7 gain stage + cathodyne, cathode-biased 6V6 pair with\n"
        << "screen/grid current, flux-state output transformer, explicit 5Y3 rectifier + 3-stage supply, electromechanical speaker load.\n"
        << "Acoustic stage: PROVISIONAL physics-based cone-velocity -> microphone operator (no measured cabinet data exists).\n"
        << "The Mic position / distance controls only move that provisional model (open-back dipole, piston directivity) and are ignored by the IR modes;\n"
        << "there are no calibrated microphone positions, so they must not be read as physically accurate positions (dossier Sec. 19).\n\n"
        << "STANDBY: separate automatable switch (PLAY = audible, STBY = silent after supply discharge), matching Fender's documented behavior.\n"
        << "  * This is an approximate HV supply interruption, NOT a verified electrical placement of the modern switch. POWER remains on,\n"
        << "    heaters remain warm in standby, and restoring PLAY resumes without a cold tube startup. Fender recommends standby for 60 seconds\n"
        << "    after turning POWER on.\n\n"
        << "SPEAKER LOAD: 'Stock' uses the unmeasured Eminence Special Design alnico proxy. The two supported *external* wiring\n"
        << "  arrangements use nominal equivalent resistive loads only, with NO captured external driver, cabinet or microphone response.\n"
        << "  * Fender does NOT recommend using the internal speaker together with the extension jack. That earlier choice is now marked\n"
        << "    Experimental but retains its original 5-choice parameter index and normalised host automation. Resistive dummy loads are bench modes.\n"
        << "  * Fender-supported external configurations live in a SEPARATE optional External Setup selector (default NONE), which overrides\n"
        << "    the original load only while selected. The new selector preserves older preset/DAW automation mappings.\n\n"
        << "CALIBRATION: " << String(proc_.calibrationStatus()) << "\n"
        << "  * Exact modern volume-pot tapers are not publicly available; Instrument and Mic have separate specimen-measured map paths.\n"
        << "  * The Eminence PN 7708599000 Re/Le/fs/Qms/Qes/Mms/Sd and cabinet response have not been physically measured here.\n\n"
        << "USER IR (measured): the file is the impulse response of 'voltage at the speaker terminals -> microphone signal'.\n"
        << "  * Gain is NOT normalised: samples are output-full-scale per terminal-full-scale (28.28 V peak = 0 dBFS). A single 1.0 sample = the\n"
        << "    'Speaker terminals' output exactly. Gains above 0 dBFS need a 32-bit float file. Polarity is preserved.\n"
        << "  * Arrival time: sample 0 = the instant the voltage reaches the terminals; microphone propagation delay stays in the file. Nothing is\n"
        << "    trimmed. Use a mono file (channel 1 is used), at most 2 s (longer files are cut with a 10 ms fade), any sample rate.\n"
        << "  * Sample-rate conversion: windowed-sinc, transfer function (gain, phase, delay) preserved. The raw file is never modified; its path and a hash are saved in the state.\n"
        << "  * 'User IR' convolves the terminal voltage with the IR. 'User IR on cone motion (hybrid)' applies the same IR through a regularised\n"
        << "    residual R = H G*/(|G|^2+lambda) to the model's cone velocity, so the model's own voice-coil heating and cone motion stay in the loop\n"
        << "    without counting the cone twice (dossier Sec. 18/27). It needs ~0.3 ms of arrival delay in the IR and is exact only inside the band\n"
        << "    shown in the status line. With a dummy load there is no cone motion, so the hybrid mode is silent.\n"
        << "  * The model's speaker parameters are ASSUMED (generic 12-inch driver); the residual absorbs any difference to the real driver.\n";
    AlertWindow::showMessageBoxAsync(MessageBoxIconType::InfoIcon, "Provenance", msg);
}
