#pragma once

// The complete Clean Tone Studio signal chain (everything except the Output knob),
// packaged so the plugin can run one live instance and the level matcher can run
// an independent offline copy with identical settings.

#include "DSP.h"
#include "Convolution.h"
#include <cstring>

namespace ct
{
// Plain snapshot of every setting that affects the sound. All floats, so two
// snapshots can be compared byte-for-byte to detect changes.
struct Settings
{
    float inGain = 0, pickup = 0, hum = 0, gate = 0;
    float compOn = 1, sustain = 35, attack = 15, compMix = 100;
    float warmth = 30, bass = 0, mid = 0, midFreq = 750, presence = 0, treble = 0;
    float lowCut = 70, highCut = 12000, cab = 1;
    float dlyOn = 0, dlyTimeMs = 380, dlyFeedback = 30, dlyTone = 4000, dlyMod = 20, dlyDuck = 30, dlyWidth = 50, dlyMix = 25;
    float revOn = 1, revSize = 60, revDecay = 2.2f, revPreDelay = 20, revTone = 6500, revLowCut = 200, revMix = 18;
    float irA = 0, irB = 0, irBlend = 50;          // IR serial numbers (0 = empty slot) and the A/B blend
    float compLook = 0, dlyStyle = 0;              // compressor lookahead, delay modulation style (0 sine, 1 tape)
    float clarity = 0, pick = 0, warmTight = 0, revDuck = 0;   // 1.5: Clarity %, Pick % (-100..100), Tight (0/1), reverb Ducking %

    bool operator== (const Settings& o) const { return std::memcmp (this, &o, sizeof (Settings)) == 0; }
    bool operator!= (const Settings& o) const { return ! (*this == o); }
};

static constexpr int kCabIR = 4;    // Cabinet choice that uses the loaded impulse responses

class Chain
{
public:
    // Smoothed controls are stepped every kStep samples, on a grid counted from prepare(),
    // so the result never depends on how the host slices audio into buffers.
    static constexpr int kStep = kCoefRamp;

    Chain()
    {
        for (auto* p : { &midFreq, &lowCut, &highCut, &attack, &dlyTone, &revTone, &revLowCut })
            p->logScale = true;
    }

    void prepare (double fs, int maxBlockSize)
    {
        sr = fs;
        maxBlock = std::max (16, maxBlockSize);
        hum.prepare (sr, maxBlock);
        gate.prepare (sr);
        pickup.prepare (sr, maxBlock);
        comp.prepare (sr);
        pick.prepare (sr);
        clarity.prepare (sr);
        warmth.prepare (sr, maxBlock, 2);
        tone.prepare (sr);
        cab.prepare (sr, maxBlock);
        irCab.prepare (sr);
        delay.prepare (sr);
        reverb.prepare (sr);
        gainCoef = onePoleCoef (8.0, sr);   // two stages: ~20 ms S-shaped glide
        stepCoef = (float) (1.0 - std::exp (-(double) kStep / (0.030 * sr)));   // ~30 ms glide
        resetControls();
    }

    // Clears all audio state without allocating (the level matcher calls this on the audio thread).
    void resetState()
    {
        hum.resetState(); gate.prepare (sr); pickup.resetState(); comp.resetState(); pick.resetState(); clarity.resetState();
        warmth.resetState(); tone.prepare (sr); cab.resetState(); irCab.resetState(); delay.resetState(); reverb.resetState();
        resetControls();
    }

    int getMaxBlock() const { return maxBlock; }
    float getLatency() const { return warmth.getLatency(); }
    float getGainReductionDb() const { return comp.getGainReductionDb(); }

    // Which impulse responses the IR cabinet uses (call before apply). snap = no crossfade.
    void setIRs (const IRSpectra* a, const IRSpectra* b, bool snap) { irCab.setSources (a, b, snap); }

