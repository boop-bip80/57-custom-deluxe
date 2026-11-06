#include "PluginEditor.h"

using namespace juce;
namespace pid = cd57ui::id;

namespace {
const Colour kTweed(0xffd2ac55), kTweedDark(0xffa8802f), kChrome(0xffd9dadb), kChromeDark(0xff8e9091), kInk(0xff241a12),
             kBrown(0xff5a2a1c), kCream(0xfff3e6c4), kAmber(0xffffc04d);
Font F(float h, bool bold = false) { return Font(FontOptions(h).withStyle(bold ? "Bold" : "Regular")); }
} // namespace

// ---------------------------------------------------------------------------------------------------- look & feel
CD57LookAndFeel::CD57LookAndFeel()
{
    setColour(Label::textColourId, kInk);
    setColour(ComboBox::backgroundColourId, Colour(0xff2b2119));
    setColour(ComboBox::textColourId, kCream);
    setColour(ComboBox::outlineColourId, Colour(0xff6b5a3a));
    setColour(ComboBox::arrowColourId, kCream);
    setColour(PopupMenu::backgroundColourId, Colour(0xff2b2119));
    setColour(PopupMenu::textColourId, kCream);
    setColour(PopupMenu::highlightedBackgroundColourId, kBrown);
    setColour(TextButton::buttonColourId, Colour(0xff4a3828));
    setColour(TextButton::textColourOffId, kCream);
    setColour(ToggleButton::textColourId, kInk);
    setColour(ToggleButton::tickColourId, kBrown);
    setColour(Slider::textBoxTextColourId, kInk);
    setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
}

Font CD57LookAndFeel::getComboBoxFont(ComboBox&) { return F(12.5f); }
Font CD57LookAndFeel::getLabelFont(Label& l) { return l.getFont(); }

void CD57LookAndFeel::drawRotarySlider(Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider&)
{
    const float d = (float)jmin(w, h) - 8.0f;
    const auto c = Rectangle<float>((float)x + (w - d) * 0.5f, (float)y + (h - d) * 0.5f, d, d);
    const auto ctr = c.getCentre();
    // printed scale 0..12
    g.setColour(kInk);
    for (int i = 0; i <= 12; ++i) {
        const float a = a0 + (a1 - a0) * (float)i / 12.0f;
        const float r0 = d * 0.5f + 3.0f, r1 = d * 0.5f + (i % 3 == 0 ? 9.0f : 6.0f);
        g.drawLine(ctr.x + std::sin(a) * r0, ctr.y - std::cos(a) * r0, ctr.x + std::sin(a) * r1, ctr.y - std::cos(a) * r1, i % 3 == 0 ? 1.6f : 1.0f);
        if (i % 3 == 0) {
            g.setFont(F(9.5f, true));
            const float rt = d * 0.5f + 16.0f;
            g.drawText(String(i), Rectangle<float>(ctr.x + std::sin(a) * rt - 8, ctr.y - std::cos(a) * rt - 6, 16, 12), Justification::centred);
        }
    }
    // chrome skirt + bakelite body
    g.setGradientFill(ColourGradient(kChrome, c.getX(), c.getY(), kChromeDark, c.getRight(), c.getBottom(), false));
    g.fillEllipse(c);
    const auto body = c.reduced(d * 0.1f);
    g.setGradientFill(ColourGradient(Colour(0xff3a342f), body.getX(), body.getY(), Colour(0xff0c0a09), body.getRight(), body.getBottom(), false));
    g.fillEllipse(body);
    g.setColour(Colour(0x33ffffff));
    g.drawEllipse(body.reduced(1.0f), 1.0f);
    // pointer
    const float ang = a0 + pos * (a1 - a0);
    Path p;
    p.addRoundedRectangle(-2.2f, -body.getHeight() * 0.5f + 3.0f, 4.4f, body.getHeight() * 0.42f, 1.8f);
    g.setColour(Colour(0xfff5f0e6));
    g.fillPath(p, AffineTransform::rotation(ang).translated(ctr.x, ctr.y));
}

