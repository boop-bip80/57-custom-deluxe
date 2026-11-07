#include "PluginEditor.h"
#include "CD57Assets.h"

using namespace juce;
namespace pid = cd57ui::id;

namespace {
// palette: lacquered tweed, brown chassis, chrome plate, cream knobs (all colours chosen here; no external artwork)
const Colour kTweed(0xffcf9f52), kTweedLight(0xffe6c27a), kTweedDark(0xff8f6326), kLacquer(0xff6b3f12),
             kChrome(0xffe4e6e8), kChromeMid(0xffb9bcbf), kChromeDark(0xff7d8184), kInk(0xff2a1d14), kEngrave(0xff3b3027),
             kBrown(0xff4a2615), kWell(0xff24170e), kStrip(0xff1b1410), kStripLine(0xff5d4630), kCream(0xfff1e4c3),
             kCreamDark(0xffc9b68c), kAmber(0xffffc04d), kJewel(0xffff3324);
Font F(float h, bool bold = false) { return Font(FontOptions(h).withStyle(bold ? "Bold" : "Regular")); }
// small caps-style label font with a little tracking (screened/engraved legends)
Font FL(float h, bool bold = true) { return Font(FontOptions(h).withStyle(bold ? "Bold" : "Regular").withKerningFactor(0.09f)); }
#define CD57_IMG(n) ImageCache::getFromMemory(cd57assets::n, cd57assets::n##Size)
void drawImg(Graphics& g, const Image& im, Rectangle<float> r, float alpha = 1.0f)
{
    g.setOpacity(alpha);
    g.drawImage(im, r, RectanglePlacement::stretchToFit);
    g.setOpacity(1.0f);
}
void drawImgXf(Graphics& g, const Image& im, Rectangle<float> r, float angle, float alpha = 1.0f)
{
    const auto xf = AffineTransform::scale(r.getWidth() / (float)im.getWidth(), r.getHeight() / (float)im.getHeight())
                        .translated(-r.getWidth() * 0.5f, -r.getHeight() * 0.5f).rotated(angle).translated(r.getCentreX(), r.getCentreY());
    g.setOpacity(alpha);
    g.drawImageTransformed(im, xf, false);
    g.setOpacity(1.0f);
}
// engraved legend on metal: a light lower edge under a dark, slightly transparent fill (light from the top left)
void engrave(Graphics& g, const String& t, Rectangle<float> r, float h, Justification j = Justification::centred, Colour ink = Colour(0xff2f2620))
{
    g.setFont(FL(h));
    g.setColour(Colours::white.withAlpha(0.55f)); g.drawText(t, r.translated(0.0f, jmax(0.6f, h * 0.07f)), j);
    g.setColour(ink.withAlpha(0.82f)); g.drawText(t, r, j);
}
// screened legend on tweed/strip: plain ink, no relief
void screen(Graphics& g, const String& t, Rectangle<float> r, float h, Colour c, Justification j = Justification::centred, bool bold = false)
{
    g.setFont(FL(h, bold)); g.setColour(c); g.drawText(t, r, j);
}
// outline of the cream pointer, in units of the skirt radius (must match tools/ui/make_ui_textures.py)
Path pointerPath(float kr)
{
    const float nose = 1.22f, tail = 0.62f, half = 0.36f;
    Path p; const int n = 48;
    auto w = [&](float y) { return y < 0 ? half * std::pow(jlimit(0.0f, 1.0f, 1.0f + y / nose), 0.75f) : half * std::sqrt(jmax(0.0f, 1.0f - (y / tail) * (y / tail))); };
    for (int i = 0; i <= n; ++i) { const float y = -nose + (nose + tail) * (float)i / (float)n; const float x = w(y); if (i == 0) p.startNewSubPath(x * kr, y * kr); else p.lineTo(x * kr, y * kr); }
    for (int i = n; i >= 0; --i) { const float y = -nose + (nose + tail) * (float)i / (float)n; p.lineTo(-w(y) * kr, y * kr); }
    p.closeSubPath();
    return p;
}
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
    if (b.getProperties()["tab"]) {   // tabs: no box, an amber rule under the active page
        if (b.getToggleState()) { g.setColour(kAmber.withAlpha(0.9f)); g.fillRoundedRectangle(r.withTop(r.getBottom() - 2.0f * scale).reduced(r.getWidth() * 0.18f, 0), 1.0f); }
        else if (over) { g.setColour(kCreamDark.withAlpha(0.35f)); g.fillRect(r.withTop(r.getBottom() - 1.0f).reduced(r.getWidth() * 0.25f, 0)); }
        return;
    }
    g.setColour(Colours::white.withAlpha(down ? 0.10f : (over ? 0.06f : 0.02f)));
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    g.setColour((over ? kAmber : kCreamDark).withAlpha(over ? 0.8f : 0.45f));
    g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.0f);
}

