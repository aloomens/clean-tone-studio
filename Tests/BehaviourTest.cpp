// Behaviour tests that drive the processor directly, sample-accurately:
//   1) clicks when any control or switch changes while playing
//   2) identical sound after saving and reloading settings (incl. a host re-sending the program)
//   3) noise gate at extreme settings, and recovery afterwards
//   4) identical sound at every buffer size
//
//   5) click scan of every switch, every choice and every control (Mastering Studio method)
//   6) THE_LAB-style measurements: distortion, aliasing, response, latency, silence, CPU
//
// Usage: BehaviourTest [--verbose]
//        BehaviourTest --lab [report.md]      full measurement report
//        BehaviourTest --clicks [all]         click scan only
//        BehaviourTest --snapshots <dir>      editor screenshots
//        BehaviourTest --bench                CPU at 48 / 96 kHz
//        BehaviourTest --recall               recall checks only
//        BehaviourTest --stress               THE_LAB stress replicas only
//        BehaviourTest --modules              1.4 module checks only (IR loader, reverb, compressor, ...)
//        BehaviourTest --spur                 THE_LAB high-note (10 kHz) spur test
//        BehaviourTest --bufsize [rate]       buffer-size checks only (env CTS_BS_SET="warmth=0,..." to vary settings)

#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "LabMeasurements.h"

using namespace juce;

// Count heap allocations while gCountAllocs is set (used to prove processBlock never allocates).
static std::atomic<bool> gCountAllocs { false };
static std::atomic<long> gAllocs { 0 };
void* operator new (std::size_t n)
{
    if (gCountAllocs.load (std::memory_order_relaxed)) ++gAllocs;
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { if (p && gCountAllocs.load (std::memory_order_relaxed)) ++gAllocs; std::free (p); }
void operator delete[] (void* p) noexcept { operator delete (p); }
void operator delete (void* p, std::size_t) noexcept { operator delete (p); }
void operator delete[] (void* p, std::size_t) noexcept { operator delete (p); }

static double kSR = 48000.0;           // set per run: tests repeat at 44.1, 48 and 96 kHz
static int failures = 0;
static bool verbose = false;

static void check (bool ok, const String& what)
{
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << std::endl;
    if (! ok) ++failures;
}

//==============================================================================
static AudioBuffer<float> makeGuitar (double seconds, double silenceAfter, float noise = 0.0003f)
{
    const int total = (int) ((seconds + silenceAfter) * kSR);
    AudioBuffer<float> b (1, total);
    b.clear();
    Random rng (42);
    const float notes[] = { 82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f, 196.0f, 261.63f, 164.81f, 220.0f };
    int pos = 0, idx = 0;
    while (pos < (int) (seconds * kSR))
    {
        const int period = (int) (kSR / notes[idx++ % 10]);
        std::vector<float> line ((size_t) period);
        float lp = 0.0f;
        for (auto& s : line) { lp += 0.22f * ((rng.nextFloat() * 2.0f - 1.0f) - lp); s = lp * 3.0f; }
        float last = 0.0f;
        const int len = (int) (kSR * 0.9);
        for (int i = 0; i < len && pos + i < total; ++i)
        {
            const size_t k = (size_t) (i % period);
            const float cur = line[k];
            line[k] = 0.4985f * (cur + last);
            last = cur;
            b.addSample (0, pos + i, cur);
        }
        pos += len;
    }
    b.applyGain (0.25f / std::max (1.0e-6f, b.getMagnitude (0, 0, total)));
    for (int i = 0; i < total; ++i)
        b.addSample (0, i, (rng.nextFloat() - 0.5f) * noise
                         + noise * std::sin (2.0f * MathConstants<float>::pi * 60.0f * (float) i / (float) kSR));
    return b;
}

static AudioBuffer<float> makeSine (double seconds, float freq, float amp)
{
    AudioBuffer<float> b (1, (int) (seconds * kSR));
    const int fade = (int) (0.05 * kSR);   // gentle start: an abrupt onset is itself a click (and the delay repeats it)
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float env = i < fade ? 0.5f - 0.5f * std::cos (MathConstants<float>::pi * (float) i / (float) fade) : 1.0f;
        b.setSample (0, i, env * amp * (float) std::sin (2.0 * MathConstants<double>::pi * freq * i / kSR));
    }
    return b;
}

//==============================================================================
static float get (CleanToneAudioProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }
static void set (CleanToneAudioProcessor& p, const String& id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

static void prepare (CleanToneAudioProcessor& p, int blockSize)
{
    AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (AudioChannelSet::mono());
    layout.outputBuses.add (AudioChannelSet::stereo());
    p.releaseResources();
    p.setBusesLayout (layout);
    p.setRateAndBufferSizeDetails (kSR, blockSize);
    p.prepareToPlay (kSR, blockSize);
}

// Auto Level now runs in audio time inside processBlock, so there is never anything to wait for.
static void waitForAutoLevel (CleanToneAudioProcessor&) {}

// Render mono input -> stereo output. perSample is called at every sample index before processing
// the block that contains it is NOT possible, so changes are applied at block starts via perBlock.
static AudioBuffer<float> render (CleanToneAudioProcessor& p, const AudioBuffer<float>& in, int blockSize,
                                  std::function<void (int startSample)> perBlock = {}, bool doPrepare = true)
{
    if (doPrepare) prepare (p, blockSize);
    const int total = in.getNumSamples();
    AudioBuffer<float> out (2, total);
    MidiBuffer midi;
    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = std::min (blockSize, total - pos);
        AudioBuffer<float> buf (2, n);
        buf.copyFrom (0, 0, in, 0, pos, n);
        buf.clear (1, 0, n);
        if (perBlock) perBlock (pos);
        p.processBlock (buf, midi);
        out.copyFrom (0, pos, buf, 0, 0, n);
        out.copyFrom (1, pos, buf, 1, 0, n);
    }
    return out;
}

static bool allFinite (const AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (c, i))) return false;
    return true;
}

static float diffDb (const AudioBuffer<float>& a, const AudioBuffer<float>& b, int from = 0)
{
    float maxDiff = 0, maxA = 1e-9f;
    for (int c = 0; c < 2; ++c)
        for (int i = from; i < std::min (a.getNumSamples(), b.getNumSamples()); ++i)
        {
            maxDiff = std::max (maxDiff, std::abs (a.getSample (c, i) - b.getSample (c, i)));
            maxA = std::max (maxA, std::abs (a.getSample (c, i)));
        }
    return Decibels::gainToDecibels (maxDiff / maxA, -200.0f);   // difference relative to signal peak
}

static float rmsDb (const AudioBuffer<float>& b, int from, int to)
{
    double s = 0;
    for (int c = 0; c < 2; ++c)
        for (int i = from; i < to; ++i) s += (double) b.getSample (c, i) * b.getSample (c, i);
    return (float) (10.0 * std::log10 (s / (2.0 * (to - from)) + 1e-20));
}

// Click metric: a click is a broadband burst, so look at what the output contains above 4 kHz
// (24 dB/oct high-pass) while a low sine plays. Smooth glides only move energy near the sine's
// own harmonics; any step in level, filter state or delay position shows up up here.
static float clickDb (const AudioBuffer<float>& b, int from)
{
    float pk = 1.0e-9f, hf = 0.0f;
    for (int c = 0; c < 2; ++c)
    {
        ct::Biquad h1, h2;
        h1.setHighPass (kSR, 4000.0, 0.7071);
        h2.setHighPass (kSR, 4000.0, 0.7071);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float x = b.getSample (c, i);
            const float y = h2.process (h1.process (x, 0), 0);
            pk = std::max (pk, std::abs (x));
            if (i >= from) hf = std::max (hf, std::abs (y));
        }
    }
    return Decibels::gainToDecibels (hf / pk, -200.0f);
}
//==============================================================================
static void resetToBaseline (CleanToneAudioProcessor& p)
{
    p.loadFactoryPreset (0);          // Studio Clean
    set (p, IDs::autoLevel, 0);
    set (p, IDs::outGain, 0);
    set (p, IDs::compOn, 1);
}

static void clickSection()
{
    std::cout << "1) Clicks when controls change while playing (each control flipped min <-> max repeatedly)\n";
    {
        // A low sine is the most revealing signal: any step in gain, filter state or delay
        // position stands out against its smooth, small curvature.
        const auto sine = makeSine (3.0, 147.0f, 0.2f);
        const int settle = (int) (0.5 * kSR);

        struct Move { String id; float a, b; };
        std::vector<Move> moves = {
            { IDs::inGain, -12, 12 }, { IDs::pickup, 0, 1 }, { IDs::pickup, 1, 2 }, { IDs::hum, 0, 2 },
            { IDs::gate, 0, 100 }, { IDs::compOn, 1, 0 }, { IDs::sustain, 0, 100 }, { IDs::attack, 1, 60 },
            { IDs::compMix, 0, 100 }, { IDs::warmth, 0, 100 }, { IDs::bass, -12, 12 }, { IDs::mid, -12, 12 },
            { IDs::midFreq, 200, 3000 }, { IDs::presence, -12, 12 }, { IDs::treble, -12, 12 },
            { IDs::lowCut, 20, 400 }, { IDs::highCut, 2000, 20000 }, { IDs::cab, 0, 3 }, { IDs::cab, 1, 2 },
            { IDs::dlyOn, 0, 1 }, { IDs::dlyTime, 100, 900 }, { IDs::dlyFeedback, 0, 95 }, { IDs::dlyTone, 800, 12000 },
            { IDs::dlyMod, 0, 100 }, { IDs::dlyDuck, 0, 100 }, { IDs::dlyWidth, 0, 100 }, { IDs::dlyMix, 0, 100 },
            { IDs::dlySync, 0, 1 }, { IDs::revOn, 0, 1 }, { IDs::revSize, 0, 100 }, { IDs::revDecay, 0.3f, 12 },
            { IDs::revPreDelay, 0, 150 }, { IDs::revTone, 1500, 16000 }, { IDs::revLowCut, 50, 800 },
            { IDs::revMix, 0, 100 }, { IDs::outGain, -24, 12 }, { IDs::autoLevel, 1, 0 } };

        CleanToneAudioProcessor p;
        int worstIdx = -1; float worst = -200.0f;
        for (size_t m = 0; m < moves.size(); ++m)
        {
            const auto& mv = moves[m];
            auto renderWith = [&] (std::function<void (int)> cb)
            {
                resetToBaseline (p);
                set (p, IDs::dlyOn, 1);   // keep the time effects running so their controls are exercised
                set (p, IDs::dlyMix, 40);
                set (p, mv.id, mv.a);
                return render (p, sine, 256, cb);
            };
            // steady references at both values
            const float steadyA = clickDb (renderWith ({}), settle);
            resetToBaseline (p);
            auto outB = [&] { set (p, IDs::dlyOn, 1); set (p, IDs::dlyMix, 40); set (p, mv.id, mv.b);
                              prepare (p, 256); return render (p, sine, 256, {}, false); }();
            const float steadyB = clickDb (outB, settle);
            // flip between the two values several times while the sine plays
            auto moving = renderWith ([&] (int pos)
            {
                if (pos >= settle && (pos / 256) % 40 == 0)
                    set (p, mv.id, ((pos / 256) / 40) % 2 ? mv.b : mv.a);
            });
            // allowed: what the setting produces anyway (+3 dB), or -80 dB below the signal (inaudible)
            const float allowed = std::max (std::max (steadyA, steadyB) + 3.0f, -80.0f);
            const float excess = clickDb (moving, settle) - allowed;
            if (excess > worst) { worst = excess; worstIdx = (int) m; }
            if (std::getenv ("CTS_CLICK_TRACE") && mv.id == String (std::getenv ("CTS_CLICK_TRACE")))
            {
                // where does the worst high-frequency burst happen, relative to the control moves?
                ct::Biquad h1, h2; h1.setHighPass (kSR, 4000.0, 0.7071); h2.setHighPass (kSR, 4000.0, 0.7071);
                float best = 0; int at = 0;
                for (int i = 0; i < moving.getNumSamples(); ++i)
                {
                    const float y = std::abs (h2.process (h1.process (moving.getSample (0, i), 0), 0));
                    if (i >= settle && y > best) { best = y; at = i; }
                }
                std::cout << "      worst burst at sample " << at << " (" << (at % (40 * 256)) << " samples after a move), "
                          << "steady A " << steadyA << " dB, steady B " << steadyB << " dB\n";
            }
            const bool ok = allFinite (moving) && excess <= 0.0f;
            if (verbose || ! ok)
                check (ok, mv.id + " " + String (mv.a) + " <-> " + String (mv.b) + ": click energy "
                           + String (clickDb (moving, settle), 1) + " dB (allowed " + String (allowed, 1) + " dB)");
        }
        check (worst <= 0.0f, "no control clicks (worst: " + (worstIdx >= 0 ? moves[(size_t) worstIdx].id : String("none"))
                              + ", " + String (std::abs (worst), 1) + (worst > 0.0f ? " dB over" : " dB under") + " the limit)");
    }

}

static void recallSection()
{
    std::cout << "\n2) Same sound after saving and reloading settings\n";
    {
        const auto guitar = makeGuitar (6.0, 1.0);
        CleanToneAudioProcessor a;
        a.loadFactoryPreset (6);                       // Ambient Swell, then tweak it
        set (a, IDs::bass, 3.5f); set (a, IDs::treble, -2.0f); set (a, IDs::sustain, 72);
        set (a, IDs::dlyFeedback, 61); set (a, IDs::revSize, 77); set (a, IDs::cab, 3);
        set (a, IDs::outGain, -3.0f);
        prepare (a, 512);
        render (a, guitar, 512, {}, false);           // play for a while: auto level learns the playing level
        waitForAutoLevel (a);
        MemoryBlock state;
        a.getStateInformation (state);
        auto outA = render (a, guitar, 512);          // reference: same instance, same settings

        // 2a) a fresh instance restored from the saved state (like reopening a project)
        CleanToneAudioProcessor b;
        b.setStateInformation (state.getData(), (int) state.getSize());
        prepare (b, 512);
        auto outB = render (b, guitar, 512, {}, false);
        const float d1 = diffDb (outA, outB);
        check (d1 < -60.0f, "reloaded project sounds identical (difference " + String (d1, 1) + " dB)");

        // 2b) hosts like Cubase re-send the program number after loading a project
        CleanToneAudioProcessor c;
        c.setStateInformation (state.getData(), (int) state.getSize());
        c.setCurrentProgram (c.getCurrentProgram());
        const bool kept = std::abs (get (c, IDs::bass) - 3.5f) < 0.01f && get (c, IDs::cab) == 3.0f;
        check (kept, "host re-sending the program number doesn't reset tweaked knobs");
        prepare (c, 512);
        auto outC = render (c, guitar, 512, {}, false);
        const float d2 = diffDb (outA, outC);
        check (d2 < -60.0f, "...and it still sounds identical (difference " + String (d2, 1) + " dB)");
    }

}