void CD57LookAndFeel::drawLinearSlider(Graphics& g, int x, int y, int w, int h, float pos, float, float, Slider::SliderStyle, Slider&)
{
    const float cy = (float)y + h * 0.5f;
    g.setColour(Colour(0xff2b2119));
    g.fillRoundedRectangle((float)x, cy - 2.5f, (float)w, 5.0f, 2.5f);
    g.setColour(kBrown);
    g.fillRoundedRectangle((float)x, cy - 2.5f, pos - (float)x, 5.0f, 2.5f);
    g.setGradientFill(ColourGradient(kChrome, pos, cy - 8, kChromeDark, pos, cy + 8, false));
    g.fillEllipse(pos - 7.0f, cy - 7.0f, 14.0f, 14.0f);
    g.setColour(kInk.withAlpha(0.6f));
    g.drawEllipse(pos - 7.0f, cy - 7.0f, 14.0f, 14.0f, 1.0f);
}

void CD57LookAndFeel::drawComboBox(Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& b)
{
    const auto r = Rectangle<float>(0, 0, (float)w, (float)h);
    g.setColour(b.findColour(ComboBox::backgroundColourId));
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(b.findColour(ComboBox::outlineColourId));
    g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
    Path a; a.addTriangle((float)w - 16, (float)h * 0.40f, (float)w - 8, (float)h * 0.40f, (float)w - 12, (float)h * 0.62f);
    g.setColour(kCream); g.fillPath(a);
}

void CD57LookAndFeel::drawToggleButton(Graphics& g, ToggleButton& b, bool, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    const float s = 16.0f;
    const auto box = Rectangle<float>(2.0f, (r.getHeight() - s) * 0.5f, s, s);
    g.setColour(Colour(0xff2b2119)); g.fillRoundedRectangle(box, 3.0f);
    if (b.getToggleState()) { g.setColour(kAmber); g.fillRoundedRectangle(box.reduced(4.0f), 2.0f); }
    g.setColour(kInk); g.setFont(F(12.5f));
    g.drawText(b.getButtonText(), (int)(box.getRight() + 6), 0, b.getWidth() - (int)box.getRight() - 6, b.getHeight(), Justification::centredLeft);
}

// ---------------------------------------------------------------------------------------------------- small components
void JackComponent::paint(Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const float d = 38.0f;
    const auto c = Rectangle<float>((r.getWidth() - d) * 0.5f, 6.0f, d, d);
    g.setGradientFill(ColourGradient(kChrome, c.getX(), c.getY(), kChromeDark, c.getRight(), c.getBottom(), false));
    g.fillEllipse(c);
    g.setColour(Colour(0xff0b0907)); g.fillEllipse(c.reduced(8.0f));
    g.setColour(Colour(0xff3d352b)); g.drawEllipse(c.reduced(8.0f), 1.5f);
    if (selected) {   // plug + cable
        g.setGradientFill(ColourGradient(Colour(0xfff1d98a), c.getX(), c.getY(), Colour(0xff9c7a21), c.getRight(), c.getBottom(), false));
        g.fillEllipse(c.reduced(11.5f));
        Path cab; cab.startNewSubPath(c.getCentreX(), c.getCentreY());
        cab.cubicTo(c.getCentreX() - 14, c.getBottom() + 20, c.getCentreX() + 30, c.getBottom() + 26, c.getCentreX() + 8, r.getBottom());
        g.setColour(kInk); g.strokePath(cab, PathStrokeType(3.0f));
    }
    g.setColour(kInk); g.setFont(F(11.0f, true));
    g.drawText(label, getLocalBounds().removeFromBottom(14), Justification::centred);
}

void PowerSwitch::paint(Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setGradientFill(ColourGradient(kChrome, 0, 0, kChromeDark, r.getWidth(), r.getHeight(), false));
    g.fillRoundedRectangle(r.reduced(6.0f, 4.0f).withTrimmedBottom(14), 6.0f);
    const float cx = r.getCentreX(), cy = r.getCentreY() - 7.0f;
    g.setColour(Colour(0xff111111)); g.fillEllipse(cx - 11, cy - 11, 22, 22);
    Path lever; lever.addRoundedRectangle(-4.0f, on ? -22.0f : 0.0f, 8.0f, 22.0f, 3.0f);
    g.setColour(Colour(0xff1e1b18)); g.fillPath(lever, AffineTransform::translation(cx, cy));
    g.setColour(Colour(0x55ffffff)); g.drawRoundedRectangle(cx - 3.0f, cy + (on ? -21.0f : 1.0f), 6.0f, 20.0f, 2.5f, 1.0f);
    g.setColour(kInk); g.setFont(F(11.0f, true));
    g.drawText("POWER", getLocalBounds().removeFromBottom(14), Justification::centred);
}