    // Call once per chunk before process(): sets new targets; switches start crossfades.
    void apply (const Settings& s)
    {
        last = s;
        hum.setMode ((int) s.hum);
        pickup.setMode ((int) s.pickup);
        cab.setMode ((int) s.cab);
        irCab.setParams ((int) s.cab == kCabIR, s.irBlend * 0.01f);
        gate.setAmount (s.gate * 0.01f);

        bass.setTarget (s.bass);   mid.setTarget (s.mid);   midFreq.setTarget (s.midFreq);
        presence.setTarget (s.presence); treble.setTarget (s.treble);
        lowCut.setTarget (s.lowCut); highCut.setTarget (s.highCut);
        warmthAmt.setTarget (s.warmth); warmTight.setTarget (s.warmTight > 0.5f ? 1.0f : 0.0f);
        clarityAmt.setTarget (s.clarity); pickAmt.setTarget (s.pick); revDuck.setTarget (s.revDuck);
        sustain.setTarget (s.sustain); attack.setTarget (s.attack);
        dlyTone.setTarget (s.dlyTone);
        revSize.setTarget (s.revSize); revDecay.setTarget (s.revDecay);
        revTone.setTarget (s.revTone); revLowCut.setTarget (s.revLowCut);

        inGainT = dbToGain (s.inGain + pickup.newest().getLevelOffsetDb());
        if (! primed) { inGain.snap (inGainT); primed = true; }

        // push current (smoothed) values plus the per-sample-smoothed controls
        pushTone(); pushWarmth(); pushComp(); pushDelay(); pushReverb(); pushDynamics();
        moving = false;
        for (auto* p : allSmoothed()) moving = moving || p->isMoving();
    }

    // ch: numOutCh channel pointers (1 or 2); the first mainCh hold input. n <= maxBlock.
    void process (float* const* ch, int numOutCh, int mainCh, int n)
    {
        int i = 0;
        while (i < n)
        {
            if (phase == 0 && moving) stepSmoothers();
            // while something glides, stop at every grid point to refresh filter coefficients
            const int len = moving ? std::min (kStep - phase, n - i) : n - i;
            float* seg[2] = { ch[0] + i, numOutCh > 1 ? ch[1] + i : nullptr };
            processSegment (seg, numOutCh, mainCh, len);
            phase = (phase + len) % kStep;
            i += len;
        }
    }

private:
    void processSegment (float* const* ch, int numOutCh, int mainCh, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = inGain.next (inGainT, gainCoef);
            for (int c = 0; c < mainCh; ++c) ch[c][i] *= g;
        }

        hum.process (ch, mainCh, n);
        gate.process (ch, mainCh, n);
        pickup.process (ch, mainCh, n);
        comp.process (ch, mainCh, n);
        pick.process (ch, mainCh, n);
        warmth.process (ch, mainCh, n);
        tone.process (ch, mainCh, n);
        cab.process (ch, mainCh, n);
        irCab.process (ch, mainCh, n);
        clarity.process (ch, mainCh, n);

        if (numOutCh == 2 && mainCh == 1)
            std::copy (ch[0], ch[0] + n, ch[1]);

        float* R = numOutCh > 1 ? ch[1] : nullptr;
        delay.process (ch[0], R, n);
        reverb.process (ch[0], R, n);
    }

    void stepSmoothers()
    {
        const float c = stepCoef;
        bool t = false, w = false, k = false, d = false, r = false, y = false;
        for (auto* p : { &bass, &mid, &midFreq, &presence, &treble, &lowCut, &highCut }) t = p->step (c) || t;
        w = warmthAmt.step (c); w = warmTight.step (c) || w;
        y = clarityAmt.step (c); y = pickAmt.step (c) || y;
        k = sustain.step (c); k = attack.step (c) || k;
        d = dlyTone.step (c);
        for (auto* p : { &revSize, &revDecay, &revTone, &revLowCut, &revDuck }) r = p->step (c) || r;
        if (t) pushTone();
        if (w) pushWarmth();
        if (k) pushComp();
        if (d) pushDelay();
        if (r) pushReverb();
        if (y) pushDynamics();
        moving = t || w || k || d || r || y;
    }

