#pragma once

// Clean Tone Studio - DSP building blocks.
// Everything here is allocation-free in process(); memory is sized in prepare().
//
// Click-free rule: nothing that reaches the audio may change in one step.
//  - gains, mixes and delay positions glide per sample inside each module
//  - filter coefficients are fed from smoothed parameters (see SmoothParam / Chain)
//  - mode switches (pickup, cabinet, hum filter) crossfade between old and new (Crossfaded)

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>

namespace ct
{
constexpr double kPiD = 3.14159265358979323846;
constexpr float  kPi  = 3.14159265358979f;

inline float dbToGain (float db)     { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)      { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }
inline float onePoleCoef (double ms, double fs)
{
    return (float) (1.0 - std::exp (-1.0 / (std::max (ms, 0.001) * 0.001 * fs)));
}

// Filter coefficients glide linearly over this many samples whenever they're updated,
// and the Chain refreshes them on the same grid - so filter sweeps have no steps at all.
constexpr int kCoefRamp = 4;

//==============================================================================
// Two cascaded one-pole smoothers: an S-shaped move with no sharp corner at the start
// (a plain one-pole jumps to full speed instantly, which is itself a faint click).
struct Glide
{
    float s1 = 0.0f, s2 = 0.0f;
    void snap (float v) { s1 = s2 = v; }
    inline float next (float target, float c)
    {
        // when a step rounds to nothing in float, the move is finished: land exactly on the target
        const float n1 = s1 + c * (target - s1); s1 = (n1 == s1) ? target : n1;
        const float n2 = s2 + c * (s1 - s2);     s2 = (n2 == s2) ? s1 : n2;
        return s2;
    }
    float value() const { return s2; }
};

//==============================================================================
// A parameter that glides towards its target (S-shaped). Stepped by the Chain every
// kCoefRamp samples; 'logScale' smooths frequencies in the log domain so sweeps sound even.
struct SmoothParam
{
    float s1 = 0.0f, cur = 0.0f, target = 0.0f;
    bool logScale = false, primed = false;

    void reset() { primed = false; }
    void setTarget (float v)
    {
        target = logScale ? std::log (std::max (v, 1.0e-6f)) : v;
        if (! primed) { s1 = cur = target; primed = true; }
    }
    bool isMoving() const { return cur != target || s1 != target; }
    bool step (float c)
    {
        if (! isMoving()) return false;
        // a step that rounds to nothing in float means the glide is finished: land exactly on the target
        // (otherwise it stalls a hair short, forever "moving", and the control never quite arrives)
        const float n1 = s1 + c * (target - s1); s1 = (n1 == s1) ? target : n1;
        const float n2 = cur + c * (s1 - cur);   cur = (n2 == cur) ? s1 : n2;
        const float eps = 1.0e-5f * (1.0f + std::abs (target));
        if (std::abs (target - s1) <= eps && std::abs (target - cur) <= eps) s1 = cur = target;
        return true;
    }
    float value() const { return logScale ? std::exp (cur) : cur; }
};

//==============================================================================
// RBJ-cookbook biquad, transposed direct form II, up to 2 channels. Coefficients and state are
// double precision: low-cut and low-shelf filters have poles very close to 1, and in float their
// rounding noise is amplified enormously near DC (measured: -76 dB below a 10 kHz tone at 11 Hz).
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }

    // Glide this filter's coefficients to those of 't' over 'samples' calls of advance().
    void glideTo (const Biquad& t, int samples)
    {
        tgt[0] = t.b0; tgt[1] = t.b1; tgt[2] = t.b2; tgt[3] = t.a1; tgt[4] = t.a2;
        if (samples <= 0) { b0 = t.b0; b1 = t.b1; b2 = t.b2; a1 = t.a1; a2 = t.a2; rampLeft = 0; return; }
        const double inv = 1.0 / (double) samples;
        d[0] = (t.b0 - b0) * inv; d[1] = (t.b1 - b1) * inv; d[2] = (t.b2 - b2) * inv;
        d[3] = (t.a1 - a1) * inv; d[4] = (t.a2 - a2) * inv;
        rampLeft = samples;
    }
    inline void advance() noexcept     // once per sample (not per channel)
    {
        if (rampLeft <= 0) return;
        if (--rampLeft == 0) { b0 = tgt[0]; b1 = tgt[1]; b2 = tgt[2]; a1 = tgt[3]; a2 = tgt[4]; return; }
        b0 += d[0]; b1 += d[1]; b2 += d[2]; a1 += d[3]; a2 += d[4];
    }

    inline float process (float xf, int ch) noexcept
    {
        const double x = xf;
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float) y;
    }

    void setIdentity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    void setPeak (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        double w, c, al; prep (fs, f, q, w, c, al);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }

    void setLowShelf (double fs, double f, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        double w, c, al; prep (fs, f, 0.7071, w, c, al);
        const double sA = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1) - (A - 1) * c + sA),
             2 * A * ((A - 1) - (A + 1) * c),
             A * ((A + 1) - (A - 1) * c - sA),
             (A + 1) + (A - 1) * c + sA,
             -2 * ((A - 1) + (A + 1) * c),
             (A + 1) + (A - 1) * c - sA);
    }

    void setHighShelf (double fs, double f, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        double w, c, al; prep (fs, f, 0.7071, w, c, al);
        const double sA = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1) + (A - 1) * c + sA),
             -2 * A * ((A - 1) + (A + 1) * c),
             A * ((A + 1) + (A - 1) * c - sA),
             (A + 1) - (A - 1) * c + sA,
             2 * ((A - 1) - (A + 1) * c),
             (A + 1) - (A - 1) * c - sA);
    }

    void setLowPass (double fs, double f, double q)
    {
        double w, c, al; prep (fs, f, q, w, c, al);
        set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void setHighPass (double fs, double f, double q)
    {
        double w, c, al; prep (fs, f, q, w, c, al);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void setNotch (double fs, double f, double q)
    {
        double w, c, al; prep (fs, f, q, w, c, al);
        set (1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
    }

private:
    static void prep (double fs, double f, double q, double& w, double& c, double& al)
    {
        f = std::clamp (f, 10.0, fs * 0.45);
        w = 2.0 * kPiD * f / fs;
        c = std::cos (w);
        al = std::sin (w) / (2.0 * std::max (q, 0.05));
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
        a1 = na1 / na0; a2 = na2 / na0;
        rampLeft = 0;
    }

    double d[5] {}, tgt[5] {};
    int rampLeft = 0;
};

//==============================================================================
// Runs two copies of a filter stage and crossfades from the old setting to the new one
// whenever the mode changes, so switching pickup / cabinet / hum filter never clicks.
template <typename Stage>
class Crossfaded
{
public:
    void prepare (double fs, int maxBlock)
    {
        rate = fs;
        fadeLen = std::max (1, (int) (0.030 * fs));
        for (auto& t : tmp) t.assign ((size_t) std::max (1, maxBlock), 0.0f);
        resetState();
    }

    void resetState()      // no allocation
    {
        a.prepare (rate);
        b.prepare (rate);
        fadeLeft = 0;
        current = -1;
    }

    void setMode (int m)
    {
        if (current < 0) { a.setMode (m); current = m; return; }
        if (m == current) return;
        if (fadeLeft > 0) a = b;          // changed again mid-fade: continue from the newer one
        b = a;                            // same filter state, new setting
        b.setMode (m);
        current = m;
        fadeLeft = fadeLen;
    }

    const Stage& newest() const { return fadeLeft > 0 ? b : a; }

    void process (float* const* ch, int numCh, int n)
    {
        if (fadeLeft <= 0) { a.process (ch, numCh, n); return; }

        float* t[2] = { tmp[0].data(), tmp[1].data() };
        for (int c = 0; c < numCh; ++c) std::copy (ch[c], ch[c] + n, t[c]);
        a.process (ch, numCh, n);
        b.process (t, numCh, n);

        const float inv = 1.0f / (float) fadeLen;
        for (int i = 0; i < n; ++i)
        {
            // raised-cosine crossfade: starts and ends gently
            const float g = fadeLeft > 0 ? 0.5f - 0.5f * std::cos (kPi * (1.0f - (float) fadeLeft * inv)) : 1.0f;
            for (int c = 0; c < numCh; ++c) ch[c][i] += g * (t[c][i] - ch[c][i]);
            if (fadeLeft > 0) --fadeLeft;
        }
        if (fadeLeft == 0) a = b;
    }

private:
    Stage a, b;
    double rate = 48000.0;
    int fadeLen = 1, fadeLeft = 0, current = -1;
    std::array<std::vector<float>, 2> tmp;
};

//==============================================================================
// Hum filter: narrow notches on mains fundamental + 3 harmonics (single coils!)
class HumFilter
{
public:
    void prepare (double fs) { sr = fs; for (auto& b : notch) b.reset(); current = -1; }
    void setMode (int mode) // 0 off, 1 = 50 Hz, 2 = 60 Hz
    {
        if (mode == current) return;
        if (current <= 0) for (auto& b : notch) b.reset();   // waking up: no stale filter state
        current = mode;
        if (mode == 0) return;
        const double f0 = mode == 1 ? 50.0 : 60.0;
        for (int k = 0; k < 4; ++k)
            notch[(size_t) k].setNotch (sr, f0 * (k + 1), 18.0 + 6.0 * k);
    }
    void process (float* const* ch, int numCh, int n)
    {
        if (current <= 0) return;
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < n; ++i)
            {
                float x = ch[c][i];
                for (auto& b : notch) x = b.process (x, c);
                ch[c][i] = x;
            }
    }
private:
    double sr = 48000.0;
    int current = -1;
    std::array<Biquad, 4> notch;
};

//==============================================================================
// Noise gate with hysteresis and hold. It decides open/closed from a held peak envelope,
// then fades smoothly, so it never chatters or buzzes on low notes near the threshold.
class Gate
{
public:
    void prepare (double fs)
    {
        env = 0.0f; gain = 1.0f; open = true; holdLeft = 0;
        detRel   = onePoleCoef (40.0, fs);
        holdLen  = (int) (0.035 * fs);          // longer than a low-E cycle, so ripple can't close it
        gOpen    = onePoleCoef (0.8, fs);
        gClose   = onePoleCoef (120.0, fs);
        floorGain = dbToGain (-50.0f);
    }

    void setAmount (float a01)
    {
        enabled = a01 > 0.001f;
        const float thr = -85.0f + 45.0f * a01;  // -85 .. -40 dBFS
        openLin  = dbToGain (thr);
        closeLin = dbToGain (thr - 6.0f);        // 6 dB hysteresis
    }

    float getGain() const { return gain; }

    void process (float* const* ch, int numCh, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            float peak = 0.0f;
            for (int c = 0; c < numCh; ++c) peak = std::max (peak, std::abs (ch[c][i]));
            env = peak > env ? peak : env + detRel * (peak - env);

            if (env >= openLin)       { open = true; holdLeft = holdLen; }
            else if (env < closeLin)  { if (holdLeft > 0) --holdLeft; else open = false; }
            else if (open)            { holdLeft = holdLen; }

            const float target = (! enabled || open) ? 1.0f : floorGain;
            if (gain != target)
            {
                gain += (target > gain ? gOpen : gClose) * (target - gain);
                if (std::abs (target - gain) < 1.0e-6f) gain = target;
            }
            if (gain != 1.0f)
                for (int c = 0; c < numCh; ++c) ch[c][i] *= gain;
        }
    }
private:
    bool enabled = false, open = true;
    int holdLen = 1, holdLeft = 0;
    float env = 0, gain = 1, detRel = 0, gOpen = 0, gClose = 0, floorGain = 0.003f;
    float openLin = 0, closeLin = 0;
};

