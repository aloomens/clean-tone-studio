#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Chain.h"

namespace IDs
{
    #define CT_PARAM(name) inline constexpr const char* name = #name;
    CT_PARAM (inGain)  CT_PARAM (pickup)  CT_PARAM (hum)      CT_PARAM (gate)
    CT_PARAM (compOn)  CT_PARAM (sustain) CT_PARAM (attack)  CT_PARAM (compMix)
    CT_PARAM (warmth)  CT_PARAM (bass)    CT_PARAM (mid)      CT_PARAM (midFreq)
    CT_PARAM (presence) CT_PARAM (treble) CT_PARAM (lowCut)   CT_PARAM (highCut) CT_PARAM (cab)
    CT_PARAM (dlyOn)   CT_PARAM (dlySync) CT_PARAM (dlyDiv)   CT_PARAM (dlyTime)
    CT_PARAM (dlyFeedback) CT_PARAM (dlyTone) CT_PARAM (dlyMod) CT_PARAM (dlyDuck)
    CT_PARAM (dlyWidth) CT_PARAM (dlyMix)
    CT_PARAM (revOn)   CT_PARAM (revSize) CT_PARAM (revDecay) CT_PARAM (revPreDelay)
    CT_PARAM (revTone) CT_PARAM (revLowCut) CT_PARAM (revMix)
    CT_PARAM (outGain) CT_PARAM (autoLevel)
    CT_PARAM (irBlend) CT_PARAM (compLook) CT_PARAM (dlyStyle) CT_PARAM (irAlign) CT_PARAM (dlyBpm)
    CT_PARAM (clarity) CT_PARAM (pick) CT_PARAM (warmTight) CT_PARAM (revDuck)
    #undef CT_PARAM

    // every parameter ID, so the audio thread can look them up by pointer (no strings, no allocation)
    inline constexpr const char* kAll[] = { inGain, pickup, hum, gate, compOn, sustain, attack, compMix,
        warmth, bass, mid, midFreq, presence, treble, lowCut, highCut, cab, dlyOn, dlySync, dlyDiv, dlyTime,
        dlyFeedback, dlyTone, dlyMod, dlyDuck, dlyWidth, dlyMix, revOn, revSize, revDecay, revPreDelay,
        revTone, revLowCut, revMix, outGain, autoLevel, irBlend, compLook, dlyStyle, irAlign, dlyBpm, clarity, pick, warmTight, revDuck };
}

