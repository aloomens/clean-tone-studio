#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    using NRange = juce::NormalisableRange<float>;

    NRange logRange (float lo, float hi)
    {
        NRange r (lo, hi);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    juce::String fmtDb (float v, int)   { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; }
    juce::String fmtPct (float v, int)  { return juce::String (juce::roundToInt (v)) + " %"; }
    juce::String fmtMs (float v, int)   { return v < 100.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; }
    juce::String fmtHz (float v, int)   { return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz" : juce::String (v / 1000.0f, 1) + " kHz"; }
    juce::String fmtSec (float v, int)  { return juce::String (v, 1) + " s"; }

    struct Preset
    {
        const char* name;
        std::vector<std::pair<const char*, float>> values;
    };

    const std::vector<Preset>& getPresets()
    {
        static const std::vector<Preset> presets =
        {
            { "Studio Clean", {} },
            { "Glassy Single Coil", { { IDs::pickup, 0 }, { IDs::sustain, 45 }, { IDs::attack, 20 }, { IDs::warmth, 20 },
                                      { IDs::bass, 1 }, { IDs::mid, -1 }, { IDs::midFreq, 600 }, { IDs::presence, 1.5f },
                                      { IDs::treble, 2 }, { IDs::cab, 1 }, { IDs::revSize, 55 }, { IDs::revDecay, 2.0f },
                                      { IDs::revMix, 18 } } },
            { "Warm Jazz Humbucker", { { IDs::pickup, 1 }, { IDs::sustain, 30 }, { IDs::attack, 25 }, { IDs::warmth, 45 },
                                       { IDs::bass, 2 }, { IDs::mid, 1 }, { IDs::midFreq, 500 }, { IDs::presence, -2 },
                                       { IDs::treble, -4 }, { IDs::highCut, 7000 }, { IDs::cab, 2 }, { IDs::revSize, 40 },
                                       { IDs::revDecay, 1.4f }, { IDs::revMix, 12 } } },
            { "Humbucker Chime", { { IDs::pickup, 1 }, { IDs::sustain, 40 }, { IDs::warmth, 25 }, { IDs::mid, -2 },
                                   { IDs::midFreq, 400 }, { IDs::presence, 2 }, { IDs::treble, 2.5f }, { IDs::cab, 1 },
                                   { IDs::revMix, 16 } } },
            { "Funk Snap", { { IDs::pickup, 0 }, { IDs::gate, 25 }, { IDs::sustain, 60 }, { IDs::attack, 30 },
                             { IDs::compMix, 80 }, { IDs::warmth, 15 }, { IDs::bass, -1 }, { IDs::mid, 2 },
                             { IDs::midFreq, 1200 }, { IDs::presence, 3 }, { IDs::treble, 1 }, { IDs::lowCut, 100 },
                             { IDs::cab, 1 }, { IDs::revSize, 30 }, { IDs::revDecay, 0.8f }, { IDs::revMix, 8 } } },
            { "Endless Sustain", { { IDs::gate, 35 }, { IDs::sustain, 85 }, { IDs::attack, 8 }, { IDs::warmth, 40 },
                                   { IDs::dlyOn, 1 }, { IDs::dlyTime, 450 }, { IDs::dlyFeedback, 35 }, { IDs::dlyMix, 22 },
                                   { IDs::revDecay, 3.0f }, { IDs::revMix, 22 } } },
            { "Ambient Swell", { { IDs::sustain, 65 }, { IDs::warmth, 30 }, { IDs::treble, -1 },
                                 { IDs::dlyOn, 1 }, { IDs::dlySync, 1 }, { IDs::dlyDiv, 4 }, { IDs::dlyFeedback, 55 },
                                 { IDs::dlyMod, 40 }, { IDs::dlyDuck, 50 }, { IDs::dlyWidth, 80 }, { IDs::dlyMix, 35 },
                                 { IDs::revSize, 90 }, { IDs::revDecay, 7 }, { IDs::revPreDelay, 40 },
                                 { IDs::revTone, 5000 }, { IDs::revMix, 40 } } },
            { "DI Clean (No Cab)", { { IDs::cab, 0 }, { IDs::sustain, 30 }, { IDs::warmth, 15 }, { IDs::revOn, 0 },
                                     { IDs::highCut, 16000 } } },
        };
        return presets;
    }

    const float kDivBeats[] = { 2.0f, 1.5f, 1.0f, 2.0f / 3.0f, 0.75f, 0.5f, 1.0f / 3.0f, 0.25f };
}

juce::StringArray CleanToneAudioProcessor::getDelayDivisionNames()
{
    return { "1/2", "1/4 dotted", "1/4", "1/4 triplet", "1/8 dotted", "1/8", "1/8 triplet", "1/16" };
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout CleanToneAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto addFloat = [&] (const char* id, const char* name, NRange r, float def,
                         String (*fmt) (float, int))
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
                        AudioParameterFloatAttributes().withStringFromValueFunction (fmt)));
    };
    auto addChoice = [&] (const char* id, const char* name, StringArray choices, int def)
    {
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, choices, def));
    };
    auto addBool = [&] (const char* id, const char* name, bool def)
    {
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    };

    // Input
    addFloat  (IDs::inGain, "Input", NRange (-24.0f, 24.0f, 0.1f), 0.0f, fmtDb);
    addChoice (IDs::pickup, "Pickup", { "Single Coil", "Humbucker", "Flat" }, 0);
    addChoice (IDs::hum, "Hum Filter", { "Off", "50 Hz", "60 Hz" }, 0);
    addFloat  (IDs::gate, "Noise Gate", NRange (0.0f, 100.0f, 1.0f), 0.0f, fmtPct);

    // Dynamics
    addBool  (IDs::compOn, "Compressor On", true);
    addFloat (IDs::sustain, "Sustain", NRange (0.0f, 100.0f, 1.0f), 35.0f, fmtPct);
    { auto r = NRange (1.0f, 60.0f, 0.1f); r.setSkewForCentre (12.0f);
      addFloat (IDs::attack, "Attack", r, 15.0f, fmtMs); }
    addFloat (IDs::compMix, "Comp Blend", NRange (0.0f, 100.0f, 1.0f), 100.0f, fmtPct);
    addBool  (IDs::compLook, "Lookahead", false);
    addFloat (IDs::pick, "Pick", NRange (-100.0f, 100.0f, 1.0f), 0.0f,
              [] (float v, int) { return (v > 0.5f ? "+" : "") + juce::String (juce::roundToInt (v)) + " %"; });

    // Tone
    addFloat  (IDs::warmth, "Warmth", NRange (0.0f, 100.0f, 1.0f), 30.0f, fmtPct);
    addBool   (IDs::warmTight, "Warmth Tight", false);
    addFloat  (IDs::clarity, "Clarity", NRange (0.0f, 100.0f, 1.0f), 0.0f, fmtPct);
    addFloat  (IDs::bass, "Bass", NRange (-12.0f, 12.0f, 0.1f), 0.0f, fmtDb);
    addFloat  (IDs::mid, "Mid", NRange (-12.0f, 12.0f, 0.1f), 0.0f, fmtDb);
    addFloat  (IDs::midFreq, "Mid Freq", logRange (200.0f, 3000.0f), 750.0f, fmtHz);
    addFloat  (IDs::presence, "Presence", NRange (-12.0f, 12.0f, 0.1f), 0.0f, fmtDb);
    addFloat  (IDs::treble, "Treble", NRange (-12.0f, 12.0f, 0.1f), 0.0f, fmtDb);
    addFloat  (IDs::lowCut, "Low Cut", logRange (20.0f, 400.0f), 70.0f, fmtHz);
    addFloat  (IDs::highCut, "High Cut", logRange (2000.0f, 20000.0f), 12000.0f, fmtHz);
    addChoice (IDs::cab, "Cabinet", { "Off (DI)", "Open 1x12", "Classic 2x12", "Closed 4x12", "IR Loader" }, 1);
    addFloat  (IDs::irBlend, "IR Blend", NRange (0.0f, 100.0f, 1.0f), 50.0f, fmtPct);
    addBool   (IDs::irAlign, "IR Align", true);

    // Delay
    addBool   (IDs::dlyOn, "Delay On", false);
    addBool   (IDs::dlySync, "Delay Sync", false);
    addChoice (IDs::dlyDiv, "Delay Division", getDelayDivisionNames(), 4);
    addFloat  (IDs::dlyTime, "Delay Time", logRange (20.0f, 2000.0f), 380.0f, fmtMs);
    addFloat  (IDs::dlyFeedback, "Feedback", NRange (0.0f, 95.0f, 1.0f), 30.0f, fmtPct);
    addFloat  (IDs::dlyTone, "Delay Tone", logRange (800.0f, 12000.0f), 4000.0f, fmtHz);
    addFloat  (IDs::dlyMod, "Delay Mod", NRange (0.0f, 100.0f, 1.0f), 20.0f, fmtPct);
    addFloat  (IDs::dlyDuck, "Ducking", NRange (0.0f, 100.0f, 1.0f), 30.0f, fmtPct);
    addFloat  (IDs::dlyWidth, "Ping-Pong", NRange (0.0f, 100.0f, 1.0f), 50.0f, fmtPct);
    addFloat  (IDs::dlyMix, "Delay Level", NRange (0.0f, 100.0f, 1.0f), 25.0f, fmtPct);
    addChoice (IDs::dlyStyle, "Delay Mod Style", { "Sine", "Tape" }, 0);
    addFloat  (IDs::dlyBpm, "Delay Tempo", NRange (30.0f, 300.0f, 0.1f), 120.0f,
               [] (float v, int) { return juce::String (v, 1) + " BPM"; });

    // Reverb
    addBool  (IDs::revOn, "Reverb On", true);
    addFloat (IDs::revSize, "Size", NRange (0.0f, 100.0f, 1.0f), 60.0f, fmtPct);
    { auto r = NRange (0.3f, 12.0f, 0.01f); r.setSkewForCentre (2.5f);
      addFloat (IDs::revDecay, "Decay", r, 2.2f, fmtSec); }
    addFloat (IDs::revPreDelay, "Pre-Delay", NRange (0.0f, 150.0f, 0.1f), 20.0f, fmtMs);
    addFloat (IDs::revTone, "Reverb Tone", logRange (1500.0f, 16000.0f), 6500.0f, fmtHz);
    addFloat (IDs::revLowCut, "Reverb Low Cut", logRange (50.0f, 800.0f), 200.0f, fmtHz);
    addFloat (IDs::revMix, "Reverb Level", NRange (0.0f, 100.0f, 1.0f), 18.0f, fmtPct);
    addFloat (IDs::revDuck, "Reverb Ducking", NRange (0.0f, 100.0f, 1.0f), 0.0f, fmtPct);

    // Output
    addFloat (IDs::outGain, "Output", NRange (-24.0f, 12.0f, 0.1f), 0.0f, fmtDb);
    addBool  (IDs::autoLevel, "Auto Level", true);

    return layout;
}