//==============================================================================
// Pickup voicing: fixed corrective curves + level normalisation so the
// compressor and warmth stage respond the same to single coils and humbuckers.
class PickupVoicer
{
public:
    void prepare (double fs) { sr = fs; for (auto& b : eq) b.reset(); current = -1; }

    void setMode (int mode)
    {
        if (mode == current) return;
        if (current == 2 || current < 0) for (auto& b : eq) b.reset();   // from Flat: no stale state
        current = mode;
        if (mode == 0)          // Single coil: add body, tame the ice-pick, smooth the top
        {
            eq[0].setLowShelf (sr, 180.0, 2.0);
            eq[1].setPeak (sr, 400.0, 0.9, 1.0);
            eq[2].setPeak (sr, 3300.0, 1.3, -2.5);
            eq[3].setHighShelf (sr, 9000.0, -1.0);
            levelDb = 3.0f;
        }
        else if (mode == 1)     // Humbucker: clear the mud, open up detail and air
        {
            eq[0].setLowShelf (sr, 110.0, -1.0);
            eq[1].setPeak (sr, 320.0, 0.9, -2.5);
            eq[2].setPeak (sr, 2600.0, 0.8, 2.0);
            eq[3].setHighShelf (sr, 6000.0, 1.5);
            levelDb = -2.5f;
        }
        else
        {
            for (auto& b : eq) b.setIdentity();
            levelDb = 0.0f;
        }
    }

    float getLevelOffsetDb() const { return levelDb; }

    void process (float* const* ch, int numCh, int n)
    {
        if (current == 2) return;
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < n; ++i)
            {
                float x = ch[c][i];
                for (auto& b : eq) x = b.process (x, c);
                ch[c][i] = x;
            }
    }
private:
    double sr = 48000.0;
    int current = -1;
    float levelDb = 0.0f;
    std::array<Biquad, 4> eq;
};

//==============================================================================
// Optical-style sustain compressor: RMS detection through a sidechain high-pass (so low notes
// don't over-compress), soft knee, a two-stage opto release (fast, then a slow "memory" tail like
// an LA-2A), automatic make-up referenced to -18 dBFS, parallel blend, and optional 3 ms lookahead.
class SustainCompressor
{
public:
    static constexpr double kLookaheadMs = 3.0;

    void prepare (double fs)
    {
        sr = fs; rms = 0.0f; grDb = 0.0f; fast = 0.0f; mem = 0.0f;
        rmsCoef = onePoleCoef (8.0, fs);
        fadeCoef = onePoleCoef (7.0, fs);      // two stages: ~20 ms S-shaped fade
        paramCoef = onePoleCoef (12.0, fs);    // Sustain moves glide (two stages, ~40 ms): no step in the gain
        // fast release: program-dependent, 70 ms (light) to 900 ms (heavy), see process()
        chargeCoef = onePoleCoef (500.0, fs);  // ...the memory charges while you keep compressing...
        relSlow = onePoleCoef (1500.0, fs);    // ...and lets go slowly
        relLin = (float) (2.0 / fs);           // 2 dB per second on top: a long tail, but one that ends
        laCoef = onePoleCoef (10.0, fs);
        scHp.reset();
        scHp.setHighPass (fs, 100.0, 0.7071);
        laLen = std::max (1, (int) std::lround (kLookaheadMs * 0.001 * fs));
        int size = 1; while (size < laLen + 2) size <<= 1;
        for (auto& l : laBuf) l.assign ((size_t) size, 0.0f);
        laMask = size - 1; laW = 0;
        primed = false;
    }

    void resetState()      // no allocation
    {
        rms = 0.0f; grDb = 0.0f; fast = 0.0f; mem = 0.0f;
        scHp.reset();
        for (auto& l : laBuf) std::fill (l.begin(), l.end(), 0.0f);
        laW = 0;
        primed = false;
    }

    static int lookaheadSamples (double fs) { return std::max (1, (int) std::lround (kLookaheadMs * 0.001 * fs)); }