class CleanToneAudioProcessor : public juce::AudioProcessor,
                                private juce::AudioProcessorValueTreeState::Listener,
                                private juce::AsyncUpdater
{
public:
    CleanToneAudioProcessor();
    ~CleanToneAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { prepared = false; }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return CTS_PRODUCT_NAME; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 12.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;     // host-facing: ignores a re-send of the current program
    void loadFactoryPreset (int index);               // the plugin's own menu: always applies
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // meters (read by the editor)
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f }, gainReductionDb { 0.0f }, autoGainDb { 0.0f };

    static juce::StringArray getDelayDivisionNames();

    // Auto Level status (also used by tests)
    float getAutoLevelCorrectionDb() const { return matcher.getCorrectionDb(); }
    bool isAutoLevelSettled() { return matcher.isSettled (makeSettings()); }

    //==============================================================================
    // User presets (call from the message thread). Stored as small XML files in
    // Documents\<product name>\Presets, shared by the app and the VST3 (presets saved under the old
    // "Clean Tone Studio" name are copied over the first time).
    // Like factory presets, they never change Output, Auto Level or the compressor's On switch.
    static juce::File getUserPresetFolder();
    static juce::File presetFolderIn (const juce::File& documentsFolder);   // (also brings back 1.6.0 presets, once)
    static void setUserPresetFolderOverride (const juce::File& folder);   // for tests
    juce::StringArray getUserPresetNames() const;
    bool userPresetExists (const juce::String& name) const;
    bool saveUserPreset (const juce::String& name);
    bool loadUserPreset (const juce::String& name);
    bool deleteUserPreset (const juce::String& name);       // moves the file to the Recycle Bin

    juce::String getCurrentPresetName();
    bool isCurrentPresetUser() const { return currentPresetIsUser.load(); }
    bool isPresetModified() const;                           // knobs moved since the preset was loaded

    //==============================================================================
    // Impulse responses for the "IR Loader" cabinet (message thread). Two slots, A (0) and B (1).
    // The IR's samples are stored inside the project and in user presets, so they travel with them.
    juce::String loadIRFile (int slot, const juce::File& file);   // returns an error message, or empty
    void setIRSamples (int slot, const juce::String& name, double rate, std::vector<float> samples);
    void clearIR (int slot);
    juce::String getIRName (int slot) const;
    bool hasIR (int slot) const;
    static juce::String getIRFileWildcard() { return "*.wav;*.aif;*.aiff;*.flac"; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    ct::Settings makeSettings() const;
    static int measureLatency (double sampleRate, bool lookahead);
    void parameterChanged (const juce::String& id, float) override;
    void handleAsyncUpdate() override;
    void publishIR (int slot, bool snap);
    void writeIRs (juce::ValueTree& state) const;
    void writeIRs (juce::XmlElement& xml) const;
    bool readIRs (const juce::XmlElement& xml, bool snap);
    void processChunk (float* const* chans, int numOutCh, int mainCh, int n, const ct::Settings& settings);
    // Real-time safe: IDs:: constants match by pointer; anything else falls back to a string compare.
    float param (const char* id) const
    {
        for (const auto& e : paramTable) if (e.id == id) return e.value->load();
        for (const auto& e : paramTable) if (std::strcmp (e.id, id) == 0) return e.value->load();
        jassertfalse;
        return 0.0f;
    }
    struct ParamEntry { const char* id; std::atomic<float>* value; };
    // the user's own setup rather than the sound: presets leave these alone
    static bool isUserTrim (const juce::String& id) { return id == IDs::outGain || id == IDs::autoLevel || id == IDs::compOn || id == IDs::compLook || id == IDs::dlyBpm; }
    juce::File presetFile (const juce::String& name) const;
    void captureSnapshot();

    std::vector<std::atomic<float>*> snapshotParams;    // every param except the user trims
    std::vector<float> snapshot;
    mutable juce::SpinLock snapshotLock;
    juce::String userPresetName;                         // message thread only
    std::atomic<bool> currentPresetIsUser { false };

    std::vector<ParamEntry> paramTable;             // filled once in the constructor

    std::atomic<double> hostBpm { 120.0 };
    std::atomic<bool> hostHasTempo { false };     // the host sent a tempo with the last block
public:
    // Delay Sync tempo: the host's when it sends one (a DAW), otherwise the plugin's own Tempo (the app)
    bool isUsingHostTempo() const { return hostHasTempo.load(); }
    double getHostBpm() const     { return hostBpm.load(); }
    double getSyncBpm() const     { return hostHasTempo.load() ? hostBpm.load() : (double) param (IDs::dlyBpm); }
private:
    ct::Chain chain;
    ct::LevelMatcher matcher;
    std::atomic<bool> snapAutoGainPending { false };
    ct::Glide outGainG, autoGainG;                 // S-shaped gain glides (no corners = no clicks)
    float outGainT = 1.0f, autoGainT = 1.0f, outCoef = 0.01f, autoCoef = 0.001f;
    double sr = 48000.0;
    int maxBlock = 512;
    int currentProgram = 0;

    // IRs: the message thread owns the samples and the pre-transformed spectra; the audio thread
    // takes over a new spectrum through 'pending' at the start of a block.
    struct IRSlot { juce::String name; double rate = 0.0; std::vector<float> samples; int serial = 0; };
    std::array<IRSlot, 2> irSlots;
    std::unique_ptr<ct::IRSpectra> buildSpectra (const IRSlot&) const;   // for the current rate and ALIGN setting
    std::atomic<bool> alignDirty { false };
    void keepOldIRTiming (const juce::XmlElement& stateOrPreset);
    int nextIRSerial = 1;
    std::vector<std::unique_ptr<ct::IRSpectra>> irStore;      // keeps recent spectra alive
    std::array<std::atomic<const ct::IRSpectra*>, 2> irPending { nullptr, nullptr };
    std::array<std::atomic<bool>, 2> irHasPending { false, false };
    std::atomic<bool> snapIRPending { false };
    std::array<const ct::IRSpectra*, 2> irCurrent { nullptr, nullptr };   // audio thread
    bool prepared = false;
public:
    int editorWidth = 0, editorHeight = 0;     // last editor size (not saved; this session only)
    bool isStandaloneApp() const { return wrapperType == wrapperType_Standalone; }
private:

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CleanToneAudioProcessor)
};
