// Plugin measurements in the spirit of THE_LAB: harmonic distortion, worst-case aliasing,
// frequency response of the voicings, latency (reported vs measured), silence and CPU cost per module.
// Run:  BehaviourTest.exe --lab [report.md]
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"
#include "LabMeasurements.h"

namespace lab
{
namespace
{
constexpr double Fs = 48000.0;
constexpr int Order = 16, N = 1 << Order;     // 65536-point analysis
constexpr int Guard = 6;                       // bins either side of a tone (Blackman-Harris main lobe is +/-4)

double coherent (double f, double fs = Fs) { return std::round (f * N / fs) * fs / N; }
juce::String db1 (double v) { return v < -199 ? juce::String ("--") : juce::String (v, 1); }

//==============================================================================
// A configuration of the whole plugin. Unless it starts from a preset, it starts from the
// most neutral setting: Flat pickup, compressor off, Warmth 0, flat EQ, filters wide open,
// cabinet off, gate/hum/delay/reverb off. Auto Level is always off so levels are absolute.
struct Setup
{
    juce::String name;
    std::vector<std::pair<juce::String, float>> params;
    int preset = -1;
    bool keepEffects = false;     // presets: keep their delay / reverb
};

void set (CleanToneAudioProcessor& p, const juce::String& id, float v)
{
    if (auto* prm = p.apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

void apply (CleanToneAudioProcessor& p, const Setup& s)
{
    p.loadFactoryPreset (s.preset >= 0 ? s.preset : 0);
    if (s.preset < 0)
        for (auto [id, v] : std::initializer_list<std::pair<const char*, float>> {
                 { IDs::pickup, 2 }, { IDs::compOn, 0 }, { IDs::warmth, 0 }, { IDs::bass, 0 }, { IDs::mid, 0 },
                 { IDs::presence, 0 }, { IDs::treble, 0 }, { IDs::lowCut, 20 }, { IDs::highCut, 20000 },
                 { IDs::cab, 0 }, { IDs::gate, 0 }, { IDs::hum, 0 }, { IDs::dlyOn, 0 }, { IDs::revOn, 0 } })
            set (p, id, v);
    if (s.preset >= 0 && ! s.keepEffects) { set (p, IDs::dlyOn, 0); set (p, IDs::revOn, 0); }
    set (p, IDs::autoLevel, 0);
    set (p, IDs::outGain, 0);
    set (p, IDs::compOn, s.preset >= 0 ? 1.0f : 0.0f);
    for (auto& [id, v] : s.params) set (p, id, v);
}

// mono in -> stereo out, whole processor, fixed 512-sample blocks
juce::AudioBuffer<float> process (CleanToneAudioProcessor& p, const std::vector<float>& in, double fs)
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::mono());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    p.releaseResources();
    p.setBusesLayout (layout);
    p.setRateAndBufferSizeDetails (fs, 512);
    p.prepareToPlay (fs, 512);
    juce::MidiBuffer midi;
    const int total = (int) in.size();
    juce::AudioBuffer<float> out (2, total), blk (2, 512);
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = std::min (512, total - pos);
        blk.setSize (2, n, false, false, true);
        blk.copyFrom (0, 0, in.data() + pos, n);
        blk.clear (1, 0, n);
        p.processBlock (blk, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, blk, ch, 0, n);
    }
    return out;
}

// Renders a steady sine and returns the settled, post-latency analysis segment (left channel).
std::vector<float> renderSine (const Setup& s, double freq, float levelDb, double settleSec = 1.5)
{
    CleanToneAudioProcessor p;
    apply (p, s);
    p.setRateAndBufferSizeDetails (Fs, 512);
    p.prepareToPlay (Fs, 512);
    const int lat = p.getLatencySamples();
    const int settle = (int) (settleSec * Fs), total = settle + lat + N;
    std::vector<float> in ((size_t) total);
    const float a = juce::Decibels::decibelsToGain (levelDb);
    for (int i = 0; i < total; ++i) in[(size_t) i] = a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / Fs);
    const auto out = process (p, in, Fs);
    const float* d = out.getReadPointer (0, settle + lat);
    return std::vector<float> (d, d + N);
}