void CD57LookAndFeel::drawRotarySlider(Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider& s)
{
    g.setImageResamplingQuality(Graphics::highResamplingQuality);
    const auto bounds = Rectangle<float>((float)x, (float)y, (float)w, (float)h);
    const float bubbleH = 20.0f * scale;
    const auto area = bounds.withTrimmedBottom(bubbleH + 2.0f * scale);
    const float R = jmin(area.getWidth(), area.getHeight()) * 0.5f;
    const auto ctr = area.getCentre();
    const float kr = R * 0.56f;                                          // skirt radius
    // engraved scale 1..12 and tick marks on the plate
    for (int i = 1; i <= 12; ++i) {
        const float a = a0 + (a1 - a0) * (float)(i - 1) / 11.0f;
        const float sn = std::sin(a), cs = -std::cos(a);
        const float r0 = kr + 5.0f * scale, r1 = kr + 9.5f * scale;
        g.setColour(Colours::white.withAlpha(0.5f));
        g.drawLine(ctr.x + sn * r0, ctr.y + cs * r0 + 0.8f * scale, ctr.x + sn * r1, ctr.y + cs * r1 + 0.8f * scale, 1.1f * scale);
        g.setColour(Colour(0xff2f2620).withAlpha(0.8f));
        g.drawLine(ctr.x + sn * r0, ctr.y + cs * r0, ctr.x + sn * r1, ctr.y + cs * r1, 1.1f * scale);
        const float rt = kr + 19.0f * scale;
        engrave(g, String(i), Rectangle<float>(20.0f * scale, 14.0f * scale).withCentre({ctr.x + sn * rt, ctr.y + cs * rt}), 11.0f * scale);
    }
    // sprites: the skirt is rotationally symmetric, so it and its shadow stay fixed (lighting stays top-left);
    // the pointer and its shadow rotate; a screen-fixed highlight is added over the pointer afterwards
    static const Image skirt = CD57_IMG(knob_skirt_png), skirtSh = CD57_IMG(knob_skirt_shadow_png),
                       ptr = CD57_IMG(knob_pointer_png), ptrSh = CD57_IMG(knob_pointer_shadow_png);
    const float side = kr / 0.40f;                                       // sprite side: skirt radius = 0.40 x side
    const auto spr = Rectangle<float>(side, side).withCentre(ctr);
    const float ang = a0 + pos * (a1 - a0);
    drawImg(g, skirtSh, spr.translated(3.0f * scale, 5.0f * scale).expanded(2.0f * scale));
    drawImg(g, skirt, spr);
    drawImgXf(g, ptrSh, spr.translated(2.5f * scale, 4.0f * scale), ang, 0.9f);
    drawImgXf(g, ptr, spr, ang);
    {
        Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(pointerPath(kr), AffineTransform::rotation(ang).translated(ctr.x, ctr.y));
        g.setGradientFill(ColourGradient(Colours::white.withAlpha(0.42f), ctr.x - kr * 0.9f, ctr.y - kr * 1.0f, Colours::black.withAlpha(0.18f), ctr.x + kr * 0.8f, ctr.y + kr * 0.9f, false));
        g.fillRect(spr);
    }
    // value readout while hovering or dragging
    if (s.isMouseOverOrDragging()) {
        const String txt = s.getTextFromValue(s.getValue());
        g.setFont(F(12.0f * scale, true));
        const float tw = jmax(48.0f * scale, GlyphArrangement::getStringWidth(g.getCurrentFont(), txt) + 16.0f * scale);
        const auto pill = Rectangle<float>(tw, bubbleH).withCentre({ctr.x, bounds.getBottom() - bubbleH * 0.5f - 1.0f});
        g.setColour(Colour(0xe6120d0a)); g.fillRoundedRectangle(pill, bubbleH * 0.5f);
        g.setColour(kAmber.withAlpha(0.7f)); g.drawRoundedRectangle(pill, bubbleH * 0.5f, 1.0f);
        g.setColour(kCream); g.drawText(txt, pill, Justification::centred);
    }
}