//==============================================================================
CleanToneAudioProcessor::CleanToneAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "CleanToneStudio", createLayout())
{
    for (auto* id : IDs::kAll)
        paramTable.push_back ({ id, apvts.getRawParameterValue (id) });
    jassert ((int) paramTable.size() == getParameters().size());

    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (! isUserTrim (rp->getParameterID()))
                snapshotParams.push_back (apvts.getRawParameterValue (rp->getParameterID()));
    snapshot.resize (snapshotParams.size());
    captureSnapshot();
    apvts.addParameterListener (IDs::compLook, this);
    apvts.addParameterListener (IDs::irAlign, this);
}

CleanToneAudioProcessor::~CleanToneAudioProcessor()
{
    apvts.removeParameterListener (IDs::compLook, this);
    apvts.removeParameterListener (IDs::irAlign, this);
    cancelPendingUpdate();
}

// Lookahead changes the plugin's latency: report the new figure to the host. ALIGN changes how the IRs
// are prepared: rebuild them (they crossfade in). Both on the message thread.
void CleanToneAudioProcessor::parameterChanged (const juce::String& id, float)
{
    if (id == IDs::irAlign) alignDirty = true;
    triggerAsyncUpdate();
}
void CleanToneAudioProcessor::handleAsyncUpdate()
{
    if (prepared) setLatencySamples (measureLatency (sr, param (IDs::compLook) > 0.5f));
    if (alignDirty.exchange (false))
        for (int s = 0; s < 2; ++s)
            if (! irSlots[(size_t) s].samples.empty()) publishIR (s, false);
}