static void gateSection()
{
    std::cout << "\n3) Noise gate at extreme settings\n";
    {
        // 3a) the gate itself: a steady low E at every level around the threshold must come out
        //     either fully open or fully closed - never buzzing (gain wobbling with each cycle)
        float worstRipple = 0.0f, worstAt = 0.0f;
        for (float amount : { 0.5f, 1.0f })
            for (float levelDb = -75.0f; levelDb <= -25.0f; levelDb += 1.0f)
            {
                ct::Gate g; g.prepare (kSR); g.setAmount (amount);
                const float amp = Decibels::decibelsToGain (levelDb);
                const int total = (int) (2.0 * kSR), from = (int) (1.6 * kSR);
                std::vector<float> x ((size_t) total), y ((size_t) total);
                for (int i = 0; i < total; ++i) x[(size_t) i] = y[(size_t) i] = amp * std::sin (2.0f * MathConstants<float>::pi * 82.41f * i / (float) kSR);
                for (int pos = 0; pos < total; pos += 64)
                {
                    float* ch[1] = { y.data() + pos };
                    g.process (ch, 1, std::min (64, total - pos));
                }
                float lo = 1e9f, hi = 0.0f;
                for (int i = from; i < total; ++i)
                    if (std::abs (x[(size_t) i]) > 0.5f * amp)
                    {
                        const float gn = y[(size_t) i] / x[(size_t) i];
                        lo = std::min (lo, gn); hi = std::max (hi, gn);
                    }
                const float ripple = Decibels::gainToDecibels (hi / lo);
                if (ripple > worstRipple) { worstRipple = ripple; worstAt = levelDb; }
            }
        check (worstRipple < 0.2f, "no buzzing near the threshold (worst gain wobble " + String (worstRipple, 2)
                                   + " dB at " + String (worstAt, 0) + " dBFS)");

        //     a note dying away through the threshold closes the gate once - no stuttering
        {
            ct::Gate g; g.prepare (kSR); g.setAmount (1.0f);
            const int total = (int) (3.0 * kSR);
            int closes = 0, opens = 0; bool wasOpen = true;
            for (int i = 0; i < total; ++i)
            {
                const float amp = Decibels::decibelsToGain (-20.0f - 60.0f * (float) i / (float) total);
                float s = amp * std::sin (2.0f * MathConstants<float>::pi * 82.41f * i / (float) kSR);
                float* ch[1] = { &s };
                g.process (ch, 1, 1);
                const bool isOpen = g.getGain() > 0.5f;
                if (wasOpen && ! isOpen) ++closes;
                if (! wasOpen && isOpen) ++opens;
                wasOpen = isOpen;
            }
            check (closes == 1 && opens == 0, "a dying note closes the gate exactly once (closed " + String (closes)
                                              + "x, reopened " + String (opens) + "x)");
        }

        // 3b) the whole plugin at extreme settings: finite, no clipping, notes still come through
        const auto guitar = makeGuitar (8.0, 1.0, 0.002f);   // noisy single-coil DI
        CleanToneAudioProcessor p;
        auto setup = [&] (float gate)
        {
            resetToBaseline (p);
            p.loadFactoryPreset (5);                 // Endless Sustain: lots of compression after the gate
            set (p, IDs::autoLevel, 1);
            set (p, IDs::gate, gate);
        };
        setup (30);
        prepare (p, 256);
        render (p, guitar, 256, {}, false);
        waitForAutoLevel (p);
        auto normal = render (p, guitar, 256, {}, false);

        set (p, IDs::gate, 100);
        render (p, guitar, 256, {}, false);
        waitForAutoLevel (p);
        auto extreme = render (p, guitar, 256, {}, false);
        const float pk = Decibels::gainToDecibels (extreme.getMagnitude (0, extreme.getNumSamples()));
        check (allFinite (extreme), "gate 100%: output finite");
        check (pk < 0.0f, "gate 100%: no clipping (peak " + String (pk, 1) + " dBFS)");
        const float noteLevel = rmsDb (extreme, (int) (0.02 * kSR), (int) (0.3 * kSR)) - rmsDb (normal, (int) (0.02 * kSR), (int) (0.3 * kSR));
        check (std::abs (noteLevel) < 6.0f, "gate 100%: notes still come through at a sensible level (" + String (noteLevel, 1) + " dB vs normal)");
        check (std::abs (p.getAutoLevelCorrectionDb()) < 12.0f, "gate 100%: Auto Level doesn't run away (" + String (p.getAutoLevelCorrectionDb(), 1) + " dB)");

        // 3c) back to normal while still playing: must end up exactly where it would have been.
        //     Compare with a session that never went extreme (skip 4 s so old echoes have died away).
        set (p, IDs::inGain, 24); set (p, IDs::sustain, 100); set (p, IDs::warmth, 100);
        auto wild = render (p, guitar, 256, {}, false);
        check (allFinite (wild), "gate 100% + input +24 dB + sustain 100% + warmth 100%: finite");
        set (p, IDs::gate, 30); set (p, IDs::inGain, 0); set (p, IDs::sustain, 85); set (p, IDs::warmth, 40);
        render (p, guitar, 256, {}, false);
        waitForAutoLevel (p);
        const float corrAfter = p.getAutoLevelCorrectionDb();
        // sample-exact comparison: hold Auto Level and switch the modulated effects off, so the only
        // thing that could differ is leftover state in the gate / compressor / warmth / filters
        auto exactSetup = [] (CleanToneAudioProcessor& x) { set (x, IDs::autoLevel, 0); set (x, IDs::dlyOn, 0); set (x, IDs::revOn, 0); };
        exactSetup (p);
        render (p, guitar, 256, {}, false);
        auto after = render (p, guitar, 256, {}, false);

        CleanToneAudioProcessor q;
        resetToBaseline (q); q.loadFactoryPreset (5); set (q, IDs::autoLevel, 1); set (q, IDs::gate, 30);
        prepare (q, 256);
        for (int k = 0; k < 4; ++k) { render (q, guitar, 256, {}, false); waitForAutoLevel (q); }
        const float corrControl = q.getAutoLevelCorrectionDb();
        exactSetup (q);
        render (q, guitar, 256, {}, false);
        auto control = render (q, guitar, 256, {}, false);
        const float d = diffDb (control, after, (int) (1.0 * kSR));
        check (allFinite (after) && d < -60.0f, "gate, compressor and tone fully recover after extreme settings (difference " + String (d, 1) + " dB)");
        check (std::abs (corrAfter - corrControl) < 0.5f, "Auto Level back to its normal correction (" + String (corrAfter, 2)
                                                          + " dB vs " + String (corrControl, 2) + " dB)");
    }
}

static void bufferSection()
{
    std::cout << "\n4) Same sound at every buffer size\n";
    {
        const auto guitar = makeGuitar (6.0, 2.0);
        CleanToneAudioProcessor p;
        auto run = [&] (int bs, bool autoLevel)
        {
            resetToBaseline (p);
            p.loadFactoryPreset (6);                 // Ambient Swell: delay + reverb + comp
            set (p, IDs::autoLevel, autoLevel ? 1.0f : 0.0f);
            if (auto* dbg = std::getenv ("CTS_BS_SET"))   // debugging aid: "warmth=0,revOn=0"
                for (auto& kv : StringArray::fromTokens (dbg, ",", ""))
                    set (p, kv.upToFirstOccurrenceOf ("=", false, false), kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue());
            return render (p, guitar, bs);
        };
        const auto ref = run (512, false);
        float worst = -200; int worstBs = 0;
        for (int bs : { 16, 32, 64, 100, 128, 256, 441, 1024, 2048, 4096 })
        {
            const float d = diffDb (ref, run (bs, false));
            if (verbose)
            {
                std::cout << "    buffer " << bs << ": difference " << d << " dB";
                if (d > -150.0f)
                {
                    const auto other = run (bs, false);
                    int at = 0; float md = 0;
                    for (int i = 0; i < ref.getNumSamples(); ++i) { const float e = std::abs (ref.getSample (0, i) - other.getSample (0, i)); if (e > md) { md = e; at = i; } }
                    float firstAt = -1;
                    for (int i = 0; i < ref.getNumSamples(); ++i) if (ref.getSample (0, i) != other.getSample (0, i)) { firstAt = (float) i; break; }
                    std::cout << "  (first differs at sample " << firstAt << " = " << firstAt / kSR << " s; largest at " << at / kSR
                              << " s, signal there " << ref.getSample (0, at) << ")";
                }
                std::cout << "\n";
            }
            if (d > worst) { worst = d; worstBs = bs; }
        }
        check (worst < -90.0f, "Auto Level off: all buffer sizes identical (worst " + String (worst, 1) + " dB at " + String (worstBs) + ")");

        // with Auto Level on: let it learn the playing at one buffer size, then compare
        auto settled = [&] (int bs)
        {
            resetToBaseline (p);
            p.loadFactoryPreset (6);
            set (p, IDs::autoLevel, 1);
            prepare (p, bs);
            render (p, guitar, bs, {}, false);
            waitForAutoLevel (p);
            render (p, guitar, bs, {}, false);
            waitForAutoLevel (p);
            return std::make_pair (p.getAutoLevelCorrectionDb(), render (p, guitar, bs, {}, false));
        };
        const auto [c64, o64] = settled (64);
        const auto [c2048, o2048] = settled (2048);
        std::cout << "    Auto Level correction: buffer 64 -> " << String (c64, 2) << " dB, buffer 2048 -> " << String (c2048, 2) << " dB\n";
        check (std::abs (c64 - c2048) < 0.05f, "Auto Level on: same correction at buffer 64 and 2048");
        const float d = diffDb (o64, o2048);
        check (d < -60.0f, "Auto Level on: same sound at buffer 64 and 2048 (difference " + String (d, 1) + " dB)");
    }

}

//==============================================================================
// More recall checks (same ideas as Mastering Studio): the same settings reached different ways
// must produce identical audio.
static void recallExtraSection()
{
    const auto guitar = makeGuitar (4.0, 0.5);
    auto setUp = [] (CleanToneAudioProcessor& p)
    {
        p.loadFactoryPreset (6);   // Ambient Swell: every module in use
        for (auto [id, v] : std::initializer_list<std::pair<const char*, float>> {
                 { IDs::pickup, 1 }, { IDs::hum, 2 }, { IDs::gate, 20 }, { IDs::bass, 2.5f }, { IDs::midFreq, 1300 },
                 { IDs::cab, 2 }, { IDs::dlySync, 0 }, { IDs::dlyTime, 420 }, { IDs::revPreDelay, 33 }, { IDs::autoLevel, 0 } })
            set (p, id, v);
    };
    CleanToneAudioProcessor a;
    setUp (a);
    const auto ref = render (a, guitar, 512);
    MemoryBlock state;
    a.getStateInformation (state);

    // host prepares first, then restores state (common DAW order)
    {
        CleanToneAudioProcessor b;
        prepare (b, 512);
        b.setStateInformation (state.getData(), (int) state.getSize());
        const auto out = render (b, guitar, 512, {}, false);
        const float d = diffDb (ref, out);
        check (d < -90.0f, "state restored after the host prepares: difference " + String (d, 1) + " dB");
    }
    // a user preset saved and loaded into a fresh instance
    {
        const String name = "RecallTest " + String (Random::getSystemRandom().nextInt (100000));
        a.saveUserPreset (name);
        CleanToneAudioProcessor b;
        b.loadUserPreset (name);
        set (b, IDs::autoLevel, 0);           // presets leave Output / Auto Level / Compressor On alone
        const auto out = render (b, guitar, 512);
        a.deleteUserPreset (name);
        const float d = diffDb (ref, out);
        check (d < -90.0f, "user preset saved and loaded into a fresh instance: difference " + String (d, 1) + " dB");
    }
    // every parameter value survives a save/restore exactly
    {
        CleanToneAudioProcessor b;
        b.setStateInformation (state.getData(), (int) state.getSize());
        String bad;
        for (auto* ap : a.getParameters())
            if (auto* ra = dynamic_cast<RangedAudioParameter*> (ap))
                if (auto* rb = b.apvts.getParameter (ra->getParameterID()))
                    if (std::abs (ra->getValue() - rb->getValue()) > 1e-6f) bad << ra->getParameterID() << " ";
        check (bad.isEmpty(), "all parameters restored exactly" + (bad.isEmpty() ? String() : ": differ: " + bad));
        String rawDiffs;
        for (auto* ap : a.getParameters())
            if (auto* ra = dynamic_cast<RangedAudioParameter*> (ap))
            {
                const float va = a.apvts.getRawParameterValue (ra->getParameterID())->load();
                const float vb = b.apvts.getRawParameterValue (ra->getParameterID())->load();
                if (va != vb) rawDiffs << ra->getParameterID() << " (" << String (va, 9) << " vs " << String (vb, 9) << ") ";
            }
        check (rawDiffs.isEmpty(), "all parameters restored bit-exactly" + (rawDiffs.isEmpty() ? String() : ": " + rawDiffs));
    }
}

//==============================================================================
// Click scan (Mastering Studio method): jump each parameter once while a smooth two-tone signal plays
// through the whole plugin, with every module on. Every switch both ways, every choice to every other
// choice, every continuous control across most of its range. A click is a burst above 6 kHz (8th-order
// high-pass) in the 30 ms after the change that is far above that band before or after it.
struct ClickResult { String what; float burstDb, steadyDb; };

static std::vector<ClickResult> clickScan (bool printAll)
{
    constexpr int block = 256;
    auto renderMove = [&] (const String& id, float from, float to, float& burst, float& steady)
    {
        CleanToneAudioProcessor p;
        p.loadFactoryPreset (6);                        // Ambient Swell: compressor, warmth, delay, reverb all on
        set (p, IDs::autoLevel, 0);
        set (p, IDs::gate, 15);
        if (id != IDs::autoLevel) set (p, IDs::autoLevel, 0);
        auto* prm = p.apvts.getParameter (id);
        prm->setValueNotifyingHost (from);
        prepare (p, block);
        ct::Biquad hp[4];
        for (auto& h : hp) h.setHighPass (kSR, 6000.0, 0.7071);
        const int total = (int) (1.1 * kSR), change = ((int) (0.6 * kSR) / block) * block;
        float pre = 0, trans = 0, post = 0;
        double ph1 = 0, ph2 = 0;
        MidiBuffer midi;
        AudioBuffer<float> buf (2, block);
        ct::Biquad hpR[4];
        for (auto& h : hpR) h.setHighPass (kSR, 6000.0, 0.7071);
        for (int pos = 0; pos < total; pos += block)
        {
            if (pos == change) prm->setValueNotifyingHost (to);
            for (int i = 0; i < block; ++i)
            {
                ph1 += 80.0 / kSR; ph2 += 220.0 / kSR;
                const double fadeIn = std::min (1.0, (pos + i) / (0.05 * kSR));
                buf.setSample (0, i, (float) (fadeIn * 0.12 * (std::sin (2 * MathConstants<double>::pi * ph1) + std::sin (2 * MathConstants<double>::pi * ph2))));
                buf.setSample (1, i, 0.0f);
            }
            p.processBlock (buf, midi);
            for (int i = 0; i < block; ++i)
            {
                const int n = pos + i;
                float yl = buf.getSample (0, i), yr = buf.getSample (1, i);
                for (auto& h : hp)  yl = h.process (yl, 0);
                for (auto& h : hpR) yr = h.process (yr, 0);
                const float h = std::max (std::abs (yl), std::abs (yr));
                if (n >= (int) (0.4 * kSR) && n < change) pre = std::max (pre, h);
                else if (n >= change && n < change + (int) (0.03 * kSR)) trans = std::max (trans, h);
                else if (n >= (int) (0.9 * kSR)) post = std::max (post, h);
            }
        }
        burst = Decibels::gainToDecibels (trans, -200.0f);
        steady = Decibels::gainToDecibels (std::max (pre, post), -200.0f);
    };

    std::vector<ClickResult> clicks;
    CleanToneAudioProcessor probe;
    int moves = 0;
    for (auto* ap : probe.getParameters())
    {
        auto* rp = dynamic_cast<RangedAudioParameter*> (ap);
        if (rp == nullptr) continue;
        const auto id = rp->getParameterID();
        std::vector<std::pair<float, float>> mv;
        if (dynamic_cast<AudioParameterBool*> (rp) != nullptr) mv = { { 0.0f, 1.0f }, { 1.0f, 0.0f } };
        else if (auto* c = dynamic_cast<AudioParameterChoice*> (rp))
        {
            const int n = c->choices.size();
            for (int a = 0; a < n; ++a)
                for (int b = 0; b < n; ++b)
                    if (a != b) mv.push_back ({ rp->convertTo0to1 ((float) a), rp->convertTo0to1 ((float) b) });
        }
        else mv = { { 0.2f, 0.8f }, { 0.8f, 0.2f }, { 0.0f, 1.0f }, { 1.0f, 0.0f } };

        for (auto [from, to] : mv)
        {
            float burst, steady;
            renderMove (id, from, to, burst, steady);
            ++moves;
            const String what = rp->getName (40) + "  " + rp->getText (from, 0) + " -> " + rp->getText (to, 0);
            const bool click = burst > -72.0f && burst > steady + 12.0f;
            if (click) clicks.push_back ({ what, burst, steady });
            if (printAll || click)
                std::cout << (click ? "  CLICK " : "        ") << what.paddedRight (' ', 60) << " burst " << String (burst, 1)
                          << " dB, steady " << String (steady, 1) << " dB" << std::endl;
        }
    }
    std::cout << "    " << moves << " parameter moves scanned" << std::endl;
    return clicks;
}