void CD57LookAndFeel::drawLinearSlider(Graphics& g, int x, int y, int w, int h, float pos, float, float, Slider::SliderStyle, Slider& s)
{
    const float cy = (float)y + h * 0.5f, th = 2.0f * scale, kr = 6.0f * scale;
    g.setColour(Colours::black.withAlpha(0.55f));
    g.fillRoundedRectangle((float)x, cy - th * 0.5f, (float)w, th, th * 0.5f);
    g.setColour(Colours::white.withAlpha(0.06f)); g.drawHorizontalLine((int)(cy + th), (float)x, (float)(x + w));
    g.setColour(kAmber.withAlpha(0.55f));
    g.fillRoundedRectangle((float)x, cy - th * 0.5f, jmax(0.0f, pos - (float)x), th, th * 0.5f);
    g.setColour(Colours::black.withAlpha(0.4f)); g.fillEllipse(pos - kr + 1.0f, cy - kr + 2.0f, kr * 2, kr * 2);
    g.setGradientFill(ColourGradient(kCream.brighter(0.2f), pos - kr * 0.5f, cy - kr, kCreamDark.darker(0.25f), pos + kr * 0.5f, cy + kr, false));
    g.fillEllipse(pos - kr, cy - kr, kr * 2, kr * 2);
    if (s.isMouseOverOrDragging()) { g.setColour(kAmber.withAlpha(0.8f)); g.drawEllipse(pos - kr, cy - kr, kr * 2, kr * 2, 1.0f); }
}

void CD57LookAndFeel::drawComboBox(Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& b)
{
    const auto r = Rectangle<float>(0, 0, (float)w, (float)h);
    if (b.isMouseOver()) { g.setColour(Colours::white.withAlpha(0.04f)); g.fillRoundedRectangle(r, 3.0f * scale); }
    g.setColour((b.isMouseOver() ? kAmber : kCreamDark).withAlpha(b.isMouseOver() ? 0.8f : 0.28f));
    g.fillRect(r.withTop(r.getBottom() - 1.0f));
    const float ax = (float)w - 10.0f * scale, ay = (float)h * 0.5f;
    Path a; a.startNewSubPath(ax - 3.5f * scale, ay - 1.5f * scale); a.lineTo(ax, ay + 2.0f * scale); a.lineTo(ax + 3.5f * scale, ay - 1.5f * scale);
    g.setColour(kCreamDark); g.strokePath(a, PathStrokeType(1.2f * scale, PathStrokeType::curved, PathStrokeType::rounded));
}

void CD57LookAndFeel::drawToggleButton(Graphics& g, ToggleButton& b, bool over, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    const float hh = 12.0f * scale, ww = 22.0f * scale;
    const auto tr = Rectangle<float>(2.0f, (r.getHeight() - hh) * 0.5f, ww, hh);
    const bool on = b.getToggleState();
    g.setColour(on ? kAmber.withAlpha(0.75f) : Colours::black.withAlpha(0.55f)); g.fillRoundedRectangle(tr, hh * 0.5f);
    g.setColour((over ? kAmber : kCreamDark).withAlpha(0.45f)); g.drawRoundedRectangle(tr, hh * 0.5f, 1.0f);
    const float d = hh - 3.0f * scale;
    g.setColour(kCream); g.fillEllipse(on ? tr.getRight() - d - 1.5f * scale : tr.getX() + 1.5f * scale, tr.getY() + 1.5f * scale, d, d);
    g.setColour(on ? kCream : kCreamDark); g.setFont(F(12.0f * scale));
    g.drawText(b.getButtonText(), (int)(tr.getRight() + 8 * scale), 0, b.getWidth() - (int)tr.getRight() - 8, b.getHeight(), Justification::centredLeft);
}

// ---------------------------------------------------------------------------------------------------- small components
void JackComponent::paint(Graphics& g)
{
    g.setImageResamplingQuality(Graphics::highResamplingQuality);
    static const Image nut = CD57_IMG(jack_nut_png), plug = CD57_IMG(jack_plug_png), sh = CD57_IMG(nut_shadow_png);
    const auto r = getLocalBounds().toFloat();
    const float lab = r.getHeight() * 0.22f;
    const float d = jmin(r.getWidth(), r.getHeight() - lab) * 0.86f;
    const auto c = Rectangle<float>(d, d).withCentre({r.getCentreX(), r.getY() + (r.getHeight() - lab) * 0.5f});
    drawImg(g, sh, c.translated(d * 0.03f, d * 0.05f).expanded(d * 0.04f));
    drawImg(g, nut, c);
    if (selected) drawImg(g, plug, c);
    engrave(g, label, r.withTop(r.getBottom() - lab), lab * 0.62f);
}

