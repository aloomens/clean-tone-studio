#include "PluginEditor.h"

using namespace juce;

namespace ui
{
//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (Slider::textBoxTextColourId, col::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Slider::textBoxHighlightColourId, col::accent.withAlpha (0.4f));
    setColour (Label::textColourId, col::text);
    setColour (ComboBox::backgroundColourId, col::knob);
    setColour (ComboBox::outlineColourId, col::panelEdge);
    setColour (ComboBox::textColourId, col::text);
    setColour (ComboBox::arrowColourId, col::accent);
    setColour (PopupMenu::backgroundColourId, col::panel);
    setColour (PopupMenu::textColourId, col::text);
    setColour (PopupMenu::highlightedBackgroundColourId, col::accent.withAlpha (0.85f));
    setColour (PopupMenu::highlightedTextColourId, Colours::black);
    setColour (ToggleButton::textColourId, col::text);
    setColour (TextEditor::backgroundColourId, col::knob);
    setColour (TextEditor::textColourId, col::text);
    setColour (TextEditor::highlightColourId, col::accent.withAlpha (0.4f));
    setColour (CaretComponent::caretColourId, col::accent);
}

Font LookAndFeel::getLabelFont (Label& l)
{
    return Font (FontOptions (l.getFont().getHeight()));
}

void LookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos,
                                    float startAngle, float endAngle, Slider& s)
{
    const auto bounds = Rectangle<int> (x, y, w, h).toFloat();
    const float radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 4.0f;
    if (radius < 6.0f) return;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const auto accent = s.findColour (Slider::rotarySliderFillColourId);
    const bool enabled = s.isEnabled();

    // track
    Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour (col::panelEdge.brighter (0.1f));
    g.strokePath (track, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));

    // value arc (bipolar params start from their zero point)
    float fromAngle = startAngle;
    if (s.getMinimum() < 0.0 && s.getMaximum() > 0.0)
        fromAngle = startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle);
    Path value;
    value.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                         jmin (fromAngle, angle), jmax (fromAngle, angle), true);
    g.setColour (enabled ? accent : col::dim);
    g.strokePath (value, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));

    // knob body
    const float r2 = radius * 0.74f;
    ColourGradient grad (col::knob.brighter (0.25f), centre.x, centre.y - r2,
                         col::knob.darker (0.4f), centre.x, centre.y + r2, false);
    g.setGradientFill (grad);
    g.fillEllipse (centre.x - r2, centre.y - r2, r2 * 2.0f, r2 * 2.0f);
    g.setColour (Colours::black.withAlpha (0.5f));
    g.drawEllipse (centre.x - r2, centre.y - r2, r2 * 2.0f, r2 * 2.0f, 1.0f);

    // pointer
    Path p;
    p.addRoundedRectangle (-1.5f, -r2 + 3.0f, 3.0f, r2 * 0.55f, 1.5f);
    p.applyTransform (AffineTransform::rotation (angle).translated (centre));
    g.setColour (enabled ? col::text : col::dim);
    g.fillPath (p);
}

void LookAndFeel::drawToggleButton (Graphics& g, ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const auto accent = b.findColour (ToggleButton::tickColourId);
    g.setColour (on ? accent.withAlpha (0.9f) : col::knob.brighter (highlighted ? 0.15f : 0.0f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (on ? accent.brighter (0.3f) : col::panelEdge);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
    g.setColour (on ? Colours::black : col::text);
    g.setFont (FontOptions (12.5f, Font::bold));
    g.drawText (b.getButtonText(), r, Justification::centred);
}

//==============================================================================
Knob::Knob (AudioProcessorValueTreeState& s, const char* id, const String& name, Colour c)
{
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, 76, 16);
    slider.setColour (Slider::rotarySliderFillColourId, c);
    slider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    slider.setColour (Slider::textBoxTextColourId, col::text);
    slider.setRotaryParameters (MathConstants<float>::pi * 1.2f, MathConstants<float>::pi * 2.8f, true);
    addAndMakeVisible (slider);

    label.setText (name, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (FontOptions (13.0f, Font::bold));
    label.setColour (Label::textColourId, col::text);
    addAndMakeVisible (label);

    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);
    if (auto* p = s.getParameter (id))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    slider.setTooltip (name + " (double-click to reset)");
}

void Knob::resized()
{
    // keep knob + label + value as one compact block, centred vertically
    auto r = getLocalBounds();
    const int sliderH = jmin (r.getHeight() - 18, r.getWidth() + 16);
    auto block = r.withSizeKeepingCentre (r.getWidth(), sliderH + 18);
    label.setBounds (block.removeFromTop (18));
    slider.setBounds (block);
}

Choice::Choice (AudioProcessorValueTreeState& s, const char* id, const String& name)
{
    if (auto* p = dynamic_cast<AudioParameterChoice*> (s.getParameter (id)))
        box.addItemList (p->choices, 1);
    box.setJustificationType (Justification::centred);
    addAndMakeVisible (box);
    label.setText (name, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (FontOptions (13.0f, Font::bold));
    addAndMakeVisible (label);
    attachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (s, id, box);
}

void Choice::resized()
{
    auto r = getLocalBounds();
    if (label.getText().isEmpty()) { label.setVisible (false); box.setBounds (r); return; }
    label.setBounds (r.removeFromTop (18));
    box.setBounds (r.removeFromTop (26));
}

Toggle::Toggle (AudioProcessorValueTreeState& s, const char* id, const String& name)
{
    button.setButtonText (name);
    button.setClickingTogglesState (true);
    addAndMakeVisible (button);
    attachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, id, button);
}

void Meter::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto titleArea = r.removeFromBottom (16.0f);
    g.setColour (col::dim);
    g.setFont (FontOptions (11.0f, Font::bold));
    g.drawText (title, titleArea, Justification::centred);

    auto bar = r.reduced (r.getWidth() * 0.3f, 2.0f);
    g.setColour (Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (bar, 3.0f);

    if (isGR)
    {
        const float frac = jlimit (0.0f, 1.0f, value / 24.0f);
        auto fill = bar.withHeight (bar.getHeight() * frac);
        g.setColour (col::accent);
        g.fillRoundedRectangle (fill, 3.0f);
    }
    else
    {
        const float frac = jlimit (0.0f, 1.0f, (value + 60.0f) / 60.0f);
        auto fill = bar.withTop (bar.getBottom() - bar.getHeight() * frac);
        g.setColour (value > -1.0f ? Colour (0xffe0533d) : value > -12.0f ? col::accent : Colour (0xff7cc576));
        g.fillRoundedRectangle (fill, 3.0f);
    }
}
} // namespace ui

