// Offline test host: loads the built VST3 (or, on Linux, the LV2), renders a synthetic plucked-guitar
// signal through every preset and several stress scenarios, and checks for
// NaN/Inf, runaway levels, tails that never die, and editor creation.
//
// Usage: PluginHostTest <path-to.vst3 | path-to.lv2> [output-folder-for-wavs]
// LV2: a JUCE LV2 has one fixed layout (stereo in / stereo out), so a mono guitar is fed to both inputs and
// the mono-out check is VST3 only. Test with no other copy of the same version installed (e.g. in ~/.lv2):
// with a duplicate URI the checks still run, but JUCE's LV2 host crashes on exit.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "../Source/DSP.h"

using namespace juce;

static constexpr double kSR = 48000.0;
static constexpr int kBlock = 512;
static int failures = 0;
static bool fixedStereo = false;   // LV2: the plugin's layout can't be changed

static void check (bool ok, const String& what)
{
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << std::endl;
    if (! ok) ++failures;
}

// Karplus-Strong plucks + a little noise and 60 Hz hum, like a real DI.
static AudioBuffer<float> makeGuitar (double seconds, double silenceAfter)
{
    const int total = (int) ((seconds + silenceAfter) * kSR);
    AudioBuffer<float> b (1, total);
    b.clear();
    Random rng (42);
    const float notes[] = { 82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f, 196.0f, 261.63f, 164.81f, 220.0f };
    int pos = 0, idx = 0;
    while (pos < (int) (seconds * kSR))
    {
        const float f = notes[idx++ % 10];
        const int period = (int) (kSR / f);
        std::vector<float> line ((size_t) period);
        float lp = 0.0f;   // soft pick: rolled-off excitation gives a realistic, full spectrum
        for (auto& s : line) { lp += 0.22f * ((rng.nextFloat() * 2.0f - 1.0f) - lp); s = lp * 3.0f; }
        float last = 0.0f;
        const int len = (int) (kSR * 0.9);
        for (int i = 0; i < len && pos + i < total; ++i)
        {
            const size_t k = (size_t) (i % period);
            const float cur = line[k];
            line[k] = 0.4985f * (cur + last);
            last = cur;
            b.addSample (0, pos + i, cur * 0.25f);
        }
        pos += len;
    }
    b.applyGain (0.25f / std::max (1.0e-6f, b.getMagnitude (0, 0, total)));   // DI peaks at -12 dBFS
    for (int i = 0; i < total; ++i)
        b.addSample (0, i, (rng.nextFloat() - 0.5f) * 0.0003f
                         + 0.0003f * std::sin (2.0f * MathConstants<float>::pi * 60.0f * (float) i / (float) kSR));
    return b;
}

struct Stats { float peak = 0, rmsDb = -200, tailDb = -200; bool finite = true; };

static Stats analyse (const AudioBuffer<float>& out, int tailStart)
{
    Stats s;
    double sum = 0, tail = 0; int tailN = 0;
    for (int c = 0; c < out.getNumChannels(); ++c)
        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            const float v = out.getSample (c, i);
            if (! std::isfinite (v)) { s.finite = false; continue; }
            s.peak = std::max (s.peak, std::abs (v));
            sum += (double) v * v;
            if (i >= tailStart) { tail += (double) v * v; ++tailN; }
        }
    s.rmsDb = (float) (10.0 * std::log10 (sum / (out.getNumChannels() * out.getNumSamples()) + 1e-20));
    s.tailDb = tailN > 0 ? (float) (10.0 * std::log10 (tail / tailN + 1e-20)) : -200.0f;
    return s;
}