    // enabled = the compressor's On switch; switching (or Sustain to 0) fades instead of clicking.
    void setParams (float sustain01, float attackMs, float mix01, bool enabled = true, bool lookahead = false)
    {
        thrT     = -12.0f - 32.0f * sustain01;
        const float ratio = 1.0f + 11.0f * sustain01 * sustain01 + 2.0f * sustain01;
        slopeT   = 1.0f - 1.0f / ratio;
        atkCoef  = onePoleCoef (attackMs, sr);
        makeupT  = 0.85f * gainComputer (-18.0f, thrT, slopeT) + 0.5f * gainComputer (-30.0f, thrT, slopeT) * sustain01;
        mixTarget = (enabled && sustain01 > 0.001f) ? mix01 : 0.0f;
        laTarget = lookahead ? 1.0f : 0.0f;
        if (! primed)
        {
            thr.snap (thrT); slope.snap (slopeT); makeup.snap (makeupT); mixG.snap (mixTarget); laG.snap (laTarget);
            primed = true;
        }
    }

    float getGainReductionDb() const { return grDb * std::clamp (mixG.value(), 0.0f, 1.0f); }   // what's actually applied

    void process (float* const* ch, int numCh, int n)
    {
        // the detector always runs (cheap), so switching back on resumes at the correct gain
        for (int i = 0; i < n; ++i)
        {
            float peak = 0.0f;
            for (int c = 0; c < numCh; ++c) peak = std::max (peak, std::abs (scHp.process (ch[c][i], c)));
            rms += rmsCoef * (peak * peak - rms);
            const float lvlDb = 10.0f * std::log10 (rms + 1.0e-12f) + 3.0f;
            const float thrNow = thr.next (thrT, paramCoef), slopeNow = slope.next (slopeT, paramCoef);
            const float makeupNow = makeup.next (makeupT, paramCoef);
            const float target = gainComputer (lvlDb, thrNow, slopeNow);

            // two-stage opto release: the fast stage follows the signal; the memory charges towards half
            // of the reduction while compression lasts and releases slowly - quick on short notes, a long
            // smooth tail after sustained playing
            if (target > fast) fast += atkCoef * (target - fast);
            else
            {
                // the harder it was squeezed, the slower it lets go (70 ms light ... 900 ms heavy)
                const double relMs = std::clamp (70.0 + 45.0 * (double) fast, 70.0, 900.0);
                fast += onePoleCoef (relMs, sr) * (target - fast);
            }
            const float memT = 0.5f * target;
            if (memT > mem) mem += chargeCoef * (memT - mem);
            else
            {
                // plus a gentle steady release (easing in smoothly near the end), so the tail always finishes
                const float d = mem - memT;
                mem = std::max (memT, mem - relSlow * d - relLin * d / (d + 0.5f));
            }
            grDb = std::max (fast, mem);

            // lookahead: the audio is heard 3 ms late, so the gain is already moving when a pick hits
            const float la = laG.next (laTarget, laCoef);
            float in[2];
            for (int c = 0; c < numCh; ++c)
            {
                auto& buf = laBuf[(size_t) c];
                buf[(size_t) laW] = ch[c][i];
                const float delayed = buf[(size_t) ((laW - laLen) & laMask)];
                in[c] = ch[c][i] + la * (delayed - ch[c][i]);
            }
            laW = (laW + 1) & laMask;

            const float wetAmt = mixG.next (mixTarget, fadeCoef);
            const float g = wetAmt < 1.0e-6f ? 1.0f : dbToGain (makeupNow - grDb);
            for (int c = 0; c < numCh; ++c)
                ch[c][i] = in[c] + wetAmt * (in[c] * g - in[c]);
        }
    }
private:
    static float gainComputer (float lvlDb, float thrDb, float slope)
    {
        const float knee = 12.0f;
        const float over = lvlDb - thrDb;
        if (2.0f * over < -knee) return 0.0f;
        if (2.0f * std::abs (over) <= knee)
        {
            const float t = over + knee * 0.5f;
            return slope * t * t / (2.0f * knee);
        }
        return slope * over;
    }

    double sr = 48000.0;
    bool primed = false;
    float rms = 0, grDb = 0, fast = 0, mem = 0, rmsCoef = 0, atkCoef = 0, paramCoef = 0.01f, fadeCoef = 0.01f;
    float chargeCoef = 0, relSlow = 0, relLin = 0, laCoef = 0.01f;
    float thrT = -20, slopeT = 0.5f, makeupT = 0, mixTarget = 1, laTarget = 0;
    Glide thr, slope, makeup, mixG, laG;
    Biquad scHp;
    std::array<std::vector<float>, 2> laBuf;
    int laLen = 1, laMask = 1, laW = 0;
};