//==============================================================================
CleanToneAudioProcessorEditor::CleanToneAudioProcessorEditor (CleanToneAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      inGain (p.apvts, IDs::inGain, "Input"),
      gate (p.apvts, IDs::gate, "Noise Gate"),
      pickup (p.apvts, IDs::pickup, "Pickup"),
      hum (p.apvts, IDs::hum, "Hum Filter"),
      compOn (p.apvts, IDs::compOn, "ON"),
      compLook (p.apvts, IDs::compLook, "LOOKAHEAD"),
      sustain (p.apvts, IDs::sustain, "Sustain"),
      attack (p.apvts, IDs::attack, "Attack"),
      compMix (p.apvts, IDs::compMix, "Blend"),
      pick (p.apvts, IDs::pick, "Pick"),
      outGain (p.apvts, IDs::outGain, "Output"),
      autoLevel (p.apvts, IDs::autoLevel, "AUTO LEVEL"),
      warmth (p.apvts, IDs::warmth, "Warmth"),
      clarity (p.apvts, IDs::clarity, "Clarity"),
      bass (p.apvts, IDs::bass, "Bass"),
      mid (p.apvts, IDs::mid, "Mid"),
      midFreq (p.apvts, IDs::midFreq, "Mid Freq"),
      presence (p.apvts, IDs::presence, "Presence"),
      treble (p.apvts, IDs::treble, "Treble"),
      lowCut (p.apvts, IDs::lowCut, "Low Cut"),
      highCut (p.apvts, IDs::highCut, "High Cut"),
      warmTight (p.apvts, IDs::warmTight, "TIGHT"),
      cab (p.apvts, IDs::cab, "Cabinet"),
      irAlign (p.apvts, IDs::irAlign, "ALIGN"),
      dlyOn (p.apvts, IDs::dlyOn, "ON"),
      dlySync (p.apvts, IDs::dlySync, "SYNC"),
      dlyDiv (p.apvts, IDs::dlyDiv, ""),
      dlyStyle (p.apvts, IDs::dlyStyle, ""),
      dlyTime (p.apvts, IDs::dlyTime, "Time", ui::col::accent2),
      dlyFeedback (p.apvts, IDs::dlyFeedback, "Feedback", ui::col::accent2),
      dlyTone (p.apvts, IDs::dlyTone, "Tone", ui::col::accent2),
      dlyMod (p.apvts, IDs::dlyMod, "Mod", ui::col::accent2),
      dlyDuck (p.apvts, IDs::dlyDuck, "Ducking", ui::col::accent2),
      dlyWidth (p.apvts, IDs::dlyWidth, "Ping-Pong", ui::col::accent2),
      dlyMix (p.apvts, IDs::dlyMix, "Level", ui::col::accent2),
      revOn (p.apvts, IDs::revOn, "ON"),
      revSize (p.apvts, IDs::revSize, "Size", ui::col::accent2),
      revDecay (p.apvts, IDs::revDecay, "Decay", ui::col::accent2),
      revPreDelay (p.apvts, IDs::revPreDelay, "Pre-Delay", ui::col::accent2),
      revTone (p.apvts, IDs::revTone, "Tone", ui::col::accent2),
      revLowCut (p.apvts, IDs::revLowCut, "Low Cut", ui::col::accent2),
      revMix (p.apvts, IDs::revMix, "Level", ui::col::accent2),
      revDuck (p.apvts, IDs::revDuck, "Ducking", ui::col::accent2)
{
    setLookAndFeel (&lnf);

    for (Component* c : std::initializer_list<Component*> {
             &inGain, &gate, &pickup, &hum, &compOn, &compLook, &sustain, &attack, &compMix, &pick, &grMeter,
             &outGain, &autoLevel, &autoLevelReadout, &inMeter, &outMeter, &warmth, &clarity, &warmTight, &bass, &mid, &midFreq, &presence, &treble,
             &lowCut, &highCut, &cab, &dlyOn, &dlySync, &dlyDiv, &dlyStyle, &dlyStyleLabel, &dlyTime, &dlyFeedback, &dlyTone,
             &dlyMod, &dlyDuck, &dlyWidth, &dlyMix, &revOn, &revSize, &revDecay, &revPreDelay,
             &revTone, &revLowCut, &revMix, &revDuck })
        body.addAndMakeVisible (c);

    for (auto* t : { &dlyOn, &dlySync, &revOn })
        t->button.setColour (ToggleButton::tickColourId, ui::col::accent2);
    autoLevel.button.setColour (ToggleButton::tickColourId, ui::col::accent);
    compOn.button.setColour (ToggleButton::tickColourId, ui::col::accent);
    compOn.button.setTooltip ("Switch the sustain compressor off when you already use a hardware compressor "
                              "before your interface. Stays as you set it when you change presets.");
    autoLevel.button.setTooltip ("Keeps the output as loud as the input whenever you change presets or settings. "
                                 "Use Output to set the final level.");
    compLook.button.setColour (ToggleButton::tickColourId, ui::col::accent);
    warmTight.button.setColour (ToggleButton::tickColourId, ui::col::accent);
    warmTight.button.setTooltip ("Tight Warmth: keeps the deep lows out of the tube stage, so you get its harmonics "
                                 "without the low end blooming. Best with humbuckers, drop tunings or a lot of Warmth.");
    pick.slider.setTooltip ("Pick: brings the attack of each note forward (+) for more snap and definition, or "
                            "softens it (-) for a rounder start. The body of the note is left alone. Double-click to reset.");
    clarity.slider.setTooltip ("Clarity: trims the muddy low mids (around 300 Hz) only when they build up - chords, "
                               "the neck pickup, a boomy cabinet - and leaves a balanced tone alone. Double-click to reset.");
    revDuck.slider.setTooltip ("Reverb Ducking: the reverb backs off while you play and blooms in the gaps, so it "
                               "never washes over the notes. Double-click to reset.");
    compLook.button.setTooltip ("Lets the compressor see each pick attack 3 ms early, so even the fastest transients "
                                "are caught smoothly. Adds 3 ms of latency (reported to the host). "
                                "Stays as you set it when you change presets.");

    dlyStyle.box.setTooltip ("How the delay repeats move: Sine = a smooth, even chorus-like sway; "
                             "Tape = a slow, irregular wow plus a little flutter and tape warmth");
    dlyStyleLabel.setText ("MOD", dontSendNotification);
    dlyStyleLabel.setFont (FontOptions (11.5f, Font::bold));
    dlyStyleLabel.setColour (Label::textColourId, ui::col::dim);
    dlyStyleLabel.setJustificationType (Justification::centredRight);

    // ---- Sync tempo ----
    bpmSlider.setSliderStyle (Slider::LinearBar);
    bpmSlider.setColour (Slider::trackColourId, ui::col::accent2.withAlpha (0.35f));
    bpmSlider.setColour (Slider::backgroundColourId, ui::col::knob);
    bpmSlider.setColour (Slider::textBoxTextColourId, ui::col::text);
    bpmSlider.setColour (Slider::textBoxOutlineColourId, ui::col::panelEdge);
    bpmSlider.setTextBoxIsEditable (true);
    bpmSlider.setTooltip ("Tempo for Sync: drag, or double-click to type (30 - 300 BPM). Use TAP to tap it in. "
                          "Stays as you set it when you change presets.");
    bpmAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, IDs::dlyBpm, bpmSlider);
    bpmSlider.setDoubleClickReturnValue (false, 120.0);
    body.addAndMakeVisible (bpmSlider);

    tapButton.setColour (TextButton::buttonColourId, ui::col::knob);
    tapButton.setColour (TextButton::textColourOffId, ui::col::text);
    tapButton.setTooltip ("Tap in time (two taps or more) to set the Sync tempo. A pause of 2 seconds starts a new count.");
    tapButton.setTriggeredOnMouseDown (true);       // the tempo follows the moment you press, not when you let go
    tapButton.onClick = [this]
    {
        if (auto bpm = tapper.tap (Time::getMillisecondCounterHiRes()))
            if (auto* prm = proc.apvts.getParameter (IDs::dlyBpm))
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (prm->convertTo0to1 ((float) *bpm));
                prm->endChangeGesture();
            }
    };
    body.addAndMakeVisible (tapButton);

    hostBpmLabel.setJustificationType (Justification::centred);
    hostBpmLabel.setFont (FontOptions (12.5f));
    hostBpmLabel.setColour (Label::textColourId, ui::col::dim);
    hostBpmLabel.setColour (Label::outlineColourId, ui::col::panelEdge);
    hostBpmLabel.setTooltip ("Sync follows the project tempo from your DAW.");
    body.addChildComponent (hostBpmLabel);

    // ---- impulse-response loader: slots A and B with a blend ----
    for (int slot = 0; slot < 2; ++slot)
    {
        auto& nm = irName[slot];
        nm.setFont (FontOptions (12.5f));
        nm.setColour (Label::backgroundColourId, Colours::black.withAlpha (0.3f));
        nm.setColour (Label::textColourId, ui::col::text);
        nm.setMinimumHorizontalScale (0.7f);
        nm.setTooltip (String ("Impulse response slot ") + (slot == 0 ? "A" : "B")
                       + " (WAV, AIFF or FLAC; any sample rate; up to 0.3 s is used)");
        body.addAndMakeVisible (nm);

        auto& ld = irLoad[slot];
        ld.setButtonText ("Load");
        ld.setTooltip ("Load an impulse response file (switches Cabinet to IR Loader)");
        ld.onClick = [this, slot] { chooseIR (slot); };
        auto& cl = irClear[slot];
        cl.setButtonText (String::fromUTF8 ("\xc3\x97"));
        cl.setTooltip ("Remove this impulse response");
        cl.onClick = [this, slot] { proc.clearIR (slot); };
        for (auto* b : { &ld, &cl })
        {
            b->setColour (TextButton::buttonColourId, ui::col::knob);
            b->setColour (TextButton::textColourOffId, ui::col::text);
            body.addAndMakeVisible (b);
        }
    }
    irBlendLabel.setText ("A / B", dontSendNotification);
    irBlendLabel.setFont (FontOptions (12.0f, Font::bold));
    irBlendLabel.setColour (Label::textColourId, ui::col::dim);
    body.addAndMakeVisible (irBlendLabel);
    irBlend.setSliderStyle (Slider::LinearHorizontal);
    irBlend.setTextBoxStyle (Slider::TextBoxRight, false, 44, 18);
    irBlend.setColour (Slider::trackColourId, ui::col::accent);
    irBlend.setColour (Slider::thumbColourId, ui::col::accent);
    irBlend.setColour (Slider::backgroundColourId, ui::col::knob);
    irBlend.setTooltip ("Blend between IR A (left) and IR B (right), 50 = equal mix (double-click to reset)");
    irBlendAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, IDs::irBlend, irBlend);
    irBlend.setDoubleClickReturnValue (true, 50.0);
    body.addAndMakeVisible (irBlend);
    irAlign.button.setColour (ToggleButton::tickColourId, ui::col::accent);
    irAlign.button.setTooltip ("Lines the impulse responses up: each one starts right away (no added delay) and the "
                               "two blend without their low end cancelling. Same frequency response; turn it off to "
                               "use the IRs exactly as recorded.");
    body.addAndMakeVisible (irAlign);
    autoLevelReadout.setJustificationType (Justification::centred);
    autoLevelReadout.setFont (FontOptions (13.0f));
    autoLevelReadout.setColour (Label::textColourId, ui::col::dim);

    // ---- presets: factory list + "My Presets", Save / Delete ----
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id <= 0) return;
        if (id >= kUserIdBase)
        {
            const auto name = userPresetNames[id - kUserIdBase];
            if (! proc.loadUserPreset (name))
                setStatus ("Couldn't load \"" + name + "\"", Colour (0xffe0533d));
        }
        else
            proc.loadFactoryPreset (id - 1);
        deleteArmTicks = 0;
        updatePresetDisplay();
    };
    refreshPresetBox();
    body.addAndMakeVisible (presetBox);

    for (auto* b : { &saveButton, &deleteButton, &confirmSaveButton, &cancelSaveButton })
    {
        b->setColour (TextButton::buttonColourId, ui::col::knob);
        b->setColour (TextButton::textColourOffId, ui::col::text);
        body.addChildComponent (b);
    }
    confirmSaveButton.setColour (TextButton::buttonColourId, ui::col::accent);
    confirmSaveButton.setColour (TextButton::textColourOffId, Colours::black);
    saveButton.setVisible (true);
    deleteButton.setVisible (true);
    saveButton.setTooltip ("Save the current sound as one of your own presets");
    deleteButton.setTooltip ("Delete this preset (moves it to the Recycle Bin)");

    saveButton.onClick = [this] { showSaveBar (true); };
    cancelSaveButton.onClick = [this] { showSaveBar (false); };
    confirmSaveButton.onClick = [this] { commitSave(); };

    deleteButton.onClick = [this]
    {
        if (! proc.isCurrentPresetUser()) return;
        const auto name = proc.getCurrentPresetName();
        if (deleteArmTicks <= 0)
        {
            deleteArmTicks = 120;     // ~4 s to confirm
            deleteButton.setButtonText ("Sure?");
            setStatus ("Click \"Sure?\" to delete \"" + name + "\"", ui::col::accent);
            return;
        }
        deleteArmTicks = 0;
        deleteButton.setButtonText ("Delete");
        if (proc.deleteUserPreset (name))
            setStatus ("Deleted \"" + name + "\" (it's in the Recycle Bin)", ui::col::dim);
        else
            setStatus ("Couldn't delete \"" + name + "\"", Colour (0xffe0533d));
        refreshPresetBox();
    };

    nameEditor.setColour (TextEditor::outlineColourId, ui::col::accent);
    nameEditor.setColour (TextEditor::focusedOutlineColourId, ui::col::accent);
    nameEditor.setFont (FontOptions (15.0f));
    nameEditor.setTextToShowWhenEmpty ("Name your preset", ui::col::dim);
    nameEditor.setInputRestrictions (60);
    nameEditor.onReturnKey = [this] { commitSave(); };
    nameEditor.onEscapeKey = [this] { showSaveBar (false); };
    nameEditor.onTextChange = [this] { overwriteArmed = false; confirmSaveButton.setButtonText ("Save"); };
    body.addChildComponent (nameEditor);

    statusLabel.setFont (FontOptions (13.0f));
    statusLabel.setJustificationType (Justification::centredRight);
    body.addAndMakeVisible (statusLabel);

    body.onPaint = [this] (Graphics& g) { paintBody (g); };
    body.onMouseUp = [this] (const MouseEvent& e)
    {
        if (titleArea().contains (e.getPosition()))
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, String ("About ") + CTS_PRODUCT_NAME, aboutText(), "OK", this);
    };
    body.onMouseMove = [this] (const MouseEvent& e)
    {
        body.setMouseCursor (titleArea().contains (e.getPosition()) ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
    };
    body.onResize = [this] { layoutBody(); };
    addAndMakeVisible (body);
    body.setBounds (0, 0, kBaseW, kBaseH);

    // resizable (corner handle in hosts; in the app the window can also be maximized)
    setResizable (true, true);
    setResizeLimits (kBaseW / 2, kBaseH / 2, kBaseW * 4, kBaseH * 4);
    // in a host, dragging the corner keeps the proportions; the app's window can take any shape (it can be
    // maximized), and the layout is centred in it
    if (! proc.isStandaloneApp())
        if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) kBaseW / kBaseH);
    setSize (proc.editorWidth > 0 ? proc.editorWidth : kBaseW, proc.editorHeight > 0 ? proc.editorHeight : kBaseH);
    timerCallback();      // show the current state (IR names, enabled controls) right away
    sizeReady = true;     // from now on, sizes are the user's (not the ones passed through while setting up)
    startTimerHz (30);
}