void ToggleSwitch::paint(Graphics& g)
{
    g.setImageResamplingQuality(Graphics::highResamplingQuality);
    static const Image up = CD57_IMG(toggle_up_png), down = CD57_IMG(toggle_down_png);
    const auto r = getLocalBounds().toFloat();
    const float u = r.getWidth() / 80.0f, txtH = 14.0f * u;
    engrave(g, onText, r.withHeight(txtH), 10.5f * u, Justification::centred, on ? Colour(0xff2f2620) : Colour(0x802f2620));
    engrave(g, offText, r.withTrimmedBottom(txtH).removeFromBottom(txtH), 10.5f * u, Justification::centred, !on ? Colour(0xff2f2620) : Colour(0x802f2620));
    const float side = jmin(r.getWidth() * 1.05f, r.getHeight() - 2.0f * txtH - 4.0f * u);
    drawImg(g, on ? up : down, Rectangle<float>(side, side).withCentre({r.getCentreX(), r.getCentreY() - 3.0f * u}));
    engrave(g, label, r.withTop(r.getBottom() - txtH), 11.0f * u);
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
        tabBtn_[i].getProperties().set("tab", true);
        tabBtn_[i].setColour(TextButton::textColourOnId, kCream);
        tabBtn_[i].setColour(TextButton::textColourOffId, kCreamDark.withAlpha(0.6f));
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
    addAndMakeVisible(k.s); addChildComponent(k.l);   // the legend is engraved on the plate in paint(); the label keeps it accessible
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
// Artwork is drawn directly from the high-resolution textures on every paint (no logical-size cache), so it stays
// sharp on HiDPI displays.  Light comes from the top left everywhere.
void CD57Editor::renderBackground() {}

void CD57Editor::paintCabinet(Graphics& g)
{
    g.setImageResamplingQuality(Graphics::highResamplingQuality);
    static const Image tweed = CD57_IMG(tweed_jpg), plate = CD57_IMG(plate_png), screw = CD57_IMG(screw_png);
    const float s = sc_;
    const auto top = getLocalBounds().withBottom(strip_.getY()).toFloat();
    drawImg(g, tweed, top.withHeight(top.getWidth() * (float)tweed.getHeight() / (float)tweed.getWidth()));

    // chassis well: dark opening with a soft inner shadow from the top-left light
    const auto wf = well_.toFloat();
    g.setColour(Colours::white.withAlpha(0.10f)); g.drawRoundedRectangle(wf.expanded(1.5f * s).translated(0, 1.0f * s), 10.0f * s, 1.2f * s);
    g.setColour(kWell); g.fillRoundedRectangle(wf, 9.0f * s);
    {
        Graphics::ScopedSaveState ss(g);
        Path wp; wp.addRoundedRectangle(wf, 9.0f * s); g.reduceClipRegion(wp);
        DropShadow(Colours::black.withAlpha(0.85f), roundToInt(16 * s), {roundToInt(4 * s), roundToInt(6 * s)}).drawForRectangle(g, plate_);
        g.setGradientFill(ColourGradient(Colours::black.withAlpha(0.65f), 0, wf.getY(), Colours::transparentBlack, 0, wf.getY() + 22.0f * s, false));
        g.fillRect(wf);
    }
    // brushed plate and its screws
    drawImg(g, plate, plate_.toFloat());
    const auto pf = plate_.toFloat();
    for (float sx : {pf.getX() + 12.0f * s, pf.getRight() - 12.0f * s})
        for (float sy : {pf.getY() + 12.0f * s, pf.getBottom() - 12.0f * s}) {
            g.setColour(Colours::black.withAlpha(0.25f)); g.fillEllipse(Rectangle<float>(10.0f * s, 10.0f * s).withCentre({sx + 0.8f * s, sy + 1.4f * s}));
            drawImg(g, screw, Rectangle<float>(9.5f * s, 9.5f * s).withCentre({sx, sy}));
        }
    // engraved section rules (groove: dark line with a light lower/right edge)
    auto rule = [&](float x) { g.setColour(Colour(0xff2f2620).withAlpha(0.45f)); g.fillRect(Rectangle<float>((float)S(x), pf.getY() + 20 * s, 1.0f * s, pf.getHeight() - 40 * s));
                               g.setColour(Colours::white.withAlpha(0.55f)); g.fillRect(Rectangle<float>((float)S(x) + 1.0f * s, pf.getY() + 20 * s, 1.0f * s, pf.getHeight() - 40 * s)); };
    rule(248.0f); rule(762.0f);

    // name plate on the tweed: dark lacquered brown with a screened cream legend (plain type; no manufacturer marks)
    const auto np = R(410, 18, 280, 40).toFloat();
    DropShadow(Colours::black.withAlpha(0.5f), roundToInt(6 * s), {roundToInt(2 * s), roundToInt(3 * s)}).drawForRectangle(g, np.toNearestInt());
    g.setGradientFill(ColourGradient(Colour(0xff3f2215), np.getX(), np.getY(), Colour(0xff1d0f08), np.getRight(), np.getBottom(), false));
    g.fillRoundedRectangle(np, 4.0f * s);
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(0.14f), 0, np.getY(), Colours::transparentWhite, 0, np.getCentreY(), false));
    g.fillRoundedRectangle(np.reduced(1.0f * s), 3.5f * s);
    g.setColour(Colour(0xffb59a6a).withAlpha(0.55f)); g.drawRoundedRectangle(np.reduced(3.0f * s), 2.5f * s, 0.8f * s);
    screen(g, "'57 CUSTOM DELUXE", np.withTrimmedBottom(13.0f * s).translated(0, 1.0f * s), 18.0f * s, kCream.withAlpha(0.92f), Justification::centred, true);
    screen(g, "CIRCUIT-LEVEL MODEL  -  DOCUMENTED APPROXIMATION", np.withTrimmedTop(25.0f * s).withTrimmedBottom(4.0f * s), 8.0f * s, Colour(0xffcdb488));

    // lower strip: deep brown-black, soft top edge, no boxes
    const auto sf = strip_.toFloat();
    g.setGradientFill(ColourGradient(Colour(0xff17110d), 0, sf.getY(), Colour(0xff0b0806), 0, sf.getBottom(), false));
    g.fillRect(sf);
    g.setGradientFill(ColourGradient(Colours::black.withAlpha(0.7f), 0, sf.getY(), Colours::transparentBlack, 0, sf.getY() + 10.0f * s, false));
    g.fillRect(sf.withHeight(10.0f * s));
    g.setColour(Colour(0xff6a5236).withAlpha(0.5f)); g.drawHorizontalLine(strip_.getY(), 0.0f, (float)getWidth());
    g.setColour(Colours::white.withAlpha(0.05f)); g.drawHorizontalLine(S(392), sf.getX() + 24 * s, sf.getRight() - 24 * s);
    screen(g, "hover a knob for its value  -  double-click resets  -  click a jack to plug in  -  drag the corner to resize",
           R(560, 364, 516, 22).toFloat(), 10.0f * s, kCreamDark.withAlpha(0.45f), Justification::centredRight, false);
    g.setColour(Colours::white.withAlpha(0.05f)); g.drawHorizontalLine(status_.getY() - S(6), sf.getX() + 24 * s, sf.getRight() - 24 * s);
}