// THE_LAB's high-note test: a 9999.76 Hz tone (odd FFT bin) at -6 dBFS through the plugin; returns the
// loudest non-harmonic component outside +/-5 % of the tone, in dB re the tone, and where it is.
static std::pair<float, double> worstSpur (std::function<void (CleanToneAudioProcessor&)> setup, bool stereoIn = false, double settleSec = 2.0)
{
    constexpr int order = 16, N = 1 << order;
    const double f = 13653.0 * kSR / N;
    CleanToneAudioProcessor p;
    p.loadFactoryPreset (0);
    setup (p);
    const int total = (int) (settleSec * kSR) + N;
    AudioBuffer<float> in (1, total);
    for (int i = 0; i < total; ++i) in.setSample (0, i, 0.5f * (float) std::sin (2.0 * MathConstants<double>::pi * f * i / kSR));
    AudioBuffer<float> out;
    if (! stereoIn) out = render (p, in, 512);
    else
    {
        // THE_LAB feeds the same signal to both inputs of a stereo-in / stereo-out plugin
        AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (AudioChannelSet::stereo());
        layout.outputBuses.add (AudioChannelSet::stereo());
        p.releaseResources(); p.setBusesLayout (layout);
        p.setRateAndBufferSizeDetails (kSR, 512); p.prepareToPlay (kSR, 512);
        out.setSize (2, total);
        AudioBuffer<float> blk (2, 512);
        MidiBuffer midi;
        for (int pos = 0; pos < total; pos += 512)
        {
            const int k = std::min (512, total - pos);
            blk.setSize (2, k, false, false, true);
            blk.copyFrom (0, 0, in, 0, pos, k); blk.copyFrom (1, 0, in, 0, pos, k);
            p.processBlock (blk, midi);
            for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, k);
        }
    }
    std::vector<float> d ((size_t) N * 2, 0.0f);
    for (int i = 0; i < N; ++i)
    {
        const double t = 2.0 * MathConstants<double>::pi * i / (N - 1);
        d[(size_t) i] = (float) (out.getSample (0, total - N + i) * (0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t)));
    }
    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (d.data());
    double tone = 0;
    for (int k = 13653 - 6; k <= 13653 + 6; ++k) tone = std::max (tone, (double) d[(size_t) k] * d[(size_t) k]);
    double worst = 1e-30; int wk = 0;
    for (int k = 8; k <= N / 2; ++k)
    {
        const double hz = k * kSR / N;
        if (std::abs (hz - f) < 0.05 * f) continue;
        bool harmonic = false;
        for (int h = 2; h * f < kSR / 2; ++h) if (std::abs (hz - h * f) < 6 * kSR / N) harmonic = true;
        if (harmonic) continue;
        const double pw = (double) d[(size_t) k] * d[(size_t) k];
        if (pw > worst) { worst = pw; wk = k; }
    }
    return { (float) (10.0 * std::log10 (worst / tone)), wk * kSR / N };
}
//==============================================================================
// 7) Replicas of THE_LAB's stress and software tests (the same failure modes, judged stricter).
static void labStressSection()
{
    std::cout << "\n7) THE_LAB stress replicas (Auto Level on, no waiting, as a DAW would run it)\n";
    // a hot DI (peaks -4 dBFS) so Auto Level has to re-learn the playing level while rendering
    auto hot = makeGuitar (6.0, 1.0);
    hot.applyGain (Decibels::decibelsToGain (8.0f));

    auto fresh = [] (CleanToneAudioProcessor& p)
    {
        p.loadFactoryPreset (0);
        set (p, IDs::autoLevel, std::getenv ("CTS_AUTO_OFF") ? 0.0f : 1.0f);
        if (auto* dbg = std::getenv ("CTS_EXT_SET"))   // debugging aid: "revOn=0,warmth=0"
            for (auto& kv : StringArray::fromTokens (dbg, ",", ""))
                set (p, kv.upToFirstOccurrenceOf ("=", false, false), kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue());
    };

    // 7a) memory allocation inside processBlock
    {
        CleanToneAudioProcessor p;
        fresh (p);
        p.loadFactoryPreset (6);
        set (p, IDs::autoLevel, 1);
        prepare (p, 512);
        MidiBuffer midi;
        AudioBuffer<float> blk (2, 512);
        long worst = 0; int buffers = 0, dirty = 0;
        auto* bass = p.apvts.getParameter (IDs::bass);
        auto* dly = p.apvts.getParameter (IDs::dlyOn);
        for (int pos = 0, b = 0; pos + 512 <= hot.getNumSamples(); pos += 512, ++b)
        {
            if (b % 37 == 20) bass->setValueNotifyingHost (bass->getValue() > 0.5f ? 0.3f : 0.7f);   // knob moves -> re-measure
            if (b % 101 == 50) dly->setValueNotifyingHost (dly->getValue() > 0.5f ? 0.0f : 1.0f);    // delay wakes up
            blk.copyFrom (0, 0, hot, 0, pos, 512);
            blk.clear (1, 0, 512);
            gAllocs = 0;
            gCountAllocs = true;
            p.processBlock (blk, midi);
            gCountAllocs = false;
            if (b * 512 >= (int) kSR)        // the first second is lazy setup
            {
                ++buffers;
                if (gAllocs.load() > 0) ++dirty;
                worst = std::max (worst, gAllocs.load());
            }
        }
        check (dirty == 0, "no memory allocation on the audio thread (" + String (dirty) + " of " + String (buffers)
                           + " buffers allocated; knob moves, Auto Level re-measuring and the delay switching included)");
    }

    // 7b) Auto Level never makes the sound depend on the buffer size (a fresh copy per run, as THE_LAB does)
    {
        auto run = [&] (int prepared, std::function<int()> nextSize, bool zeroBlocks)
        {
            CleanToneAudioProcessor p;
            fresh (p);
            prepare (p, prepared);
            AudioBuffer<float> out (2, hot.getNumSamples());
            MidiBuffer midi;
            AudioBuffer<float> blk (2, prepared), empty (2, 0);
            for (int pos = 0; pos < hot.getNumSamples();)
            {
                const int k = std::min (nextSize(), hot.getNumSamples() - pos);
                blk.setSize (2, k, false, false, true);
                blk.copyFrom (0, 0, hot, 0, pos, k);
                blk.clear (1, 0, k);
                if (zeroBlocks) p.processBlock (empty, midi);
                p.processBlock (blk, midi);
                for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, k);
                pos += k;
            }
            return out;
        };
        const auto ref = run (512, [] { return 512; }, false);
        Random r (11);
        struct Case { const char* name; int prepared; std::function<int()> size; bool zeros; };
        const Case cases[] = { { "64", 64, [] { return 64; }, false },
                               { "uneven 1..512", 512, [&r] { return 1 + r.nextInt (512); }, false },
                               { "8192", 8192, [] { return 8192; }, false },
                               { "512 with 0-sample buffers in between", 512, [] { return 512; }, true } };
        float worst = -300; String worstName;
        for (auto& c : cases)
        {
            const float d = diffDb (ref, run (c.prepared, c.size, c.zeros));
            if (d > worst) { worst = d; worstName = c.name; }
        }
        check (worst < -120.0f, "buffer sizes with Auto Level on: identical (worst " + String (worst, 1) + " dB, " + worstName + ")");
    }

    // 7c) two copies running side by side don't disturb each other
    {
        CleanToneAudioProcessor solo;
        fresh (solo);
        const auto ref = render (solo, hot, 512);

        CleanToneAudioProcessor a, b;
        fresh (a); b.loadFactoryPreset (6);
        prepare (a, 512); prepare (b, 512);
        const auto other = makeGuitar (6.0, 1.0, 0.002f);
        AudioBuffer<float> out (2, hot.getNumSamples());
        MidiBuffer midi;
        AudioBuffer<float> blkA (2, 512), blkB (2, 512);
        Random r (5);
        auto* bp = &b;
        std::unique_ptr<CleanToneAudioProcessor> dummy;
        for (int pos = 0, k = 0; pos + 512 <= hot.getNumSamples(); pos += 512, ++k)
        {
            blkA.copyFrom (0, 0, hot, 0, pos, 512); blkA.clear (1, 0, 512);
            a.processBlock (blkA, midi);
            for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blkA, c, 0, 512);
            if (bp != nullptr)
            {
                if (k % 20 == 3)
                    for (int m = 0; m < 12; ++m)
                        if (auto* prm = bp->getParameters()[r.nextInt (bp->getParameters().size())]) prm->setValueNotifyingHost (r.nextFloat());
                blkB.copyFrom (0, 0, other, 0, pos, 512); blkB.clear (1, 0, 512);
                bp->processBlock (blkB, midi);
                if (k == 400) { bp->releaseResources(); bp = nullptr; }      // the other copy closes
            }
        }
        const int n = (hot.getNumSamples() / 512) * 512;
        AudioBuffer<float> refT (2, n), outT (2, n);
        for (int c = 0; c < 2; ++c) { refT.copyFrom (c, 0, ref, c, 0, n); outT.copyFrom (c, 0, out, c, 0, n); }
        const float d = diffDb (refT, outT);
        check (d < -120.0f, "two copies playing together: each sounds exactly as when alone (difference " + String (d, 1) + " dB)");
    }

    // 7d) recall in the same copy: save, move every control, load back - same sound immediately
    {
        CleanToneAudioProcessor p;
        fresh (p);
        p.loadFactoryPreset (6);
        set (p, IDs::autoLevel, std::getenv ("CTS_RECALL_AUTO_OFF") ? 0.0f : 1.0f);
        if (auto* dbg = std::getenv ("CTS_RECALL_SET"))
            for (auto& kv : StringArray::fromTokens (dbg, ",", ""))
                set (p, kv.upToFirstOccurrenceOf ("=", false, false), kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue());
        prepare (p, 512);
        render (p, hot, 512, {}, false);                     // learn the hot playing level
        MemoryBlock st;
        p.getStateInformation (st);
        std::map<String, float> savedVals;
        for (auto* ap : p.getParameters())
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (ap)) savedVals[rp->getParameterID()] = rp->getValue();
        const auto ref = render (p, hot, 512);               // reference from a clean start
        const float corrRef = p.getAutoLevelCorrectionDb();
        const auto memRef = std::getenv ("CTS_RECALL_DBG") ? String (corrRef, 6) : String();
        Random r (3);
        if (! std::getenv ("CTS_RECALL_NOMOVES"))
            for (auto* prm : p.getParameters()) prm->setValueNotifyingHost (r.nextFloat());
        if (! std::getenv ("CTS_RECALL_NORENDER")) render (p, hot, 512, {}, false);
        p.setStateInformation (st.getData(), (int) st.getSize());
        const auto out = render (p, hot, 512);
        const float d = diffDb (ref, out);
        if (std::getenv ("CTS_RECALL_DBG"))
        {
            std::cout << "      recall diff per second:";
            for (int s0 = 0; s0 + (int) kSR <= out.getNumSamples(); s0 += (int) kSR)
            {
                float md = 0, mc = 1e-9f; int at = 0;
                for (int c = 0; c < 2; ++c)
                    for (int i = s0; i < s0 + (int) kSR; ++i)
                    { const float e = std::abs (out.getSample (c, i) - ref.getSample (c, i)); if (e > md) { md = e; at = i; } mc = std::max (mc, std::abs (ref.getSample (c, i))); }
                std::cout << " " << String (Decibels::gainToDecibels (md / mc, -200.0f), 0);
            }
            int first = -1;
            for (int i = 0; i < out.getNumSamples() && first < 0; ++i) if (out.getSample (0, i) != ref.getSample (0, i)) first = i;
            std::cout << "  (first difference at sample " << first << ")  correction ref " << memRef << " out " << String (p.getAutoLevelCorrectionDb(), 6) << "\n";
            for (auto* ap : p.getParameters())
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (ap))
                    if (savedVals[rp->getParameterID()] != rp->getValue())
                        std::cout << "        differs: " << rp->getParameterID() << " saved " << savedVals[rp->getParameterID()] << " now " << rp->getValue() << "\n";
            std::cout << "        (all others identical)\n";
        }
        check (d < -120.0f, "state loaded back into the same copy after moving every control: identical from the first sample ("
                            + String (d, 1) + " dB)");
    }

    // 7e) every continuous control to 0 % and 100 % while playing, then back: nothing stays broken
    {
        const auto loop = makeGuitar (4.0, 0.5);
        AudioBuffer<float> in (1, loop.getNumSamples() * 3);
        for (int k = 0; k < 3; ++k) in.copyFrom (0, k * loop.getNumSamples(), loop, 0, 0, loop.getNumSamples());
        const int seg = loop.getNumSamples();
        // THE_LAB's method: ONE copy, prepared before every run. Anything the plugin carries over from
        // earlier runs (learned levels, caches, modulation) shows up here as a difference.
        CleanToneAudioProcessor shared;
        fresh (shared);
        const auto control = render (shared, in, 512);
        {
            fresh (shared);
            const auto again = render (shared, in, 512);
            const float rr = diffDb (control, again);
            check (rr < -120.0f, "same copy, same signal, run twice: identical (" + String (rr, 1) + " dB)");
            // THE_LAB's music-like test is far hotter than a guitar; play it, then the normal signal again
            fresh (shared);
            AudioBuffer<float> loud (in); loud.applyGain (Decibels::decibelsToGain (14.0f));
            render (shared, loud, 512);
            fresh (shared);
            const auto after = render (shared, in, 512);
            const float h = diffDb (control, after);
            check (h < -120.0f, "after playing something much louder, the same copy sounds exactly as before (" + String (h, 1) + " dB)");
        }
        String broken; float loudest = -200; int tested = 0;
        CleanToneAudioProcessor probe;
        for (auto* ap : probe.getParameters())
        {
            auto* rp = dynamic_cast<RangedAudioParameter*> (ap);
            if (rp == nullptr || rp->isDiscrete() || rp->isBoolean()) continue;
            const auto id = rp->getParameterID();
            if (auto* only = std::getenv ("CTS_EXT_ONLY")) if (id != String (only)) continue;
            for (float extreme : { 0.0f, 1.0f })
            {
                auto& p = shared;
                fresh (p);
                auto* prm = p.apvts.getParameter (id);
                const float normal = prm->getValue();
                const auto out = render (p, in, 512, [&] (int pos)
                {
                    if (pos == ((seg / 3) / 512) * 512) prm->setValueNotifyingHost (extreme);          // to the extreme...
                    if (pos == ((2 * seg / 3) / 512) * 512) prm->setValueNotifyingHost (normal);       // ...and back
                });
                ++tested;
                loudest = std::max (loudest, Decibels::gainToDecibels (std::max (out.getMagnitude (0, 0, out.getNumSamples()),
                                                                                   out.getMagnitude (1, 0, out.getNumSamples())), -200.0f));
                const float d = diffDb (control, out, 2 * seg);             // the last pass of the loop
                if (std::getenv ("CTS_EXT_ONLY"))
                {
                    std::cout << "      " << rp->getName (20) << (extreme > 0.5f ? " 100%" : " 0%") << " diff per second:";
                    for (int s0 = 0; s0 + (int) kSR <= out.getNumSamples(); s0 += (int) kSR)
                    {
                        float md = 0, mc = 1e-9f;
                        for (int c = 0; c < 2; ++c)
                            for (int i = s0; i < s0 + (int) kSR; ++i)
                            { md = std::max (md, std::abs (out.getSample (c, i) - control.getSample (c, i))); mc = std::max (mc, std::abs (control.getSample (c, i))); }
                        std::cout << " " << String (Decibels::gainToDecibels (md / mc, -200.0f), 0);
                    }
                    std::cout << "\n";
                }
                if (! allFinite (out) || d > -60.0f)
                    broken << rp->getName (30) << (extreme > 0.5f ? " 100%" : " 0%") << " (" << String (d, 1) << " dB)  ";
            }
        }
        check (broken.isEmpty(), "every continuous control at 0 % and 100 % then back, one copy throughout: fully recovers (" + String (tested)
                                 + " moves" + (broken.isEmpty() ? String() : "; still different: " + broken) + ")");
        check (loudest < 30.0f, "loudest output at any extreme " + String (loudest, 1) + " dBFS (limit +30)");
    }

    // 7f) left and right are processed identically, and don't leak into each other
    {
        CleanToneAudioProcessor p;
        auto stereoRun = [&] (bool effects, float lGain, float rGain)
        {
            fresh (p);
            if (! effects) { set (p, IDs::dlyOn, 0); set (p, IDs::revOn, 0); }
            AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (AudioChannelSet::stereo());
            layout.outputBuses.add (AudioChannelSet::stereo());
            p.releaseResources(); p.setBusesLayout (layout);
            p.setRateAndBufferSizeDetails (kSR, 512); p.prepareToPlay (kSR, 512);
            const auto g = makeGuitar (3.0, 0.5);
            AudioBuffer<float> out (2, g.getNumSamples()), blk (2, 512);
            MidiBuffer midi;
            for (int pos = 0; pos + 512 <= g.getNumSamples(); pos += 512)
            {
                blk.copyFrom (0, 0, g, 0, pos, 512); blk.applyGain (0, 0, 512, lGain);
                blk.copyFrom (1, 0, g, 0, pos, 512); blk.applyGain (1, 0, 512, rGain);
                p.processBlock (blk, midi);
                for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, 512);
            }
            return out;
        };
        const auto same = stereoRun (false, 1.0f, 1.0f);
        bool identical = true;
        for (int i = 0; i < same.getNumSamples(); ++i) identical = identical && same.getSample (0, i) == same.getSample (1, i);
        const auto leftOnly = stereoRun (false, 1.0f, 0.0f);
        const float leak = leftOnly.getMagnitude (1, 0, leftOnly.getNumSamples());
        check (identical, "same signal in both inputs (delay/reverb off): left and right outputs bit-identical");
        check (leak == 0.0f, "left-only input (delay/reverb off): right output exactly silent");
        const auto fx = stereoRun (true, 1.0f, 1.0f);
        double sideE = 0, midE = 0;
        for (int i = 0; i < fx.getNumSamples(); ++i)
        {
            const double l = fx.getSample (0, i), r = fx.getSample (1, i);
            sideE += (l - r) * (l - r); midE += (l + r) * (l + r);
        }
        const auto spur = worstSpur ([] (CleanToneAudioProcessor&) {});
        check (spur.first < -90.0f, "high notes: no aliasing or noise spurs below a 10 kHz tone at the defaults (worst "
                                    + String (spur.first, 1) + " dB at " + String (spur.second, 0) + " Hz; THE_LAB flags above -60)");
        // THE_LAB analyses right after the tone starts. With the reverb off there must be nothing even then
        // (aliasing would be there from the first sample); with it on, the reverb rings out the tone's onset
        // click, decaying with the reverb - reported, not a fault.
        const auto early = worstSpur ([] (CleanToneAudioProcessor& p) { set (p, IDs::revOn, 0); }, true, 0.0);
        check (early.first < -100.0f, "high notes, stereo in, analysed from the first sample, reverb off: no aliasing (worst "
                                      + String (early.first, 1) + " dB)");
        const auto ring0 = worstSpur ([] (CleanToneAudioProcessor&) {}, true, 0.0), ring1 = worstSpur ([] (CleanToneAudioProcessor&) {}, true, 1.0);
        std::cout << "  info  with the reverb on, analysed from the first sample: " << String (ring0.first, 1) << " dB at " << String (ring0.second, 0)
                  << " Hz, 1 s later " << String (ring1.first, 1) << " dB - the reverb's tail of the test tone's onset, decaying with the reverb" << std::endl;
        std::cout << "  info  with the reverb on (stereo effect), side vs mid: "
                  << String (10.0 * std::log10 (sideE / (midE + 1e-30)), 1) << " dB - the reverb's own stereo width" << std::endl;
    }
}