int CleanToneAudioProcessorEditor::titleWidth()
{
    return GlyphArrangement::getStringWidthInt (Font (FontOptions (24.0f, Font::bold)), titleText());
}

Rectangle<int> CleanToneAudioProcessorEditor::titleArea() const
{
    return { 12, 8, titleWidth() + 18, 40 };
}

// About box (the "Appropriate Legal Notices" of the GNU AGPL): copyright, no warranty, license.
String CleanToneAudioProcessorEditor::aboutText()
{
    return String (CTS_PRODUCT_NAME) + " " + CTS_VERSION + "\n"
           "Copyright (C) 2026 Al Oomens\n\n"
           "This program is free software: you can redistribute it and/or modify it under the terms of the "
           "GNU Affero General Public License as published by the Free Software Foundation, either version 3 "
           "of the License, or (at your option) any later version.\n\n"
           "This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without "
           "even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU "
           "Affero General Public License (LICENSE.txt, included with the program) for more details.\n\n"
           "The source code is distributed alongside the program (" + String (CTS_PRODUCT_NAME).removeCharacters (" ")
           + "-" CTS_VERSION "-source.zip).\n\n"
           "Built with JUCE (AGPLv3) and the Steinberg VST3 and ASIO SDKs (GPLv3). "
           "VST and ASIO are trademarks of Steinberg Media Technologies GmbH. "
           "See THIRD-PARTY-NOTICES.txt.";
}