// Projects and presets saved with IRs before ALIGN existed (1.4.0) used the IRs as recorded: keep it that way.
void CleanToneAudioProcessor::keepOldIRTiming (const juce::XmlElement& xml)
{
    if (xml.getChildByName ("IR") == nullptr) return;
    for (auto* e : xml.getChildIterator())
        if (e->getStringAttribute ("id") == IDs::irAlign) return;
    if (auto* p = apvts.getParameter (IDs::irAlign)) p->setValueNotifyingHost (0.0f);
}

std::unique_ptr<ct::IRSpectra> CleanToneAudioProcessor::buildSpectra (const IRSlot& s) const
{
    auto ir = ct::prepareIR (s.samples, s.rate, sr);
    const bool align = param (IDs::irAlign) > 0.5f;
    if (align) ir = ct::minimumPhase (ir);
    // the serial tells Auto Level which IR (and which preparation of it) is playing
    return ct::IRSpectra::build (ir, sr, s.serial * 2 + (align ? 1 : 0));
}

bool CleanToneAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (in  != juce::AudioChannelSet::mono() && in  != juce::AudioChannelSet::stereo()) return false;
    return in.size() <= out.size();
}

void CleanToneAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    maxBlock = std::max (16, samplesPerBlock);

    prepared = true;
    // (re)build the IRs for this sample rate; the audio isn't running, so install them directly
    irStore.clear();
    for (int s = 0; s < 2; ++s)
    {
        irHasPending[(size_t) s] = false;
        irCurrent[(size_t) s] = nullptr;
        if (! irSlots[(size_t) s].samples.empty())
        {
            irStore.push_back (buildSpectra (irSlots[(size_t) s]));
            irCurrent[(size_t) s] = irStore.back().get();
        }
    }
    snapIRPending = false;

    chain.prepare (sr, maxBlock);
    // measures the current settings right away (not on the audio thread)
    matcher.prepare (sr, makeSettings(), irCurrent[0], irCurrent[1]);

    outCoef  = ct::onePoleCoef (12.0, sr);    // two stages: ~30 ms S-shaped glide
    autoCoef = ct::onePoleCoef (60.0, sr);    // Auto Level corrections settle gently (~150 ms)
    outGainT = ct::dbToGain (param (IDs::outGain));
    autoGainT = param (IDs::autoLevel) > 0.5f ? ct::dbToGain (matcher.getCorrectionDb()) : 1.0f;
    outGainG.snap (outGainT);
    autoGainG.snap (autoGainT);

    setLatencySamples (measureLatency (sr, param (IDs::compLook) > 0.5f));
}