    void pushTone()   { tone.set (bass.value(), mid.value(), midFreq.value(), presence.value(), treble.value(),
                                  lowCut.value(), highCut.value()); }
    void pushWarmth() { warmth.setAmount (warmthAmt.value() * 0.01f, warmTight.value()); }
    void pushDynamics() { clarity.setAmount (clarityAmt.value() * 0.01f); pick.setAmount (pickAmt.value() * 0.01f); }
    void pushComp()   { comp.setParams (sustain.value() * 0.01f, attack.value(), last.compMix * 0.01f, last.compOn > 0.5f, last.compLook > 0.5f); }
    void pushDelay()  { delay.setParams (std::min (last.dlyTimeMs, 2900.0f), last.dlyFeedback * 0.01f, dlyTone.value(),
                                         last.dlyMod * 0.01f, last.dlyDuck * 0.01f, last.dlyWidth * 0.01f,
                                         last.dlyMix * 0.01f, last.dlyOn > 0.5f, last.dlyStyle > 0.5f); }
    void pushReverb() { reverb.setParams (revSize.value() * 0.01f, revDecay.value(), last.revPreDelay, revTone.value(),
                                          revLowCut.value(), last.revMix * 0.01f, last.revOn > 0.5f, revDuck.value() * 0.01f); }

    void resetControls()
    {
        inGain.snap (1.0f);
        for (auto* p : allSmoothed()) p->reset();
        phase = 0;
        moving = false;
        primed = false;
    }

    std::array<SmoothParam*, 19> allSmoothed()
    {
        return { &bass, &mid, &midFreq, &presence, &treble, &lowCut, &highCut, &warmthAmt,
                 &sustain, &attack, &dlyTone, &revSize, &revDecay, &revTone, &revLowCut,
                 &warmTight, &clarityAmt, &pickAmt, &revDuck };
    }

    double sr = 48000.0;
    int maxBlock = 512, phase = 0;
    bool primed = false, moving = false;
    float stepCoef = 0.1f;
    Settings last;

    SmoothParam bass, mid, midFreq, presence, treble, lowCut, highCut, warmthAmt, sustain, attack,
                dlyTone, revSize, revDecay, revTone, revLowCut, warmTight, clarityAmt, pickAmt, revDuck;

    Crossfaded<HumFilter> hum;
    Gate gate;
    Crossfaded<PickupVoicer> pickup;
    SustainCompressor comp;
    PickShaper pick;
    Warmth warmth;
    Clarity clarity;
    ToneStack tone;
    Crossfaded<CabSim> cab;
    IRCab irCab;
    SmoothDelay delay;
    CleanReverb reverb;
    Glide inGain;
    float inGainT = 1.0f, gainCoef = 0.01f;
};

//==============================================================================
// "Auto Level" engine. The correction depends on the settings ONLY - never on what has been played
// before - so the plugin sounds the same every time with the same settings, in every bounce, after
// every reload and in every copy. It runs inside the audio callback, in audio time.
//
// It keeps a fixed reference performance (guitar-like plucks at a typical DI level). When the
// settings change (and have been still for 50 ms) it renders that reference through its own copy of
// the chain, kSpeed reference samples per audio sample, compares K-weighted loudness in vs out and
// publishes the correction at an exact sample. Results are remembered, so going back to settings it
// has already measured is instant. Controls that can't change the sound (the delay's while the delay
// is off, and so on) are ignored, so moving them never triggers a measurement.
class LevelMatcher
{
public:
    static constexpr int kChunk = 512;
    static constexpr int kSpeed = 4;                 // reference samples measured per audio sample
    static constexpr double kRefSeconds = 2.0, kSkipSeconds = 0.2, kDebounceSeconds = 0.05;
    static constexpr float kRefLevelDb = -25.5f;     // reference RMS level: a typical DI guitar while playing
    static constexpr int kMaxEvents = 8, kCacheSize = 32;

    struct Event { int at = 0; float db = 0.0f; };   // correction 'db' takes effect at sample 'at' of the block