// Power spectrum with a 4-term Blackman-Harris window.
std::vector<double> powerSpectrum (const std::vector<float>& x)
{
    static juce::dsp::FFT fft (Order);
    std::vector<float> d ((size_t) N * 2, 0.0f);
    for (int i = 0; i < N; ++i)
    {
        const double t = 2.0 * juce::MathConstants<double>::pi * i / (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t);
        d[(size_t) i] = (float) (x[(size_t) i] * w);
    }
    fft.performFrequencyOnlyForwardTransform (d.data());
    std::vector<double> p ((size_t) N / 2 + 1);
    for (int k = 0; k <= N / 2; ++k) p[(size_t) k] = (double) d[(size_t) k] * d[(size_t) k];
    return p;
}

struct ToneAnalysis { double thdDb = -200, aliasDb = -200, worstAliasDb = -200, worstAliasHz = 0, audibleAliasDb = -200, audibleAliasHz = 0, fundDb = -200; };

// Splits the spectrum into fundamental, harmonics (n*f below Nyquist) and everything else
// (aliased products + noise). Aliasing is what remains outside DC, the fundamental and its harmonics.
ToneAnalysis analyse (const std::vector<float>& x, double freq)
{
    const auto p = powerSpectrum (x);
    const int k0 = (int) std::lround (freq * N / Fs);
    std::vector<char> used (p.size(), 0);
    auto band = [&] (int k) { double e = 0; for (int j = std::max (0, k - Guard); j <= std::min ((int) p.size() - 1, k + Guard); ++j) { e += p[(size_t) j]; used[(size_t) j] = 1; } return e; };
    for (int j = 0; j <= Guard * 2; ++j) used[(size_t) j] = 1; // DC
    const double fund = band (k0);
    double harm = 0;
    for (int h = 2; h * k0 + Guard < N / 2; ++h) harm += band (h * k0);
    double rest = 0, worst = 0, audible = 0; int worstK = 0, audibleK = 0;
    const size_t k15 = (size_t) (15000.0 * N / Fs);
    for (size_t k = 0; k < p.size(); ++k)
        if (! used[k])
        {
            rest += p[k];
            if (p[k] > worst) { worst = p[k]; worstK = (int) k; }
            if (k < k15 && p[k] > audible) { audible = p[k]; audibleK = (int) k; }
        }
    double fundPeak = 0;
    for (int j = k0 - Guard; j <= k0 + Guard; ++j) fundPeak = std::max (fundPeak, p[(size_t) j]);
    ToneAnalysis a;
    a.thdDb = 10.0 * std::log10 (harm / fund + 1e-30);
    a.aliasDb = 10.0 * std::log10 (rest / fund + 1e-30);
    a.worstAliasDb = 10.0 * std::log10 (worst / fundPeak + 1e-30);
    a.worstAliasHz = worstK * Fs / N;
    a.audibleAliasDb = 10.0 * std::log10 (audible / fundPeak + 1e-30);
    a.audibleAliasHz = audibleK * Fs / N;
    a.fundDb = 10.0 * std::log10 (fund + 1e-30);
    return a;
}

// Level of a single frequency in a signal (Goertzel on a Hann-windowed segment), dB.
double toneLevelDb (const std::vector<float>& x, double freq)
{
    const int n = (int) x.size();
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / Fs;
    double re = 0, im = 0, wsum = 0;
    for (int i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
        re += x[(size_t) i] * win * std::cos (w * i);
        im += x[(size_t) i] * win * std::sin (w * i);
        wsum += win;
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (re * re + im * im) / wsum + 1e-30);
}

const Setup neutral { "Neutral (Flat pickup, compressor off, Warmth 0, flat EQ, cabinet off)", {} };
} // namespace