// The oversampling filters' delay isn't a whole number of samples, so rather than rounding a figure,
// send an impulse through a neutral copy of the chain and report exactly where it comes out.
int CleanToneAudioProcessor::measureLatency (double sampleRate, bool lookahead)
{
    ct::Chain probe;
    probe.prepare (sampleRate, 256);
    ct::Settings s;
    s.pickup = 2; s.compOn = 0; s.warmth = 0; s.bass = s.mid = s.presence = s.treble = 0;
    s.lowCut = 20; s.highCut = 20000; s.cab = 0; s.gate = 0; s.hum = 0; s.dlyOn = 0; s.revOn = 0;
    s.compLook = lookahead ? 1.0f : 0.0f;
    std::vector<float> L (256, 0.0f), R (256, 0.0f);
    constexpr int at = 16;
    L[at] = 1.0f;
    float* ch[2] = { L.data(), R.data() };
    probe.apply (s);
    probe.process (ch, 2, 1, 256);
    int peak = at;
    for (int i = at; i < 256; ++i)
        if (std::abs (L[(size_t) i]) > std::abs (L[(size_t) peak])) peak = i;
    return peak - at;
}

ct::Settings CleanToneAudioProcessor::makeSettings() const
{
    ct::Settings s;
    s.inGain = param (IDs::inGain);   s.pickup = param (IDs::pickup); s.hum = param (IDs::hum);
    s.gate = param (IDs::gate);       s.compOn = param (IDs::compOn);       s.sustain = param (IDs::sustain); s.attack = param (IDs::attack);
    s.compMix = param (IDs::compMix); s.warmth = param (IDs::warmth); s.bass = param (IDs::bass);
    s.mid = param (IDs::mid);         s.midFreq = param (IDs::midFreq); s.presence = param (IDs::presence);
    s.treble = param (IDs::treble);   s.lowCut = param (IDs::lowCut); s.highCut = param (IDs::highCut);
    s.cab = param (IDs::cab);

    s.dlyOn = param (IDs::dlyOn);
    float timeMs = param (IDs::dlyTime);
    if (param (IDs::dlySync) > 0.5f)
    {
        const int div = std::clamp ((int) param (IDs::dlyDiv), 0, 7);
        timeMs = (float) (kDivBeats[div] * 60000.0 / getSyncBpm());
    }
    s.dlyTimeMs = std::min (timeMs, 2900.0f);
    s.dlyFeedback = param (IDs::dlyFeedback); s.dlyTone = param (IDs::dlyTone); s.dlyMod = param (IDs::dlyMod);
    s.dlyDuck = param (IDs::dlyDuck); s.dlyWidth = param (IDs::dlyWidth); s.dlyMix = param (IDs::dlyMix);

    s.revOn = param (IDs::revOn); s.revSize = param (IDs::revSize); s.revDecay = param (IDs::revDecay);
    s.revPreDelay = param (IDs::revPreDelay); s.revTone = param (IDs::revTone);
    s.revLowCut = param (IDs::revLowCut); s.revMix = param (IDs::revMix);
    s.irBlend = param (IDs::irBlend); s.compLook = param (IDs::compLook); s.dlyStyle = param (IDs::dlyStyle);
    s.clarity = param (IDs::clarity); s.pick = param (IDs::pick); s.warmTight = param (IDs::warmTight); s.revDuck = param (IDs::revDuck);
    s.irA = irCurrent[0] != nullptr ? (float) irCurrent[0]->serial : 0.0f;
    s.irB = irCurrent[1] != nullptr ? (float) irCurrent[1]->serial : 0.0f;
    return s;
}

void CleanToneAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = std::min (getTotalNumOutputChannels(), buffer.getNumChannels());
    const int n      = buffer.getNumSamples();
    for (int c = numIn; c < numOut; ++c) buffer.clear (c, 0, n);
    if (numOut <= 0 || n <= 0) return;

    const int outCh  = std::min (numOut, 2);
    int mainCh = std::clamp (std::min (numIn, outCh), 1, 2);

    // Standalone app: an interface delivers inputs as a pair but the guitar is plugged into
    // just one of them, so fold to mono (guitar centred, stereo delay/reverb around it).
    if (wrapperType == wrapperType_Standalone && mainCh == 2)
    {
        buffer.addFrom (0, 0, buffer, 1, 0, n);
        mainCh = 1;
    }

    // the host's tempo, if it sends one (the standalone app has none: the Tempo control is used)
    bool hostTempo = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
            {
                hostBpm.store (std::clamp (*b, 30.0, 300.0));
                hostTempo = true;
            }
    hostHasTempo.store (hostTempo);

    float inMax = 0.0f;
    for (int c = 0; c < mainCh; ++c)
        inMax = std::max (inMax, buffer.getMagnitude (c, 0, n));

    // Auto Level runs in audio time on this thread. Its correction depends on the settings only, and
    // it reports any new correction at the exact sample where it takes effect.
    // newly loaded impulse responses take over at the start of a block (pointer swap, no allocation)
    for (int s = 0; s < 2; ++s)
        if (irHasPending[(size_t) s].exchange (false))
            irCurrent[(size_t) s] = irPending[(size_t) s].load();
    chain.setIRs (irCurrent[0], irCurrent[1], snapIRPending.exchange (false));

    const auto settings = makeSettings();
    const bool autoOn = param (IDs::autoLevel) > 0.5f;
    const bool justLoaded = snapAutoGainPending.exchange (false);
    float compDb = autoOn ? matcher.getCorrectionDb() : 0.0f;          // the correction in force at sample 0
    ct::LevelMatcher::Event events[ct::LevelMatcher::kMaxEvents];
    const int numEvents = matcher.process (n, settings, events, irCurrent[0], irCurrent[1]);
    if (justLoaded)                                                     // a session was loaded: start at its level
        compDb = autoOn ? matcher.getCorrectionDb() : 0.0f;
    autoGainT = ct::dbToGain (compDb);
    outGainT = ct::dbToGain (param (IDs::outGain));
    if (justLoaded) autoGainG.snap (autoGainT);

    int done = 0, ev = 0;
    while (done < n)
    {
        for (; ev < numEvents && events[ev].at <= done; ++ev)
            if (autoOn) { compDb = events[ev].db; autoGainT = ct::dbToGain (compDb); }
        int len = std::min (maxBlock, n - done);
        if (ev < numEvents) len = std::min (len, events[ev].at - done);
        float* chans[2] = { buffer.getWritePointer (0, done), outCh > 1 ? buffer.getWritePointer (1, done) : nullptr };
        processChunk (chans, outCh, mainCh, len, settings);
        done += len;
    }

    float outMax = 0.0f;
    for (int c = 0; c < outCh; ++c) outMax = std::max (outMax, buffer.getMagnitude (c, 0, n));

    auto storeMax = [] (std::atomic<float>& a, float v)
    {
        float prev = a.load();
        while (v > prev && ! a.compare_exchange_weak (prev, v)) {}
    };
    storeMax (inPeak, inMax);
    storeMax (outPeak, outMax);
    gainReductionDb.store (chain.getGainReductionDb());
    autoGainDb.store (compDb);
}

void CleanToneAudioProcessor::processChunk (float* const* ch, int numOutCh, int mainCh, int n, const ct::Settings& settings)
{
    chain.apply (settings);
    chain.process (ch, numOutCh, mainCh, n);

    for (int i = 0; i < n; ++i)
    {
        const float g = outGainG.next (outGainT, outCoef) * autoGainG.next (autoGainT, autoCoef);
        for (int c = 0; c < numOutCh; ++c) ch[c][i] *= g;
    }
}
//==============================================================================
int CleanToneAudioProcessor::getNumPrograms() { return (int) getPresets().size(); }

const juce::String CleanToneAudioProcessor::getProgramName (int index)
{
    const auto& p = getPresets();
    return juce::isPositiveAndBelow (index, (int) p.size()) ? juce::String (p[(size_t) index].name) : juce::String();
}

void CleanToneAudioProcessor::setCurrentProgram (int index)
{
    // Hosts (Cubase among them) re-send the current program number after loading a project.
    // Re-applying it would reset every knob you tweaked to the factory preset, so ignore it.
    // Picking a preset in the plugin's own menu uses loadFactoryPreset(), which always applies.
    if (index == currentProgram) return;
    loadFactoryPreset (index);
}