//==============================================================================
// Warmth: a tube-style stage, 4x oversampled.
//  - Asymmetric soft saturation (mostly 2nd harmonic), unity gain for small signals.
//  - Frequency-dependent: lows are pushed into the curve harder than highs (pre-emphasis before it,
//    the exact inverse after), so it thickens the low end and leaves the top clear.
//  - Breathing bias: the operating point shifts as you dig in (like grid-bias shift in a tube), so
//    the colour blooms with playing dynamics instead of being a fixed curve.
//  - Antiderivative anti-aliasing (ADAA) on the curve, on top of the oversampling.
//  - The bottom 10 % of the knob fades it in from nothing, so 0 % is truly clean.
class Warmth
{
public:
    void prepare (double fs, int maxBlock, int numCh)
    {
        sr = fs;
        os = std::make_unique<juce::dsp::Oversampling<float>> (
                 (size_t) numCh, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os->initProcessing ((size_t) maxBlock);
        const double osRate = fs * 4.0;
        dcCoef = 1.0 - 2.0 * kPiD * 8.0 / osRate;
        kCoef = onePoleCoef (20.0, osRate);
        wetCoef = onePoleCoef (10.0, osRate);
        envAtt = onePoleCoef (3.0, osRate);
        tightCoef = onePoleCoef (20.0, osRate);
        tightLp.setLowPass (osRate, 200.0, 0.7071);
        envRel = onePoleCoef (120.0, osRate);
        resetState();
    }

    float getLatency() const { return os ? os->getLatencyInSamples() : 0.0f; }

    void resetState()      // no allocation
    {
        os->reset();
        for (auto& d : dc) d = {};
        for (auto& s : prevU) s = (double) 0.3f;     // = the resting bias c0 exactly
        for (auto& s : prevLU) s = logCosh ((double) 0.3f);
        prevC = (double) 0.3f;
        prevLC = logCosh ((double) 0.3f);
        resume = false;
        tightLp.reset(); tightWasOn = false;
        full.reset(); smooth.reset(); pre.reset(); post.reset();
        env = 0.0f;
        kCur = -1.0f;
    }

    // tight01: 0 = normal, 1 = "Tight": the deep lows (below ~200 Hz) go around the curve untouched
    // (a band split inside the oversampled stage), so they add no bloom - and nothing is boosted afterwards
    void setAmount (float w01, float tight01 = 0.0f)
    {
        w01 = std::max (0.0f, w01);
        kTarget = 0.5f + 4.5f * std::pow (w01, 1.4f);
        wetTarget = std::min (1.0f, w01 / 0.1f);
        biasDepth = 0.2f * w01;
        const bool first = kCur < 0.0f;
        if (first) { kCur = kTarget; wet.snap (wetTarget); }
        Biquad tf, ts, tp, tq;
        tf.setPeak (sr, 220.0, 0.7, 2.5 * w01);
        ts.setHighShelf (sr, 7500.0, -2.0 * w01);
        // lows drive the tube harder (or, Tight, much less hard)... and are put back exactly afterwards
        tightTarget = std::clamp (tight01, 0.0f, 1.0f);
        if (first) tightNow = tightTarget;
        const double emph = 3.0 * w01;                        // (the Tight split works on top of this)
        tp.setLowShelf (sr, 300.0, emph);
        tq.setLowShelf (sr, 300.0, -emph);
        full.glideTo (tf, first ? 0 : kCoefRamp);
        smooth.glideTo (ts, first ? 0 : kCoefRamp);
        pre.glideTo (tp, first ? 0 : kCoefRamp);
        post.glideTo (tq, first ? 0 : kCoefRamp);
    }

    void process (float* const* ch, int numCh, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            pre.advance();
            for (int c = 0; c < numCh; ++c) ch[c][i] = pre.process (ch[c][i], c);
        }

        juce::dsp::AudioBlock<float> block (ch, (size_t) numCh, (size_t) n);
        auto up = os->processSamplesUp (block);

        const float c0 = 0.3f;                       // resting bias -> even harmonics
        const size_t nCh = std::min (up.getNumChannels(), (size_t) numCh);
        const size_t nUp = up.getNumSamples();
        float* d[2] = { up.getChannelPointer (0), nCh > 1 ? up.getChannelPointer (1) : nullptr };
        for (size_t i = 0; i < nUp; ++i)
        {
            { const float k2 = kCur + kCoef * (kTarget - kCur); kCur = (k2 == kCur) ? kTarget : k2; }   // drive glides, lands exactly
            const float wetNow = wet.next (wetTarget, wetCoef);

            // breathing bias: follows how hard you play (both channels together)
            float peak = 0.0f;
            for (size_t chn = 0; chn < nCh; ++chn) peak = std::max (peak, std::abs (d[chn][i]));
            env += (peak > env ? envAtt : envRel) * (peak - env);

            if (wetNow == 0.0f && wetTarget == 0.0f)
            {
                // Warmth at 0 %: the curve isn't heard (y = x exactly), so it isn't computed; only the DC
                // blocker runs, as always. The curve picks up from the current signal when it comes back.
                resume = true;
                for (size_t chn = 0; chn < nCh; ++chn)
                {
                    auto& st = dc[chn];
                    const float y = d[chn][i];
                    const double o = (double) y - st.x1 + dcCoef * st.y1;
                    st.x1 = y; st.y1 = o;
                    d[chn][i] = (float) o;
                }
                continue;
            }

            const double c = (double) c0 + (double) biasDepth * std::tanh ((double) (env * kCur));
            const double tc = std::tanh (c);
            const double lc = logCosh (c);
            if (resume)
            {
                prevC = c; prevLC = lc;
                for (size_t chn = 0; chn < nCh; ++chn) { prevU[chn] = (double) kCur * d[chn][i] + c; prevLU[chn] = logCosh (prevU[chn]); }
                resume = false;
            }
            // what the curve gives for silence as the bias moves (same ADAA form): subtracted, so the bias
            // shift itself never makes a sound - only the signal is coloured (and a silent channel stays silent)
            const double dcB = c - prevC;
            const double g0 = std::abs (dcB) > 1.0e-6 ? (lc - prevLC) / dcB : std::tanh (0.5 * (c + prevC));
            prevC = c; prevLC = lc;
            const double norm = 1.0 / ((double) kCur * (1.0 - tc * tc));
            // Tight glides in and out (~20 ms); its band split runs only while it is in use
            { const float t2 = tightNow + tightCoef * (tightTarget - tightNow); tightNow = std::abs (tightTarget - t2) < 1.0e-6f ? tightTarget : t2; }
            const bool tightOn = tightNow > 0.0f;
            if (tightOn && ! tightWasOn) tightLp.reset();
            tightWasOn = tightOn;
            const float tNow = tightNow;

            for (size_t chn = 0; chn < nCh; ++chn)
            {
                auto& st = dc[chn];
                const float x = d[chn][i];
                // Tight: the lows that go around the curve (the same signal, split exactly: x = lo + rest)
                const float lo = tightOn ? tightLp.process (x, (int) chn) : 0.0f;
                const float xin = x - tNow * lo;
                // first-order ADAA of tanh: the average of the curve between the previous and this
                // input, from its antiderivative log(cosh(u)) - removes most of the aliasing.
                // log(cosh) of the previous input is kept from the last sample (one per sample, not two).
                const double u = (double) kCur * xin + c, u0 = prevU[chn];
                const double lu = logCosh (u), lu0 = prevLU[chn];
                prevU[chn] = u; prevLU[chn] = lu;
                const double du = u - u0;
                const double g = std::abs (du) > 1.0e-6 ? (lu - lu0) / du : std::tanh (0.5 * (u + u0));
                const float shaped = (float) ((g - g0) * norm);
                // the lows that went around the curve are still kept under its ceiling (a limiter that is
                // exactly linear at playing levels, so Tight adds no bloom; it only catches very hot peaks)
                const float loOut = tightOn ? limitLows (lo) : 0.0f;
                const float y = x + wetNow * ((shaped + tNow * loOut) - x);
                const double o = (double) y - st.x1 + dcCoef * st.y1;   // DC blocker (double: pole at 0.9997)
                st.x1 = y; st.y1 = o;
                d[chn][i] = (float) o;
            }
        }
        os->processSamplesDown (block);

        for (int i = 0; i < n; ++i)
        {
            post.advance(); full.advance(); smooth.advance();
            for (int cc = 0; cc < numCh; ++cc)
                ch[cc][i] = smooth.process (full.process (post.process (ch[cc][i], cc), cc), cc);
        }
    }
private:
    // exactly linear up to -6 dBFS (far above playing level), then a smooth knee up to a 0 dBFS ceiling
    static inline float limitLows (float v)
    {
        constexpr float c = 1.0f, th = 0.5f;
        const float a = std::abs (v);
        if (a <= th) return v;
        const float s = th + (c - th) * std::tanh ((a - th) / (c - th));
        return v < 0.0f ? -s : s;
    }

    static inline double logCosh (double u)
    {
        const double a = std::abs (u);
        return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531;   // stable log(cosh(u))
    }

    struct DC { double x1 = 0, y1 = 0; };
    double sr = 48000.0;
    float kTarget = 1, kCur = -1, kCoef = 0.01f, wetTarget = 1, wetCoef = 0.01f;
    float env = 0, envAtt = 0.01f, envRel = 0.001f, biasDepth = 0;
    double dcCoef = 0.999;
    Glide wet;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    std::array<DC, 2> dc;
    std::array<double, 2> prevU { 0.3, 0.3 };
    double prevC = 0.3, prevLC = 0.0;
    float tightNow = 0.0f, tightTarget = 0.0f, tightCoef = 0.01f;
    bool tightWasOn = false;
    Biquad tightLp;
    std::array<double, 2> prevLU { 0.0, 0.0 };
    bool resume = false;
    Biquad full, smooth, pre, post;
};

//==============================================================================
// Clarity: a dynamic low-mid cut. It compares the low mids (~200-450 Hz, where mud lives) with the
// definition region (~1-3 kHz). When the low mids pile up beyond a natural balance - chords, the neck
// pickup, a boomy cabinet - it dips ~300 Hz, only as far and as long as needed. Balanced playing passes
// untouched, so it never thins the tone the way a fixed EQ cut does. At 0 it is switched out (bit-exact).
class Clarity
{
public:
    static constexpr double kFreq = 300.0, kQ = 0.8;
    static constexpr float kMaxCutDb = 8.0f;

    void prepare (double fs)
    {
        sr = fs;
        loHp.setHighPass (fs, 200.0, 0.7071); loLp.setLowPass (fs, 450.0, 0.7071);
        hiHp.setHighPass (fs, 1000.0, 0.7071); hiLp.setLowPass (fs, 3000.0, 0.7071);
        detCoef = onePoleCoef (40.0, fs);          // band powers averaged over ~40 ms
        cutAtt = onePoleCoef (25.0, fs);           // the dip comes in quickly...
        cutRel = onePoleCoef (250.0, fs);          // ...and lets go gently
        resetState();
    }

    void resetState()
    {
        loHp.reset(); loLp.reset(); hiHp.reset(); hiLp.reset(); eq.reset();
        Biquad flat; flat.setIdentity(); eq.glideTo (flat, 0);
        pLo = pHi = 1.0e-12; cutDb = 0.0f; appliedDb = 0.0f; counter = 0; active = false;
    }

    void setAmount (float amount01) { amt = std::clamp (amount01, 0.0f, 1.0f); }
    float getCutDb() const { return active ? appliedDb : 0.0f; }