    // prepareToPlay (allocates), then measures 'initial' straight away so the correction is right from the first sample
    void prepare (double fs, const Settings& initial, const IRSpectra* a = nullptr, const IRSpectra* b = nullptr)
    {
        irA = a; irB = b;
        sr = fs;
        chain.prepare (fs, kChunk);
        buildReference (fs);
        work.setSize (2, kChunk);
        total = reference.getNumSamples();
        skip = (int) (kSkipSeconds * fs);
        debounce = (int64_t) (kDebounceSeconds * fs);
        now = 0;
        changedAt = 0;
        measuring = false;
        for (auto& c : cache) c.valid = false;      // a fresh start remembers nothing: same history every run
        nextSlot = 0;
        target = canonical (initial);
        measureNow (target);
        immediate = false;
        publish();
    }

    // Audio thread, once per block BEFORE the block is processed.
    // Returns how many corrections take effect inside this block, at exact sample positions.
    int process (int n, const Settings& raw, Event* events, const IRSpectra* a = nullptr, const IRSpectra* b = nullptr)
    {
        irA = a; irB = b;
        int numEvents = 0;
        auto push = [&] (int at, float db) { if (numEvents < kMaxEvents) events[numEvents++] = { at, db }; else events[kMaxEvents - 1] = { at, db }; };

        const Settings s = canonical (raw);
        if (s != target)
        {
            target = s;
            changedAt = now;
            if (measuring && measuringSettings != target) measuring = false;
            if (const float* hit = lookup (target))           // measured before: instant
            {
                correction = *hit;
                measuredSettings = target;
                haveMeasured = true;
                push (0, correction);
            }
        }
        if (immediate.exchange (false) && ! (haveMeasured && measuredSettings == target))
        {
            measureNow (target);                               // a session was just loaded: right from sample 0
            push (0, correction);
        }

        if (! measuring && needsMeasuring())
        {
            const int64_t t0 = std::max (changedAt + debounce, now);
            if (t0 < now + n) start (target, t0);
        }
        if (measuring)
        {
            if (completeAt < now + n)
            {
                processReference (total);
                finish();
                push ((int) (completeAt - now), correction);
            }
            else
                processReference ((int) std::min<int64_t> (total, (now + n - startedAt) * kSpeed));
        }

        now += n;
        publish();
        return numEvents;
    }

    // message thread: after a session is loaded, have the next block measure (if needed) before any audio
    void requestImmediate() { immediate = true; }

    float getCorrectionDb() const { return pubCorrection.load(); }
    bool isSettled (const Settings& s) const { return settled.load() && canonical (s) == pubTarget; }

    // Only what can change the sound counts.
    static Settings canonical (Settings s)
    {
        if (s.dlyOn < 0.5f) { s.dlyOn = 0; s.dlyTimeMs = s.dlyFeedback = s.dlyTone = s.dlyMod = s.dlyDuck = s.dlyWidth = s.dlyMix = 0; }
        if (s.revOn < 0.5f) { s.revOn = 0; s.revSize = s.revDecay = s.revPreDelay = s.revTone = s.revLowCut = s.revMix = 0; }
        if (s.compOn < 0.5f || s.sustain <= 0.0f) { s.compOn = 0; s.sustain = s.attack = s.compMix = 0; }
        if (s.mid == 0.0f) s.midFreq = 0;
        if ((int) s.cab != kCabIR) { s.irA = s.irB = s.irBlend = 0; }
        else if (s.irA == 0.0f || s.irB == 0.0f) s.irBlend = 0;   // blend only matters with both slots loaded
        if (s.dlyOn < 0.5f) s.dlyStyle = 0;
        if (s.revOn < 0.5f) s.revDuck = 0;
        if (s.warmth <= 0.0f) s.warmTight = 0;                      // Tight only shapes the Warmth curve
        s.compLook = 0;                                            // lookahead shifts timing, not loudness
        return s;
    }

private:
    bool needsMeasuring() const { return ! haveMeasured || target != measuredSettings; }