//==============================================================================
// --snapshots <dir>: renders the editor in several states to PNGs (UI review without a window).
static int snapshots (const File& dir)
{
    dir.createDirectory();
    CleanToneAudioProcessor p;
    auto save = [&dir] (const Image& img, const String& name)
    {
        auto f = dir.getChildFile (name);
        f.deleteFile();
        if (auto os = f.createOutputStream()) PNGImageFormat().writeImageToStream (img, *os);
    };
    std::unique_ptr<AudioProcessorEditor> e (p.createEditor());
    save (e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f), "01_default.png");

    p.loadFactoryPreset (6);
    set (p, IDs::compOn, 0);
    set (p, IDs::bass, 4);
    e.reset (p.createEditor());
    save (e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f), "02_ambient_compressor_off_edited.png");

    for (auto* holder : e->getChildren())          // the controls live in the scaled body component
        for (auto* c : holder->getChildren())
            if (auto* b = dynamic_cast<TextButton*> (c))
                if (b->isVisible() && b->getButtonText() == "Save" && b->onClick) { b->onClick(); break; }
    save (e->createComponentSnapshot ({ 0, 0, e->getWidth(), 60 }, true, 1.0f), "03_save_bar.png");

    // IR loader in use, and the window enlarged (everything scales)
    {
        std::vector<float> ir ((size_t) (0.1 * 48000.0));
        Random rng (7);
        for (size_t i = 0; i < ir.size(); ++i) ir[i] = (rng.nextFloat() * 2.0f - 1.0f) * std::exp (-(float) i / 900.0f);
        p.setIRSamples (0, "Celestion V30 SM57", 48000.0, ir);
        p.setIRSamples (1, "Jensen C10Q Royer 121", 48000.0, ir);
        set (p, IDs::cab, (float) ct::kCabIR);
        set (p, IDs::compLook, 1);
        set (p, IDs::dlyStyle, 1);
        e.reset (p.createEditor());
        e->setSize (e->getWidth() * 3 / 2, e->getHeight() * 3 / 2);
        save (e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f), "04_ir_loader_large.png");
    }
    std::cout << "snapshots written to " << dir.getFullPathName() << std::endl;
    return 0;
}

//==============================================================================
// 8) The 1.4 modules: IR convolution and recall, reverb decay, compressor, lookahead, tape modulation,
//    the warmth stage, and the resizable / maximizable window.
static std::vector<float> testIR (int len, int seed, float decaySamples)
{
    std::vector<float> ir ((size_t) len);
    Random rng (seed);
    for (int i = 0; i < len; ++i) ir[(size_t) i] = (rng.nextFloat() * 2.0f - 1.0f) * std::exp (-(float) i / decaySamples);
    return ir;
}

static void setNorm (CleanToneAudioProcessor& p, const char* id, float norm) { p.apvts.getParameter (id)->setValueNotifyingHost (norm); }

// Schroeder backward integration: RT60 from the slope between -5 and -25 dB of the energy decay
static double rt60 (const std::vector<double>& e2, double fs, int from)
{
    std::vector<double> sch (e2.size() + 1, 0.0);
    for (int i = (int) e2.size() - 1; i >= from; --i) sch[(size_t) i] = sch[(size_t) i + 1] + e2[(size_t) i];
    const double ref = sch[(size_t) from];
    int i5 = -1, i25 = -1;
    for (int i = from; i < (int) e2.size(); ++i)
    {
        const double db = 10.0 * std::log10 (sch[(size_t) i] / ref + 1.0e-300);
        if (i5 < 0 && db <= -5.0) i5 = i;
        if (db <= -25.0) { i25 = i; break; }
    }
    if (i5 < 0 || i25 < 0) return -1.0;
    return 3.0 * (i25 - i5) / fs;
}

// level of the harmonics 2..5 relative to the fundamental (dB), over a whole number of cycles
static double thdDb (const std::vector<float>& y, int from, int len, double f, double fs)
{
    auto amp = [&] (double fr)
    {
        double c = 0, s = 0;
        for (int i = 0; i < len; ++i)
        {
            const double ph = 2.0 * MathConstants<double>::pi * fr * i / fs;
            c += y[(size_t) (from + i)] * std::cos (ph); s += y[(size_t) (from + i)] * std::sin (ph);
        }
        return std::sqrt (c * c + s * s);
    };
    const double a1 = amp (f);
    double h = 0;
    for (int k = 2; k <= 5; ++k) { const double a = amp (f * k); h += a * a; }
    return 10.0 * std::log10 (h / (a1 * a1) + 1.0e-30);
}