// ---------------------------------------------------------------------------------------------------- editor
CD57Editor::CD57Editor(CD57AudioProcessor& p) : AudioProcessorEditor(&p), proc_(p)
{
    setLookAndFeel(&laf_);
    setSize(980, 826);
    paintTweed();

    knob(kInst_, pid::instVol, "INSTRUMENT VOL");
    knob(kMic_, pid::micVol, "MIC VOL");
    knob(kTone_, pid::tone, "TONE");

    static const char* jn[4] = {"INST 1", "INST 2", "MIC 1", "MIC 2"};
    for (int i = 0; i < 4; ++i) {
        jacks_[i].label = jn[i];
        jacks_[i].onClick = [this, i] { setChoice(pid::jack, i); };
        addAndMakeVisible(jacks_[i]);
    }
    jackAtt_ = std::make_unique<ParameterAttachment>(*proc_.apvts.getParameter(pid::jack), [this](float v) {
        const int sel = (int)std::lround(v);
        for (int i = 0; i < 4; ++i) { jacks_[i].selected = (i == sel); jacks_[i].repaint(); }
    });
    jackAtt_->sendInitialUpdate();

    addAndMakeVisible(powerSw_);
    powerSw_.onChange = [this](bool on) { powerAtt_->setValueAsCompleteGesture(on ? 1.0f : 0.0f); };
    powerAtt_ = std::make_unique<ParameterAttachment>(*proc_.apvts.getParameter(pid::power), [this](float v) { powerSw_.on = v > 0.5f; powerSw_.repaint(); });
    powerAtt_->sendInitialUpdate();

    combo(cPatch_, pid::patch, "Jumper cable");
    combo(cLoad_, pid::load, "Speaker load");
    combo(cOut_, pid::outMode, "Output");
    combo(cMarket_, pid::market, "Market variant");
    combo(cV1_, pid::v1, "V1 tube");
    combo(cV2_, pid::v2, "V2 tube");
    combo(cRect_, pid::rect, "Rectifier");
    combo(cOS_, pid::oversample, "Oversampling");
    combo(cSource_, pid::source, "Source");
    combo(cTaper_, pid::taper, "Volume taper (assumed)");
    combo(cInCh_, pid::inputCh, "Input channel");
    slider(sMicPos_, pid::micPos, "Mic position (provisional model)");
    slider(sMicDist_, pid::micDist, "Mic distance (provisional model)");
    slider(sTrim_, pid::inTrim, "Input trim");
    slider(sOutLevel_, pid::outLevel, "Output level");
    slider(sInCal_, pid::inCal, "Input cal (V pk @ FS)");
    slider(sLine_, pid::linePct, "Line voltage");
    slider(sSrcR_, pid::srcR, "Source R");
    slider(sVar_, pid::variation, "Specimen variation");
    slider(sSeed_, pid::seed, "Variation seed");
    slider(sHum_, pid::hum, "Hum");
    slider(sNoise_, pid::noise, "Noise");
    for (auto* b : {&heatBtn_, &bypassBtn_, &otSatBtn_}) addAndMakeVisible(*b);
    otSatAtt_ = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(proc_.apvts, pid::otSat, otSatBtn_);
    heatAtt_ = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(proc_.apvts, pid::heating, heatBtn_);
    bypassAtt_ = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(proc_.apvts, pid::bypass, bypassBtn_);
    addAndMakeVisible(irBtn_); addAndMakeVisible(aboutBtn_);
    irBtn_.onClick = [this] {
        chooser_ = std::make_unique<FileChooser>("Select a speaker-terminal-voltage to microphone IR (untrimmed, not normalised; see Provenance)", File(), "*.wav;*.aif;*.aiff;*.flac");
        chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this](const FileChooser& fc) {
            const auto f = fc.getResult();
            if (f.existsAsFile()) { proc_.loadImpulseResponse(f); setChoice(pid::outMode, cd57ui::outmode::userIr); }
        });
    };
    aboutBtn_.onClick = [this] { showProvenance(); };
    startTimerHz(20);
}

CD57Editor::~CD57Editor() { stopTimer(); setLookAndFeel(nullptr); }