void CleanToneAudioProcessorEditor::resized()
{
    // scale the fixed layout to fit, centred (a maximized window that isn't the same shape gets side bars)
    const float s = jmin ((float) getWidth() / kBaseW, (float) getHeight() / kBaseH);
    const float x = 0.5f * ((float) getWidth()  - kBaseW * s);
    const float y = 0.5f * ((float) getHeight() - kBaseH * s);
    body.setTransform (AffineTransform::scale (s).translated (x, y));
    const auto* peer = getPeer();
    if (sizeReady && (peer == nullptr || ! (peer->isFullScreen() || peer->isMinimised())))
    {
        proc.editorWidth = getWidth();      // remembered for the next time the window opens
        proc.editorHeight = getHeight();
    }
}

void CleanToneAudioProcessorEditor::paint (Graphics& g)
{
    g.fillAll (ui::col::bg);
}

// Standalone only: JUCE's standalone window offers just minimize/close. Once the editor is inside that
// window, turn on the maximize button too (double-clicking the title bar also maximizes). A maximized
// window can be any shape (the layout is centred in it).
void CleanToneAudioProcessorEditor::parentHierarchyChanged()
{
    AudioProcessorEditor::parentHierarchyChanged();
    if (! proc.isStandaloneApp()) return;
    auto* window = findParentComponentOfClass<DocumentWindow>();
    if (window == nullptr || window == setUpWindow.getComponent()) return;
    setUpWindow = window;
    // done once the window has finished setting itself up: changing the title bar while it is still
    // being built makes it lay out its not-yet-sized content and squeeze the editor
    MessageManager::callAsync ([w = Component::SafePointer<DocumentWindow> (window)]
    {
        if (w != nullptr) w->setTitleBarButtonsRequired (DocumentWindow::allButtons, false);
    });
}