static void modulesSection()
{
    std::cout << "\n8) The 1.4 modules (IR loader, reverb, compressor, delay modulation, warmth, window)\n";

    // --- convolution: exact against direct convolution, zero latency, at every rate ---
    {
        float worst = -300.0f;
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            const int B = ct::IRSpectra::blockSizeFor (fs);
            const auto ir = testIR ((int) (0.3 * fs), 11, (float) (0.05 * fs));
            const auto spec = ct::IRSpectra::build (ir, fs, 1);
            juce::dsp::FFT fft (ct::IRSpectra::fftOrderFor (B));
            ct::PartConvolver pc;
            pc.prepare (B, spec->numParts + 1, &fft);
            pc.setIR (spec.get());
            const int n = (int) (0.5 * fs);
            Random rng (3);
            std::vector<float> x ((size_t) n);
            for (auto& v : x) v = rng.nextFloat() * 2.0f - 1.0f;
            double maxErr = 0, maxRef = 1e-30;
            for (int i = 0; i < n; ++i)
            {
                const float y = pc.process (x[(size_t) i]);
                double ref = 0;
                for (int k = 0; k <= i && k < (int) ir.size(); ++k) ref += (double) ir[(size_t) k] * x[(size_t) (i - k)];
                maxErr = std::max (maxErr, std::abs ((double) y - ref));
                maxRef = std::max (maxRef, std::abs (ref));
            }
            worst = std::max (worst, (float) (20.0 * std::log10 (maxErr / maxRef + 1e-30)));
        }
        check (worst < -100.0f, "IR convolution matches exact convolution with zero latency at 44.1/48/96 kHz (error " + String (worst, 1) + " dB)");
    }

    kSR = 48000.0;
    auto irSetup = [] (CleanToneAudioProcessor& p)
    {
        resetToBaseline (p);
        p.setIRSamples (0, "Test A", 44100.0, testIR ((int) (0.2 * 44100), 21, 2000.0f));   // resampled to 48 kHz
        p.setIRSamples (1, "Test B", 48000.0, testIR ((int) (0.1 * 48000), 22, 900.0f));
        set (p, IDs::cab, (float) ct::kCabIR);
        set (p, IDs::irBlend, 30);
    };
    const auto guitar = makeGuitar (4.0, 1.0);

    // --- IR loader in the plugin: buffer sizes, recall, old sessions, presets ---
    {
        CleanToneAudioProcessor p;
        irSetup (p);
        const auto a = render (p, guitar, 512);
        const auto b = render (p, guitar, 64);
        const float d = diffDb (a, b);
        check (d < -120.0f, "IR loader: identical at buffer 512 and 64 (" + String (d, 1) + " dB)");
        set (p, IDs::cab, 0);
        const auto dry = render (p, guitar, 512);
        check (diffDb (a, dry) > -20.0f, "IR loader: the IRs are actually heard");
        set (p, IDs::cab, (float) ct::kCabIR);

        MemoryBlock st;
        p.getStateInformation (st);
        CleanToneAudioProcessor q;
        q.setStateInformation (st.getData(), (int) st.getSize());
        check (q.hasIR (0) && q.hasIR (1) && q.getIRName (0) == "Test A" && q.getIRName (1) == "Test B",
               "IR loader: both IRs and their names come back with the project");
        const auto qa = render (q, guitar, 512);
        const auto pa = render (p, guitar, 512);
        const float r = diffDb (pa, qa);
        check (r < -120.0f, "IR loader: reloaded project sounds identical (" + String (r, 1) + " dB)");

        // a session saved before 1.4 (4 cabinet choices, no IRs): "Closed 4x12" must stay "Closed 4x12"
        if (auto xml = AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize()))
        {
            for (int i = xml->getNumChildElements(); --i >= 0;)
                if (xml->getChildElement (i)->hasTagName ("IR")) xml->removeChildElement (xml->getChildElement (i), true);
            for (auto* e : xml->getChildIterator())
                if (e->getStringAttribute ("id") == IDs::cab) { e->setAttribute ("value", 3.0); e->setAttribute ("norm", 1.0); }
            MemoryBlock old;
            AudioProcessor::copyXmlToBinary (*xml, old);
            CleanToneAudioProcessor o;
            o.setIRSamples (0, "stale", 48000.0, testIR (1000, 5, 100.0f));
            o.setStateInformation (old.getData(), (int) old.getSize());
            check ((int) get (o, IDs::cab) == 3 && ! o.hasIR (0) && ! o.hasIR (1),
                   "older projects: Cabinet comes back as saved (Closed 4x12), IR slots empty (cab " + String (get (o, IDs::cab)) + ")");
        }

        const bool saved = p.saveUserPreset ("IR recall test");
        p.clearIR (0); p.clearIR (1);
        const bool loaded = p.loadUserPreset ("IR recall test");
        check (saved && loaded && p.hasIR (0) && p.hasIR (1) && p.getIRName (1) == "Test B", "IR loader: user presets keep their IRs");
        const auto pr = render (p, guitar, 512);
        check (diffDb (pa, pr) < -120.0f, "IR loader: the preset sounds identical to before (" + String (diffDb (pa, pr), 1) + " dB)");
        p.deleteUserPreset ("IR recall test");
    }

    // --- changing IRs while playing: no allocation on the audio thread, no clicks ---
    {
        CleanToneAudioProcessor p;
        irSetup (p);
        set (p, IDs::revOn, 0); set (p, IDs::dlyOn, 0);
        const auto sine = makeSine (3.0, 110.0f, 0.25f);
        const int bs = 256;
        prepare (p, bs);
        AudioBuffer<float> out (2, sine.getNumSamples());
        MidiBuffer midi;
        long allocs = 0;
        const auto otherIR = testIR ((int) (0.25 * 48000), 31, 3000.0f);
        for (int pos = 0, blk = 0; pos < sine.getNumSamples(); pos += bs, ++blk)
        {
            const int n = std::min (bs, sine.getNumSamples() - pos);
            if (blk == 200) p.setIRSamples (0, "Other", 48000.0, otherIR);   // message thread, between blocks
            if (blk == 350) p.clearIR (1);
            AudioBuffer<float> buf (2, n);
            buf.copyFrom (0, 0, sine, 0, pos, n);
            buf.clear (1, 0, n);
            gAllocs = 0; gCountAllocs = true;
            p.processBlock (buf, midi);
            gCountAllocs = false; allocs += gAllocs;
            out.copyFrom (0, pos, buf, 0, 0, n);
            out.copyFrom (1, pos, buf, 1, 0, n);
        }
        check (allocs == 0, "IR loader: swapping and removing IRs while playing allocates nothing on the audio thread (" + String ((int) allocs) + ")");
        const float c = clickDb (out, (int) (0.5 * kSR));
        check (c < -80.0f, "IR loader: swapping and removing IRs while playing is click-free (" + String (c, 1) + " dB, allowed -80)");
    }

    // --- reverb: Decay means the real RT60; highs die about 4x faster ---
    {
        String table;
        bool ok = true;
        for (float decay : { 1.0f, 2.5f, 6.0f })
        {
            ct::CleanReverb rv;
            rv.prepare (kSR);
            rv.setParams (0.5f, decay, 0.0f, 1000.0f, 20.0f, 1.0f, true);
            const int n = (int) ((decay * 2.0 + 1.0) * kSR);
            std::vector<float> L ((size_t) n, 0.0f), R ((size_t) n, 0.0f);
            L[0] = R[0] = 1.0f;
            for (int pos = 0; pos < n; pos += 512) rv.process (L.data() + pos, R.data() + pos, std::min (512, n - pos));
            ct::Biquad lo1, lo2, hi1, hi2;
            lo1.setLowPass (kSR, 300.0, 0.7071); lo2.setLowPass (kSR, 300.0, 0.7071);
            hi1.setHighPass (kSR, 2500.0, 0.7071); hi2.setHighPass (kSR, 2500.0, 0.7071);
            std::vector<double> eLo ((size_t) n), eHi ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const float a = lo2.process (lo1.process (L[(size_t) i], 0), 0);
                const float b = hi2.process (hi1.process (L[(size_t) i], 0), 0);
                eLo[(size_t) i] = (double) a * a; eHi[(size_t) i] = (double) b * b;
            }
            const double tLo = rt60 (eLo, kSR, (int) (0.08 * kSR)), tHi = rt60 (eHi, kSR, (int) (0.08 * kSR));
            table << "Decay " << String (decay, 1) << " s -> RT60 lows " << String (tLo, 2) << " s, highs " << String (tHi, 2) << " s;  ";
            ok = ok && std::abs (tLo / decay - 1.0) < 0.15 && tHi > 0.1 * decay && tHi < 0.45 * decay;
        }
        check (ok, "reverb: measured decay matches the Decay knob within 15 %, highs decay faster (" + table.trimEnd() + ")");
    }

    // --- compressor: low notes don't over-compress; opto two-stage release that always ends ---
    {
        auto grFor = [] (float freq, double onSec, double offSec, double readAfterOff, float sustain = 0.7f, float amp = 0.5f)
        {
            ct::SustainCompressor c;
            c.prepare (kSR);
            c.setParams (sustain, 10.0f, 1.0f, true, false);
            const int on = (int) (onSec * kSR), total = (int) ((onSec + offSec) * kSR), readAt = on + (int) (readAfterOff * kSR);
            std::vector<float> x ((size_t) 64);
            float gr = 0.0f; double grSum = 0; int grN = 0;
            for (int pos = 0; pos < total; pos += 64)
            {
                for (int i = 0; i < 64; ++i)
                    x[(size_t) i] = pos + i < on ? amp * (float) std::sin (2.0 * MathConstants<double>::pi * freq * (pos + i) / kSR) : 0.0f;
                float* ch[1] = { x.data() };
                c.process (ch, 1, 64);
                if (readAfterOff < 0.0 && pos > on - (int) (0.3 * kSR) && pos < on) { grSum += c.getGainReductionDb(); ++grN; }
                if (pos <= readAt && readAt < pos + 64) gr = c.getGainReductionDb();
            }
            return readAfterOff < 0.0 ? (float) (grSum / std::max (1, grN)) : gr;
        };
        const float gr60 = grFor (60.0f, 1.5, 0.0, -1.0), gr1k = grFor (1000.0f, 1.5, 0.0, -1.0);
        check (gr60 < gr1k - 1.5f, "compressor: the sidechain high-pass keeps low notes from over-compressing (60 Hz " + String (gr60, 1)
                                   + " dB vs 1 kHz " + String (gr1k, 1) + " dB of reduction)");
        // moderate compression (heavy squeezing already releases slowly on its own)
        const float shortNote = grFor (1000.0f, 0.15, 2.0, 0.8, 0.4f, 0.2f), longNote = grFor (1000.0f, 4.0, 2.0, 0.8, 0.4f, 0.2f);
        check (longNote > shortNote + 1.0f, "compressor: opto release - quick after a short note, a longer tail after sustained playing ("
                                            + String (shortNote, 2) + " vs " + String (longNote, 2) + " dB, 0.8 s after)");
        const float ended = grFor (1000.0f, 4.0, 8.0, 7.0);
        check (ended < 0.01f, "compressor: the release tail always finishes (" + String (ended, 3) + " dB left after 7 s)");
    }

    // --- lookahead: the extra latency is reported exactly ---
    {
        CleanToneAudioProcessor p;
        resetToBaseline (p);
        for (auto [id, v] : std::initializer_list<std::pair<const char*, float>> {
                 { IDs::revOn, 0 }, { IDs::dlyOn, 0 }, { IDs::warmth, 0 }, { IDs::cab, 0 }, { IDs::pickup, 2 },
                 { IDs::bass, 0 }, { IDs::mid, 0 }, { IDs::presence, 0 }, { IDs::treble, 0 }, { IDs::lowCut, 20 }, { IDs::highCut, 20000 } })
            set (p, id, v);
        AudioBuffer<float> imp (1, (int) kSR);
        imp.clear();
        imp.setSample (0, (int) (0.5 * kSR), 0.05f);
        int lat[2] {}, peak[2] {};
        for (int la = 0; la < 2; ++la)
        {
            set (p, IDs::compLook, (float) la);
            const auto out = render (p, imp, 256);
            lat[la] = p.getLatencySamples();
            float best = 0;
            for (int i = 0; i < out.getNumSamples(); ++i)
                if (std::abs (out.getSample (0, i)) > best) { best = std::abs (out.getSample (0, i)); peak[la] = i - (int) (0.5 * kSR); }
        }
        check (lat[1] - lat[0] == ct::SustainCompressor::lookaheadSamples (kSR) && std::abs (peak[1] - lat[1]) <= 1 && std::abs (peak[0] - lat[0]) <= 1,
               "lookahead: latency reported to the host matches the audio (" + String (lat[0]) + " -> " + String (lat[1])
               + " samples; measured " + String (peak[0]) + " -> " + String (peak[1]) + ")");
    }

    // --- delay modulation: Tape style is repeatable, buffer-size independent, and different from Sine ---
    {
        auto setup = [] (CleanToneAudioProcessor& p, int style)
        {
            resetToBaseline (p);
            set (p, IDs::dlyOn, 1); set (p, IDs::dlyStyle, (float) style);
            setNorm (p, IDs::dlyMod, 1.0f); set (p, IDs::dlyMix, 40); set (p, IDs::dlyFeedback, 40);
        };
        CleanToneAudioProcessor a, b, c;
        setup (a, 1); setup (b, 1); setup (c, 0);
        const auto ta = render (a, guitar, 512);
        const auto tb = render (b, guitar, 64);
        const auto ta2 = render (a, guitar, 1024);
        const auto sc = render (c, guitar, 512);
        check (diffDb (ta, tb) < -120.0f && diffDb (ta, ta2) < -120.0f,
               "delay Tape mode: identical every run and at every buffer size (" + String (std::max (diffDb (ta, tb), diffDb (ta, ta2)), 1) + " dB)");
        check (diffDb (ta, sc) > -40.0f, "delay Tape mode sounds different from Sine (" + String (diffDb (ta, sc), 1) + " dB apart)");
    }

    // --- warmth: lows are thickened more than highs ---
    {
        auto thdAt = [] (double f)
        {
            ct::Warmth w;
            w.prepare (kSR, 512, 1);
            w.setAmount (1.0f);
            const int n = (int) kSR;
            std::vector<float> y ((size_t) n);
            for (int i = 0; i < n; ++i) y[(size_t) i] = 0.5f * (float) std::sin (2.0 * MathConstants<double>::pi * f * i / kSR);
            for (int pos = 0; pos < n; pos += 512) { float* ch[1] = { y.data() + pos }; w.process (ch, 1, std::min (512, n - pos)); }
            return thdDb (y, n / 2, n / 2, f, kSR);
        };
        const double lo = thdAt (100.0), hi = thdAt (1600.0);
        check (lo > hi + 3.0, "warmth: frequency-dependent - more harmonics on low notes than high ones (100 Hz " + String (lo, 1)
                              + " dB, 1.6 kHz " + String (hi, 1) + " dB)");
    }


    // --- 1.4.1: ALIGN (minimum-phase IRs), old projects keep their IR timing, Auto Level at every rate ---
    {
        // cabinet-like IRs: a resonant low end, a speaker roll-off, a mic lead-in; A is cut short like many IR packs
        auto cabIR = [] (int lead, int len, double resHz, double q, double lpHz, int seed)
        {
            std::vector<float> h ((size_t) len, 0.0f);
            ct::Biquad hp, lp, pk;
            hp.setHighPass (48000.0, resHz, q); lp.setLowPass (48000.0, lpHz, 0.7071); pk.setPeak (48000.0, 2500.0, 1.2, 5.0);
            Random rng (seed);
            for (int i = 0; i < len; ++i)
            {
                float x = i == lead ? 1.0f : (i > lead && i < lead + 40 ? 0.15f * (rng.nextFloat() * 2 - 1) * std::exp (-(float) (i - lead) / 8.0f) : 0.0f);
                h[(size_t) i] = pk.process (lp.process (hp.process (x, 0), 0), 0);
            }
            return h;
        };
        const auto A = ct::prepareIR (cabIR (7, 1000, 95.0, 1.6, 5200.0, 1), 48000.0, 48000.0);
        const auto B = ct::prepareIR (cabIR (11, 9600, 75.0, 2.2, 4300.0, 2), 48000.0, 48000.0);
        const auto mA = ct::minimumPhase (A), mB = ct::minimumPhase (B);
        auto resp = [] (const std::vector<float>& h, double f)
        {
            std::complex<double> acc;
            for (size_t n = 0; n < h.size(); ++n) acc += (double) h[n] * std::polar (1.0, -2.0 * MathConstants<double>::pi * f * (double) n / 48000.0);
            return acc;
        };
        double magErr = 0, worstRaw = 0, worstAligned = 0;
        for (double f = 30.0; f < 8000.0; f *= 1.04)
        {
            const double a = std::abs (resp (A, f)), b = std::abs (resp (B, f));
            magErr = std::max ({ magErr, std::abs (20.0 * std::log10 (std::abs (resp (mA, f)) / a)), std::abs (20.0 * std::log10 (std::abs (resp (mB, f)) / b)) });
            const double quieter = std::min (a, b);
            worstRaw = std::min (worstRaw, 20.0 * std::log10 (std::abs (0.5 * resp (A, f) + 0.5 * resp (B, f)) / quieter));
            worstAligned = std::min (worstAligned, 20.0 * std::log10 (std::abs (0.5 * resp (mA, f) + 0.5 * resp (mB, f)) / quieter));
        }
        int pA = 0, pB = 0;
        for (int i = 0; i < 64; ++i) { if (std::abs (mA[(size_t) i]) > std::abs (mA[(size_t) pA])) pA = i; if (std::abs (mB[(size_t) i]) > std::abs (mB[(size_t) pB])) pB = i; }
        check (magErr < 0.1, "ALIGN keeps each IR's frequency response (within " + String (magErr, 3) + " dB, 30 Hz - 8 kHz)");
        check (pA <= 2 && pB <= 2, "ALIGN removes the mic lead-in (main peak at sample " + String (pA) + " and " + String (pB) + ", was 7 and 11)");
        check (worstAligned > -3.0, "ALIGN: a 50/50 blend never cancels (worst " + String (worstAligned, 1) + " dB vs the quieter IR; as recorded: "
                                    + String (worstRaw, 1) + " dB)");

        // in the plugin: ALIGN on = no added delay; recall; projects saved by 1.4.0 keep their IRs as recorded
        CleanToneAudioProcessor p;
        resetToBaseline (p);
        for (auto [id, v] : std::initializer_list<std::pair<const char*, float>> {
                 { IDs::revOn, 0 }, { IDs::dlyOn, 0 }, { IDs::warmth, 0 }, { IDs::compOn, 0 }, { IDs::pickup, 2 },
                 { IDs::lowCut, 20 }, { IDs::highCut, 20000 }, { IDs::cab, (float) ct::kCabIR } })
            set (p, id, v);
        p.setIRSamples (0, "lead-in", 48000.0, cabIR (12, 4800, 90.0, 1.4, 6000.0, 3));
        auto peakAfter = [&]
        {
            AudioBuffer<float> imp (1, (int) kSR); imp.clear(); imp.setSample (0, (int) (0.5 * kSR), 0.05f);
            const auto out = render (p, imp, 256);
            int pk = 0; float best = 0;
            for (int i = 0; i < out.getNumSamples(); ++i) if (std::abs (out.getSample (0, i)) > best) { best = std::abs (out.getSample (0, i)); pk = i; }
            return pk - (int) (0.5 * kSR) - p.getLatencySamples();
        };
        check (get (p, IDs::irAlign) > 0.5f, "ALIGN is on by default");
        const int alignedPeak = peakAfter();
        set (p, IDs::irAlign, 0);           // (render() prepares again, which rebuilds the IRs)
        const int rawPeak = peakAfter();
        check (std::abs (alignedPeak) <= 2 && rawPeak >= 10, "ALIGN: the IR adds no delay (peak " + String (alignedPeak) + " samples after the reported latency - the speaker roll-off's own rise time; "
                                                             + String (rawPeak) + " as recorded)");
        set (p, IDs::irAlign, 1);
        MemoryBlock st;
        p.getStateInformation (st);
        CleanToneAudioProcessor q;
        q.setStateInformation (st.getData(), (int) st.getSize());
        const float rd = diffDb (render (p, guitar, 512), render (q, guitar, 512));
        check (get (q, IDs::irAlign) > 0.5f && rd < -120.0f, "ALIGN: reloaded project sounds identical (" + String (rd, 1) + " dB)");
        if (auto xml = AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize()))
        {
            for (int i = xml->getNumChildElements(); --i >= 0;)
                if (xml->getChildElement (i)->getStringAttribute ("id") == IDs::irAlign) xml->removeChildElement (xml->getChildElement (i), true);
            MemoryBlock old;
            AudioProcessor::copyXmlToBinary (*xml, old);
            CleanToneAudioProcessor o;
            o.setStateInformation (old.getData(), (int) old.getSize());
            check (get (o, IDs::irAlign) < 0.5f && o.hasIR (0), "projects saved by 1.4.0 with IRs keep the IRs as recorded (ALIGN off)");
        }

        // Auto Level: the same settings get the same correction at every sample rate
        String corr; float lo = 100, hi = -100;
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            const double keep = kSR; kSR = fs;
            CleanToneAudioProcessor a;
            a.loadFactoryPreset (0);
            render (a, makeGuitar (1.0, 0.0), 512);
            const float c = a.autoGainDb.load();
            lo = std::min (lo, c); hi = std::max (hi, c);
            corr << String ((int) fs) << " Hz " << String (c, 2) << " dB  ";
            kSR = keep;
        }
        check (hi - lo < 0.3f, "Auto Level: the same correction at 44.1 / 48 / 96 kHz (" + corr.trimEnd() + ")");
    }
    // --- 1.4.2: Sync tempo - tap tempo, recall, the host's tempo wins in a DAW, the plugin's own tempo otherwise ---
    {
        {
            TapTempo t;
            bool ok = ! t.tap (1000.0).has_value();                         // one tap: no tempo yet
            const auto r1 = t.tap (1500.0);
            t.tap (2000.0);
            const auto r3 = t.tap (2510.0);                                 // 500, 500, 510 ms
            ok = ok && r1 && std::abs (*r1 - 120.0) < 1e-9 && r3 && std::abs (*r3 - 60000.0 * 3 / 1510.0) < 1e-9;
            const auto r4 = t.tap (6000.0);                                 // after a 3.5 s pause: a new count
            const auto r5 = t.tap (6400.0);
            ok = ok && ! r4 && r5 && std::abs (*r5 - 150.0) < 1e-9;
            for (double tt = 6800.0; tt <= 8000.0; tt += 400.0) t.tap (tt);
            const auto r6 = t.tap (8250.0);                                 // the last 4 intervals: 400, 400, 400, 250
            ok = ok && r6 && std::abs (*r6 - 60000.0 * 4 / 1450.0) < 1e-9;
            TapTempo fast;
            fast.tap (0.0);
            const auto r7 = fast.tap (50.0);                                // 1200 BPM -> limited to 300
            ok = ok && r7 && *r7 == 300.0;
            check (ok, "tap tempo: averages the last 4 taps, starts a new count after a 2 s pause, stays within 30 - 300 BPM");
        }

        auto setup = [] (CleanToneAudioProcessor& p, float bpm)
        {
            resetToBaseline (p);
            set (p, IDs::revOn, 0); set (p, IDs::dlyOn, 1); set (p, IDs::dlySync, 1); set (p, IDs::dlyDiv, 2);   // 1/4 note
            set (p, IDs::dlyMix, 40); set (p, IDs::dlyFeedback, 40); set (p, IDs::dlyDuck, 0);
            set (p, IDs::dlyBpm, bpm);
        };
        struct FakePlayHead : AudioPlayHead
        {
            double bpm = 90.0;
            Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setBpm (bpm); i.setIsPlaying (true); return i; }
        } host;

        CleanToneAudioProcessor daw, own90, own140;
        setup (daw, 140.0f); setup (own90, 90.0f); setup (own140, 140.0f);
        daw.setPlayHead (&host);
        const auto a = render (daw, guitar, 512);
        const auto b = render (own90, guitar, 512);
        const auto c = render (own140, guitar, 512);
        check (daw.isUsingHostTempo() && diffDb (a, b) < -120.0f,
               "Sync in a DAW: the project tempo (90 BPM) wins over the plugin's own Tempo (140) - identical to Tempo 90 ("
               + String (diffDb (a, b), 1) + " dB)");
        check (! own140.isUsingHostTempo() && diffDb (a, c) > -40.0f,
               "Sync without a host tempo (the app): the plugin's own Tempo is used (140 BPM sounds different from 90)");

        // editor: read-only host tempo in a DAW; editable tempo + TAP without one
        auto findIn = [] (AudioProcessorEditor& e, std::function<bool (Component*)> pred) -> Component*
        {
            for (auto* holder : e.getChildren())
                for (auto* ch : holder->getChildren())
                    if (pred (ch)) return ch;
            return nullptr;
        };
        auto isTap = [] (Component* ch) { auto* b = dynamic_cast<TextButton*> (ch); return b != nullptr && b->getButtonText().startsWith ("TAP"); };
        auto isHostLabel = [] (Component* ch) { auto* l = dynamic_cast<Label*> (ch); return l != nullptr && l->getText().contains ("host"); };
        {
            std::unique_ptr<AudioProcessorEditor> ed (daw.createEditor());
            auto* tap = findIn (*ed, isTap);
            auto* lbl = findIn (*ed, isHostLabel);
            check (tap != nullptr && ! tap->isVisible() && lbl != nullptr && lbl->isVisible() && dynamic_cast<Label*> (lbl)->getText().startsWith ("90.0 BPM"),
                   "editor in a DAW: shows the project tempo read-only (\"" + (lbl ? dynamic_cast<Label*> (lbl)->getText() : String ("none")) + "\"), no TAP");
        }
        {
            std::unique_ptr<AudioProcessorEditor> ed (own140.createEditor());
            auto* tap = findIn (*ed, isTap);
            check (tap != nullptr && tap->isVisible() && findIn (*ed, isHostLabel) == nullptr,
                   "editor without a host tempo (the app): editable Tempo and TAP are shown");
        }
        daw.setPlayHead (nullptr);

        // recall: the Tempo comes back exactly; presets leave it alone (like Output and Auto Level)
        set (own140, IDs::dlyBpm, 97.5f);
        MemoryBlock st;
        own140.getStateInformation (st);
        CleanToneAudioProcessor q;
        q.setStateInformation (st.getData(), (int) st.getSize());
        q.loadFactoryPreset (3);
        check (std::abs (get (q, IDs::dlyBpm) - 97.5f) < 1e-4f, "Tempo comes back with the project, and changing presets leaves it ("
                                                                 + String (get (q, IDs::dlyBpm), 1) + " BPM)");

        // changing the tempo while repeats ring: they glide to the new time, no click
        CleanToneAudioProcessor g;
        setup (g, 120.0f);
        const auto tone = makeSine (4.0, 110.0f, 0.25f);
        const auto out = render (g, tone, 256, [&] (int pos) { if (pos == ((int) (2.0 * kSR) / 256) * 256) set (g, IDs::dlyBpm, 60.0f); });
        const float ck = clickDb (out, (int) (1.9 * kSR));
        check (ck < -80.0f, "changing the Tempo while the delay rings is click-free (" + String (ck, 1) + " dB, allowed -80)");
    }

    // --- 1.5: Clarity, Pick, Tight Warmth, reverb Ducking ---
    {
        auto bandDb = [] (const AudioBuffer<float>& b, double lo, double hi)
        {
            ct::Biquad h1, h2, l1, l2;
            h1.setHighPass (kSR, lo, 0.7071); h2.setHighPass (kSR, lo, 0.7071); l1.setLowPass (kSR, hi, 0.7071); l2.setLowPass (kSR, hi, 0.7071);
            double e = 0;
            for (int i = (int) (0.5 * kSR); i < b.getNumSamples(); ++i)
            { const float y = l2.process (l1.process (h2.process (h1.process (b.getSample (0, i), 0), 0), 0), 0); e += (double) y * y; }
            return 10.0 * std::log10 (e + 1e-30);
        };
        auto darken = [] (AudioBuffer<float> b, double hz)
        {
            ct::Biquad l1, l2; l1.setLowPass (kSR, hz, 0.7071); l2.setLowPass (kSR, hz, 0.7071);
            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, l2.process (l1.process (b.getSample (0, i), 0), 0));
            b.applyGain (0.25f / b.getMagnitude (0, 0, b.getNumSamples()));
            return b;
        };
        auto clarityCut = [&] (const AudioBuffer<float>& sig, double& definition)
        {
            float lo[2], hi[2];
            for (int on = 0; on < 2; ++on)
            {
                CleanToneAudioProcessor p; resetToBaseline (p); set (p, IDs::revOn, 0); set (p, IDs::clarity, on ? 100.0f : 0.0f);
                const auto out = render (p, sig, 512);
                lo[on] = (float) bandDb (out, 200, 450); hi[on] = (float) bandDb (out, 1000, 3000);
            }
            definition = hi[1] - hi[0];
            return lo[1] - lo[0];
        };
        double defBright = 0, defDark = 0;
        const double cutBright = clarityCut (guitar, defBright), cutDark = clarityCut (darken (guitar, 1500.0), defDark);
        check (cutDark < cutBright - 1.0 && cutDark < -2.5 && std::abs (defDark) < 1.0 && std::abs (defBright) < 1.0,
               "Clarity: trims the low mids harder when they build up (dark tone " + String (cutDark, 1) + " dB, bright tone "
               + String (cutBright, 1) + " dB) and leaves 1-3 kHz alone (" + String (defDark, 2) + " / " + String (defBright, 2) + " dB)");

        // Pick: attack (first 15 ms of each note) against the body (300-600 ms later), averaged over the notes
        auto attackToBody = [&] (float pickPct)
        {
            CleanToneAudioProcessor p; resetToBaseline (p); set (p, IDs::revOn, 0); set (p, IDs::pick, pickPct);
            const auto out = render (p, guitar, 512);
            const int lat = p.getLatencySamples(), noteLen = (int) (0.9 * kSR);
            double atk = 0, body = 0;
            for (int k = 1; (k + 1) * noteLen < (int) (4.0 * kSR); ++k)
            {
                const int s0 = k * noteLen + lat;
                float pk = 0; for (int i = s0; i < s0 + (int) (0.015 * kSR); ++i) pk = std::max (pk, std::abs (out.getSample (0, i)));
                double e = 0; const int b0 = s0 + (int) (0.3 * kSR), b1 = s0 + (int) (0.6 * kSR);
                for (int i = b0; i < b1; ++i) e += (double) out.getSample (0, i) * out.getSample (0, i);
                atk += 20.0 * std::log10 (pk + 1e-9); body += 10.0 * std::log10 (e / (b1 - b0) + 1e-18);
            }
            return std::make_pair (atk, body);
        };
        const auto p0 = attackToBody (0.0f), pPlus = attackToBody (100.0f), pMinus = attackToBody (-100.0f);
        const int nNotes = 3;
        const double dPlus = ((pPlus.first - pPlus.second) - (p0.first - p0.second)) / nNotes;
        const double dMinus = ((pMinus.first - pMinus.second) - (p0.first - p0.second)) / nNotes;
        const double bodyMove = std::max (std::abs (pPlus.second - p0.second), std::abs (pMinus.second - p0.second)) / nNotes;
        check (dPlus > 2.0 && dMinus < -2.0 && bodyMove < 0.5, "Pick: +100 brings the attack forward by " + String (dPlus, 1) + " dB, -100 softens it by "
                                                             + String (-dMinus, 1) + " dB; the body of the note moves " + String (bodyMove, 2) + " dB");

        // Tight: fewer harmonics on low notes, the same on high notes
        auto thdAt = [] (double f, float tight)
        {
            ct::Warmth w; w.prepare (kSR, 512, 1); w.setAmount (1.0f, tight);
            const int n = (int) kSR;
            std::vector<float> y ((size_t) n);
            for (int i = 0; i < n; ++i) y[(size_t) i] = 0.5f * (float) std::sin (2.0 * MathConstants<double>::pi * f * i / kSR);
            for (int pos = 0; pos < n; pos += 512) { float* c[1] = { y.data() + pos }; w.process (c, 1, std::min (512, n - pos)); }
            return thdDb (y, n / 2, n / 2, f, kSR);
        };
        const double loN = thdAt (100.0, 0.0f), loT = thdAt (100.0, 1.0f), hiN = thdAt (1600.0, 0.0f), hiT = thdAt (1600.0, 1.0f);
        check (loT < loN - 6.0 && std::abs (hiT - hiN) < 1.5, "Tight Warmth: low notes " + String (loN, 1) + " -> " + String (loT, 1)
                                                             + " dB of harmonics, high notes " + String (hiN, 1) + " -> " + String (hiT, 1) + " dB");

        // Tight must not boost what the curve itself makes down low (the thump when a bright note starts)
        auto lowByproducts = [] (float tight)
        {
            ct::Warmth w; w.prepare (kSR, 512, 1); w.setAmount (1.0f, tight);
            const int n = (int) (0.5 * kSR);
            std::vector<float> y ((size_t) n, 0.0f);
            for (int i = (int) (0.05 * kSR); i < n; ++i) y[(size_t) i] = 0.5f * (float) std::sin (2.0 * MathConstants<double>::pi * 6000.0 * i / kSR);
            for (int pos = 0; pos < n; pos += 512) { float* c[1] = { y.data() + pos }; w.process (c, 1, std::min (512, n - pos)); }
            ct::Biquad l1, l2; l1.setLowPass (kSR, 300.0, 0.7071); l2.setLowPass (kSR, 300.0, 0.7071);
            double e = 0; for (int i = 0; i < n; ++i) { const float v = l2.process (l1.process (y[(size_t) i], 0), 0); e += (double) v * v; }
            return 10.0 * std::log10 (e + 1e-30);
        };
        const double bpN = lowByproducts (0.0f), bpT = lowByproducts (1.0f);
        check (bpT < bpN + 1.0, "Tight adds no low thump of its own when a bright note starts (" + String (bpT - bpN, 1) + " dB vs Tight off)");

        // Tight must keep very hot signals in check too (the Warmth curve also works as a soft limiter)
        {
            AudioBuffer<float> hot (1, (int) (2.0 * kSR));
            Random rng (3);
            for (int i = 0; i < hot.getNumSamples(); ++i)
                hot.setSample (0, i, 0.35f * (rng.nextFloat() * 2 - 1) + 0.5f * (float) std::sin (2.0 * MathConstants<double>::pi * 110.0 * i / kSR));
            float worst = -100.0f, worstOff = -100.0f;
            for (const char* ctl : { IDs::inGain, IDs::sustain, IDs::warmth })
                for (float tight : { 0.0f, 1.0f })
                {
                    CleanToneAudioProcessor p; p.loadFactoryPreset (0); set (p, IDs::warmTight, tight);
                    p.apvts.getParameter (ctl)->setValueNotifyingHost (1.0f);
                    const auto out = render (p, hot, 512);
                    const float pk = Decibels::gainToDecibels (out.getMagnitude (0, 0, out.getNumSamples()));
                    (tight > 0.5f ? worst : worstOff) = std::max (tight > 0.5f ? worst : worstOff, pk);
                }
            check (worst < 6.0f && worst < worstOff + 8.0f, "Tight keeps very hot input in check (loudest " + String (worst, 1)
                                                            + " dBFS with Tight, " + String (worstOff, 1) + " without)");
        }

        // Reverb Ducking: the reverb backs off while you play, blooms back when you stop
        auto wetLevels = [] (float duck)
        {
            ct::CleanReverb rv; rv.prepare (kSR); rv.setParams (0.6f, 2.5f, 20.0f, 6500.0f, 200.0f, 1.0f, true, duck);
            const auto in = makeGuitar (3.0, 3.0);
            const int n = in.getNumSamples();
            std::vector<float> L ((size_t) n), R ((size_t) n);
            for (int i = 0; i < n; ++i) L[(size_t) i] = R[(size_t) i] = in.getSample (0, i);
            for (int pos = 0; pos < n; pos += 512) rv.process (L.data() + pos, R.data() + pos, std::min (512, n - pos));
            auto wetDb = [&] (double t0, double t1)
            {
                double e = 0; for (int i = (int) (t0 * kSR); i < (int) (t1 * kSR); ++i) { const double w = L[(size_t) i] - in.getSample (0, i); e += w * w; }
                return 10.0 * std::log10 (e + 1e-30);
            };
            return std::make_pair (wetDb (1.0, 2.9), wetDb (3.8, 4.8));   // while playing / in the gap after
        };
        const auto d0 = wetLevels (0.0f), d1 = wetLevels (1.0f);
        check (d1.first < d0.first - 6.0 && d1.second > d0.second - 4.0,
               "Reverb Ducking: the reverb drops " + String (d0.first - d1.first, 1) + " dB while you play and is back within "
               + String (d0.second - d1.second, 1) + " dB once you stop");
    }

    // --- 1.6: free software - the name the host shows, and the About box's license notice ---
    {
        CleanToneAudioProcessor p;
        const auto about = CleanToneAudioProcessorEditor::aboutText();
        check (p.getName() == "Clean Tone Studio" && about.contains ("CleanToneStudio-" CTS_VERSION "-source.zip") && about.contains (CTS_VERSION) && about.contains ("GNU Affero General Public License")
                   && about.contains ("WITHOUT ANY WARRANTY") && about.contains ("Copyright (C) 2026") && about.contains ("-source.zip"),
               "free software: the plugin is called \"" + p.getName() + "\" and its About box carries the version, copyright, AGPLv3 and no-warranty notice");
    }

    // --- 1.6.1: back to "Clean Tone Studio" - presets saved under the 1.6.0 name come back, once ---
    {
        const auto docs = File::getSpecialLocation (File::tempDirectory).getChildFile ("CleanToneNameTest");
        docs.deleteRecursively();
        const auto interim = docs.getChildFile ("Open Clean Tone Studio").getChildFile ("Presets");
        const auto home = docs.getChildFile ("Clean Tone Studio").getChildFile ("Presets");
        interim.createDirectory(); home.createDirectory();
        interim.getChildFile ("Saved In 160.ctpreset").replaceWithText ("new");
        interim.getChildFile ("Both.ctpreset").replaceWithText ("from 1.6.0");
        home.getChildFile ("Both.ctpreset").replaceWithText ("original");
        const auto folder = CleanToneAudioProcessor::presetFolderIn (docs);
        const bool came = folder == home && home.getChildFile ("Saved In 160.ctpreset").loadFileAsString() == "new";
        const bool kept = home.getChildFile ("Both.ctpreset").loadFileAsString() == "original";
        home.getChildFile ("Saved In 160.ctpreset").deleteFile();            // the user deletes it afterwards...
        CleanToneAudioProcessor::presetFolderIn (docs);
        const bool once = ! home.getChildFile ("Saved In 160.ctpreset").exists();   // ...and it stays deleted
        const bool untouched = interim.getChildFile ("Saved In 160.ctpreset").exists();
        docs.deleteRecursively();
        check (came && kept && once && untouched, "presets saved by 1.6.0 come back to Documents\\Clean Tone Studio\\Presets once, "
                                                  "without replacing existing ones (copied " + String ((int) came) + ", kept " + String ((int) kept)
                                                  + ", once " + String ((int) once) + ", 1.6.0 folder untouched " + String ((int) untouched) + ")");
    }

    // --- window: scales with its size; the app's window has a maximize button ---
    {
        CleanToneAudioProcessor p;
        std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
        check (ed->isResizable(), "window: resizable");
        check (ed->getWidth() == CleanToneAudioProcessorEditor::kBaseW && ed->getHeight() == CleanToneAudioProcessorEditor::kBaseH,
               "window: opens at its full size (" + String (ed->getWidth()) + "x" + String (ed->getHeight()) + ")");
        ed->setSize (CleanToneAudioProcessorEditor::kBaseW * 2, CleanToneAudioProcessorEditor::kBaseH * 2);
        auto* body = ed->getChildComponent (0);
        const auto r = body->getBoundsInParent();
        check (std::abs (body->getTransform().mat00 - 2.0f) < 1.0e-3f && r == ed->getLocalBounds(),
               "window: at twice the size everything is drawn twice as large and fills it");
        {
            DocumentWindow win ("Clean Tone Studio", Colours::black, DocumentWindow::minimiseButton | DocumentWindow::closeButton);
            win.setContentNonOwned (ed.get(), true);
            check (win.getMaximiseButton() == nullptr, "window: plugin editors leave the host's window alone");
            win.clearContentComponent();
        }
        ed.reset();

        AudioProcessor::setTypeOfNextNewPlugin (AudioProcessor::wrapperType_Standalone);
        CleanToneAudioProcessor app;
        AudioProcessor::setTypeOfNextNewPlugin (AudioProcessor::wrapperType_Undefined);
        std::unique_ptr<AudioProcessorEditor> aed (app.createEditor());
        DocumentWindow win ("Clean Tone Studio", Colours::black, DocumentWindow::minimiseButton | DocumentWindow::closeButton);
        win.setResizable (true, false);
        win.setContentNonOwned (aed.get(), true);
        MessageManager::getInstance()->runDispatchLoopUntil (100);   // the button is added once the window has settled
        check (win.getMaximiseButton() != nullptr, "window: the app's window has a maximize button");
        win.setSize (1920, 1080);           // what maximizing on a 1080p screen does
        auto* abody = aed->getChildComponent (0);
        const float s = abody->getTransform().mat00;
        const float expect = std::min ((float) aed->getWidth() / CleanToneAudioProcessorEditor::kBaseW,
                                       (float) aed->getHeight() / CleanToneAudioProcessorEditor::kBaseH);
        check (std::abs (s - expect) < 1.0e-3f && s > 1.5f && aed->getWidth() > 1800,
               "window: maximized, the controls scale up to fill the screen (x" + String (s, 2) + ", editor "
               + String (aed->getWidth()) + "x" + String (aed->getHeight()) + ")");
        win.clearContentComponent();
    }
}