//==============================================================================
Results run (juce::String& md, bool verbose)
{
    Results res;
    auto out = [&] (const juce::String& s) { md << s << "\n"; if (verbose) std::cout << s << std::endl; };

    out (juce::String ("# ") + CTS_PRODUCT_NAME + " " + CTS_VERSION + " - plugin measurements\n");
    out ("Measured offline on the whole plugin (mono in, stereo out, left channel analysed), with Auto Level off so levels are "
         "absolute. Unless a row names a preset it starts from the most neutral setting: Flat pickup, compressor off, Warmth 0, "
         "flat EQ, filters wide open, cabinet, gate, delay and reverb off. THD: harmonics 2 and up, relative to the fundamental. "
         "Aliasing: everything that is not DC, the tone or its harmonics (fold-back products plus noise), relative to the tone. "
         "Lower is better throughout. A typical DI guitar peaks around -12 dBFS.\n");

    // ---- THD vs level ----
    const std::vector<Setup> thdSetups {
        neutral,
        { "Warmth 5 % (fading in)", { { IDs::warmth, 5 } } },
        { "Warmth 30 % (default)", { { IDs::warmth, 30 } } },
        { "Warmth 60 %", { { IDs::warmth, 60 } } },
        { "Warmth 100 %", { { IDs::warmth, 100 } } },
        { "Compressor: Sustain 35 %, Attack 15 ms (default)", { { IDs::compOn, 1 }, { IDs::sustain, 35 }, { IDs::attack, 15 } } },
        { "Compressor: Sustain 100 %, Attack 1 ms (fastest)", { { IDs::compOn, 1 }, { IDs::sustain, 100 }, { IDs::attack, 1 } } },
        { "Noise gate 100 % (open)", { { IDs::gate, 100 } } },
        { "Preset: Studio Clean (effects off)", {}, 0 },
        { "Preset: Endless Sustain (effects off)", {}, 5 },
    };
    const float levels[] { -24.0f, -12.0f, -6.0f, -1.0f };
    float neutralThd12 = 0, gateThd12 = 0;
    for (double toneHz : { 110.0, 1000.0 })
    {
        out ("## Harmonic distortion (THD) at " + juce::String (toneHz < 1000 ? "110 Hz (low A string)" : "1 kHz") + "\n");
        juce::String head = "| Configuration |", rule = "|---|";
        for (float l : levels) { head << " " << juce::String ((int) l) << " dBFS |"; rule << "---|"; }
        out (head); out (rule);
        const double f = coherent (toneHz);
        for (auto& s : thdSetups)
        {
            juce::String row = "| " + s.name + " |";
            for (float l : levels)
            {
                const auto a = analyse (renderSine (s, f, l), f);
                row << " " << db1 (a.thdDb) << " dB |";
                if (toneHz > 500 && l == -12.0f)
                {
                    if (&s == &thdSetups[0]) neutralThd12 = (float) a.thdDb;
                    if (s.name.startsWith ("Noise gate")) gateThd12 = (float) a.thdDb;
                }
            }
            out (row);
        }
        out ("");
    }
    res.warmthZeroThd = neutralThd12;
    res.gateOpenThdDelta = gateThd12 - neutralThd12;
    out ("Warmth is a gentle asymmetric (mostly 2nd-harmonic) curve. The bottom 10 % of the knob fades it in from nothing, "
         "so at 0 % the path is clean; presets all use 15 % or more.\n");

    // ---- Aliasing sweep ----
    out ("## Worst-case aliasing (sine sweep 2-19 kHz at -1 dBFS)\n");
    out ("Each tone is pushed through the configuration with the cabinet off and High Cut fully open, so nothing hides the "
         "fold-back. The table shows the worst result over the sweep: total non-harmonic energy, the single largest fold-back "
         "product, and where it landed. Warmth runs at 4x oversampling.\n");
    out ("| Configuration | Total aliasing + noise | Largest alias (anywhere) | Largest alias below 15 kHz |");
    out ("|---|---|---|---|");
    const std::vector<Setup> aliasSetups { neutral, thdSetups[2], thdSetups[4],
        { "Warmth 100 % + Sustain 100 % (fastest)", { { IDs::warmth, 100 }, { IDs::compOn, 1 }, { IDs::sustain, 100 }, { IDs::attack, 1 } } } };
    const double sweep[] { 2000, 5000, 8000, 11000, 14000, 17000, 19000 };
    for (auto& s : aliasSetups)
    {
        ToneAnalysis worst, aud; double worstTone = 0, audTone = 0;
        for (double f : sweep)
        {
            const double fc = coherent (f);
            const auto a = analyse (renderSine (s, fc, -1.0f, 0.6), fc);
            if (a.worstAliasDb > worst.worstAliasDb) { const double t = worst.aliasDb; worst = a; worst.aliasDb = std::max (t, a.aliasDb); worstTone = fc; }
            else worst.aliasDb = std::max (worst.aliasDb, a.aliasDb);
            if (a.audibleAliasDb > aud.audibleAliasDb) { aud = a; audTone = fc; }
        }
        auto where = [] (double db, double hz, double tone) { return db1 (db) + " dB at " + juce::String (hz / 1000.0, 2) + " kHz (" + juce::String (tone / 1000.0, 1) + " kHz tone)"; };
        out ("| " + s.name + " | " + db1 (worst.aliasDb) + " dB | " + where (worst.worstAliasDb, worst.worstAliasHz, worstTone) + " | "
             + where (aud.audibleAliasDb, aud.audibleAliasHz, audTone) + " |");
        res.aliasing.push_back ({ s.name, (float) aud.audibleAliasDb });
    }
    out ("");

    // ---- Frequency response of the voicings ----
    out ("## Frequency response of the voicings (small signal, -30 dBFS)\n");
    out ("Level relative to the neutral setting at each frequency. Pickup rows include the pickup's level normalisation "
         "(+3 dB single coil, -2.5 dB humbucker). Cabinet rows include the cabinets' fixed -1.4 dB trim.\n");
    const double freqs[] { 60, 100, 200, 400, 800, 1600, 3200, 6400, 10000 };
    {
        juce::String head = "| Voicing |", rule = "|---|";
        for (double f : freqs) { head << " " << (f >= 1000 ? juce::String (f / 1000.0, 1) + "k" : juce::String ((int) f)) << " |"; rule << "---|"; }
        out (head); out (rule);
    }
    auto responseRow = [&] (const Setup& s, std::vector<double>* raw)
    {
        std::vector<double> lv;
        for (double f : freqs) { const double fc = coherent (f); lv.push_back (toneLevelDb (renderSine (s, fc, -30.0f, 0.5), fc)); }
        if (raw) *raw = lv;
        return lv;
    };
    std::vector<double> ref;
    responseRow (neutral, &ref);
    {
        // the neutral setting itself vs the input level
        juce::String row = "| Neutral setting vs input |";
        for (size_t i = 0; i < ref.size(); ++i)
        {
            const double dev = ref[i] - (-30.0);
            row << " " << juce::String (dev, 1) << " |";
            res.neutralResponseDev = std::max (res.neutralResponseDev, (float) std::abs (dev));
        }
        out (row);
    }
    const std::vector<Setup> voicings {
        { "Pickup: Single Coil", { { IDs::pickup, 0 } } }, { "Pickup: Humbucker", { { IDs::pickup, 1 } } },
        { "Cabinet: Open 1x12", { { IDs::cab, 1 } } }, { "Cabinet: Classic 2x12", { { IDs::cab, 2 } } },
        { "Cabinet: Closed 4x12", { { IDs::cab, 3 } } },
        { "Warmth 30 % (fullness curve)", { { IDs::warmth, 30 } } },
        { "Preset: Studio Clean (effects off)", {}, 0 } };
    for (auto& v : voicings)
    {
        const auto lv = responseRow (v, nullptr);
        juce::String row = "| " + v.name + " |";
        for (size_t i = 0; i < lv.size(); ++i) row << " " << juce::String (lv[i] - ref[i], 1) << " |";
        out (row);
    }
    out ("\nValues in dB. The neutral row shows the plugin's own coloration with everything off (the 20 Hz low cut, the 20 kHz "
         "high cut and the oversampling filters); ideally 0.0.\n");

    // ---- Latency ----
    out ("## Latency: reported vs measured\n");
    out ("| Sample rate | Reported to host | Measured (impulse, neutral) | Measured (noise cross-correlation, neutral) |");
    out ("|---|---|---|---|");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        auto measure = [fs] (const Setup& s, bool impulse, int& reported)
        {
            CleanToneAudioProcessor p;
            apply (p, s);
            p.setRateAndBufferSizeDetails (fs, 512);
            p.prepareToPlay (fs, 512);
            reported = p.getLatencySamples();
            const int n = (int) fs;
            std::vector<float> src ((size_t) n);
            juce::Random r (9);
            for (int i = 0; i < n; ++i) src[(size_t) i] = impulse ? (i == 1000 ? 0.5f : 0.0f) : 0.03f * (r.nextFloat() * 2 - 1);
            const auto o = process (p, src, fs);
            int best = 0; double bestV = -1;
            for (int lag = 0; lag < 200; ++lag)
            {
                double acc = 0;
                if (impulse) acc = std::abs (o.getSample (0, 1000 + lag));
                else for (int i = 2000; i < n - 2000; i += 2) acc += (double) src[(size_t) i] * o.getSample (0, i + lag);
                if (acc > bestV) { bestV = acc; best = lag; }
            }
            return best;
        };
        int reported = 0, rep2 = 0;
        const int mImp = measure (neutral, true, reported);
        const int mXc = measure (neutral, false, rep2);
        out ("| " + juce::String ((int) fs) + " Hz | " + juce::String (reported) + " samples (" + juce::String (1000.0 * reported / fs, 3) + " ms) | "
             + juce::String (mImp) + " | " + juce::String (mXc) + " |");
        res.latencyError = std::max (res.latencyError, std::max (std::abs (mImp - reported), std::abs (mXc - reported)));
    }
    out ("\nThe pickup, EQ and cabinet filters are minimum-phase, like analogue ones: they shift the phase of some frequencies "
         "but add no latency, so latency is measured on the neutral path.\n");

    // ---- Silence ----
    out ("## Silence in, silence out\n");
    {
        CleanToneAudioProcessor p;
        apply (p, { "Ambient Swell (delay + reverb on)", {}, 6, true });
        std::vector<float> in ((size_t) (40 * Fs), 0.0f);
        for (int i = 0; i < (int) (2 * Fs); ++i) in[(size_t) i] = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / Fs);
        const auto o = process (p, in, Fs);
        const float tail = o.getMagnitude (0, (int) (39 * Fs), (int) Fs);
        res.silenceOutDb = juce::Decibels::gainToDecibels (tail, -400.0f);
        out ("Ambient Swell (7 s reverb, 55 % delay feedback), 2 s of tone then 37 s of digital silence: output in the last second "
             + (tail == 0.0f ? juce::String ("is exact digital silence.") : "peaks at " + juce::String (res.silenceOutDb, 1) + " dBFS.")
             + " The plugin adds no noise of its own; the delay and reverb switch themselves off once their echoes have faded.\n");
    }

    // ---- Reverb decay ----
    out ("## Reverb decay: the Decay knob vs the measured RT60\n");
    out ("Impulse into the reverb alone (Size 50 %, Tone 1 kHz), decay measured by Schroeder integration (-5 to -25 dB, x3) "
         "below 300 Hz and above 2.5 kHz.\n");
    out ("| Decay knob | RT60 lows | RT60 highs |");
    out ("|---|---|---|");
    for (float decay : { 0.5f, 1.0f, 2.5f, 6.0f, 10.0f })
    {
        auto rt = [] (const std::vector<double>& e2, double fs, int from)
        {
            std::vector<double> sch (e2.size() + 1, 0.0);
            for (int i = (int) e2.size() - 1; i >= from; --i) sch[(size_t) i] = sch[(size_t) i + 1] + e2[(size_t) i];
            int i5 = -1;
            for (int i = from; i < (int) e2.size(); ++i)
            {
                const double db = 10.0 * std::log10 (sch[(size_t) i] / sch[(size_t) from] + 1.0e-300);
                if (i5 < 0 && db <= -5.0) i5 = i;
                if (db <= -25.0) return 3.0 * (i - i5) / fs;
            }
            return -1.0;
        };
        ct::CleanReverb rv; rv.prepare (Fs); rv.setParams (0.5f, decay, 0.0f, 1000.0f, 20.0f, 1.0f, true);
        const int n = (int) ((decay * 2.0 + 1.0) * Fs);
        std::vector<float> L ((size_t) n, 0.0f), R ((size_t) n, 0.0f);
        L[0] = R[0] = 1.0f;
        for (int pos = 0; pos < n; pos += 512) rv.process (L.data() + pos, R.data() + pos, std::min (512, n - pos));
        ct::Biquad lo1, lo2, hi1, hi2;
        lo1.setLowPass (Fs, 300.0, 0.7071); lo2.setLowPass (Fs, 300.0, 0.7071);
        hi1.setHighPass (Fs, 2500.0, 0.7071); hi2.setHighPass (Fs, 2500.0, 0.7071);
        std::vector<double> eLo ((size_t) n), eHi ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const float a = lo2.process (lo1.process (L[(size_t) i], 0), 0), b = hi2.process (hi1.process (L[(size_t) i], 0), 0);
            eLo[(size_t) i] = (double) a * a; eHi[(size_t) i] = (double) b * b;
        }
        out ("| " + juce::String (decay, 1) + " s | " + juce::String (rt (eLo, Fs, (int) (0.08 * Fs)), 2) + " s | "
             + juce::String (rt (eHi, Fs, (int) (0.08 * Fs)), 2) + " s |");
    }
    out ("\nEvery delay line has a calibrated absorption filter, so the lows ring for the time on the knob and the highs "
         "die away about four times faster, like a real room.\n");

    // ---- CPU ----
    out ("## CPU cost (single core, 256-sample blocks)\n");
    out ("| Module | 48 kHz | 96 kHz |");
    out ("|---|---|---|");
    {
        struct Row { juce::String name; double pct[2] {}; };
        std::vector<Row> rows { { "Noise gate" }, { "Pickup voicing" }, { "Sustain compressor" }, { "Warmth (4x oversampled)" },
                                { "Tone stack" }, { "Cabinet" }, { "Delay (stereo)" }, { "Reverb (stereo)" },
                                { "IR loader (two 0.3 s IRs blended, stereo)" },
                                { "Clarity (dynamic low-mid cut, 100 %)" }, { "Pick (transient shaper, +100 %)" },
                                { "Whole plugin (processBlock, Ambient Swell: everything on)" },
                                { "Auto Level measurement, per new setting (total ms, spread over ~0.5 s of audio)" } };
        int col = 0;
        for (double fs : { 48000.0, 96000.0 })
        {
            const double secs = 10.0;
            const int n = (int) (secs * fs);
            std::vector<float> src ((size_t) n);
            juce::Random r (4);
            for (auto& v : src) v = 0.2f * (r.nextFloat() * 2 - 1);
            auto timeMono = [&] (auto&& fn)
            {
                std::vector<float> b (src);
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                for (int pos = 0; pos + 256 <= n; pos += 256) { float* ch[1] = { b.data() + pos }; fn (ch, 256); }
                return 100.0 * (juce::Time::getMillisecondCounterHiRes() - t0) / (secs * 1000.0);
            };
            auto timeStereo = [&] (auto&& fn)
            {
                std::vector<float> L (src), R (src);
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                for (int pos = 0; pos + 256 <= n; pos += 256) fn (L.data() + pos, R.data() + pos, 256);
                return 100.0 * (juce::Time::getMillisecondCounterHiRes() - t0) / (secs * 1000.0);
            };
            { ct::Gate m; m.prepare (fs); m.setAmount (0.5f); rows[0].pct[col] = timeMono ([&] (float* const* c, int k) { m.process (c, 1, k); }); }
            { ct::PickupVoicer m; m.prepare (fs); m.setMode (0); rows[1].pct[col] = timeMono ([&] (float* const* c, int k) { m.process (c, 1, k); }); }
            { ct::SustainCompressor m; m.prepare (fs); m.setParams (0.35f, 15.0f, 1.0f, true); rows[2].pct[col] = timeMono ([&] (float* const* c, int k) { m.process (c, 1, k); }); }
            { ct::Warmth m; m.prepare (fs, 256, 1); m.setAmount (0.3f); rows[3].pct[col] = timeMono ([&] (float* const* c, int k) { m.process (c, 1, k); }); }
            { ct::ToneStack m; m.prepare (fs); m.set (2, -1, 750, 1, 1, 70, 12000); rows[4].pct[col] = timeMono ([&] (float* const* c, int k) { m.process (c, 1, k); }); }
            { ct::CabSim m; m.prepare (fs); m.setMode (1); rows[5].pct[col] = timeMono ([&] (float* const* c, int k) { m.process (c, 1, k); }); }
            { ct::SmoothDelay m; m.prepare (fs); m.setParams (380, 0.5f, 4000, 0.2f, 0.3f, 0.5f, 0.3f, true);
              rows[6].pct[col] = timeStereo ([&] (float* l, float* rr, int k) { m.process (l, rr, k); }); }
            { ct::CleanReverb m; m.prepare (fs); m.setParams (0.6f, 2.2f, 20, 6500, 200, 0.2f, true);
              rows[7].pct[col] = timeStereo ([&] (float* l, float* rr, int k) { m.process (l, rr, k); }); }
            {
                std::vector<float> ir ((size_t) (0.3 * fs));
                juce::Random rr (9);
                for (size_t i = 0; i < ir.size(); ++i) ir[i] = (rr.nextFloat() * 2 - 1) * std::exp (-(float) i / (float) (0.04 * fs));
                const auto a = ct::IRSpectra::build (ir, fs, 1), b = ct::IRSpectra::build (ir, fs, 2);
                ct::IRCab m; m.prepare (fs); m.setSources (a.get(), b.get(), true); m.setParams (true, 0.5f);
                rows[8].pct[col] = timeStereo ([&] (float* l, float* r2, int k) { float* c2[2] = { l, r2 }; m.process (c2, 2, k); });
            }
            { ct::Clarity m; m.prepare (fs); m.setAmount (1.0f); rows[9].pct[col] = timeMono ([&] (float* const* c, int k) { m.process (c, 1, k); }); }
            { ct::PickShaper m; m.prepare (fs); m.setAmount (1.0f); rows[10].pct[col] = timeMono ([&] (float* const* c, int k) { m.process (c, 1, k); }); }
            {
                CleanToneAudioProcessor p; apply (p, { "Ambient Swell", {}, 6, true });
                juce::AudioProcessor::BusesLayout layout;
                layout.inputBuses.add (juce::AudioChannelSet::mono()); layout.outputBuses.add (juce::AudioChannelSet::stereo());
                p.setBusesLayout (layout);
                p.setRateAndBufferSizeDetails (fs, 256); p.prepareToPlay (fs, 256);
                juce::MidiBuffer midi; juce::AudioBuffer<float> blk (2, 256);
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                for (int pos = 0; pos + 256 <= n; pos += 256) { blk.copyFrom (0, 0, src.data() + pos, 256); blk.clear (1, 0, 256); p.processBlock (blk, midi); }
                rows[11].pct[col] = 100.0 * (juce::Time::getMillisecondCounterHiRes() - t0) / (secs * 1000.0);
                if (col == 0) res.cpu48 = (float) rows[11].pct[0];
            }
            {
                // what the Auto Level thread does per settings change: 3 s of reference audio through a full chain
                ct::Chain c; c.prepare (fs, 512);
                ct::Settings s; s.dlyOn = 1; s.revOn = 1;
                std::vector<float> L ((size_t) (3.0 * fs)), R (L.size());
                for (size_t i = 0; i < L.size(); ++i) L[i] = src[i % src.size()];
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                c.resetState();
                for (int pos = 0; pos < (int) L.size(); pos += 512)
                {
                    const int k = std::min (512, (int) L.size() - pos);
                    float* ch[2] = { L.data() + pos, R.data() + pos };
                    c.apply (s); c.process (ch, 2, 1, k);
                }
                rows[12].pct[col] = juce::Time::getMillisecondCounterHiRes() - t0;
            }
            ++col;
        }
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            const bool ms = i + 1 == rows.size();
            out ("| " + r.name + " | " + juce::String (r.pct[0], 2) + (ms ? " ms" : " %") + " | " + juce::String (r.pct[1], 2) + (ms ? " ms" : " %") + " |");
        }
    }
    out ("\nSwitched-off delay and reverb rest completely once faded out. The compressor's detector keeps running while it's "
         "switched off so it can come back in at exactly the right level.");
    return res;
}
} // namespace lab