void CleanToneAudioProcessorEditor::chooseIR (int slot)
{
    static File lastFolder;
    const auto start = lastFolder.isDirectory() ? lastFolder : File::getSpecialLocation (File::userDocumentsDirectory);
    irChooser = std::make_unique<FileChooser> (String ("Load impulse response ") + (slot == 0 ? "A" : "B"),
                                               start, CleanToneAudioProcessor::getIRFileWildcard());
    irChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                            [this, slot] (const FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == File()) return;
        lastFolder = file.getParentDirectory();
        const auto err = proc.loadIRFile (slot, file);
        if (err.isNotEmpty()) { setStatus (err, Colour (0xffe0533d)); return; }
        if (auto* p = proc.apvts.getParameter (IDs::cab))
            if ((int) p->convertFrom0to1 (p->getValue()) != ct::kCabIR)
                p->setValueNotifyingHost (p->convertTo0to1 ((float) ct::kCabIR));
        setStatus (String ("IR ") + (slot == 0 ? "A" : "B") + ": loaded \"" + proc.getIRName (slot) + "\"", Colour (0xff7cc576));
    });
}

CleanToneAudioProcessorEditor::~CleanToneAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void CleanToneAudioProcessorEditor::refreshPresetBox()
{
    userPresetNames = proc.getUserPresetNames();
    presetBox.clear (dontSendNotification);
    presetBox.addSectionHeading ("Factory");
    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem (proc.getProgramName (i), i + 1);
    presetBox.addSeparator();
    presetBox.addSectionHeading ("My Presets");
    if (userPresetNames.isEmpty())
    {
        presetBox.addItem ("(none yet - press Save)", kUserIdBase - 1);
        presetBox.setItemEnabled (kUserIdBase - 1, false);
    }
    for (int i = 0; i < userPresetNames.size(); ++i)
        presetBox.addItem (userPresetNames[i], kUserIdBase + i);
    updatePresetDisplay();
}