//==============================================================================
int main (int argc, char* argv[])
{
    ScopedJuceInitialiser_GUI init;
    CleanToneAudioProcessor::setUserPresetFolderOverride (File::getSpecialLocation (File::tempDirectory).getChildFile ("CleanToneBehaviourTest"));
    const String mode = argc > 1 ? String (argv[1]) : String();
    verbose = mode == "--verbose";

    if (mode == "--snapshots" && argc > 2) return snapshots (File (String (argv[2])));
    if (mode == "--lab")
    {
        String md;
        lab::run (md, true);
        if (argc > 2) { File (String (argv[2])).replaceWithText (md); std::cout << "\nreport written to " << argv[2] << std::endl; }
        return 0;
    }
    if (mode == "--bufsize") { kSR = argc > 2 ? String (argv[2]).getDoubleValue() : 44100.0; verbose = true; bufferSection(); return failures; }
    if (mode == "--recall") { recallExtraSection(); return failures; }
    if (mode == "--spur")
    {
        std::cout << "THE_LAB high-note test: worst non-harmonic component below a 10 kHz tone at -6 dBFS\n";
        auto show = [] (const char* name, std::function<void (CleanToneAudioProcessor&)> setup)
        { const auto r = worstSpur (setup); std::cout << "  " << String (name).paddedRight (' ', 44) << String (r.first, 1) << " dB at " << String (r.second, 0) << " Hz\n"; };
        show ("defaults (Studio Clean, Auto Level on)", [] (CleanToneAudioProcessor&) {});
        auto showS = [] (const char* name, std::function<void (CleanToneAudioProcessor&)> setup)
        { const auto r = worstSpur (setup, true); std::cout << "  " << String (name).paddedRight (' ', 44) << String (r.first, 1) << " dB at " << String (r.second, 0) << " Hz\n"; };
        showS ("STEREO IN: defaults", [] (CleanToneAudioProcessor&) {});
        for (double settle : { 0.0, 0.1, 0.3, 0.6, 1.0 })
        {
            const auto r = worstSpur ([] (CleanToneAudioProcessor&) {}, true, settle);
            std::cout << "  STEREO IN: defaults, analysed after " << String (settle, 1) << " s      " << String (r.first, 1) << " dB at " << String (r.second, 0) << " Hz\n";
        }
        for (double settle : { 0.0, 0.3 })
        {
            const auto r = worstSpur ([] (CleanToneAudioProcessor& p) { set (p, IDs::compOn, 0); }, true, settle);
            std::cout << "  STEREO IN: compressor off, after " << String (settle, 1) << " s        " << String (r.first, 1) << " dB at " << String (r.second, 0) << " Hz\n";
            const auto r2 = worstSpur ([] (CleanToneAudioProcessor& p) { set (p, IDs::warmth, 0); }, true, settle);
            std::cout << "  STEREO IN: warmth 0, after " << String (settle, 1) << " s             " << String (r2.first, 1) << " dB at " << String (r2.second, 0) << " Hz\n";
            const auto r3 = worstSpur ([] (CleanToneAudioProcessor& p) { set (p, IDs::revOn, 0); }, true, settle);
            std::cout << "  STEREO IN: reverb off, after " << String (settle, 1) << " s           " << String (r3.first, 1) << " dB at " << String (r3.second, 0) << " Hz\n";
        }
        showS ("STEREO IN: reverb off", [] (CleanToneAudioProcessor& p) { set (p, IDs::revOn, 0); });
        showS ("STEREO IN: reverb off, compressor off", [] (CleanToneAudioProcessor& p) { set (p, IDs::revOn, 0); set (p, IDs::compOn, 0); });
        showS ("STEREO IN: reverb off, warmth 0", [] (CleanToneAudioProcessor& p) { set (p, IDs::revOn, 0); set (p, IDs::warmth, 0); });
        showS ("STEREO IN: reverb, comp off, warmth 0", [] (CleanToneAudioProcessor& p) { set (p, IDs::revOn, 0); set (p, IDs::compOn, 0); set (p, IDs::warmth, 0); });
        show ("defaults, reverb off", [] (CleanToneAudioProcessor& p) { set (p, IDs::revOn, 0); });
        show ("neutral (everything off, filters open)", [] (CleanToneAudioProcessor& p) { for (auto [id, v] : std::initializer_list<std::pair<const char*, float>> {
                  { IDs::revOn, 0 }, { IDs::compOn, 0 }, { IDs::warmth, 0 }, { IDs::cab, 0 }, { IDs::pickup, 2 }, { IDs::lowCut, 20 }, { IDs::highCut, 20000 } }) set (p, id, v); });
        show ("Warmth 100 %, cabinet off, filters open", [] (CleanToneAudioProcessor& p) { set (p, IDs::warmth, 100); set (p, IDs::cab, 0); set (p, IDs::highCut, 20000); });
        return 0;
    }    if (mode == "--stress") { labStressSection(); return failures; }
    if (mode == "--claprobe")   // Clarity: how much it cuts on single notes, chords and a dark neck pickup
    {
        kSR = 48000.0;
        auto band = [] (const AudioBuffer<float>& b, double lo, double hi)
        {
            ct::Biquad h1, h2, l1, l2;
            h1.setHighPass (kSR, lo, 0.7071); h2.setHighPass (kSR, lo, 0.7071); l1.setLowPass (kSR, hi, 0.7071); l2.setLowPass (kSR, hi, 0.7071);
            double e = 0;
            for (int i = (int) (0.5 * kSR); i < b.getNumSamples(); ++i)
            {
                const float y = l2.process (l1.process (h2.process (h1.process (b.getSample (0, i), 0), 0), 0), 0);
                e += (double) y * y;
            }
            return 10.0 * std::log10 (e + 1e-30);
        };
        // chords: several plucked notes at once, strummed every 0.9 s
        auto chords = []
        {
            AudioBuffer<float> sum (1, (int) (6.0 * kSR)); sum.clear();
            const float shapes[2][4] = { { 82.41f, 123.47f, 164.81f, 207.65f }, { 110.0f, 164.81f, 220.0f, 277.18f } };
            Random rng (5);
            for (int s = 0; s < 6; ++s)
                for (int v = 0; v < 4; ++v)
                {
                    const float f0 = shapes[s & 1][v];
                    const int period = (int) (kSR / f0), start = (int) (s * 0.9 * kSR) + v * 400;
                    std::vector<float> line ((size_t) period); float lp = 0;
                    for (auto& x : line) { lp += 0.22f * ((rng.nextFloat() * 2 - 1) - lp); x = lp * 3.0f; }
                    float last = 0;
                    for (int i = 0; i < (int) (0.9 * kSR) && start + i < sum.getNumSamples(); ++i)
                    { const size_t k = (size_t) (i % period); const float cur = line[k]; line[k] = 0.4985f * (cur + last); last = cur; sum.addSample (0, start + i, cur); }
                }
            sum.applyGain (0.25f / sum.getMagnitude (0, 0, sum.getNumSamples()));
            return sum;
        };
        auto neck = [] (AudioBuffer<float> b) { ct::Biquad l; l.setLowPass (kSR, 1800.0, 0.7071); for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, l.process (b.getSample (0, i), 0)); b.applyGain (0.25f / b.getMagnitude (0, 0, b.getNumSamples())); return b; };
        const auto notes = makeGuitar (6.0, 0.0);
        struct Sig { const char* name; AudioBuffer<float> b; };
        std::vector<Sig> sigs { { "single notes", notes }, { "chords", chords() }, { "neck pickup (dark)", neck (notes) }, { "neck pickup chords", neck (chords()) } };
        for (int prog : { 0, 2 })
            for (auto& s : sigs)
            {
                float res[2][2];
                for (int on = 0; on < 2; ++on)
                {
                    CleanToneAudioProcessor p; p.loadFactoryPreset (prog); set (p, IDs::autoLevel, 0); set (p, IDs::revOn, 0);
                    set (p, IDs::clarity, on ? 100.0f : 0.0f);
                    const auto out = render (p, s.b, 512);
                    res[on][0] = (float) band (out, 200, 450); res[on][1] = (float) band (out, 1000, 3000);
                }
                std::cout << (prog == 0 ? String ("Studio Clean") : String ("Warm Jazz Humbucker")).paddedRight (' ', 22) << String (s.name).paddedRight (' ', 22) << " low mids vs 1-3 kHz " << String (res[0][0] - res[0][1], 1)
                          << " dB;  Clarity 100: low mids " << String (res[1][0] - res[0][0], 2) << " dB, 1-3 kHz " << String (res[1][1] - res[0][1], 2) << " dB\n";
            }
        return 0;
    }
    if (mode == "--hotprobe")   // THE_LAB extremes: noise + tone near 0 dBFS, Input at its extremes, Tight on/off
    {
        kSR = 48000.0;
        AudioBuffer<float> in (1, (int) (3.0 * kSR));
        Random rng (3);
        for (int i = 0; i < in.getNumSamples(); ++i)
            in.setSample (0, i, 0.35f * (rng.nextFloat() * 2 - 1) + 0.5f * (float) std::sin (2.0 * MathConstants<double>::pi * 110.0 * i / kSR));
        for (const char* ctl : { "inGain", "warmth", "pick", "clarity", "sustain" })
            for (float tight : { 0.0f, 1.0f })
            {
                CleanToneAudioProcessor p; p.loadFactoryPreset (0);
                set (p, IDs::pick, 40); set (p, IDs::warmTight, tight); set (p, IDs::clarity, 60); set (p, IDs::revDuck, 50);
                p.apvts.getParameter (ctl)->setValueNotifyingHost (1.0f);
                const auto out = render (p, in, 512);
                std::cout << "  " << String (ctl).paddedRight (' ', 8) << " at 100 %, Tight " << (int) tight << ": peak "
                          << String (Decibels::gainToDecibels (out.getMagnitude (0, 0, out.getNumSamples())), 1) << " dBFS\n";
            }
        return 0;
    }
    if (mode == "--spur150")   // THE_LAB 1.5 pass (Pick 40, Tight, Clarity 60, reverb Ducking 50): what makes the 10 kHz spur
    {
        kSR = 48000.0;
        auto base = [] (CleanToneAudioProcessor& p)
        { set (p, IDs::pick, 40); set (p, IDs::warmTight, 1); set (p, IDs::clarity, 60); set (p, IDs::revDuck, 50); };
        struct V { const char* name; std::function<void (CleanToneAudioProcessor&)> f; };
        std::vector<V> vs { { "as tested", [] (CleanToneAudioProcessor&) {} },
                            { "reverb off", [] (CleanToneAudioProcessor& p) { set (p, IDs::revOn, 0); } },
                            { "Pick 0", [] (CleanToneAudioProcessor& p) { set (p, IDs::pick, 0); } },
                            { "Clarity 0", [] (CleanToneAudioProcessor& p) { set (p, IDs::clarity, 0); } },
                            { "Tight off", [] (CleanToneAudioProcessor& p) { set (p, IDs::warmTight, 0); } },
                            { "reverb Ducking 0", [] (CleanToneAudioProcessor& p) { set (p, IDs::revDuck, 0); } } };
        for (auto& v : vs)
        {
            std::cout << "  " << String (v.name).paddedRight (' ', 18);
            for (double settle : { 0.0, 1.0, 2.0, 4.0 })
            {
                const auto r = worstSpur ([&] (CleanToneAudioProcessor& p) { base (p); v.f (p); }, true, settle);
                std::cout << "  after " << String (settle, 0) << " s: " << String (r.first, 1) << " dB @" << String (r.second, 0);
            }
            std::cout << "\n";
        }
        return 0;
    }
    if (mode == "--spurb")   // THE_LAB pass B (delay on, Sync, Ducking 0, reverb off): which part makes the 10 kHz spur
    {
        kSR = 48000.0;
        auto base = [] (CleanToneAudioProcessor& p)
        {
            set (p, IDs::revOn, 0); set (p, IDs::dlyOn, 1); set (p, IDs::dlySync, 1); set (p, IDs::dlyDuck, 0);
        };
        auto show = [&] (const char* name, std::function<void (CleanToneAudioProcessor&)> extra)
        {
            for (bool st : { false, true })
            {
                const auto r = worstSpur ([&] (CleanToneAudioProcessor& p) { base (p); extra (p); }, st);
                std::cout << "  " << String (name).paddedRight (' ', 40) << (st ? " stereo in " : " mono in   ") << String (r.first, 1) << " dB at " << String (r.second, 0) << " Hz\n";
            }
        };
        for (double settle : { 0.0, 0.5, 1.0, 2.0, 3.0 })
            for (int fbOn = 0; fbOn < 2; ++fbOn)
            {
                const auto r = worstSpur ([&] (CleanToneAudioProcessor& p) { base (p); if (! fbOn) set (p, IDs::dlyFeedback, 0); }, true, settle);
                std::cout << "  analysed after " << String (settle, 1) << " s, feedback " << (fbOn ? "30 %" : "0 %  ") << ": " << String (r.first, 1) << " dB at " << String (r.second, 0) << " Hz\n";
            }
        show ("pass B settings", [] (CleanToneAudioProcessor&) {});
        show ("delay off", [] (CleanToneAudioProcessor& p) { set (p, IDs::dlyOn, 0); });
        show ("delay Mod 0", [] (CleanToneAudioProcessor& p) { set (p, IDs::dlyMod, 0); });
        show ("delay Feedback 0", [] (CleanToneAudioProcessor& p) { set (p, IDs::dlyFeedback, 0); });
        show ("delay Ping-Pong 0", [] (CleanToneAudioProcessor& p) { set (p, IDs::dlyWidth, 0); });
        show ("Sync off", [] (CleanToneAudioProcessor& p) { set (p, IDs::dlySync, 0); });
        show ("warmth 0, comp off", [] (CleanToneAudioProcessor& p) { set (p, IDs::warmth, 0); set (p, IDs::compOn, 0); });
        return 0;
    }
    if (mode == "--wavdiff" && argc > 3)   // two renders: levels, where they start to differ, and how much
    {
        AudioFormatManager fm; fm.registerBasicFormats();
        auto rd = [&] (const char* path) { AudioBuffer<float> b; if (std::unique_ptr<AudioFormatReader> r { fm.createReaderFor (File (String (path))) }) { b.setSize ((int) r->numChannels, (int) r->lengthInSamples); r->read (&b, 0, b.getNumSamples(), 0, true, true); } return b; };
        const auto a = rd (argv[2]), b = rd (argv[3]);
        const int n = std::min (a.getNumSamples(), b.getNumSamples());
        double ea = 0, eb = 0, ed = 0; int first = -1;
        for (int i = 0; i < n; ++i)
        {
            const double x = a.getSample (0, i), y = b.getSample (0, i);
            ea += x * x; eb += y * y; ed += (x - y) * (x - y);
            if (first < 0 && std::abs (x - y) > 1.0e-5) first = i;
        }
        std::cout << "level A " << String (10 * std::log10 (ea / n + 1e-30), 2) << " dB, B " << String (10 * std::log10 (eb / n + 1e-30), 2)
                  << " dB, B-A " << String (10 * std::log10 ((eb + 1e-30) / (ea + 1e-30)), 2) << " dB; difference " << String (10 * std::log10 ((ed + 1e-30) / (ea + 1e-30)), 1)
                  << " dB re A, first differs at sample " << first << "\n";
        return 0;
    }
    if (mode == "--iranalyze" && argc > 3)   // IR files: onset, polarity, alignment, A / B / blend response
    {
        struct IRF { String name; double rate = 0; int ch = 0, bits = 0; std::vector<float> s; };
        auto load = [] (const String& path)
        {
            IRF r; r.name = File (path).getFileName();
            AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<AudioFormatReader> rd (fm.createReaderFor (File (path)));
            if (rd == nullptr) return r;
            r.rate = rd->sampleRate; r.ch = (int) rd->numChannels; r.bits = (int) rd->bitsPerSample;
            AudioBuffer<float> b ((int) rd->numChannels, (int) rd->lengthInSamples);
            rd->read (&b, 0, b.getNumSamples(), 0, true, true);
            r.s.assign ((size_t) b.getNumSamples(), 0.0f);
            for (int c = 0; c < b.getNumChannels(); ++c)
                for (int i = 0; i < b.getNumSamples(); ++i) r.s[(size_t) i] += b.getSample (c, i) / (float) b.getNumChannels();
            return r;
        };
        auto peakOf = [] (const std::vector<float>& v)
        {
            int pi = 0; for (int i = 1; i < (int) v.size(); ++i) if (std::abs (v[(size_t) i]) > std::abs (v[(size_t) pi])) pi = i;
            return pi;
        };
        auto onsetOf = [] (const std::vector<float>& v, float rel)
        {
            float pk = 0; for (float x : v) pk = std::max (pk, std::abs (x));
            for (int i = 0; i < (int) v.size(); ++i) if (std::abs (v[(size_t) i]) >= rel * pk) return i;
            return 0;
        };
        auto mag = [] (const std::vector<float>& h, double f, double fs)
        {
            std::complex<double> acc;
            for (size_t n = 0; n < h.size(); ++n) acc += (double) h[n] * std::polar (1.0, -2.0 * MathConstants<double>::pi * f * (double) n / fs);
            return acc;
        };
        IRF a = load (argv[2]), b = load (argv[3]);
        for (auto* r : { &a, &b })
        {
            if (r->s.empty()) { std::cout << "could not read " << r->name << "\n"; return 1; }
            const int pk = peakOf (r->s);
            std::cout << r->name << ": " << r->rate << " Hz, " << r->ch << " ch, " << r->bits << " bit, " << r->s.size() << " samples ("
                      << String (1000.0 * (double) r->s.size() / r->rate, 1) << " ms)\n"
                      << "   main peak at sample " << pk << " (" << (r->s[(size_t) pk] < 0 ? "NEGATIVE" : "positive") << "), first sample above -40 dB of peak: "
                      << onsetOf (r->s, 0.01f) << ", above -20 dB: " << onsetOf (r->s, 0.1f) << "\n";
        }
        for (double fs : { 48000.0, 96000.0 })
        {
            const auto pa = ct::prepareIR (a.s, a.rate, fs), pb = ct::prepareIR (b.s, b.rate, fs);
            // best alignment of B against A (cross-correlation, both signs)
            int bestLag = 0; double best = 0;
            for (int lag = -64; lag <= 64; ++lag)
            {
                double c = 0;
                for (int n = 0; n < (int) pa.size(); ++n) { const int m = n - lag; if (m >= 0 && m < (int) pb.size()) c += (double) pa[(size_t) n] * pb[(size_t) m]; }
                if (std::abs (c) > std::abs (best)) { best = c; bestLag = lag; }
            }
            std::cout << "\nAt " << (int) fs << " Hz: A peak " << peakOf (pa) << ", B peak " << peakOf (pb)
                      << "; B best matches A when shifted " << bestLag << " samples, correlation " << String (best, 3)
                      << (best < 0 ? " (OPPOSITE polarity)" : " (same polarity)") << "\n";
            std::vector<float> blend (std::max (pa.size(), pb.size()), 0.0f), flip (blend.size(), 0.0f), ablend (blend.size(), 0.0f);
            const auto ma = ct::minimumPhase (pa), mb = ct::minimumPhase (pb);
            for (size_t n = 0; n < blend.size(); ++n)
            {
                const float x = n < pa.size() ? pa[n] : 0.0f, y = n < pb.size() ? pb[n] : 0.0f;
                blend[n] = 0.5f * x + 0.5f * y; flip[n] = 0.5f * x - 0.5f * y;
                ablend[n] = 0.5f * (n < ma.size() ? ma[n] : 0.0f) + 0.5f * (n < mb.size() ? mb[n] : 0.0f);
            }
            double worstMagErr = 0;
            for (double f = 30; f < 0.45 * fs; f *= 1.05)
                for (int w = 0; w < 2; ++w)
                {
                    const auto& o = w ? pb : pa; const auto& m = w ? mb : ma;
                    worstMagErr = std::max (worstMagErr, std::abs (20.0 * std::log10 ((std::abs (mag (m, f, fs)) + 1e-12) / (std::abs (mag (o, f, fs)) + 1e-12))));
                }
            std::cout << "   ALIGN: A peak at " << peakOf (ma) << ", B peak at " << peakOf (mb) << " (" << (ma[(size_t) peakOf (ma)] < 0 ? "neg" : "pos") << "/"
                      << (mb[(size_t) peakOf (mb)] < 0 ? "neg" : "pos") << "); magnitude unchanged within " << String (worstMagErr, 3) << " dB (30 Hz - 0.45 fs)\n";
            std::cout << "   freq     A dB     B dB   blend dB  blend(B flipped) dB  ALIGN blend dB\n";
            for (double f : { 30.0, 42.0, 50.0, 63.0, 80.0, 100.0, 150.0, 200.0, 400.0, 1000.0, 2000.0, 4000.0, 8000.0 })
            {
                auto db = [] (std::complex<double> z) { return String (20.0 * std::log10 (std::abs (z) + 1e-12), 1).paddedLeft (' ', 8); };
                std::cout << String (f, 0).paddedLeft (' ', 7) << " " << db (mag (pa, f, fs)) << " " << db (mag (pb, f, fs)) << "   "
                          << db (mag (blend, f, fs)) << "        " << db (mag (flip, f, fs)) << "           " << db (mag (ablend, f, fs)) << "\n";
            }
        }
        return 0;
    }
    if (mode == "--alprobe")      // Auto Level: compressor on vs off, at several playing levels (Endless Sustain)
    {
        kSR = 48000.0;
        auto kLoud = [] (const AudioBuffer<float>& b)
        {
            double pw = 0; const int n = std::min (b.getNumSamples(), (int) (8.0 * kSR));
            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                ct::Biquad sh, hp; sh.setHighShelf (kSR, 1681.0, 4.0); hp.setHighPass (kSR, 38.0, 0.5);
                for (int i = 0; i < n; ++i) { const float k = hp.process (sh.process (b.getSample (c, i), 0), 0); pw += (double) k * k; }
            }
            return (float) (10.0 * std::log10 (pw / (n * b.getNumChannels()) + 1e-20));
        };
        const int prog = argc > 2 ? String (argv[2]).getIntValue() : 5;
        {
            const auto g = makeGuitar (8.0, 1.0);
            float l[2];
            for (int w = 0; w < 2; ++w)
            {
                CleanToneAudioProcessor p; p.loadFactoryPreset (0); set (p, IDs::autoLevel, 1); if (w) set (p, IDs::warmth, 100);
                l[w] = kLoud (render (p, g, 512));
            }
            std::cout << "warmth 100 vs default: " << String (l[1] - l[0], 2) << " dB\n";
        }
        for (float gainDb : { -12.0f, -6.0f, 0.0f, 6.0f })
        {
            auto g = makeGuitar (8.0, 1.0);
            g.applyGain (Decibels::decibelsToGain (gainDb));
            float l[2];
            for (int on = 0; on < 2; ++on)
            {
                CleanToneAudioProcessor p; p.loadFactoryPreset (prog); set (p, IDs::autoLevel, 1); set (p, IDs::compOn, (float) on);
                l[on] = kLoud (render (p, g, 512));
            }
            std::cout << "input " << gainDb << " dB: comp off - on = " << String (l[0] - l[1], 2) << " dB\n";
        }
        return 0;
    }
    if (mode == "--flips") { kSR = 48000.0; clickSection(); return failures; }
    if (mode == "--modules") { kSR = 48000.0; modulesSection(); return failures; }
    if (mode == "--clicks") { auto c = clickScan (argc > 2); std::cout << c.size() << " clicking change(s)" << std::endl; return 0; }
    if (mode == "--bench")
    {
        for (double fs : { 48000.0, 96000.0 })
        {
            kSR = fs;
            CleanToneAudioProcessor p;
            p.loadFactoryPreset (6);
            const auto in = makeGuitar (30.0, 0.0);
            const double t0 = Time::getMillisecondCounterHiRes();
            render (p, in, 256);
            const double ms = Time::getMillisecondCounterHiRes() - t0;
            std::cout << (int) fs << " Hz: 30 s of audio in " << String (ms, 0) << " ms = " << String (100.0 * ms / 30000.0, 2)
                      << " % of one CPU core" << std::endl;
        }
        return 0;
    }

    std::cout << CTS_PRODUCT_NAME << " " << CTS_VERSION << " behaviour tests\n";
    std::cout << "\n== Sample rate 48000 ==\n";
    kSR = 48000.0;
    clickSection();

    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        kSR = fs;
        if (fs != 48000.0) std::cout << "\n== Sample rate " << (int) fs << " ==\n";
        recallSection();
        if (fs == 48000.0) recallExtraSection();
        gateSection();
        bufferSection();
    }
    kSR = 48000.0;

    {
        std::cout << "\n5) Click scan: every switch, every choice, every control (whole plugin, all modules on)\n";
        const auto clicks = clickScan (verbose);
        check (clicks.empty(), "every parameter jump is click-free: " + String ((int) clicks.size()) + " clicking change(s)");
    }

    kSR = 48000.0;
    labStressSection();
    modulesSection();
    kSR = 48000.0;

    {
        std::cout << "\n6) Measurements (THE_LAB style: distortion, aliasing, response, latency, silence, CPU)\n";
        String md;
        const auto r = lab::run (md, false);
        check (r.latencyError == 0, "latency reported to the host matches the measured delay exactly (44.1/48/96 kHz)");
        check (r.gateOpenThdDelta < 1.0f, "an open noise gate adds no distortion (THD change " + String (r.gateOpenThdDelta, 2) + " dB)");
        check (r.neutralResponseDev < 0.5f, "neutral setting is flat 60 Hz - 10 kHz (worst " + String (r.neutralResponseDev, 2) + " dB)");
        check (r.silenceOutDb < -120.0f, "silence in -> silence out after the echoes fade (" + String (r.silenceOutDb, 1) + " dBFS)");
        check (r.cpu48 < 10.0f, "whole plugin uses " + String (r.cpu48, 2) + " % of one core at 48 kHz (Ambient Swell)");
        std::cout << "  info  THD with Warmth at 0, 1 kHz -12 dBFS: " << String (r.warmthZeroThd, 1) << " dB" << std::endl;
        for (auto& [name, db] : r.aliasing)
            std::cout << "  info  worst alias below 15 kHz, " << name << ": " << String (db, 1) << " dB" << std::endl;
    }

    std::cout << "\n" << (failures == 0 ? "ALL BEHAVIOUR TESTS PASSED" : String (failures) + " FAILURE(S)") << "\n";
    return failures == 0 ? 0 : 1;
}