void CD57Editor::knob(KnobBox& k, const char* id, const char* text)
{
    k.s.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
    k.s.setTextBoxStyle(Slider::TextBoxBelow, false, 56, 16);
    k.s.setRotaryParameters(degreesToRadians(-135.0f), degreesToRadians(135.0f), true);
    k.s.setNumDecimalPlacesToDisplay(1);
    k.s.setDoubleClickReturnValue(true, proc_.apvts.getParameter(id)->convertFrom0to1(proc_.apvts.getParameter(id)->getDefaultValue()));
    k.l.setText(text, dontSendNotification); k.l.setJustificationType(Justification::centred); k.l.setFont(F(11.5f, true));
    addAndMakeVisible(k.s); addAndMakeVisible(k.l);
    k.a = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(proc_.apvts, id, k.s);
}
void CD57Editor::combo(ComboBoxRow& c, const char* id, const char* text)
{
    c.l.setText(text, dontSendNotification); c.l.setFont(F(11.5f));
    if (auto* ch = dynamic_cast<AudioParameterChoice*>(proc_.apvts.getParameter(id))) c.c.addItemList(ch->choices, 1);
    addAndMakeVisible(c.c); addAndMakeVisible(c.l);
    c.a = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment>(proc_.apvts, id, c.c);
}
void CD57Editor::slider(SliderRow& s, const char* id, const char* text)
{
    s.s.setSliderStyle(Slider::LinearHorizontal);
    s.s.setTextBoxStyle(Slider::TextBoxRight, false, 62, 16);
    s.l.setText(text, dontSendNotification); s.l.setFont(F(11.5f));
    addAndMakeVisible(s.s); addAndMakeVisible(s.l);
    s.a = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(proc_.apvts, id, s.s);
}
void CD57Editor::setChoice(const char* id, int idx)
{
    if (auto* p = proc_.apvts.getParameter(id)) { p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1((float)idx)); p->endChangeGesture(); }
}

void CD57Editor::paintTweed()
{
    // sized from the editor itself so the texture always covers the window (the window grew when lab controls were added)
    const int W = getWidth(), H = getHeight();
    tweed_ = Image(Image::RGB, W, H, false);
    Graphics g(tweed_);
    g.fillAll(kTweed);
    Random rnd(57);
    for (int i = -H; i < W; i += 5) {                           // twill: diagonal bands
        g.setColour(kTweedDark.withAlpha(0.35f));
        g.drawLine((float)i, 0.0f, (float)(i + H), (float)H, 1.6f);
        g.setColour(Colour(0xffe8c978).withAlpha(0.35f));
        g.drawLine((float)i + 2.5f, 0.0f, (float)(i + H) + 2.5f, (float)H, 1.0f);
    }
    for (int i = 0; i < W * H / 85; ++i) {                     // fibre noise
        g.setColour((rnd.nextBool() ? Colours::black : Colours::white).withAlpha(0.04f));
        g.fillRect(rnd.nextInt(W), rnd.nextInt(H), 1 + rnd.nextInt(2), 1);
    }
}