void CleanToneAudioProcessorEditor::updatePresetDisplay()
{
    if (presetBox.isPopupActive()) return;

    const bool isUser = proc.isCurrentPresetUser();
    const auto name = proc.getCurrentPresetName();
    const int id = isUser ? (userPresetNames.contains (name) ? kUserIdBase + userPresetNames.indexOf (name) : 0)
                          : proc.getCurrentProgram() + 1;
    const bool modified = proc.isPresetModified();
    const bool deleted = isUser && id == 0;
    const String text = (name.isEmpty() ? String ("Presets") : name)
                        + (deleted ? "  (deleted)" : modified ? "  (edited)" : "");

    // when edited, show custom text (no item ticked) so re-picking the preset reverts to it
    if (modified || id == 0)
    {
        if (presetBox.getText() != text)
            presetBox.setText (text, dontSendNotification);
    }
    else if (presetBox.getSelectedId() != id)
        presetBox.setSelectedId (id, dontSendNotification);

    deleteButton.setEnabled (isUser && userPresetNames.contains (name));
}

void CleanToneAudioProcessorEditor::setStatus (const String& text, Colour colour)
{
    statusLabel.setText (text, dontSendNotification);
    statusLabel.setColour (Label::textColourId, colour);
    statusTicks = 150;   // ~5 s
    repaint();
}

void CleanToneAudioProcessorEditor::showSaveBar (bool show)
{
    overwriteArmed = false;
    confirmSaveButton.setButtonText ("Save");
    presetBox.setVisible (! show);
    saveButton.setVisible (! show);
    deleteButton.setVisible (! show);
    nameEditor.setVisible (show);
    confirmSaveButton.setVisible (show);
    cancelSaveButton.setVisible (show);
    if (show)
    {
        nameEditor.setText (proc.isCurrentPresetUser() ? proc.getCurrentPresetName() : String(), false);
        nameEditor.selectAll();
        nameEditor.grabKeyboardFocus();
    }
}

void CleanToneAudioProcessorEditor::commitSave()
{
    const auto name = File::createLegalFileName (nameEditor.getText().trim());
    if (name.isEmpty())
    {
        setStatus ("Type a name for your preset", ui::col::accent);
        return;
    }
    if (proc.userPresetExists (name) && ! overwriteArmed)
    {
        overwriteArmed = true;
        confirmSaveButton.setButtonText ("Replace");
        setStatus ("\"" + name + "\" already exists - click Replace to overwrite it", ui::col::accent);
        return;
    }
    if (proc.saveUserPreset (name))
    {
        showSaveBar (false);
        refreshPresetBox();
        setStatus ("Saved \"" + name + "\"", Colour (0xff7cc576));
    }
    else
        setStatus ("Couldn't save to " + CleanToneAudioProcessor::getUserPresetFolder().getFullPathName(), Colour (0xffe0533d));
}