    void process (float* const* ch, int numCh, int n)
    {
        if (amt <= 0.0f && ! active) return;                 // switched out: untouched
        if (! active) { resetState(); active = true; }        // fresh start when switched in
        for (int i = 0; i < n; ++i)
        {
            float x = ch[0][i];
            if (numCh > 1) x = 0.5f * (x + ch[1][i]);
            const double lo = loLp.process (loHp.process (x, 0), 0);
            const double hi = hiLp.process (hiHp.process (x, 0), 0);
            pLo += detCoef * (lo * lo - pLo);
            pHi += detCoef * (hi * hi - pHi);
            // how far the low mids sit above their natural share: on a balanced clean guitar they sit about
            // 4-7 dB under the 1-3 kHz region (a ringing note darkens towards 3 dB); chords and a dark neck pickup bring them up to level with it
            const double ratioDb = 10.0 * std::log10 ((pLo + 1.0e-12) / (pHi + 1.0e-12));
            const float excess = (float) std::max (0.0, ratioDb + 3.0);
            const float want = amt * std::min (kMaxCutDb, 1.6f * excess);
            cutDb += (want > cutDb ? cutAtt : cutRel) * (want - cutDb);

            // the EQ follows every 16 samples (a short coefficient ramp in between: no zipper noise)
            if (++counter >= 16)
            {
                counter = 0;
                if (std::abs (cutDb - appliedDb) > 1.0e-4f || (cutDb == 0.0f && appliedDb != 0.0f))
                {
                    appliedDb = cutDb < 1.0e-4f ? 0.0f : cutDb;
                    Biquad t;
                    if (appliedDb == 0.0f) t.setIdentity(); else t.setPeak (sr, kFreq, kQ, -appliedDb);
                    eq.glideTo (t, 16);
                }
            }
            eq.advance();
            for (int c = 0; c < numCh; ++c) ch[c][i] = eq.process (ch[c][i], c);
        }
        if (amt <= 0.0f && appliedDb == 0.0f && cutDb < 1.0e-6f) active = false;   // faded out: switch off
    }

private:
    double sr = 48000.0, pLo = 1e-12, pHi = 1e-12;
    float detCoef = 0.01f, cutAtt = 0.01f, cutRel = 0.001f;
    float amt = 0.0f, cutDb = 0.0f, appliedDb = 0.0f;
    int counter = 0;
    bool active = false;
    Biquad loHp, loLp, hiHp, hiLp, eq;
};

//==============================================================================
// Pick: a transient shaper. A fast and a slow envelope of the signal; where the fast one runs ahead
// (the pick hitting the string) the gain is raised (Pick > 0: more snap and definition) or lowered
// (Pick < 0: a softer, rounder attack). The sustain of the note is left alone. At 0 it is switched out.
class PickShaper
{
public:
    static constexpr float kMaxDb = 9.0f;

    void prepare (double fs)
    {
        fastRel = onePoleCoef (300.0, fs);      // peak follower: catches each peak, lets go slowly
        slowAtt = onePoleCoef (25.0, fs);       // the "body" follows it, but rises 25 ms behind
        gainCoef = onePoleCoef (1.0, fs);       // smooths the gain itself (1 ms: quick enough for the pick)
        resetState();
    }

    void resetState() { fast = slow = 0.0f; gDb = 0.0f; active = false; }
    void setAmount (float bipolar) { amt = std::clamp (bipolar, -1.0f, 1.0f); }

    void process (float* const* ch, int numCh, int n)
    {
        if (amt == 0.0f && ! active) return;               // switched out: untouched
        if (! active) { fast = slow = 0.0f; gDb = 0.0f; active = true; }
        for (int i = 0; i < n; ++i)
        {
            float a = std::abs (ch[0][i]);
            if (numCh > 1) a = std::max (a, std::abs (ch[1][i]));
            // fast: the level, peak by peak. slow: the same level, but rising 25 ms late and falling with
            // it - so the two agree exactly while a note rings, and differ only when a pick makes it jump
            if (a > fast) fast = a; else fast += fastRel * (a - fast);
            if (fast > slow) slow += slowAtt * (fast - slow); else slow = fast;
            // how far the attack runs ahead of the body, in dB; the first 1 dB is waveform ripple, ignored
            const float lead = 20.0f * std::log10 ((fast + 1.0e-6f) / (slow + 1.0e-6f)) - 1.0f;
            const float target = std::clamp (amt * std::max (0.0f, lead), -kMaxDb, kMaxDb);
            gDb += gainCoef * (target - gDb);
            const float g = dbToGain (gDb);
            for (int c = 0; c < numCh; ++c) ch[c][i] *= g;
        }
        if (amt == 0.0f && std::abs (gDb) < 1.0e-5f) active = false;
    }

private:
    float fastRel = 0, slowAtt = 0, gainCoef = 0;
    float fast = 0, slow = 0, gDb = 0, amt = 0;
    bool active = false;
};

//==============================================================================
class ToneStack
{
public:
    void prepare (double fs) { sr = fs; for (auto& b : f) b.reset(); primed = false; }

    void set (float bass, float mid, float midFreq, float presence, float treble, float lowCut, float highCut)
    {
        std::array<Biquad, 6> t;
        t[0].setHighPass (sr, lowCut, 0.7071);
        t[1].setLowShelf (sr, 100.0, bass);
        t[2].setPeak (sr, midFreq, 0.8, mid);
        t[3].setPeak (sr, 3000.0, 0.7, presence);
        t[4].setHighShelf (sr, 6000.0, treble);
        t[5].setLowPass (sr, highCut, 0.7071);
        for (size_t k = 0; k < f.size(); ++k) f[k].glideTo (t[k], primed ? kCoefRamp : 0);
        primed = true;
    }

    void process (float* const* ch, int numCh, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            for (auto& b : f) b.advance();
            for (int c = 0; c < numCh; ++c)
            {
                float x = ch[c][i];
                for (auto& b : f) x = b.process (x, c);
                ch[c][i] = x;
            }
        }
    }
private:
    double sr = 48000.0;
    bool primed = false;
    std::array<Biquad, 6> f;
};

//==============================================================================
// Filter-based speaker cabinet voicing (no IR needed, zero latency).
class CabSim
{
public:
    void prepare (double fs) { sr = fs; for (auto& b : f) b.reset(); current = -1; }

    void setMode (int t)
    {
        if (t == current) return;
        if (current <= 0 || current >= 4) for (auto& b : f) b.reset();   // from Off / IR: no stale state
        current = t;
        switch (t)
        {
            case 1: // Open-back 1x12: airy, lighter low end
                f[0].setHighPass (sr, 75.0, 0.7);
                f[1].setPeak (sr, 115.0, 1.2, 3.0);
                f[2].setPeak (sr, 550.0, 1.0, -2.5);
                f[3].setPeak (sr, 2300.0, 1.5, 2.0);
                f[4].setLowPass (sr, 5800.0, 0.75);
                f[5].setLowPass (sr, 7500.0, 0.55);
                break;
            case 2: // 2x12: balanced, fuller
                f[0].setHighPass (sr, 70.0, 0.75);
                f[1].setPeak (sr, 105.0, 1.1, 3.5);
                f[2].setPeak (sr, 420.0, 1.0, -2.5);
                f[3].setPeak (sr, 2600.0, 1.4, 2.5);
                f[4].setLowPass (sr, 5200.0, 0.8);
                f[5].setLowPass (sr, 6800.0, 0.55);
                break;
            case 3: // Closed 4x12: tight thump, focused mids
                f[0].setHighPass (sr, 65.0, 0.9);
                f[1].setPeak (sr, 95.0, 1.0, 4.5);
                f[2].setPeak (sr, 650.0, 1.0, -2.0);
                f[3].setPeak (sr, 3000.0, 1.6, 3.0);
                f[4].setLowPass (sr, 4600.0, 0.9);
                f[5].setLowPass (sr, 6200.0, 0.55);
                break;
            default:
                for (auto& b : f) b.setIdentity();
                break;
        }
    }