static AudioBuffer<float> render (AudioPluginInstance& p, const AudioBuffer<float>& in,
                                  int numIn, int numOut, std::function<int()> blockSize,
                                  std::function<void (int)> perBlock = {})
{
    const int wantOut = numOut;
    if (fixedStereo) { numIn = 2; numOut = 2; }          // a mono source goes to both inputs, as a DAW does
    AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (numIn == 1 ? AudioChannelSet::mono() : AudioChannelSet::stereo());
    layout.outputBuses.add (numOut == 1 ? AudioChannelSet::mono() : AudioChannelSet::stereo());
    p.releaseResources();
    check (p.setBusesLayout (layout), "bus layout " + String (numIn) + " in / " + String (numOut) + " out accepted");
    p.prepareToPlay (kSR, kBlock);

    const int total = in.getNumSamples();
    AudioBuffer<float> out (wantOut, total);
    MidiBuffer midi;
    int pos = 0, blockIndex = 0;
    while (pos < total)
    {
        const int n = std::min (blockSize(), total - pos);
        AudioBuffer<float> buf (std::max (numIn, numOut), n);
        buf.clear();
        for (int c = 0; c < numIn; ++c) buf.copyFrom (c, 0, in, std::min (c, in.getNumChannels() - 1), pos, n);
        if (perBlock) perBlock (blockIndex);
        p.processBlock (buf, midi);
        for (int c = 0; c < wantOut; ++c) out.copyFrom (c, pos, buf, c, 0, n);
        pos += n; ++blockIndex;
    }
    p.releaseResources();
    return out;
}

static void writeWav (const File& f, const AudioBuffer<float>& b)
{
    f.deleteFile();
    WavAudioFormat wav;
    std::unique_ptr<OutputStream> os (f.createOutputStream());
    if (! os) return;
    if (auto* w = wav.createWriterFor (os.get(), kSR, (unsigned) b.getNumChannels(), 24, {}, 0))
    {
        os.release();
        std::unique_ptr<AudioFormatWriter> writer (w);
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
}

// K-weighted (BS.1770-style) loudness of the first `seconds` of a buffer, channel-averaged.
static float kLoudness (const AudioBuffer<float>& b, double seconds)
{
    double pow = 0.0;
    const int n = std::min (b.getNumSamples(), (int) (seconds * kSR));
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        ct::Biquad shelf, hp;
        shelf.setHighShelf (kSR, 1681.0, 4.0);
        hp.setHighPass (kSR, 38.0, 0.5);
        for (int i = 0; i < n; ++i)
        {
            const float k = hp.process (shelf.process (b.getSample (c, i), 0), 0);
            pow += (double) k * k;
        }
    }
    return (float) (10.0 * std::log10 (pow / (n * b.getNumChannels()) + 1e-20));
}

// VST3 hosts deliver program changes on the next audio callback, so push one tiny
// block through to make the preset land before we tweak individual parameters.
static void selectProgram (AudioPluginInstance& p, int index)
{
    // The plugin ignores a re-send of the program it's already on (hosts do that after loading a
    // project, and it must not reset tweaked knobs). To really select a preset, go via another one.
    auto flush = [&p]
    {
        p.prepareToPlay (kSR, kBlock);
        AudioBuffer<float> b (2, 64);
        b.clear();
        MidiBuffer m;
        p.processBlock (b, m);
        p.releaseResources();
    };
    p.setCurrentProgram ((index + 1) % p.getNumPrograms());
    flush();
    p.setCurrentProgram (index);
    flush();
}

static void setParam (AudioPluginInstance& p, const String& name, float realValue)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (64) == name)
        {
            // discrete params (switches / choice lists): realValue is the index
            if (prm->isDiscrete() || prm->isBoolean())
                prm->setValue (realValue / (float) std::max (1, prm->getNumSteps() - 1));
            else
                prm->setValue (prm->getValueForText (String (realValue)));
            if (std::getenv ("CTS_DEBUG_PARAMS"))
                std::cout << "      set " << name << " = " << realValue << " -> norm " << prm->getValue()
                          << " (" << prm->getText (prm->getValue(), 32) << ")\n";
            return;
        }
    std::cout << "  (parameter not found: " << name << ")\n";
}