void CleanToneAudioProcessorEditor::timerCallback()
{
    auto decay = [] (float current, float peakLin)
    {
        const float newDb = Decibels::gainToDecibels (peakLin, -100.0f);
        return newDb > current ? newDb : jmax (-100.0f, current - 1.5f);
    };
    inDb  = decay (inDb,  proc.inPeak.exchange (0.0f));
    outDb = decay (outDb, proc.outPeak.exchange (0.0f));
    inMeter.setDb (inDb);
    outMeter.setDb (outDb);
    grMeter.setDb (proc.gainReductionDb.load());

    const bool autoOn = proc.apvts.getRawParameterValue (IDs::autoLevel)->load() > 0.5f;
    const float comp = proc.autoGainDb.load();
    const String txt = autoOn ? "correction " + String (comp >= 0.05f ? "+" : "") + String (comp, 1) + " dB" : "off";
    if (autoLevelReadout.getText() != txt) autoLevelReadout.setText (txt, dontSendNotification);

    const bool sync = proc.apvts.getRawParameterValue (IDs::dlySync)->load() > 0.5f;
    dlyTime.slider.setEnabled (! sync);

    const bool compIsOn = proc.apvts.getRawParameterValue (IDs::compOn)->load() > 0.5f;
    for (auto* k : { &sustain, &attack, &compMix })
        if (k->slider.isEnabled() != compIsOn)
        {
            k->slider.setEnabled (compIsOn);
            k->label.setAlpha (compIsOn ? 1.0f : 0.45f);
        }
    grMeter.setAlpha (compIsOn ? 1.0f : 0.45f);
    dlyDiv.box.setEnabled (sync);

    // Sync tempo: the DAW's (read-only) when it sends one, otherwise the plugin's own (editable + TAP)
    const bool hostTempo = proc.isUsingHostTempo();
    bpmSlider.setVisible (! hostTempo);
    tapButton.setVisible (! hostTempo);
    hostBpmLabel.setVisible (hostTempo);
    if (hostTempo)
    {
        const auto hostTxt = String (proc.getHostBpm(), 1) + String::fromUTF8 (" BPM \xc2\xb7 host");
        if (hostBpmLabel.getText() != hostTxt) hostBpmLabel.setText (hostTxt, dontSendNotification);
    }
    const float tempoAlpha = sync ? 1.0f : 0.45f;
    for (Component* c : std::initializer_list<Component*> { &bpmSlider, &tapButton, &hostBpmLabel })
        if (c->getAlpha() != tempoAlpha) c->setAlpha (tempoAlpha);
    {
        const int taps = tapper.getCount (Time::getMillisecondCounterHiRes());
        const String tapTxt = taps > 0 ? "TAP " + String (taps) : "TAP";
        if (tapButton.getButtonText() != tapTxt) tapButton.setButtonText (tapTxt);
    }
    compLook.setAlpha (compIsOn ? 1.0f : 0.45f);

    // IR loader: names, and dimmed while another cabinet is chosen
    const bool irOn = (int) proc.apvts.getRawParameterValue (IDs::cab)->load() == ct::kCabIR;
    for (int slot = 0; slot < 2; ++slot)
    {
        const auto nm = String (slot == 0 ? "A  " : "B  ") + (proc.hasIR (slot) ? proc.getIRName (slot) : String ("(empty)"));
        if (irName[slot].getText() != nm) irName[slot].setText (nm, dontSendNotification);
        irName[slot].setColour (Label::textColourId, proc.hasIR (slot) ? ui::col::text : ui::col::dim);
        irClear[slot].setEnabled (proc.hasIR (slot));
    }
    const float irAlpha = irOn ? 1.0f : 0.45f;
    for (Component* c : std::initializer_list<Component*> { &irName[0], &irName[1], &irLoad[0], &irLoad[1], &irClear[0], &irClear[1], &irBlend, &irBlendLabel, &irAlign })
        if (c->getAlpha() != irAlpha) c->setAlpha (irAlpha);
    irBlend.setEnabled (proc.hasIR (0) && proc.hasIR (1));

    // presets: pick up files saved by another instance (e.g. the app while Cubase is open)
    if (++rescanTicks >= 60 && ! presetBox.isPopupActive())
    {
        rescanTicks = 0;
        if (proc.getUserPresetNames() != userPresetNames)
            refreshPresetBox();
    }
    updatePresetDisplay();

    if (deleteArmTicks > 0 && --deleteArmTicks == 0)
        deleteButton.setButtonText ("Delete");
    if (statusTicks > 0 && --statusTicks == 0)
    {
        statusLabel.setText ({}, dontSendNotification);
        repaint();
    }
}