    const float* lookup (const Settings& s) const
    {
        for (const auto& c : cache) if (c.valid && c.key == s) return &c.db;
        return nullptr;
    }

    void remember (const Settings& s, float db)
    {
        for (auto& c : cache) if (c.valid && c.key == s) { c.db = db; return; }
        cache[(size_t) nextSlot] = { s, db, true };
        nextSlot = (nextSlot + 1) % kCacheSize;
    }

    void measureNow (const Settings& s)
    {
        start (s, now);
        processReference (total);
        finish();
    }

    void start (const Settings& s, int64_t t)
    {
        chain.resetState();
        chain.setIRs (irA, irB, true);
        kInShelf = {}; kInHp = {}; kOutShelf = {}; kOutHp = {};
        kInShelf.setHighShelf (sr, 1681.0, 4.0);   kInHp.setHighPass (sr, 38.0, 0.5);
        kOutShelf.setHighShelf (sr, 1681.0, 4.0);  kOutHp.setHighPass (sr, 38.0, 0.5);
        measuringSettings = s;
        scale = dbToGain (kRefLevelDb);
        pos = 0;
        inPow = outPow = 0.0;
        startedAt = t;
        completeAt = t + (total + kSpeed - 1) / kSpeed;
        measuring = true;
    }

    void processReference (int upTo)
    {
        while (pos < upTo)
        {
            const int n = std::min (kChunk, upTo - pos);
            const float* src = reference.getReadPointer (0, pos);
            float* L = work.getWritePointer (0);
            float* R = work.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                L[i] = src[i] * scale;
                const float k = kInHp.process (kInShelf.process (L[i], 0), 0);
                if (pos + i >= skip) inPow += (double) k * k;
            }
            std::fill (R, R + n, 0.0f);
            float* chans[2] = { L, R };
            chain.apply (measuringSettings);
            chain.process (chans, 2, 1, n);
            for (int i = 0; i < n; ++i)
            {
                const float kl = kOutHp.process (kOutShelf.process (L[i], 0), 0);
                const float kr = kOutHp.process (kOutShelf.process (R[i], 1), 1);
                if (pos + i >= skip) outPow += 0.5 * ((double) kl * kl + (double) kr * kr);
            }
            pos += n;
        }
    }

    void finish()
    {
        correction = (outPow <= 1.0e-12 || inPow <= 1.0e-12) ? 0.0f
                   : std::clamp ((float) (10.0 * std::log10 (inPow / outPow)), -24.0f, 24.0f);
        measuredSettings = measuringSettings;
        haveMeasured = true;
        measuring = false;
        remember (measuredSettings, correction);
    }

    void publish()
    {
        pubCorrection.store (correction);
        pubTarget = target;
        settled.store (! measuring && ! needsMeasuring());
    }

    // A fixed, guitar-like reference performance: single notes, then overlapping ringing chord
    // tones (Karplus-Strong with a soft, rolled-off pick), normalised to 0 dB RMS.
    // The reference is always made at 48 kHz and resampled, so every sample rate measures exactly the
    // same phrase (a string synthesised at the running rate rings differently at 96 kHz, which made the
    // correction depend on the sample rate). At 48 kHz nothing changes.
    void buildReference (double fs)
    {
        constexpr double base = 48000.0;
        juce::AudioBuffer<float> src;
        synthesiseReference (src, base);
        const int len = (int) (kRefSeconds * fs);
        if (std::abs (fs - base) < 0.5) { reference.makeCopyOf (src); }
        else
        {
            reference.setSize (1, len);
            const double ratio = fs / base, cutoff = std::min (1.0, ratio);
            const double halfWidth = 24.0 / cutoff;
            constexpr int kRes = 512;                       // kernel table: 512 points per input sample
            const int tableLen = (int) std::ceil (halfWidth * kRes) + 2;
            std::vector<float> kernel ((size_t) tableLen);
            auto bessel0 = [] (double x) { double s = 1.0, t = 1.0; for (int k = 1; k < 40; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
            const double beta = 8.0, i0b = bessel0 (beta);
            for (int j = 0; j < tableLen; ++j)
            {
                const double d = (double) j / kRes, xs = cutoff * d, u = d / halfWidth;
                const double sinc = std::abs (xs) < 1e-12 ? 1.0 : std::sin (kPiD * xs) / (kPiD * xs);
                const double win = u < 1.0 ? bessel0 (beta * std::sqrt (1.0 - u * u)) / i0b : 0.0;
                kernel[(size_t) j] = (float) (cutoff * sinc * win);
            }
            const float* in = src.getReadPointer (0);
            const int inLen = src.getNumSamples();
            float* out = reference.getWritePointer (0);
            for (int n = 0; n < len; ++n)
            {
                const double tpos = (double) n / ratio;
                const int m0 = std::max (0, (int) std::ceil (tpos - halfWidth)), m1 = std::min (inLen - 1, (int) std::floor (tpos + halfWidth));
                double acc = 0.0;
                for (int m = m0; m <= m1; ++m)
                {
                    const double p = std::abs (tpos - m) * kRes;
                    const int j = (int) p;
                    if (j + 1 >= tableLen) continue;
                    const double fr = p - j;
                    acc += in[m] * (kernel[(size_t) j] + fr * (kernel[(size_t) j + 1] - kernel[(size_t) j]));
                }
                out[n] = (float) acc;
            }
        }
        const float rms = reference.getRMSLevel (0, 0, len);
        if (rms > 0.0f) reference.applyGain (1.0f / rms);
    }

    static void synthesiseReference (juce::AudioBuffer<float>& reference, double fs)
    {
        const int len = (int) (kRefSeconds * fs);
        reference.setSize (1, len);
        reference.clear();
        const float notes[] = { 82.41f, 196.0f, 146.83f, 246.94f, 110.0f, 329.63f, 164.81f, 261.63f, 98.0f, 220.0f };
        juce::Random rng (1234);
        const int half = len / 2;
        const float lpA = (float) std::min (1.0, 2.0 * kPiD * 1800.0 / fs);
        for (int startS = 0, idx = 0; startS < len; ++idx)
        {
            const bool chordal = startS >= half;
            const int noteLen = (int) ((chordal ? 1.4 : 0.35) * fs);
            const int period = std::max (2, (int) (fs / notes[idx % 10]));
            std::vector<float> line ((size_t) period);
            float lpState = 0.0f;
            for (auto& v : line) { lpState += lpA * ((rng.nextFloat() * 2.0f - 1.0f) - lpState); v = lpState * 3.0f; }
            float last = 0.0f;
            for (int i = 0; i < noteLen && startS + i < len; ++i)
            {
                const size_t k = (size_t) (i % period);
                const float cur = line[k];
                line[k] = 0.4985f * (cur + last);
                last = cur;
                reference.addSample (0, startS + i, cur);
            }
            startS += chordal ? (int) (0.22 * fs) : noteLen;
        }
    }

    struct Entry { Settings key; float db = 0.0f; bool valid = false; };

    double sr = 48000.0;
    Chain chain;
    juce::AudioBuffer<float> reference, work;
    int total = 1, skip = 0;
    int64_t now = 0, changedAt = 0, debounce = 2400, startedAt = 0, completeAt = 0;

    Settings target, measuringSettings, measuredSettings;
    bool measuring = false, haveMeasured = false;
    float scale = 1.0f, correction = 0.0f;
    int pos = 0;
    double inPow = 0.0, outPow = 0.0;
    Biquad kInShelf, kInHp, kOutShelf, kOutHp;

    std::array<Entry, kCacheSize> cache;
    int nextSlot = 0;
    const IRSpectra* irA = nullptr;
    const IRSpectra* irB = nullptr;

    std::atomic<bool> immediate { false };
    std::atomic<float> pubCorrection { 0.0f };
    std::atomic<bool> settled { false };
    Settings pubTarget;              // read only by isSettled (tests / UI), written by the audio thread
};
} // namespace ct