void CD57Editor::paint(Graphics& g)
{
    g.drawImageAt(tweed_, 0, 0);
    // title badge
    g.setColour(kBrown); g.fillRoundedRectangle(300.0f, 14.0f, 380.0f, 46.0f, 8.0f);
    g.setColour(Colour(0xffd9b565)); g.drawRoundedRectangle(304.0f, 18.0f, 372.0f, 38.0f, 6.0f, 1.2f);
    g.setColour(kCream); g.setFont(F(24.0f, true)); g.drawText("'57 CUSTOM DELUXE", 300, 14, 380, 30, Justification::centred);
    g.setFont(F(10.5f)); g.drawText("CIRCUIT-LEVEL MODEL  -  DOCUMENTED APPROXIMATION", 300, 41, 380, 14, Justification::centred);

    // control plate
    g.setGradientFill(ColourGradient(kChrome, 0, (float)plate_.getY(), kChromeDark, 0, (float)plate_.getBottom(), false));
    g.fillRoundedRectangle(plate_.toFloat(), 14.0f);
    g.setColour(Colour(0x66000000)); g.drawRoundedRectangle(plate_.toFloat().reduced(1.0f), 14.0f, 1.5f);
    // pilot jewel
    const float pil = proc_.diag.pilot.load();
    const auto jc = Point<float>((float)plate_.getX() + 168.0f, (float)plate_.getY() + 64.0f);
    g.setColour(Colour(0xff3a0a0a).interpolatedWith(Colour(0xffff3a2a), pil)); g.fillEllipse(jc.x - 13, jc.y - 13, 26, 26);
    g.setColour(Colours::white.withAlpha(0.35f * pil + 0.1f)); g.fillEllipse(jc.x - 7, jc.y - 9, 8, 6);
    if (pil > 0.05f) { g.setGradientFill(ColourGradient(Colour(0xffff4030).withAlpha(0.35f * pil), jc.x, jc.y, Colours::transparentBlack, jc.x + 34, jc.y, true)); g.fillEllipse(jc.x - 34, jc.y - 34, 68, 68); }
    g.setColour(kInk); g.setFont(F(10.5f, true)); g.drawText("JEWEL", (int)jc.x - 25, (int)jc.y + 18, 50, 12, Justification::centred);

    // service panel
    g.setColour(Colour(0xff1c1611)); g.fillRoundedRectangle(service_.toFloat(), 10.0f);
    g.setColour(Colour(0xff6b5a3a)); g.drawRoundedRectangle(service_.toFloat().reduced(1), 10.0f, 1.2f);
    const auto& d = proc_.diag;
    g.setColour(kAmber); g.setFont(F(12.5f, true));
    g.drawText("SERVICE PANEL  -  live circuit state", service_.getX() + 12, service_.getY() + 8, 400, 16, Justification::left);
    g.setFont(Font(FontOptions(Font::getDefaultMonospacedFontName(), 12.5f, Font::plain)));
    g.setColour(kCream);
    int ty = service_.getY() + 32;
    auto line = [&](const String& s) { g.drawText(s, service_.getX() + 14, ty, 360, 16, Justification::left); ty += 17; };
    line(String::formatted("B+1 (reservoir) %7.1f V", d.b1.load()));
    line(String::formatted("B+2 (screens)   %7.1f V", d.b2.load()));
    line(String::formatted("B+3 (preamp)    %7.1f V", d.b3.load()));
    line(String::formatted("6V6 cathode     %7.2f V", d.cathode.load()));
    line(String::formatted("voice-coil rise %7.1f K", d.coilTemp.load()));
    ty += 4;
    line(String::formatted("solver %.2f it/step, %llu fallbacks", d.itPerStep.load(), (unsigned long long)d.fallbacks.load()));
    line(String::formatted("unrecovered %llu, guard resets %llu", (unsigned long long)d.failures.load(), (unsigned long long)d.panics.load()));
    line(String::formatted("internal %d Hz, latency %d smp", d.internalRateHz.load(), d.latency.load()));
    const float cpu = d.cpu.load() * 100.0f;
    g.setColour(cpu > 80 ? Colour(0xffff6a50) : kCream);
    line(String::formatted("CPU (this instance) %.0f %%", cpu));

    // scope
    g.setColour(Colour(0xff0d1a10)); g.fillRoundedRectangle(scopeR_.toFloat(), 6.0f);
    g.setColour(Colour(0xff1d4a2a)); for (int i = 1; i < 4; ++i) g.drawHorizontalLine(scopeR_.getY() + i * scopeR_.getHeight() / 4, (float)scopeR_.getX(), (float)scopeR_.getRight());
    Path pth; const int N = 1024; const int pos = proc_.scopePos.load();
    for (int i = 0; i < N; ++i) {
        const float v = jlimit(-1.0f, 1.0f, proc_.scope[(pos - N + i + CD57AudioProcessor::kScope * 2) % CD57AudioProcessor::kScope]);
        const float px = scopeR_.getX() + scopeR_.getWidth() * (float)i / (float)(N - 1), py = scopeR_.getCentreY() - v * scopeR_.getHeight() * 0.48f;
        if (i == 0) pth.startNewSubPath(px, py); else pth.lineTo(px, py);
    }
    g.setColour(Colour(0xff52ff7a)); g.strokePath(pth, PathStrokeType(1.3f));
    g.setColour(Colour(0xff7fd89a)); g.setFont(F(10.5f)); g.drawText("OUTPUT", scopeR_.getX() + 6, scopeR_.getY() + 3, 80, 12, Justification::left);

    // lab
    g.setColour(Colour(0x55ffffff)); g.fillRoundedRectangle(lab_.toFloat(), 10.0f);
    g.setColour(kInk); g.setFont(F(13.0f, true));
    g.drawText("OUTPUT  /  SERVICE  (nominal values; changes carry state, see Provenance)", lab_.getX() + 12, lab_.getY() + 4, 700, 16, Justification::left);
    g.setFont(F(10.5f));
    g.drawText(proc_.irStatus(), lab_.getX() + 12, lab_.getBottom() - 13, lab_.getWidth() - 24, 12, Justification::left);
}