    void process (float* const* ch, int numCh, int n)
    {
        if (current <= 0 || current >= 4) return;          // Off, or the IR cabinet (handled by IRCab)
        for (int c = 0; c < numCh; ++c)
            for (int i = 0; i < n; ++i)
            {
                float x = ch[c][i];
                for (auto& b : f) x = b.process (x, c);
                ch[c][i] = x * 0.85f;
            }
    }
private:
    double sr = 48000.0;
    int current = -1;
    std::array<Biquad, 6> f;
};

//==============================================================================
// A smooth random wobble in [-1, 1]: picks a new random target every few tenths of a second
// (random length too) and moves there along a cosine curve. Deterministic (fixed seed).
struct SmoothRandom
{
    void init (uint32_t seed, double fs, float minSeconds, float maxSeconds)
    {
        state = seed ? seed : 1u; rate = fs; minS = minSeconds; maxS = maxSeconds;
        from = 0.0f; to = rnd(); pos = 0; len = segLength();
    }
    inline float next()
    {
        const float t = (float) pos / (float) len;
        const float v = from + (to - from) * (0.5f - 0.5f * std::cos (kPi * t));
        if (++pos >= len) { from = to; to = rnd(); pos = 0; len = segLength(); }
        return v;
    }
private:
    inline float rnd() { state = state * 1664525u + 1013904223u; return (float) (state >> 8) * (2.0f / 16777216.0f) - 1.0f; }
    int segLength() { const float u = 0.5f * (rnd() + 1.0f); return std::max (1, (int) ((minS + u * (maxS - minS)) * rate)); }
    uint32_t state = 1; double rate = 48000.0; float minS = 0.5f, maxS = 1.0f, from = 0, to = 0; int pos = 0, len = 1;
};

//==============================================================================
// Smooth stereo delay: tape-style glide on time changes, darkening repeats, ping-pong width and
// ducking. Modulation is switchable between a gentle sine wow and "tape": random wow plus flutter,
// with soft saturation that builds up on each repeat. Every control glides per sample.
class SmoothDelay
{
public:
    void prepare (double fs)
    {
        sr = fs;
        size = 1;
        const int needed = (int) (fs * 3.2) + 8;
        while (size < needed) size <<= 1;
        for (auto& b : buf) b.assign ((size_t) size, 0.0f);
        mask = size - 1;
        timeSmooth = onePoleCoef (180.0, fs);
        velCoef = onePoleCoef (15.0, fs);
        paramCoef = onePoleCoef (12.0, fs);    // two stages: ~30 ms S-shaped glide
        duckAtt = onePoleCoef (8.0, fs);
        duckRel = onePoleCoef (320.0, fs);
        hp.setHighPass (sr, 110.0, 0.6);
        resetState();
    }

    void resetState()      // no allocation, O(1): a complete restart, modulation included
    {
        clearAudio();
        w = 0;
        phase = 0.0f;
        wow.init (0x1234567u, sr, 0.6f, 1.4f);          // slow, uneven tape-speed drift
        flutL.init (0x2468aceu, sr, 0.06f, 0.16f);      // fast capstan flutter, a little different per side
        flutR.init (0x13579bdu, sr, 0.06f, 0.16f);
        primed = false;
        idle = true;
    }

    void setParams (float timeMs, float feedback01, float toneHz, float mod01, float duck01,
                    float width01, float mix01, bool enabled, bool tapeStyle = false)
    {
        styleT = tapeStyle ? 1.0f : 0.0f;
        targetDelay = std::clamp ((double) timeMs * 0.001 * sr, 4.0, sr * 3.0);
        fbT = std::min (feedback01, 0.95f);
        modT = mod01 * (float) (0.0016 * sr);
        phaseInc = (float) (2.0 * kPiD * (0.45 + 0.35 * mod01) / sr);
        duckT = duck01; widthT = width01;
        mixT = enabled ? mix01 : 0.0f;
        Biquad t; t.setLowPass (sr, toneHz, 0.6);
        lp.glideTo (t, primed ? kCoefRamp : 0);

        if (! primed)
        {
            curDelay = targetDelay;
            fb.snap (fbT); modDepth.snap (modT); duck.snap (duckT); width.snap (widthT); mix.snap (mixT);
            inFade.snap (1.0f); style.snap (styleT);
            idle = mixT <= 0.0f;
            primed = true;
        }
        else if (mixT > 0.0f && idle)
        {
            clearAudio();                 // waking up: start from silence and fade the input in too,
            curDelay = targetDelay;       // so the first echo doesn't start mid-waveform
            idle = false;
        }
    }

    // L and R are the output channels (R may be nullptr for mono).
    void process (float* L, float* R, int n)
    {
        if (idle)
        {
            // resting: keep the modulation running so it comes back in step, as if it had never stopped
            for (int i = 0; i < n; ++i)
            {
                phase += phaseInc; if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
                wow.next(); flutL.next(); flutR.next();
            }
            return;
        }
        const bool stereo = R != nullptr;
        const float pc = paramCoef;
        for (int i = 0; i < n; ++i)
        {
            // tape-style glide, but capped so the "tape" never runs more than 40% fast or slow
            // the speed itself ramps (like a tape motor), so the glide starts and stops without a corner
            delayVel += velCoef * (std::clamp (timeSmooth * (targetDelay - curDelay), -0.4, 0.4) - delayVel);
            curDelay += delayVel;
            if (std::abs (targetDelay - curDelay) < 1.0e-9 && std::abs (delayVel) < 1.0e-12) { curDelay = targetDelay; delayVel = 0.0; }
            const float fbNow = fb.next (fbT, pc), modNow = modDepth.next (modT, pc);
            const float duckNow = duck.next (duckT, pc), widthNow = width.next (widthT, pc);
            const float mixNow = mix.next (mixT, pc), inNow = inFade.next (1.0f, pc);
            const float wd = stereo ? widthNow : 0.0f;
            lp.advance();

            // modulation: sine wow, or tape (random wow + flutter); switching glides between them
            phase += phaseInc; if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
            const float st = style.next (styleT, pc);
            const float wv = wow.next(), fl = flutL.next(), fr = flutR.next();
            const float mL = std::sin (phase) + st * (std::clamp (0.8f * wv + 0.3f * fl, -1.0f, 1.0f) - std::sin (phase));
            const float mR = std::sin (phase + 1.5708f) + st * (std::clamp (0.8f * wv + 0.3f * fr, -1.0f, 1.0f) - std::sin (phase + 1.5708f));
            const double dL = curDelay + (double) (modNow * (1.0f + mL));
            const double dR = curDelay + (double) (modNow * (1.0f + mR));

            const float yL = hp.process (lp.process (read (buf[0], dL), 0), 0);
            const float yR = stereo ? hp.process (lp.process (read (buf[1], dR), 1), 1) : 0.0f;

            const float in = (stereo ? 0.5f * (L[i] + R[i]) : L[i]) * inNow;
            const float wL = in + fbNow * ((1.0f - wd) * yL + wd * yR);
            const float wR = in * (1.0f - wd) + fbNow * ((1.0f - wd) * yR + wd * yL);
            // tape: gentle saturation on what goes onto the tape, so each repeat gets a little rounder
            buf[0][(size_t) w] = softLimit (wL + st * (tapeSat (wL) - wL));
            buf[1][(size_t) w] = stereo ? softLimit (wR + st * (tapeSat (wR) - wR)) : 0.0f;
            w = (w + 1) & mask;
            if (written < size) ++written;

            const float a = std::abs (in);
            duckEnv += (a > duckEnv ? duckAtt : duckRel) * (a - duckEnv);
            const float duckGain = 1.0f - 0.9f * duckNow * std::min (1.0f, duckEnv * 8.0f);
            const float wet = mixNow * duckGain;

            L[i] += wet * yL;
            if (stereo) R[i] += wet * yR;
        }
        if (mixT <= 0.0f && mix.s1 < 1.0e-6f && mix.value() < 1.0e-6f) { mix.snap (0.0f); idle = true; }
    }
private:
    void clearAudio()
    {
        written = 0;              // older samples now read as silence - no need to erase the buffers
        duckEnv = 0; delayVel = 0;
        mix.snap (0.0f); inFade.snap (0.0f);
        lp.reset(); hp.reset();
    }