void CleanToneAudioProcessor::loadFactoryPreset (int index)
{
    const auto& presets = getPresets();
    if (! juce::isPositiveAndBelow (index, (int) presets.size())) return;
    currentProgram = index;
    currentPresetIsUser = false;

    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (! isUserTrim (rp->getParameterID()))  // Output / Auto Level / Compressor On are the user's own setup
                rp->setValueNotifyingHost (rp->getDefaultValue());

    for (const auto& [id, value] : presets[(size_t) index].values)
        if (auto* rp = apvts.getParameter (id))
            rp->setValueNotifyingHost (rp->convertTo0to1 (value));

    captureSnapshot();
}

//==============================================================================
namespace { juce::File presetFolderOverride; }

juce::File CleanToneAudioProcessor::getUserPresetFolder()
{
    if (presetFolderOverride != juce::File())
        return presetFolderOverride;
    return presetFolderIn (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
}

juce::File CleanToneAudioProcessor::presetFolderIn (const juce::File& docs)
{
    const auto folder = docs.getChildFile (CTS_PRODUCT_NAME).getChildFile ("Presets");
    // 1.6.0 was called "Open Clean Tone Studio" and kept presets in its own folder. Once, bring any preset saved
    // there back (never replacing one that is already here); the 1.6.0 folder is left as it was.
    const auto interim = docs.getChildFile ("Open Clean Tone Studio").getChildFile ("Presets");
    const auto marker = folder.getChildFile (".merged-from-1.6.0");
    if (interim.isDirectory() && interim != folder && ! marker.existsAsFile() && folder.createDirectory().wasOk())
    {
        for (const auto& f : interim.findChildFiles (juce::File::findFiles, false, "*.ctpreset"))
            if (! folder.getChildFile (f.getFileName()).exists())
                f.copyFileTo (folder.getChildFile (f.getFileName()));
        marker.create();
    }
    return folder;
}

void CleanToneAudioProcessor::setUserPresetFolderOverride (const juce::File& folder) { presetFolderOverride = folder; }

static constexpr const char* kPresetExt = ".ctpreset";

juce::File CleanToneAudioProcessor::presetFile (const juce::String& name) const
{
    return getUserPresetFolder().getChildFile (juce::File::createLegalFileName (name.trim()) + kPresetExt);
}

juce::StringArray CleanToneAudioProcessor::getUserPresetNames() const
{
    juce::StringArray names;
    for (const auto& f : getUserPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kPresetExt))
        names.add (f.getFileNameWithoutExtension());
    names.sortNatural();
    return names;
}

bool CleanToneAudioProcessor::userPresetExists (const juce::String& name) const
{
    return presetFile (name).existsAsFile();
}

bool CleanToneAudioProcessor::saveUserPreset (const juce::String& rawName)
{
    const auto name = juce::File::createLegalFileName (rawName.trim());
    if (name.isEmpty()) return false;

    juce::XmlElement xml ("CleanTonePreset");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("name", name);
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (! isUserTrim (rp->getParameterID()))
            {
                auto* e = xml.createNewChildElement ("PARAM");
                e->setAttribute ("id", rp->getParameterID());
                e->setAttribute ("value", (double) rp->convertFrom0to1 (rp->getValue()));
            }

    writeIRs (xml);                       // the impulse responses travel with the preset

    auto file = presetFile (name);
    if (! file.getParentDirectory().createDirectory().wasOk()) return false;
    if (! xml.writeTo (file)) return false;

    userPresetName = name;
    currentPresetIsUser = true;
    captureSnapshot();
    return true;
}

bool CleanToneAudioProcessor::loadUserPreset (const juce::String& name)
{
    auto xml = juce::XmlDocument::parse (presetFile (name));
    if (xml == nullptr || ! xml->hasTagName ("CleanTonePreset")) return false;

    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto id = rp->getParameterID();
            if (isUserTrim (id)) continue;
            float norm = rp->getDefaultValue();       // params missing from older presets -> default
            for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
                if (e->getStringAttribute ("id") == id)
                    norm = rp->convertTo0to1 ((float) e->getDoubleAttribute ("value"));
            rp->setValueNotifyingHost (norm);
        }
    keepOldIRTiming (*xml);
    readIRs (*xml, false);                // presets saved with IRs bring them back (crossfaded in)

    userPresetName = presetFile (name).getFileNameWithoutExtension();
    currentPresetIsUser = true;
    captureSnapshot();
    return true;
}