void CleanToneAudioProcessorEditor::paintBody (Graphics& g)
{
    g.fillAll (ui::col::bg);

    // header
    auto header = body.getLocalBounds().removeFromTop (56).toFloat();
    ColourGradient hg (ui::col::panel.brighter (0.08f), 0, 0, ui::col::bg, 0, header.getBottom(), false);
    g.setGradientFill (hg);
    g.fillRect (header);
    g.setColour (ui::col::accent);
    g.setFont (FontOptions (24.0f, Font::bold));
    g.drawText (titleText(), header.reduced (18, 0), Justification::centredLeft);
    if (statusLabel.getText().isEmpty())
    {
        g.setColour (ui::col::dim);
        g.setFont (FontOptions (13.0f));
        g.drawText (String ("clean electric guitar recording channel   v") + CTS_VERSION, header.reduced (18, 0).withTrimmedLeft (titleWidth() + 16),
                    Justification::centredLeft);
    }

    for (const auto& s : sections)
    {
        auto r = s.area.toFloat();
        g.setColour (ui::col::panel);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (ui::col::panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
        g.setColour (s.colour);
        g.setFont (FontOptions (13.5f, Font::bold));
        g.drawText (s.title, r.reduced (12, 0).removeFromTop (26), Justification::centredLeft);
        g.setColour (s.colour.withAlpha (0.35f));
        g.fillRect (r.getX() + 12.0f, r.getY() + 26.0f, 28.0f, 2.0f);
    }
}

void CleanToneAudioProcessorEditor::layoutBody()
{
    sections.clear();
    auto area = body.getLocalBounds().reduced (12);
    auto header = area.removeFromTop (44);
    {
        auto bar = header.removeFromRight (400).withSizeKeepingCentre (400, 30);
        auto right = bar;
        deleteButton.setBounds (right.removeFromRight (66));
        right.removeFromRight (6);
        saveButton.setBounds (right.removeFromRight (66));
        right.removeFromRight (6);
        presetBox.setBounds (right);

        auto edit = bar;
        cancelSaveButton.setBounds (edit.removeFromRight (66));
        edit.removeFromRight (6);
        confirmSaveButton.setBounds (edit.removeFromRight (66));
        edit.removeFromRight (6);
        nameEditor.setBounds (edit);

        statusLabel.setBounds (header.withTrimmedLeft (titleWidth() + 16).withTrimmedRight (10));
    }
    area.removeFromTop (8);

    auto addSection = [this] (Rectangle<int> r, const String& title, Colour c)
    {
        sections.push_back ({ title, r, c });
        return r.reduced (8, 0).withTrimmedTop (30).withTrimmedBottom (8);
    };
    auto layoutRow = [] (Rectangle<int> r, std::initializer_list<Component*> comps)
    {
        const int w = r.getWidth() / (int) comps.size();
        for (auto* c : comps) c->setBounds (r.removeFromLeft (w));
    };

    const int gap = 10;

    // Row 1: input | dynamics | output
    auto row1 = area.removeFromTop (170);
    area.removeFromTop (gap);
    {
        auto in = addSection (row1.removeFromLeft (360), "INPUT", ui::col::accent);
        auto choices = in.removeFromRight (140).reduced (4, 6);
        pickup.setBounds (choices.removeFromTop (52));
        choices.removeFromTop (10);
        hum.setBounds (choices.removeFromTop (52));
        layoutRow (in, { &inGain, &gate });

        row1.removeFromLeft (gap);
        auto dynRect = row1.removeFromLeft (360);
        auto dyn = addSection (dynRect, "SUSTAIN / DYNAMICS", ui::col::accent);
        auto dstrip = dynRect.reduced (10, 0).removeFromTop (30).withTrimmedLeft (190).reduced (0, 3);
        compOn.setBounds (dstrip.removeFromLeft (44));
        dstrip.removeFromLeft (6);
        compLook.setBounds (dstrip.removeFromLeft (94));
        grMeter.setBounds (dyn.removeFromRight (44));
        layoutRow (dyn, { &sustain, &attack, &compMix, &pick });

        row1.removeFromLeft (gap);
        auto out = addSection (row1, "OUTPUT", ui::col::accent);
        outMeter.setBounds (out.removeFromRight (44));
        inMeter.setBounds (out.removeFromRight (44));
        auto autoCol = out.removeFromRight (130).reduced (6, 0);
        autoCol = autoCol.withSizeKeepingCentre (autoCol.getWidth(), 58);
        autoLevel.setBounds (autoCol.removeFromTop (30));
        autoLevelReadout.setBounds (autoCol.removeFromTop (24));
        outGain.setBounds (out.withSizeKeepingCentre (jmin (out.getWidth(), 110), out.getHeight()));
    }

    // Row 2: tone
    auto row2 = area.removeFromTop (170);
    area.removeFromTop (gap);
    {
        auto t = addSection (row2, "TONE & CABINET", ui::col::accent);
        warmTight.setBounds (row2.reduced (10, 0).removeFromTop (30).withTrimmedLeft (150).reduced (0, 3).removeFromLeft (64));
        auto cabArea = t.removeFromRight (240).reduced (6, 0);
        cabArea = cabArea.withSizeKeepingCentre (cabArea.getWidth(), 128);
        cab.setBounds (cabArea.removeFromTop (44));
        for (int slot = 0; slot < 2; ++slot)
        {
            cabArea.removeFromTop (5);
            auto row = cabArea.removeFromTop (22);
            irClear[slot].setBounds (row.removeFromRight (24));
            row.removeFromRight (3);
            irLoad[slot].setBounds (row.removeFromRight (46));
            row.removeFromRight (4);
            irName[slot].setBounds (row);
        }
        cabArea.removeFromTop (5);
        auto blendRow = cabArea.removeFromTop (22);
        irBlendLabel.setBounds (blendRow.removeFromLeft (40));
        irAlign.setBounds (blendRow.removeFromRight (58));
        blendRow.removeFromRight (4);
        irBlend.setBounds (blendRow);
        layoutRow (t, { &warmth, &clarity, &bass, &mid, &midFreq, &presence, &treble, &lowCut, &highCut });
    }

    // Row 3: delay | reverb
    auto row3 = area;
    {
        auto dRect = row3.removeFromLeft (row3.getWidth() * 52 / 100);
        auto d = addSection (dRect, "DELAY", ui::col::accent2);
        auto strip = dRect.reduced (10, 0).removeFromTop (30).withTrimmedLeft (70).reduced (0, 3);
        dlyOn.setBounds (strip.removeFromLeft (48));
        strip.removeFromLeft (8);
        dlySync.setBounds (strip.removeFromLeft (58));
        strip.removeFromLeft (8);
        dlyDiv.setBounds (strip.removeFromLeft (104));
        strip.removeFromLeft (6);
        {
            auto tempo = strip.removeFromLeft (113);
            hostBpmLabel.setBounds (tempo);
            bpmSlider.setBounds (tempo.removeFromLeft (70));
            tempo.removeFromLeft (3);
            tapButton.setBounds (tempo);
        }
        strip.removeFromLeft (8);
        dlyStyleLabel.setBounds (strip.removeFromLeft (30));
        strip.removeFromLeft (3);
        dlyStyle.setBounds (strip.removeFromLeft (76));
        layoutRow (d, { &dlyTime, &dlyFeedback, &dlyTone, &dlyMod, &dlyDuck, &dlyWidth, &dlyMix });

        row3.removeFromLeft (gap);
        auto rRect = row3;
        auto r = addSection (rRect, "REVERB", ui::col::accent2);
        auto rstrip = rRect.reduced (10, 0).removeFromTop (30).withTrimmedLeft (80).reduced (0, 3);
        revOn.setBounds (rstrip.removeFromLeft (48));
        layoutRow (r, { &revSize, &revDecay, &revPreDelay, &revTone, &revLowCut, &revMix, &revDuck });
    }
}