    static inline float tapeSat (float x) { return std::tanh (1.4f * x) * (1.0f / 1.4f); }   // unity for small signals

    static inline float softLimit (float x)
    {
        const float a = std::abs (x);
        if (a < 0.8f) return x;
        const float s = 0.8f + 0.2f * std::tanh ((a - 0.8f) * 5.0f);
        return x < 0.0f ? -s : s;
    }

    // a sample written before the last reset reads as silence (lazy clear)
    inline float tap (const std::vector<float>& b, int idx) const
    {
        const int age = (w - 1 - idx) & mask;
        return age < written ? b[(size_t) (idx & mask)] : 0.0f;
    }

    // Read position in double: in float, (write position - delay) only resolves ~1/64 sample once the
    // write position has run up to a few hundred thousand, which quantises the delay time and the wow.
    inline float read (const std::vector<float>& b, double d) const
    {
        double rp = (double) w - d;
        while (rp < 0.0) rp += (double) size;
        const int i1 = (int) rp;
        const float t = (float) (rp - (double) i1);
        const float y0 = tap (b, i1 - 1), y1 = tap (b, i1), y2 = tap (b, i1 + 1), y3 = tap (b, i1 + 2);
        // 4-point Hermite
        const float c0 = y1, c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    double sr = 48000.0;
    std::array<std::vector<float>, 2> buf;
    int size = 1, mask = 0, w = 0, written = 0;
    bool primed = false, idle = true;
    double curDelay = 0, delayVel = 0, targetDelay = 1000, timeSmooth = 0.001, velCoef = 0.01;
    float paramCoef = 0.01f;
    float fbT = 0, modT = 0, duckT = 0, widthT = 0, mixT = 0, styleT = 0;
    Glide fb, modDepth, duck, width, mix, inFade, style;
    SmoothRandom wow, flutL, flutR;
    float phase = 0, phaseInc = 0, duckEnv = 0, duckAtt = 0, duckRel = 0;
    Biquad lp, hp;
};

//==============================================================================
// Clean reverb: early reflections, a 4-stage Hadamard diffuser and a 16-line Householder FDN
// with slow delay modulation (no metallic ringing). Every line has a calibrated absorption shelf
// (Jot): below the Tone frequency it decays in exactly the Decay time, above it about 4x faster,
// so the whole tail decays evenly and Decay means what it says.
class CleanReverb
{
public:
    static constexpr int N = 16;
    static constexpr int kTaps = 8;

    void prepare (double fs)
    {
        sr = fs;
        const double diffMs[4] = { 38.0, 21.0, 11.0, 6.0 };
        juce::Random rng (0x5eed1234);
        for (int s = 0; s < 4; ++s)
        {
            const double range = diffMs[s] * 0.001 * fs;
            for (int c = 0; c < N; ++c)
            {
                const double lo = range * c / N, hi = range * (c + 1) / N;
                const int d = std::max (1, (int) (lo + rng.nextDouble() * (hi - lo)));
                stages[(size_t) s].d[(size_t) c] = d;
                stages[(size_t) s].flip[(size_t) c] = rng.nextBool() ? 1.0f : -1.0f;
                stages[(size_t) s].lines[(size_t) c].init (d + 2);
            }
        }
        for (int c = 0; c < N; ++c)
        {
            fdn[(size_t) c].init ((int) (kBaseMs[c] * 1.45 * 0.001 * fs) + 64);
            const double inc = 2.0 * kPiD * (0.09 + 0.023 * c) / fs;
            rotC[(size_t) c] = std::cos (inc); rotS[(size_t) c] = std::sin (inc);
        }
        // pre-delay line also feeds the early reflections (up to 150 ms + 60 ms x 1.4)
        for (auto& p : pre) p.init ((int) (0.25 * fs) + 4);
        modDepth = (float) (0.00035 * fs);
        lenSmooth = 1.0 - std::exp (-1.0 / (0.250 * fs));
        preSmooth = 1.0 - std::exp (-1.0 / (0.075 * fs));   // two stages: S-shaped, no sudden pitch jump
        paramCoef = onePoleCoef (12.0, fs);    // two stages: ~30 ms S-shaped glide
        duckAtt = onePoleCoef (10.0, fs);
        duckRel = onePoleCoef (450.0, fs);     // blooms back in over about half a second
        duckGCoef = onePoleCoef (5.0, fs);
        resetState();
    }

    void resetState()      // no allocation, O(1) per line: a complete restart, modulation included
    {
        clearAudio();
        for (int c = 0; c < N; ++c) { lfoS[(size_t) c] = std::sin (c * 0.392); lfoC[(size_t) c] = std::cos (c * 0.392); }
        primed = false;
        idle = true;
    }

    void setParams (float size01, float decaySec, float preDelayMs, float toneHz, float lowCutHz,
                    float mix01, bool enabled, float duck01 = 0.0f)
    {
        duckAmt = std::clamp (duck01, 0.0f, 1.0f);
        const double scale = 0.4 + size01;
        const double t60 = std::max (decaySec, 0.1f), t60hf = t60 * kHfRatio;
        for (int c = 0; c < N; ++c)
        {
            target[(size_t) c] = kBaseMs[c] * scale * 0.001 * sr;
            const double secs = target[(size_t) c] / sr;
            gLfT[(size_t) c] = std::pow (10.0, -3.0 * secs / t60);     // exact decay below Tone...
            gHfT[(size_t) c] = std::pow (10.0, -3.0 * secs / t60hf);   // ...and faster above it
        }
        erScaleT = scale;
        kT = 1.0 / std::tan (kPiD * std::clamp ((double) toneHz, 200.0, 0.45 * sr) / sr);
        preT = std::clamp ((double) preDelayMs * 0.001 * sr, 0.0, (double) (pre[0].mask) - 0.07 * 1.4 * sr - 8.0);
        Biquad th, tl;
        th.setHighPass (sr, lowCutHz, 0.6);
        tl.setLowPass (sr, std::min (toneHz * 1.6f, 18000.0f), 0.6);
        inHp.glideTo (th, primed ? kCoefRamp : 0);
        inLp.glideTo (tl, primed ? kCoefRamp : 0);
        mixT = enabled ? mix01 : 0.0f;

        if (! primed)
        {
            snapControls();
            mix.snap (mixT); inFade.snap (1.0f);
            idle = mixT <= 0.0f;
            primed = true;
        }
        else if (mixT > 0.0f && idle)
        {
            clearAudio();                 // waking up: start from silence and fade the input in too
            snapControls();
            idle = false;
        }
    }