bool CleanToneAudioProcessor::deleteUserPreset (const juce::String& name)
{
    auto file = presetFile (name);
    if (! file.existsAsFile()) return false;
    const bool testing = presetFolderOverride != juce::File();      // tests: don't fill the Recycle Bin
    // the knobs stay as they are; the editor shows the name marked "(deleted)" until another preset is picked
    return testing ? file.deleteFile() : (file.moveToTrash() || file.deleteFile());
}

juce::String CleanToneAudioProcessor::getCurrentPresetName()
{
    return currentPresetIsUser ? userPresetName : getProgramName (currentProgram);
}

void CleanToneAudioProcessor::captureSnapshot()
{
    const juce::SpinLock::ScopedLockType sl (snapshotLock);
    for (size_t i = 0; i < snapshotParams.size(); ++i)
        snapshot[i] = snapshotParams[i]->load();
}

bool CleanToneAudioProcessor::isPresetModified() const
{
    const juce::SpinLock::ScopedLockType sl (snapshotLock);
    for (size_t i = 0; i < snapshotParams.size(); ++i)
        if (std::abs (snapshot[i] - snapshotParams[i]->load()) > 1.0e-4f * std::max (1.0f, std::abs (snapshot[i])))
            return true;
    return false;
}

//==============================================================================
void CleanToneAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("version", CTS_VERSION, nullptr);
    // also store each parameter's exact 0..1 position: converting the real value back on load isn't
    // bit-exact in floating point (e.g. 30 % came back as 30.0000019), which is enough to change the sound
    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto child = state.getChild (i);
        if (auto* rp = apvts.getParameter (child.getProperty ("id").toString()))
            child.setProperty ("norm", (double) rp->getValue(), nullptr);
    }
    state.setProperty ("program", currentProgram, nullptr);
    state.setProperty ("userPreset", currentPresetIsUser ? userPresetName : juce::String(), nullptr);
    if (auto xml = state.createXml())
    {
        writeIRs (*xml);                  // the impulse responses travel with the project
        copyXmlToBinary (*xml, destData);
    }
}

void CleanToneAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentProgram = tree.getProperty ("program", 0);
            userPresetName = tree.getProperty ("userPreset", juce::String()).toString();
            currentPresetIsUser = userPresetName.isNotEmpty();
            // (1.1 / 1.2 sessions also stored Auto Level's learned playing level; it's no longer used)
            for (auto* prop : { "version", "program", "userPreset", "autoLevelInputDb", "autoLevelTracked", "autoLevelWantedDb", "autoLevelMeasuredAtDb",
                                "autoLevelCorrectionDb", "autoLevelWinSum", "autoLevelWinCount" })
                tree.removeProperty (prop, nullptr);
            for (int i = tree.getNumChildren(); --i >= 0;)
                if (tree.getChild (i).hasType ("IR")) tree.removeChild (i, nullptr);
            apvts.replaceState (tree);
            for (int i = 0; i < tree.getNumChildren(); ++i)
            {
                const auto child = tree.getChild (i);
                if (child.hasProperty ("norm"))
                    if (auto* rp = apvts.getParameter (child.getProperty ("id").toString()))
                    {
                        // the exact 0..1 position - but only if it still means the same value (a choice list
                        // that grew, like Cabinet in 1.4, moves the positions; then the value itself is used)
                        const double norm = child.getProperty ("norm");
                        const float value = child.getProperty ("value");
                        if (std::abs (rp->convertFrom0to1 ((float) norm) - value) <= 1.0e-3f * std::max (1.0f, std::abs (value)))
                            rp->setValueNotifyingHost ((float) norm);
                    }
            }
            keepOldIRTiming (*xml);
            if (! readIRs (*xml, true))   // sessions from before 1.4 have no IRs: empty slots
            {
                irSlots = {};
                publishIR (0, true);
                publishIR (1, true);
            }
            captureSnapshot();
            matcher.requestImmediate();     // the next block measures the loaded settings before any audio,
            snapAutoGainPending = true;     // and starts straight at that level
        }
}

//==============================================================================
juce::String CleanToneAudioProcessor::loadIRFile (int slot, const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    if (r == nullptr) return "Couldn't read " + file.getFileName() + " (use a WAV, AIFF or FLAC file)";
    if (r->sampleRate <= 0.0 || r->lengthInSamples <= 0 || r->numChannels == 0) return file.getFileName() + " is empty";
    const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (2.0 * r->sampleRate));
    juce::AudioBuffer<float> b ((int) r->numChannels, len);
    r->read (&b, 0, len, 0, true, true);
    std::vector<float> mono ((size_t) len, 0.0f);
    for (int c = 0; c < b.getNumChannels(); ++c)                 // stereo IRs: use the average
        for (int i = 0; i < len; ++i) mono[(size_t) i] += b.getSample (c, i) / (float) b.getNumChannels();
    float peak = 0.0f;
    for (float v : mono) peak = std::max (peak, std::abs (v));
    if (peak <= 0.0f) return file.getFileName() + " is silent";
    setIRSamples (slot, file.getFileNameWithoutExtension(), r->sampleRate, std::move (mono));
    return {};
}