void CD57Editor::paint(Graphics& g)
{
    paintCabinet(g);
    for (auto* k : {&kInst_, &kMic_, &kTone_}) engrave(g, k->l.getText(), k->l.getBounds().toFloat(), 12.0f * sc_);
    const float s = sc_;

    // jewel pilot light: glass lens in a knurled chrome bezel; the lit lens and its glow follow the modelled lamp
    {
        static const Image jOff = CD57_IMG(jewel_off_png), jOn = CD57_IMG(jewel_on_png), glow = CD57_IMG(jewel_glow_png);
        const float pil = jlimit(0.0f, 1.0f, proc_.diag.pilot.load());
        const auto jc = Point<float>((float)S(800), (float)S(176));
        const float js = 50.0f * s;
        g.setColour(Colours::black.withAlpha(0.35f)); g.fillEllipse(Rectangle<float>(js * 0.92f, js * 0.92f).withCentre(jc.translated(2.0f * s, 3.5f * s)));
        drawImg(g, jOff, Rectangle<float>(js, js).withCentre(jc));
        if (pil > 0.01f) {
            drawImg(g, jOn, Rectangle<float>(js, js).withCentre(jc), pil);
            drawImg(g, glow, Rectangle<float>(js * 3.0f, js * 3.0f).withCentre(jc), 0.75f * pil);
        }
        engrave(g, "PILOT", R(760, 210, 80, 16).toFloat(), 10.5f * s);
    }

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