    void process (float* L, float* R, int n)
    {
        if (idle)
        {
            // resting: keep the modulation LFOs turning so the reverb comes back in step
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < N; ++c) stepLfo (c);
            return;
        }
        const bool stereo = R != nullptr;
        std::array<float, N> x {}, y {};
        for (int i = 0; i < n; ++i)
        {
            const float mixNow = mix.next (mixT, paramCoef);
            const float inNow = inFade.next (1.0f, paramCoef);
            glide (preMid, preT, preSmooth);
            glide (preCur, preMid, preSmooth);
            glide (erScale, erScaleT, lenSmooth);
            glide (kCur, kT, lenSmooth);
            inHp.advance(); inLp.advance();

            float inL = inLp.process (inHp.process (L[i] * inNow, 0), 0);
            float inR = stereo ? inLp.process (inHp.process (R[i] * inNow, 1), 1) : inL;
            pre[0].push (inL); pre[1].push (inR);
            const float pL = pre[0].readFrac ((float) preCur), pR = pre[1].readFrac ((float) preCur);

            // early reflections: a few distinct, decaying echoes, different on each side
            float erL = 0.0f, erR = 0.0f;
            for (int t = 0; t < kTaps; ++t)
            {
                erL += kErGain[t] * pre[0].readFrac ((float) (preCur + kErMsL[t] * 0.001 * sr * erScale));
                erR += kErGain[t] * pre[1].readFrac ((float) (preCur + kErMsR[t] * 0.001 * sr * erScale));
            }

            for (int c = 0; c < N; ++c) x[(size_t) c] = ((c & 1) ? pR : pL) * 0.35355339f;   // 8 copies each: keep the energy

            for (auto& s : stages)
            {
                for (int c = 0; c < N; ++c)
                {
                    auto& line = s.lines[(size_t) c];
                    const float out = line.readInt (s.d[(size_t) c] - 1);
                    line.push (x[(size_t) c]);
                    x[(size_t) c] = out * s.flip[(size_t) c];
                }
                hadamard (x);
            }

            const double K = kCur, invK1 = 1.0 / (K + 1.0), a1 = (1.0 - K) * invK1;
            float sum = 0.0f;
            std::array<float, N> fb {};
            for (int c = 0; c < N; ++c)
            {
                auto& cc = cur[(size_t) c];
                glide (cc, target[(size_t) c], lenSmooth);
                glide (gLf[(size_t) c], gLfT[(size_t) c], lenSmooth);
                glide (gHf[(size_t) c], gHfT[(size_t) c], lenSmooth);
                stepLfo (c);
                const float d = (float) cc + modDepth * (1.0f + (float) lfoS[(size_t) c]);
                y[(size_t) c] = fdn[(size_t) c].readFrac (d - 1.0f);
                // absorption shelf: gain gLf below the crossover, gHf above (first order, prewarped)
                const double b0 = (gHf[(size_t) c] * K + gLf[(size_t) c]) * invK1;
                const double b1 = (gLf[(size_t) c] - gHf[(size_t) c] * K) * invK1;
                auto& z = abs[(size_t) c];
                const double o = b0 * y[(size_t) c] + z;
                z = b1 * y[(size_t) c] - a1 * o;
                fb[(size_t) c] = (float) o;
                sum += (float) o;
            }
            const float hh = (2.0f / N) * sum;
            for (int c = 0; c < N; ++c)
                fdn[(size_t) c].push (fb[(size_t) c] - hh + x[(size_t) c]);

            float oL = 0.0f, oR = 0.0f;
            for (int c = 0; c < N; c += 2) { const float sgn = (c & 2) ? -1.0f : 1.0f; oL += sgn * y[(size_t) c]; oR += sgn * y[(size_t) (c + 1)]; }
            oL = oL * kOutScale + kErLevel * erL;
            oR = oR * kOutScale + kErLevel * erR;
            // ducking: the reverb backs off while you play and blooms in the gaps
            {
                const float a = stereo ? 0.5f * (std::abs (L[i]) + std::abs (R[i])) : std::abs (L[i]);
                duckEnv += (a > duckEnv ? duckAtt : duckRel) * (a - duckEnv);
            }
            const float duckTarget = 1.0f - 0.85f * duckAmt * std::min (1.0f, duckEnv * 8.0f);
            duckG += duckGCoef * (duckTarget - duckG);
            const float wetNow = mixNow * duckG;
            if (stereo) { L[i] += wetNow * oL; R[i] += wetNow * oR; }
            else        { L[i] += wetNow * 0.5f * (oL + oR); }
        }
        if (mixT <= 0.0f && mix.s1 < 1.0e-6f && mix.value() < 1.0e-6f) { mix.snap (0.0f); idle = true; }
    }
private:
    static constexpr double kBaseMs[N] = { 31.3, 35.9, 40.7, 44.9, 49.3, 54.1, 58.7, 63.1,
                                           68.3, 73.9, 79.1, 84.7, 91.3, 98.1, 105.7, 113.9 };
    static constexpr double kErMsL[kTaps] = { 7.1, 11.3, 17.9, 23.3, 29.9, 37.1, 44.3, 53.9 };
    static constexpr double kErMsR[kTaps] = { 8.3, 13.7, 19.1, 26.9, 31.7, 41.3, 47.9, 57.1 };
    static constexpr float kErGain[kTaps] = { 0.80f, -0.66f, 0.54f, 0.44f, -0.36f, 0.30f, -0.24f, 0.20f };
    static constexpr float kErLevel = 0.30f;
    static constexpr float kOutScale = 0.35355339f;   // 8 lines per side
    static constexpr double kHfRatio = 0.25;          // highs decay 4x faster than the Decay time

    struct Line
    {
        // 'filled' counts samples written since the last clear; older ones read as silence (lazy clear)
        std::vector<float> buf; int mask = 0, w = 0, filled = 0;
        void init (int maxLen) { int s = 1; while (s < maxLen + 4) s <<= 1; buf.assign ((size_t) s, 0.0f); mask = s - 1; w = 0; filled = 0; }
        void clear() { filled = 0; }
        inline void push (float v) { buf[(size_t) w] = v; w = (w + 1) & mask; if (filled <= mask) ++filled; }
        inline float at (int age) const { return age < filled ? buf[(size_t) ((w - 1 - age) & mask)] : 0.0f; }
        inline float readInt (int d) const { return at (d); }
        inline float readFrac (float d) const
        {
            const int i = (int) d; const float f = d - (float) i;
            const float a = at (i), b = at (i + 1);
            return a + f * (b - a);
        }
    };
    struct Stage { std::array<Line, N> lines; std::array<int, N> d {}; std::array<float, N> flip {}; };

    // one-pole glide in double precision that lands exactly on its target (float would stall short of it)
    static inline void glide (double& v, double t, double c) { v += c * (t - v); if (std::abs (t - v) < 1.0e-9 * (1.0 + std::abs (t))) v = t; }

    void snapControls()
    {
        cur = target; gLf = gLfT; gHf = gHfT; preCur = preMid = preT; erScale = erScaleT; kCur = kT;
    }

    void clearAudio()
    {
        for (auto& s : stages) for (auto& l : s.lines) l.clear();
        for (auto& l : fdn) l.clear();
        for (auto& p : pre) p.clear();
        abs.fill (0.0);
        inHp.reset(); inLp.reset();
        mix.snap (0.0f); inFade.snap (0.0f);
        duckEnv = 0.0f; duckG = 1.0f;
    }

    static void hadamard (std::array<float, N>& v)
    {
        for (int h = 1; h < N; h *= 2)
            for (int i = 0; i < N; i += h * 2)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = v[(size_t) j], b = v[(size_t) (j + h)];
                    v[(size_t) j] = a + b; v[(size_t) (j + h)] = a - b;
                }
        for (auto& e : v) e *= 0.25f;   // 1/sqrt(16)
    }

    double sr = 48000.0;
    std::array<Stage, 4> stages;
    std::array<Line, N> fdn;
    std::array<Line, 2> pre;
    std::array<double, N> cur {}, target {}, gLf {}, gLfT {}, gHf {}, gHfT {}, abs {};
    // modulation LFOs as rotating phasors (sine and cosine), a few multiplies per sample instead of sin()
    std::array<double, N> lfoS {}, lfoC {}, rotS {}, rotC {};
    inline void stepLfo (int c) noexcept
    {
        const double s = lfoS[(size_t) c], co = lfoC[(size_t) c];
        double ns = s * rotC[(size_t) c] + co * rotS[(size_t) c];
        double nc = co * rotC[(size_t) c] - s * rotS[(size_t) c];
        const double g = 1.5 - 0.5 * (ns * ns + nc * nc);     // keeps the amplitude at exactly 1
        lfoS[(size_t) c] = ns * g; lfoC[(size_t) c] = nc * g;
    }
    float modDepth = 0, paramCoef = 0.01f;
    double lenSmooth = 0, preSmooth = 0, preCur = 0, preMid = 0, preT = 0, erScale = 1, erScaleT = 1, kCur = 1, kT = 1;
    float mixT = 0;
    float duckAmt = 0, duckEnv = 0, duckG = 1, duckAtt = 0.01f, duckRel = 0.001f, duckGCoef = 0.01f;
    Glide mix, inFade;
    bool primed = false, idle = true;
    Biquad inHp, inLp;
};

} // namespace ct