void CleanToneAudioProcessor::setIRSamples (int slot, const juce::String& name, double rate, std::vector<float> samples)
{
    if (! juce::isPositiveAndBelow (slot, 2)) return;
    auto& s = irSlots[(size_t) slot];
    s.name = name; s.rate = rate; s.samples = std::move (samples); s.serial = nextIRSerial++;
    publishIR (slot, false);
}

void CleanToneAudioProcessor::clearIR (int slot)
{
    if (! juce::isPositiveAndBelow (slot, 2)) return;
    irSlots[(size_t) slot] = {};
    publishIR (slot, false);
}

juce::String CleanToneAudioProcessor::getIRName (int slot) const
{
    return juce::isPositiveAndBelow (slot, 2) ? irSlots[(size_t) slot].name : juce::String();
}

bool CleanToneAudioProcessor::hasIR (int slot) const
{
    return juce::isPositiveAndBelow (slot, 2) && ! irSlots[(size_t) slot].samples.empty();
}

// Builds the IR for the current sample rate (here, on the message thread) and hands it to the audio thread.
void CleanToneAudioProcessor::publishIR (int slot, bool snap)
{
    const auto& s = irSlots[(size_t) slot];
    const ct::IRSpectra* built = nullptr;
    if (! s.samples.empty())
    {
        irStore.push_back (buildSpectra (s));
        built = irStore.back().get();
    }
    // keep memory bounded: drop spectra that are many loads old (long out of use by then)
    while (irStore.size() > 12)
    {
        auto it = std::find_if (irStore.begin(), irStore.end(), [&] (const auto& e)
                                { return e.get() != built && e.get() != irCurrent[0] && e.get() != irCurrent[1]
                                      && e.get() != irPending[0].load() && e.get() != irPending[1].load(); });
        if (it == irStore.end()) break;
        irStore.erase (it);
    }
    if (! prepared)
        irCurrent[(size_t) slot] = built;      // not playing: install directly (prepare rebuilds anyway)
    else
    {
        irPending[(size_t) slot] = built;
        irHasPending[(size_t) slot] = true;
        if (snap) snapIRPending = true;
    }
}

void CleanToneAudioProcessor::writeIRs (juce::XmlElement& xml) const
{
    for (int slot = 0; slot < 2; ++slot)
    {
        const auto& s = irSlots[(size_t) slot];
        if (s.samples.empty()) continue;
        auto* e = xml.createNewChildElement ("IR");
        e->setAttribute ("slot", slot);
        e->setAttribute ("name", s.name);
        e->setAttribute ("rate", s.rate);
        e->setAttribute ("data", juce::MemoryBlock (s.samples.data(), s.samples.size() * sizeof (float)).toBase64Encoding());
    }
}

void CleanToneAudioProcessor::writeIRs (juce::ValueTree& state) const
{
    juce::XmlElement tmp ("tmp");
    writeIRs (tmp);
    for (auto* e : tmp.getChildIterator()) state.appendChild (juce::ValueTree::fromXml (*e), nullptr);
}

bool CleanToneAudioProcessor::readIRs (const juce::XmlElement& xml, bool snap)
{
    if (xml.getChildByName ("IR") == nullptr) return false;
    std::array<IRSlot, 2> loaded;
    for (auto* e : xml.getChildWithTagNameIterator ("IR"))
    {
        const int slot = e->getIntAttribute ("slot", -1);
        if (! juce::isPositiveAndBelow (slot, 2)) continue;
        juce::MemoryBlock mb;
        if (! mb.fromBase64Encoding (e->getStringAttribute ("data"))) continue;
        auto& s = loaded[(size_t) slot];
        s.name = e->getStringAttribute ("name");
        s.rate = e->getDoubleAttribute ("rate", 48000.0);
        s.samples.resize (mb.getSize() / sizeof (float));
        std::memcpy (s.samples.data(), mb.getData(), s.samples.size() * sizeof (float));
    }
    for (int slot = 0; slot < 2; ++slot)
    {
        auto& s = irSlots[(size_t) slot];
        s = std::move (loaded[(size_t) slot]);
        s.serial = s.samples.empty() ? 0 : nextIRSerial++;
        publishIR (slot, snap);
    }
    return true;
}

juce::AudioProcessorEditor* CleanToneAudioProcessor::createEditor()
{
    return new CleanToneAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CleanToneAudioProcessor();
}