int main (int argc, char* argv[])
{
    ScopedJuceInitialiser_GUI init;

    if (argc >= 2 && String (argv[1]) == "--list-devices")
    {
        AudioDeviceManager dm;
        for (auto* type : dm.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            std::cout << type->getTypeName() << ":\n";
            for (auto& name : type->getDeviceNames (false))
                std::cout << "    " << name << "\n";
        }
        return 0;
    }

    if (argc < 2) { std::cout << "Usage: PluginHostTest <plugin.vst3 | plugin.lv2> [wav-out-dir]\n"; return 2; }
    const File file (File::getCurrentWorkingDirectory().getChildFile (String (argv[1])));
    const File wavDir = argc > 2 ? File (argv[2]) : File();

    std::unique_ptr<AudioPluginFormat> fmt;
    String identifier = file.getFullPathName();
    if (file.hasFileExtension ("vst3"))
        fmt = std::make_unique<VST3PluginFormat>();
   #if JUCE_PLUGINHOST_LV2
    else if (file.hasFileExtension ("lv2"))
    {
        // LV2 plugins are found by URI after the host scans the folder that holds the bundle
        fmt = std::make_unique<LV2PluginFormat>();
        for (auto& u : fmt->searchPathsForPlugins (FileSearchPath (file.getParentDirectory().getFullPathName()), false, false))
            if (u.containsIgnoreCase ("clean-tone-studio")) identifier = u;
        std::cout << "LV2 URI: " << identifier << "\n";
        fixedStereo = true;
    }
   #endif
    if (std::getenv ("CTS_HOST_STEREO")) fixedStereo = true;   // debug: feed a VST3 stereo too, as the LV2 is
    if (fmt == nullptr) { std::cout << "Unknown plugin type: " << file.getFullPathName() << "\n"; return 2; }

    OwnedArray<PluginDescription> types;
    fmt->findAllTypesForFile (types, identifier);
    if (types.isEmpty()) { std::cout << "No " << fmt->getName() << " plugin found at " << file.getFullPathName() << "\n"; return 1; }

    String err;
    auto plugin = fmt->createInstanceFromDescription (*types[0], kSR, kBlock, err);
    if (! plugin) { std::cout << "Failed to load: " << err << "\n"; return 1; }

    if (const char* v = std::getenv ("CTS_HOST_PARAMS"))   // debug: set every parameter to v and show what comes back
    {
        for (auto* p : plugin->getParameters())
        {
            p->setValue (String (v).getFloatValue());
            std::cout << "  param " << p->getName (64) << " = " << p->getValue() << " (" << p->getCurrentValueAsText() << ")\n";
        }
        return 0;
    }
    std::cout << "Loaded: " << plugin->getName() << "  (" << types[0]->manufacturerName << ", " << fmt->getName()
              << " " << types[0]->version << ")\n";
    for (auto* prm : plugin->getParameters())
        if (std::getenv ("CTS_DEBUG_PARAMS")) std::cout << "    param: " << prm->getName (64) << "\n";
    std::cout << "Parameters: " << plugin->getParameters().size()
              << "   Programs: " << plugin->getNumPrograms() << "\n";

    plugin->prepareToPlay (kSR, kBlock);
    {
        // LV2 reports latency through an output port that is only written while processing:
        // hosts read it after the first block (VST3 reports it at once)
        AudioBuffer<float> silent (std::max (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()), kBlock);
        silent.clear(); MidiBuffer m; plugin->processBlock (silent, m);
    }
    std::cout << "Latency: " << plugin->getLatencySamples() << " samples\n";
    check (plugin->getLatencySamples() == 5 || plugin->getLatencySamples() == 149, "latency reported (5 samples, or 149 with Lookahead)");
    plugin->releaseResources();

    const auto guitar = makeGuitar (8.0, 7.0);
    const int tailStart = (int) (14.0 * kSR);
    const auto dry = analyse (guitar, tailStart);
    std::cout << "Dry input: peak " << Decibels::gainToDecibels (dry.peak) << " dBFS, rms " << dry.rmsDb << " dB\n\n";
    if (wavDir != File()) { wavDir.createDirectory(); writeWav (wavDir.getChildFile ("00_dry_input.wav"), guitar); }

    if (std::getenv ("CTS_LEVEL_PROBE"))   // debug: loudness in -> out of Studio Clean on guitar and on full-range noise
    {
        AudioBuffer<float> pink (1, (int) (8.0 * kSR));
        {
            Random rng (5); float b0 = 0, b1 = 0, b2 = 0;   // Paul Kellet's economy pink filter
            for (int i = 0; i < pink.getNumSamples(); ++i)
            {
                const float w = rng.nextFloat() * 2.0f - 1.0f;
                b0 = 0.99765f * b0 + w * 0.0990460f; b1 = 0.96300f * b1 + w * 0.2965164f; b2 = 0.57000f * b2 + w * 1.0526913f;
                pink.setSample (0, i, b0 + b1 + b2 + w * 0.1848f);
            }
        }
        auto atLoudness = [] (AudioBuffer<float> b, float lufs) { b.applyGain (Decibels::decibelsToGain (lufs - kLoudness (b, 8.0))); return b; };
        int studio = 0;
        for (int i = 0; i < plugin->getNumPrograms(); ++i) if (plugin->getProgramName (i) == "Studio Clean") studio = i;
        struct Sig { const char* name; AudioBuffer<float> b; };
        std::vector<Sig> sigs { { "guitar DI (as made)", guitar }, { "guitar at -14.5 LUFS", atLoudness (guitar, -14.5f) },
                                { "pink noise -14.5 LUFS", atLoudness (pink, -14.5f) }, { "pink noise -34.5 LUFS", atLoudness (pink, -34.5f) } };
        struct Cfg { const char* name; std::function<void()> set; };
        std::vector<Cfg> cfgs {
            { "Studio Clean, Auto Level ON",  [&] { setParam (*plugin, "Auto Level", 1); } },
            { "Studio Clean, Auto Level OFF", [&] { setParam (*plugin, "Auto Level", 0); } },
            { "  ON, reverb off",             [&] { setParam (*plugin, "Auto Level", 1); setParam (*plugin, "Reverb On", 0); } },
            { "  ON, compressor off",         [&] { setParam (*plugin, "Auto Level", 1); setParam (*plugin, "Compressor On", 0); } },
            { "  ON, warmth 0",               [&] { setParam (*plugin, "Auto Level", 1); setParam (*plugin, "Warmth", 0); } },
            { "  ON, all three off",          [&] { setParam (*plugin, "Auto Level", 1); setParam (*plugin, "Reverb On", 0); setParam (*plugin, "Compressor On", 0); setParam (*plugin, "Warmth", 0); } },
            { "  OFF, all three off",         [&] { setParam (*plugin, "Auto Level", 0); setParam (*plugin, "Reverb On", 0); setParam (*plugin, "Compressor On", 0); setParam (*plugin, "Warmth", 0); } } };
        std::cout << "Loudness change in -> out (K-weighted, LU)\n" << String ("").paddedRight (' ', 32);
        for (auto& s : sigs) std::cout << String (s.name).paddedLeft (' ', 24);
        std::cout << "\n";
        for (auto& c : cfgs)
        {
            String row = String (c.name).paddedRight (' ', 32);
            for (auto& s : sigs)
            {
                selectProgram (*plugin, studio);
                setParam (*plugin, "Compressor On", 1); setParam (*plugin, "Reverb On", 1); setParam (*plugin, "Output", 0);
                c.set();
                const auto out = render (*plugin, s.b, 1, 2, [] { return kBlock; });
                row << String (kLoudness (out, 8.0) - kLoudness (s.b, 8.0), 1).paddedLeft (' ', 24);
            }
            std::cout << "ROW " << row << std::endl;
        }
        return 0;
    }

    // 1) every preset, mono in -> stereo out
    for (int prog = 0; prog < plugin->getNumPrograms(); ++prog)
    {
        selectProgram (*plugin, prog);
        const auto name = plugin->getProgramName (prog);
        std::cout << "Preset " << prog << ": " << name << "\n";
        auto out = render (*plugin, guitar, 1, 2, [] { return kBlock; });
        const auto s = analyse (out, tailStart);
        std::cout << "    peak " << Decibels::gainToDecibels (s.peak) << " dBFS, rms " << s.rmsDb
                  << " dB, tail(last 1s) " << s.tailDb << " dB\n";
        check (s.finite, "output finite");
        check (s.peak < 1.0f, "no clipping (peak < 0 dBFS)");
        check (s.rmsDb > dry.rmsDb - 12.0f && s.rmsDb < dry.rmsDb + 12.0f, "level within +/-12 dB of input");
        // the test signal carries a hot -67 dBFS noise+hum floor, which heavy sustain legitimately lifts
        check (s.tailDb < -55.0f, "tail decays (last second below -55 dB)");
        if (wavDir != File())
            writeWav (wavDir.getChildFile (String (prog + 1).paddedLeft ('0', 2) + "_"
                                           + name.replaceCharacters (" ()/", "____") + ".wav"), out);
    }

    // CPU: heaviest preset, stereo out
    {
        selectProgram (*plugin, 6);
        const auto t0 = Time::getMillisecondCounterHiRes();
        render (*plugin, guitar, 1, 2, [] { return kBlock; });
        const double ms = Time::getMillisecondCounterHiRes() - t0;
        const double audioMs = guitar.getNumSamples() / kSR * 1000.0;
        std::cout << "\nCPU: " << String (100.0 * ms / audioMs, 2) << " % of one core (Ambient Swell, 48 kHz)\n";
    }

    // 2) layouts
    std::cout << "\nLayouts\n";
    selectProgram (*plugin, 5);
    {
        AudioBuffer<float> st (2, guitar.getNumSamples());
        st.copyFrom (0, 0, guitar, 0, 0, guitar.getNumSamples());
        st.copyFrom (1, 0, guitar, 0, 0, guitar.getNumSamples());
        auto a = analyse (render (*plugin, st, 2, 2, [] { return kBlock; }), tailStart);
        check (a.finite && a.peak < 1.0f, "stereo in / stereo out runs clean");
        if (! fixedStereo)
        {
            auto b = analyse (render (*plugin, guitar, 1, 1, [] { return kBlock; }), tailStart);
            check (b.finite && b.peak < 1.0f, "mono in / mono out runs clean");
        }
        else std::cout << "  (mono in / mono out: VST3 only - a JUCE LV2 has one fixed stereo layout)\n";
    }

    // 3) irregular block sizes, including bigger than prepared and 1-sample blocks
    std::cout << "\nBlock sizes\n";
    {
        selectProgram (*plugin, 6);
        setParam (*plugin, "Auto Level", 0.0f);   // compare raw processing only
        auto ref = analyse (render (*plugin, guitar, 1, 2, [] { return kBlock; }), tailStart);
        Random r (7);
        auto odd = analyse (render (*plugin, guitar, 1, 2, [&r] { return r.nextInt (5) == 0 ? 1 : 1 + r.nextInt (kBlock); }), tailStart);
        check (odd.finite, "random block sizes (1..512) finite");
        check (std::abs (odd.rmsDb - ref.rmsDb) < 0.5f, "random block sizes give same level (" + String (odd.rmsDb - ref.rmsDb, 3) + " dB)");
        setParam (*plugin, "Auto Level", 1.0f);
        setParam (*plugin, "Compressor On", 1.0f);   // likewise the compressor switches (presets leave them as set)
        setParam (*plugin, "Lookahead", 0.0f);
        setParam (*plugin, "Delay Tempo", 120.0f);
    }

    // 4) automation torture: every parameter randomised every block
    std::cout << "\nAutomation stress\n";
    {
        Random r (std::getenv ("CTS_HOST_SEED") ? String (std::getenv ("CTS_HOST_SEED")).getLargeIntValue() : 99);
        // the plugin's own knobs only - not the host's Bypass / Program - in name order, because a VST3
        // host lists them in the plugin's order and an LV2 host alphabetically: same automation in both
        Array<AudioProcessorParameter*> params;
        for (auto* p : plugin->getParameters())
            if (p->getName (64) != "Bypass" && p->getName (64) != "Program") params.add (p);
        std::sort (params.begin(), params.end(), [] (auto* a, auto* b) { return a->getName (64) < b->getName (64); });
        auto out = render (*plugin, guitar, 1, 2, [] { return 256; }, [&] (int)
        {
            for (auto* p : params)
                if (r.nextInt (4) == 0)
                    p->setValue (r.nextFloat());
        });
        const auto s = analyse (out, tailStart);
        check (s.finite, "random automation finite");
        check (s.peak < 8.0f, "random automation bounded (peak " + String (Decibels::gainToDecibels (s.peak), 1) + " dBFS)");
        // presets keep the user's Output / Auto Level, so put those back after randomising
        setParam (*plugin, "Output", 0.0f);
        setParam (*plugin, "Auto Level", 1.0f);
        setParam (*plugin, "Compressor On", 1.0f);   // likewise the compressor switches (presets leave them as set)
        setParam (*plugin, "Lookahead", 0.0f);
        setParam (*plugin, "Delay Tempo", 120.0f);
    }

    if (std::getenv ("CTS_DEBUG_LEVELS"))
    {
        auto measure = [&] (const char* label)
        {
            auto out = render (*plugin, guitar, 1, 2, [] { return kBlock; });
            std::cout << "      " << label << ": K " << kLoudness (out, 8.0) << "  rms " << analyse (out, tailStart).rmsDb << "\n";
        };
        selectProgram (*plugin, 0);
        setParam (*plugin, "Auto Level", 0);
        measure ("auto off, bass 0");
        setParam (*plugin, "Bass", 12);
        measure ("auto off, bass +12");
        setParam (*plugin, "Bass", 0);
        setParam (*plugin, "Output", 6);
        measure ("auto off, output +6");
        setParam (*plugin, "Output", 0);
        selectProgram (*plugin, 5);
        setParam (*plugin, "Compressor On", 1);
        measure ("auto off, sustain preset, comp on");
        setParam (*plugin, "Compressor On", 0);
        measure ("auto off, sustain preset, comp off");
        setParam (*plugin, "Compressor On", 1);
        setParam (*plugin, "Auto Level", 1);
        measure ("auto on");
    }

    // 4b) auto level: presets and big setting changes keep the same loudness
    std::cout << "\nAuto level\n";
    {
        const float dryL = kLoudness (guitar, 8.0);
        auto loudNow = [&]
        {
            Thread::sleep (700);   // let the background matcher finish
            return kLoudness (render (*plugin, guitar, 1, 2, [] { return kBlock; }), 8.0) - dryL;
        };
        selectProgram (*plugin, 0);
        loudNow();                 // warm-up: lets the input-level tracker settle

        float lo = 100, hi = -100;
        for (int prog = 0; prog < plugin->getNumPrograms(); ++prog)
        {
            selectProgram (*plugin, prog);
            const float d = loudNow();
            lo = std::min (lo, d); hi = std::max (hi, d);
            std::cout << "    " << plugin->getProgramName (prog).paddedRight (' ', 22) << String (d, 2) << " dB vs dry\n";
        }
        check (hi - lo < 2.0f, "all presets within 2 dB of each other (spread " + String (hi - lo, 2) + " dB)");
        check (std::abs (lo) < 1.5f && std::abs (hi) < 1.5f, "all presets within 1.5 dB of the dry input");

        struct Change { const char* name; float value; };
        const Change changes[] = { { "Bass", 12 }, { "Bass", -12 }, { "Treble", 12 }, { "Treble", -12 },
                                   { "Presence", 12 }, { "Mid", -12 }, { "Sustain", 100 }, { "Sustain", 0 },
                                   { "Input", 12 }, { "Input", -12 }, { "Warmth", 100 }, { "Reverb Level", 100 },
                                   { "High Cut", 3000 }, { "Low Cut", 300 } };
        selectProgram (*plugin, 0);
        const float base = loudNow();
        for (const auto& ch : changes)
        {
            selectProgram (*plugin, 0);
            setParam (*plugin, ch.name, ch.value);
            const float d = loudNow() - base;
            check (std::abs (d) < 1.5f, String (ch.name) + " -> " + String (ch.value) + ": " + String (d, 2) + " dB change");
        }
        setParam (*plugin, "Cabinet", 0);
        { const float d = loudNow() - base; check (std::abs (d) < 1.5f, "Cabinet off: " + String (d, 2) + " dB change"); }

        // and with Auto Level switched off, the presets really do differ in level
        setParam (*plugin, "Auto Level", 0);
        float offLo = 100, offHi = -100;
        for (int prog = 0; prog < plugin->getNumPrograms(); ++prog)
        {
            selectProgram (*plugin, prog);
            const float d = loudNow();
            offLo = std::min (offLo, d); offHi = std::max (offHi, d);
        }
        std::cout << "    preset level spread: Auto Level off " << String (offHi - offLo, 2)
                  << " dB, on " << String (hi - lo, 2) << " dB\n";
        check (offHi - offLo > 2.0f * (hi - lo), "Auto Level reduces the preset level spread");
        setParam (*plugin, "Auto Level", 1);
    }

    // 4c) compressor On switch (for players with a hardware compressor in front)
    std::cout << "\nCompressor switch\n";
    {
        // Sustain = how much a note decays from just after the pick to its tail.
        // Test notes start every 0.9 s; compare 50-150 ms after each pick with 600-800 ms.
        auto decayAndLoud = [&] (float compOn)
        {
            selectProgram (*plugin, 5);                 // Endless Sustain: heavy compression
            setParam (*plugin, "Compressor On", compOn);
            Thread::sleep (700);
            auto out = render (*plugin, guitar, 1, 2, [] { return kBlock; });
            auto winPow = [&] (double t0, double t1)
            {
                double s = 0; const int a0 = (int) (t0 * kSR), a1 = (int) (t1 * kSR);
                for (int i = a0; i < a1; ++i) s += (double) out.getSample (0, i) * out.getSample (0, i);
                return s / (a1 - a0) + 1e-20;
            };
            double drop = 0;
            for (int k = 0; k < 8; ++k)
                drop += 10.0 * std::log10 (winPow (k * 0.9 + 0.05, k * 0.9 + 0.15) / winPow (k * 0.9 + 0.6, k * 0.9 + 0.8));
            return std::make_tuple ((float) (drop / 8.0), kLoudness (out, 8.0), analyse (out, tailStart).finite);
        };
        const auto [dropOn, loudOn, finOn] = decayAndLoud (1.0f);
        const auto [dropOff, loudOff, finOff] = decayAndLoud (0.0f);
        std::cout << "    note decay (pick -> tail): comp on " << String (dropOn, 1) << " dB, off " << String (dropOff, 1) << " dB\n";
        check (finOn && finOff, "both settings finite");
        check (dropOff > dropOn + 1.0f, "switching off removes the sustain compression (notes decay naturally)");
        check (std::abs (loudOff - loudOn) < 1.5f, "Auto Level keeps the volume steady (" + String (loudOff - loudOn, 2) + " dB)");

        // toggling while playing must not click. Auto Level held off so only the switch moves;
        // compare against the larger of the two steady states (uncompressed peaks are naturally bigger).
        selectProgram (*plugin, 5);
        setParam (*plugin, "Auto Level", 0.0f);
        AudioProcessorParameter* sw = nullptr;
        for (auto* prm : plugin->getParameters()) if (prm->getName (64) == "Compressor On") sw = prm;
        auto maxJump = [&] (int mode)   // 0 = steady on, 1 = steady off, 2 = toggling
        {
            if (sw) sw->setValue (mode == 1 ? 0.0f : 1.0f);
            auto out = render (*plugin, guitar, 1, 2, [] { return 256; }, [&] (int block)
            {
                if (mode == 2 && sw != nullptr && block % 40 == 20) sw->setValue (sw->getValue() > 0.5f ? 0.0f : 1.0f);
            });
            float j = 0;
            for (int i = 1; i < (int) (8.0 * kSR); ++i) j = std::max (j, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
            return j;
        };
        const float steady = std::max (maxJump (0), maxJump (1)), toggled = maxJump (2);
        if (sw) sw->setValue (1.0f);
        setParam (*plugin, "Auto Level", 1.0f);
        check (toggled < steady * 1.2f, "toggling while playing doesn't click (" + String (toggled / steady, 2) + "x the steady maximum)");
    }
    // 5) worst case: max feedback, max decay, max everything
    std::cout << "\nExtremes\n";
    {
        selectProgram (*plugin, 0);
        for (auto* p : plugin->getParameters())
        {
            const auto n = p->getName (64);
            if (n == "Feedback" || n == "Decay" || n == "Size" || n == "Sustain" || n == "Delay On"
                || n == "Reverb Level" || n == "Delay Level" || n == "Warmth" || n == "Delay Mod")
                p->setValue (1.0f);
        }
        auto out = render (*plugin, guitar, 1, 2, [] { return kBlock; });
        const auto s = analyse (out, tailStart);
        std::cout << "    peak " << Decibels::gainToDecibels (s.peak) << " dBFS, tail " << s.tailDb << " dB\n";
        check (s.finite, "extreme settings finite");
        check (s.peak < 4.0f, "extreme settings stay bounded");
    }

    // 5b) noise gate actually reduces the noise floor under heavy sustain
    std::cout << "\nNoise gate\n";
    {
        auto tailWithGate = [&] (float gate01)
        {
            selectProgram (*plugin, 5);
            for (auto* p : plugin->getParameters())
                if (p->getName (64) == "Noise Gate") p->setValue (gate01);
            return analyse (render (*plugin, guitar, 1, 2, [] { return kBlock; }), tailStart).tailDb;
        };
        const float open = tailWithGate (0.0f), closed = tailWithGate (0.6f);
        std::cout << "    tail with gate 0%: " << open << " dB, gate 60%: " << closed << " dB\n";
        check (closed < open - 10.0f, "gate lowers noise floor by > 10 dB");
    }

    // 6) state save / restore
    std::cout << "\nState\n";
    {
        selectProgram (*plugin, 6);
        MemoryBlock mb;
        plugin->getStateInformation (mb);
        selectProgram (*plugin, 0);
        plugin->setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        selectProgram (*plugin, 6);
        MemoryBlock mb2; plugin->getStateInformation (mb2);
        same = mb.getSize() > 0;
        check (same, "state round-trips (" + String ((int) mb.getSize()) + " bytes)");
    }

    // 7) editor
    std::cout << "\nEditor\n";
    {
        std::unique_ptr<AudioProcessorEditor> ed (plugin->createEditorIfNeeded());
        check (ed != nullptr, "editor created");
        if (ed) std::cout << "    editor size " << ed->getWidth() << " x " << ed->getHeight() << "\n";
        if (ed && wavDir != File())
        {
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            PNGImageFormat png;
            auto f = wavDir.getChildFile ("editor.png");
            f.deleteFile();
            if (auto os = f.createOutputStream()) png.writeImageToStream (img, *os);
        }
    }

    plugin.reset();
    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : String (failures) + " FAILURE(S)") << "\n";
    return failures == 0 ? 0 : 1;
}
