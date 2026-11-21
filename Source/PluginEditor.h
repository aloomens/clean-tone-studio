#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "TapTempo.h"

namespace ui
{
    namespace col
    {
        const juce::Colour bg        { 0xff17181b };
        const juce::Colour panel     { 0xff222428 };
        const juce::Colour panelEdge { 0xff32353b };
        const juce::Colour text      { 0xffd9d6cf };
        const juce::Colour dim       { 0xff8a8a86 };
        const juce::Colour accent    { 0xffe8a54b };   // warm amber
        const juce::Colour accent2   { 0xff6fc2c9 };   // cool teal for time effects
        const juce::Colour knob      { 0xff2e3036 };
        // the title, shared by Clean Tone Studio, Open Vocal Studio and Open Mastering Studio:
        const juce::Colour titleWhite { 0xffe4e7ee };  // "CLEAN TONE"
        const juce::Colour studio     { 0xff4fc3f7 };  // "STUDIO"
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider&) override;
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
        juce::Font getLabelFont (juce::Label&) override;
    };

    struct Knob : juce::Component
    {
        Knob (juce::AudioProcessorValueTreeState& s, const char* id, const juce::String& name, juce::Colour c = col::accent);
        void resized() override;
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    struct Choice : juce::Component
    {
        Choice (juce::AudioProcessorValueTreeState& s, const char* id, const juce::String& name);
        void resized() override;
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    };

    struct Toggle : juce::Component
    {
        Toggle (juce::AudioProcessorValueTreeState& s, const char* id, const juce::String& name);
        void resized() override { button.setBounds (getLocalBounds()); }
        juce::ToggleButton button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    };

    struct Meter : juce::Component
    {
        Meter (juce::String name, bool reduction) : title (std::move (name)), isGR (reduction) {}
        void setDb (float db) { if (std::abs (db - value) > 0.05f) { value = db; repaint(); } }
        void paint (juce::Graphics&) override;
        juce::String title;
        bool isGR;
        float value = -100.0f;
    };
}

class CleanToneAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit CleanToneAudioProcessorEditor (CleanToneAudioProcessor&);
    ~CleanToneAudioProcessorEditor() override;

    static constexpr int kBaseW = 1060, kBaseH = 610;   // the layout's own size; the window scales it

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    // Everything is laid out at kBaseW x kBaseH inside this component, which is then scaled to fill the
    // window (keeping its proportions), so the window can be resized or maximized.
    struct Body : juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;
        std::function<void()> onResize;
        std::function<void (const juce::MouseEvent&)> onMouseUp, onMouseMove;
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
        void resized() override { if (onResize) onResize(); }
        void mouseUp (const juce::MouseEvent& e) override { if (onMouseUp) onMouseUp (e); }
        void mouseMove (const juce::MouseEvent& e) override { if (onMouseMove) onMouseMove (e); }
    };

    void paintBody (juce::Graphics&);
    void layoutBody();
    void chooseIR (int slot);
    static juce::String titleText() { return juce::String (CTS_PRODUCT_NAME).toUpperCase(); }
    static int titleWidth();                  // "CLEAN TONE STUDIO v1.6.2", as drawn
    juce::Rectangle<int> titleArea() const;   // click the title: About box (license notice)
public:
    static juce::String aboutText();           // copyright, license and no-warranty notice
private:
    void timerCallback() override;
    void refreshPresetBox();
    void updatePresetDisplay();
    void showSaveBar (bool show);
    void commitSave();
    void setStatus (const juce::String& text, juce::Colour colour);

    CleanToneAudioProcessor& proc;
    ui::LookAndFeel lnf;
    Body body;
    juce::Component::SafePointer<juce::DocumentWindow> setUpWindow;
    bool sizeReady = false;

    static constexpr int kUserIdBase = 1000;
    juce::ComboBox presetBox;
    juce::TextButton saveButton { "Save" }, deleteButton { "Delete" };
    juce::TextEditor nameEditor;
    juce::TextButton confirmSaveButton { "Save" }, cancelSaveButton { "Cancel" };
    juce::Label statusLabel;
    juce::StringArray userPresetNames;
    bool overwriteArmed = false;
    int deleteArmTicks = 0, statusTicks = 0, rescanTicks = 0;

    // input
    ui::Knob inGain, gate;
    ui::Choice pickup, hum;
    // dynamics
    ui::Toggle compOn, compLook;
    ui::Knob sustain, attack, compMix, pick;
    ui::Meter grMeter { "GR", true };
    // output
    ui::Knob outGain;
    ui::Toggle autoLevel;
    juce::Label autoLevelReadout;
    ui::Meter inMeter { "IN", false }, outMeter { "OUT", false };
    // tone
    ui::Knob warmth, clarity, bass, mid, midFreq, presence, treble, lowCut, highCut;
    ui::Toggle warmTight;
    ui::Choice cab;
    // impulse-response loader (Cabinet = "IR Loader")
    juce::Label irName[2], irBlendLabel;
    juce::TextButton irLoad[2], irClear[2];
    juce::Slider irBlend;
    ui::Toggle irAlign;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> irBlendAttachment;
    std::unique_ptr<juce::FileChooser> irChooser;
    // delay
    ui::Toggle dlyOn, dlySync;
    ui::Choice dlyDiv, dlyStyle;
    juce::Label dlyStyleLabel;
    // Sync tempo: editable + TAP when the plugin keeps its own tempo (the app), read-only when a DAW sets it
    juce::Slider bpmSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bpmAttachment;
    juce::TextButton tapButton { "Tap" };
    juce::Label hostBpmLabel;
    TapTempo tapper;
    ui::Knob dlyTime, dlyFeedback, dlyTone, dlyMod, dlyDuck, dlyWidth, dlyMix;
    // reverb
    ui::Toggle revOn;
    ui::Knob revSize, revDecay, revPreDelay, revTone, revLowCut, revMix, revDuck;

    struct Section { juce::String title; juce::Rectangle<int> area; juce::Colour colour; };
    std::vector<Section> sections;

    float inDb = -100.0f, outDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CleanToneAudioProcessorEditor)
};