void CD57Editor::resized()
{
    plate_ = {20, 74, getWidth() - 40, 190};
    // plate: [power | jewel] [knobs ........] [jacks ......]
    powerSw_.setBounds(plate_.getX() + 30, plate_.getY() + 28, 70, 80);
    auto kb = [&](KnobBox& k, int cx) {
        k.s.setBounds(cx - 52, plate_.getY() + 16, 104, 124); k.l.setBounds(cx - 70, plate_.getY() + 152, 140, 16);
    };
    kb(kInst_, plate_.getX() + 330); kb(kMic_, plate_.getX() + 480); kb(kTone_, plate_.getX() + 630);
    // jack block as photographed (dossier Fig. 3): two columns, MIC on the left and INST on the right, rows 1 and 2.  The engraved labels
    // in the 283x168 px photo are not legible; the 2x2 geometry is.  jacks_[] order: INST 1, INST 2, MIC 1, MIC 2.
    for (int i = 0; i < 4; ++i) jacks_[i].setBounds(plate_.getX() + 730 + (i < 2 ? 100 : 0), plate_.getY() + 20 + (i % 2) * 80, 70, 66);
    cPatch_.l.setBounds(plate_.getX() + 20, plate_.getY() + 142, 90, 16); cPatch_.c.setBounds(plate_.getX() + 20, plate_.getY() + 158, 130, 22);

    service_ = {20, 276, 420, 232};
    scopeR_ = {456, 276, getWidth() - 476, 232};
    lab_ = {20, 520, getWidth() - 40, 292};
    const int cols = 4, cw = (lab_.getWidth() - 24) / cols;
    auto place = [&](Component& l, Component& c, int col, int row, bool slider) {
        const int x = lab_.getX() + 12 + col * cw, y = lab_.getY() + 26 + row * 36;
        l.setBounds(x, y, cw - 12, 14);
        c.setBounds(x, y + 14, cw - 12, slider ? 20 : 20);
    };
    place(cOut_.l, cOut_.c, 0, 0, false); place(cLoad_.l, cLoad_.c, 1, 0, false); place(cMarket_.l, cMarket_.c, 2, 0, false); place(cOS_.l, cOS_.c, 3, 0, false);
    place(sMicPos_.l, sMicPos_.s, 0, 1, true); place(sMicDist_.l, sMicDist_.s, 1, 1, true); place(sLine_.l, sLine_.s, 2, 1, true); place(cTaper_.l, cTaper_.c, 3, 1, false);
    place(sTrim_.l, sTrim_.s, 0, 2, true); place(sOutLevel_.l, sOutLevel_.s, 1, 2, true); place(sInCal_.l, sInCal_.s, 2, 2, true); place(cSource_.l, cSource_.c, 3, 2, false);
    place(cV1_.l, cV1_.c, 0, 3, false); place(cV2_.l, cV2_.c, 1, 3, false); place(cRect_.l, cRect_.c, 2, 3, false); place(sSrcR_.l, sSrcR_.s, 3, 3, true);
    place(sVar_.l, sVar_.s, 0, 4, true); place(sSeed_.l, sSeed_.s, 1, 4, true); place(sHum_.l, sHum_.s, 2, 4, true); place(sNoise_.l, sNoise_.s, 3, 4, true);
    place(cInCh_.l, cInCh_.c, 0, 5, false);
    const int by = lab_.getY() + 26 + 5 * 36 + 4;
    otSatBtn_.setBounds(lab_.getX() + 12 + cw, lab_.getY() + 26 + 5 * 36 + 8, 2 * cw - 12, 22);
    heatBtn_.setBounds(lab_.getX() + 12, by + 40, 170, 22); bypassBtn_.setBounds(lab_.getX() + 12 + cw, by + 40, 120, 22);
    irBtn_.setBounds(lab_.getX() + 12 + 2 * cw, by + 40, 130, 22); aboutBtn_.setBounds(lab_.getX() + 12 + 3 * cw, by + 40, cw - 12, 22);
}

void CD57Editor::timerCallback() { repaint(); }

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
