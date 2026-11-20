#pragma once

// Zero-latency partitioned convolution for the IR cabinet.
//
//  - The first B taps ("head") are a direct FIR, computed sample by sample: no latency.
//  - The remaining taps are uniformly partitioned (B taps each) and convolved in the frequency
//    domain with overlap-save; partition k only affects output from k*B samples on, so it can be
//    computed at the end of each B-sample block and still be on time.
//  - B is fixed internally (64 / 128 / 256 by sample rate), so the result never depends on the
//    host's buffer size. Nothing is allocated while processing.
//  - Impulse responses are pre-transformed off the audio thread into immutable IRSpectra.

#include "DSP.h"
#include <complex>
#include <memory>

namespace ct
{
using cfloat = std::complex<float>;

//==============================================================================
struct IRSpectra
{
    int blockSize = 64;                     // B
    int numParts = 0;                       // partitions after the head
    int serial = 0;                         // identifies this IR (for Auto Level)
    std::vector<float> headReversed;        // first B taps, reversed, for the direct FIR
    std::vector<float> partsRe, partsIm;    // numParts x (B + 1) bins, real and imaginary parts apart (vectorises)

    static int blockSizeFor (double fs) { return fs > 100000.0 ? 256 : (fs > 50000.0 ? 128 : 64); }
    static int fftOrderFor (int blockSize) { int o = 0; while ((1 << o) < 2 * blockSize) ++o; return o; }

    // ir: taps at the target sample rate (already resampled, trimmed and normalised)
    static std::unique_ptr<IRSpectra> build (const std::vector<float>& ir, double fs, int serial)
    {
        auto s = std::make_unique<IRSpectra>();
        const int B = blockSizeFor (fs);
        s->blockSize = B;
        s->serial = serial;
        s->headReversed.assign ((size_t) B, 0.0f);
        for (int t = 0; t < B && t < (int) ir.size(); ++t)
            s->headReversed[(size_t) (B - 1 - t)] = ir[(size_t) t];

        const int rest = std::max (0, (int) ir.size() - B);
        s->numParts = (rest + B - 1) / B;
        s->partsRe.assign ((size_t) s->numParts * (size_t) (B + 1), 0.0f);
        s->partsIm.assign ((size_t) s->numParts * (size_t) (B + 1), 0.0f);
        juce::dsp::FFT fft (fftOrderFor (B));
        const int N = 2 * B;
        std::vector<float> buf ((size_t) (2 * N));
        for (int k = 1; k <= s->numParts; ++k)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int t = 0; t < B; ++t)
            {
                const int tap = k * B + t;
                if (tap < (int) ir.size()) buf[(size_t) t] = ir[(size_t) tap];
            }
            fft.performRealOnlyForwardTransform (buf.data(), true);
            const size_t off = (size_t) (k - 1) * (size_t) (B + 1);
            for (int b = 0; b <= B; ++b) { s->partsRe[off + (size_t) b] = buf[(size_t) (2 * b)]; s->partsIm[off + (size_t) b] = buf[(size_t) (2 * b + 1)]; }
        }
        return s;
    }
};

//==============================================================================
// One channel's input history: the newest B samples (stored twice, so the head FIR's window is
// contiguous), the block being filled, and the spectra of past input blocks (a frequency-domain delay
// line). It is shared by every IR that channel is convolved with, and it runs all the time, so an IR
// can be switched in at any moment with its whole history already there - no "starting from silence"
// transient when you load or change an IR while playing.
class ConvInput
{
public:
    void prepare (int B, int maxParts, const juce::dsp::FFT* sharedFft)
    {
        blockSize = B; fft = sharedFft; cap = std::max (1, maxParts);
        hist.assign ((size_t) (2 * B), 0.0f);
        prevBlock.assign ((size_t) B, 0.0f);
        curBlock.assign ((size_t) B, 0.0f);
        work.assign ((size_t) (4 * B), 0.0f);
        fdlRe.assign ((size_t) cap * (size_t) (B + 1), 0.0f);
        fdlIm.assign ((size_t) cap * (size_t) (B + 1), 0.0f);
        reset();
    }

    void reset()        // no allocation
    {
        std::fill (hist.begin(), hist.end(), 0.0f);
        std::fill (prevBlock.begin(), prevBlock.end(), 0.0f);
        std::fill (curBlock.begin(), curBlock.end(), 0.0f);
        std::fill (fdlRe.begin(), fdlRe.end(), 0.0f);
        std::fill (fdlIm.begin(), fdlIm.end(), 0.0f);
        p = 0; hpos = 0; newest = cap - 1;
    }

    // 1) push the sample, 2) readers take their output, 3) advance (true when a block just completed)
    inline void push (float x) noexcept
    {
        hist[(size_t) hpos] = x;
        hist[(size_t) (hpos + blockSize)] = x;
        hpos = (hpos + 1 == blockSize) ? 0 : hpos + 1;
        curBlock[(size_t) p] = x;
    }
    const float* window() const noexcept { return hist.data() + hpos; }   // newest B samples, oldest first
    int position() const noexcept { return p; }
    int capacity() const noexcept { return cap; }
    int getBlockSize() const noexcept { return blockSize; }
    const juce::dsp::FFT* getFFT() const noexcept { return fft; }

    inline bool advance() noexcept
    {
        if (++p < blockSize) return false;
        p = 0;
        // spectrum of [previous block, this block] into the delay line
        float* t = work.data();
        std::copy (prevBlock.begin(), prevBlock.end(), t);
        std::copy (curBlock.begin(), curBlock.end(), t + blockSize);
        std::fill (t + 2 * blockSize, t + 4 * blockSize, 0.0f);
        fft->performRealOnlyForwardTransform (t, true);
        newest = (newest + 1) % cap;
        const size_t off = (size_t) newest * (size_t) (blockSize + 1);
        for (int b = 0; b <= blockSize; ++b) { fdlRe[off + (size_t) b] = t[2 * b]; fdlIm[off + (size_t) b] = t[2 * b + 1]; }
        std::swap (prevBlock, curBlock);
        return true;
    }

    // age 0 = the block that completed last
    size_t spectrumOffset (int age) const noexcept { return (size_t) ((newest - age + cap) % cap) * (size_t) (blockSize + 1); }
    const float* re() const noexcept { return fdlRe.data(); }
    const float* im() const noexcept { return fdlIm.data(); }

private:
    const juce::dsp::FFT* fft = nullptr;
    int blockSize = 64, cap = 1, p = 0, hpos = 0, newest = 0;
    std::vector<float> hist, prevBlock, curBlock, work;
    std::vector<float> fdlRe, fdlIm;
};

//==============================================================================
// One IR applied to a ConvInput: the head (first B taps) as a direct FIR, the rest from the input
// spectra, computed at each block boundary for the next block (overlap-save).
class ConvReader
{
public:
    void prepare (int B)
    {
        blockSize = B;
        tail.assign ((size_t) B, 0.0f);
        accRe.assign ((size_t) (B + 1), 0.0f);
        accIm.assign ((size_t) (B + 1), 0.0f);
        work.assign ((size_t) (4 * B), 0.0f);
        ir = nullptr;
    }

    // Audio thread. Takes effect at once, already "warm" from the input history.
    void setIR (const IRSpectra* s, const ConvInput& in)
    {
        ir = (s != nullptr && s->blockSize == blockSize) ? s : nullptr;
        refresh (in);
    }
    const IRSpectra* getIR() const { return ir; }

    // recompute this block's tail from the history (after an IR change, or after not being kept up to date)
    void refresh (const ConvInput& in) { computeTail (in); }
    void blockDone (const ConvInput& in) { computeTail (in); }

    inline float output (const ConvInput& in) const noexcept
    {
        if (ir == nullptr) return 0.0f;
        const float* h = ir->headReversed.data();
        const float* w = in.window();
        float y = 0.0f;
        for (int j = 0; j < blockSize; ++j) y += h[j] * w[j];
        return y + tail[(size_t) in.position()];
    }

private:
    void computeTail (const ConvInput& in)
    {
        const int B = blockSize;
        const int parts = ir != nullptr ? std::min (ir->numParts, in.capacity()) : 0;
        if (parts == 0) { std::fill (tail.begin(), tail.end(), 0.0f); return; }
        // partition k (1..parts) meets the input spectrum from k-1 blocks ago
        std::fill (accRe.begin(), accRe.end(), 0.0f);
        std::fill (accIm.begin(), accIm.end(), 0.0f);
        float* __restrict aR = accRe.data();
        float* __restrict aI = accIm.data();
        for (int k = 1; k <= parts; ++k)
        {
            const size_t xo = in.spectrumOffset (k - 1), ho = (size_t) (k - 1) * (size_t) (B + 1);
            const float* __restrict xr = in.re() + xo;
            const float* __restrict xi = in.im() + xo;
            const float* __restrict hr = ir->partsRe.data() + ho;
            const float* __restrict hi = ir->partsIm.data() + ho;
            for (int b = 0; b <= B; ++b)
            {
                aR[b] += xr[b] * hr[b] - xi[b] * hi[b];
                aI[b] += xr[b] * hi[b] + xi[b] * hr[b];
            }
        }
        float* t = work.data();
        for (int b = 0; b <= B; ++b) { t[2 * b] = aR[b]; t[2 * b + 1] = aI[b]; }
        std::fill (t + 2 * (B + 1), t + 4 * B, 0.0f);
        in.getFFT()->performRealOnlyInverseTransform (t);
        for (int i = 0; i < B; ++i) tail[(size_t) i] = t[B + i];   // overlap-save: keep the second half
    }

    const IRSpectra* ir = nullptr;
    int blockSize = 64;
    std::vector<float> tail, work, accRe, accIm;
};

//==============================================================================
// A single self-contained convolver (one input, one IR) - used by the tests.
class PartConvolver
{
public:
    void prepare (int B, int maxParts, const juce::dsp::FFT* sharedFft) { in.prepare (B, maxParts, sharedFft); rd.prepare (B); }
    void setIR (const IRSpectra* s) { in.reset(); rd.setIR (s, in); }
    inline float process (float x) noexcept
    {
        in.push (x);
        const float y = rd.output (in);
        if (in.advance()) rd.blockDone (in);
        return y;
    }
private:
    ConvInput in;
    ConvReader rd;
};

//==============================================================================
// The IR cabinet stage: two IR slots (A, B) with a blend, crossfades when an IR changes or the
// stage is switched in/out, mono or stereo. When no IR is loaded it passes the signal unchanged.
class IRCab
{
public:
    static constexpr double kMaxSeconds = 0.3;

    void prepare (double fs)
    {
        sr = fs;
        blockSize = IRSpectra::blockSizeFor (fs);
        fft = std::make_unique<juce::dsp::FFT> (IRSpectra::fftOrderFor (blockSize));
        const int maxParts = (int) std::ceil (kMaxSeconds * fs / blockSize) + 1;
        for (auto& in : inputs) in.prepare (blockSize, maxParts, fft.get());
        for (auto& slot : slots)
            for (auto& set : slot.sets)
                for (auto& r : set) r.prepare (blockSize);
        fadeLen = std::max (1, (int) (0.030 * fs));
        wCoef = onePoleCoef (10.0, fs);   // two stages: ~25 ms S-shaped glide
        resetState();
    }

    void resetState()        // no allocation
    {
        for (auto& in : inputs) in.reset();
        historyRunning = true;
        for (auto& slot : slots)
        {
            for (auto& set : slot.sets)
                for (int c = 0; c < 2; ++c) set[(size_t) c].setIR (nullptr, inputs[(size_t) c]);
            slot.cur = 0; slot.fadeLeft = 0; slot.wanted = nullptr; slot.stale = true;
        }
        primed = false;
    }

    // Per block / chunk: which IRs to use (snap = jump without crossfade, e.g. a session was loaded)
    void setSources (const IRSpectra* a, const IRSpectra* b, bool snap)
    {
        setSlot (slots[0], a, snap || ! primed);
        setSlot (slots[1], b, snap || ! primed);
    }

    void setParams (bool enabled, float blend01)
    {
        const bool hasA = slots[0].wanted != nullptr, hasB = slots[1].wanted != nullptr;
        if (! enabled || (! hasA && ! hasB)) { tA = 0; tB = 0; tDry = 1; }
        else if (hasA && hasB)               { tA = 1.0f - blend01; tB = blend01; tDry = 0; }
        else if (hasA)                       { tA = 1; tB = 0; tDry = 0; }
        else                                 { tA = 0; tB = 1; tDry = 0; }
        if (! primed) { gA.snap (tA); gB.snap (tB); gDry.snap (tDry); primed = true; }
    }

    bool isActive() const { return tDry < 1.0f || gDry.value() < 1.0f; }

    void process (float* const* ch, int numCh, int n)
    {
        numCh = std::min (numCh, 2);
        const bool active = isActive();
        // the input history runs while an IR is loaded (so one can be switched in at any moment);
        // with no IR at all there is nothing to keep warm
        if (! active && slots[0].wanted == nullptr && slots[1].wanted == nullptr) { historyRunning = false; return; }
        if (! historyRunning)
        {
            for (auto& in : inputs) in.reset();       // stale: start from silence
            slots[0].stale = slots[1].stale = true;
            historyRunning = true;
        }
        for (int i = 0; i < n; ++i)
        {
            float wA = 0, wB = 0, wD = 1, fA = 1, fB = 1;
            bool useA = false, useB = false;
            if (active)
            {
                wA = gA.next (tA, wCoef); wB = gB.next (tB, wCoef); wD = gDry.next (tDry, wCoef);
                fA = slotFade (slots[0]); fB = slotFade (slots[1]);
                useA = wA > 0.0f || tA > 0.0f;
                useB = wB > 0.0f || tB > 0.0f;
                if (useA && slots[0].stale) refreshSlot (slots[0], numCh);   // coming back into use: catch up
                if (useB && slots[1].stale) refreshSlot (slots[1], numCh);
            }
            // the input history always runs, so any IR can join in at any moment
            for (int c = 0; c < numCh; ++c)
            {
                const float x = ch[c][i];
                auto& in = inputs[(size_t) c];
                in.push (x);
                if (! active) continue;
                float y = wD * x;
                if (useA) y += wA * slotOut (slots[0], c, fA);
                if (useB) y += wB * slotOut (slots[1], c, fB);
                ch[c][i] = y;
            }
            bool blockEnded = false;
            for (int c = 0; c < numCh; ++c) blockEnded = inputs[(size_t) c].advance();   // channels run in step
            if (blockEnded)
            {
                blockDone (slots[0], useA, numCh);
                blockDone (slots[1], useB, numCh);
            }
            if (active) { advanceFade (slots[0]); advanceFade (slots[1]); }
        }
    }

private:
    struct Slot
    {
        std::array<std::array<ConvReader, 2>, 2> sets;   // [set][channel]: two sets so an IR change can crossfade
        int cur = 0, fadeLeft = 0;
        bool stale = true;                                 // tails not kept up to date (slot unused)
        const IRSpectra* wanted = nullptr;
    };

    void refreshSlot (Slot& s, int numCh)
    {
        for (int c = 0; c < numCh; ++c)
        {
            s.sets[(size_t) s.cur][(size_t) c].refresh (inputs[(size_t) c]);
            if (s.fadeLeft > 0) s.sets[(size_t) (1 - s.cur)][(size_t) c].refresh (inputs[(size_t) c]);
        }
        s.stale = false;
    }

    void blockDone (Slot& s, bool used, int numCh)
    {
        if (! used) { s.stale = true; return; }
        for (int c = 0; c < numCh; ++c)
        {
            s.sets[(size_t) s.cur][(size_t) c].blockDone (inputs[(size_t) c]);
            if (s.fadeLeft > 1) s.sets[(size_t) (1 - s.cur)][(size_t) c].blockDone (inputs[(size_t) c]);
        }
    }

    void setSlot (Slot& s, const IRSpectra* ir, bool snap)
    {
        if (ir == s.wanted) return;
        s.wanted = ir;
        if (snap)
        {
            for (int c = 0; c < 2; ++c) s.sets[(size_t) s.cur][(size_t) c].setIR (ir, inputs[(size_t) c]);
            s.fadeLeft = 0;
            return;
        }
        const int other = 1 - s.cur;
        for (int c = 0; c < 2; ++c) s.sets[(size_t) other][(size_t) c].setIR (ir, inputs[(size_t) c]);
        if (s.stale) for (int c = 0; c < 2; ++c) s.sets[(size_t) s.cur][(size_t) c].refresh (inputs[(size_t) c]);
        s.stale = false;
        s.cur = other;
        s.fadeLeft = fadeLen;            // old set fades out, new one in
    }

    float slotFade (const Slot& s) const   // weight of the newest set (raised cosine)
    {
        return s.fadeLeft > 0 ? 0.5f - 0.5f * std::cos (kPi * (1.0f - (float) s.fadeLeft / (float) fadeLen)) : 1.0f;
    }

    float slotOut (Slot& s, int c, float fNew) const
    {
        const auto& in = inputs[(size_t) c];
        float y = fNew * s.sets[(size_t) s.cur][(size_t) c].output (in);
        if (s.fadeLeft > 0) y += (1.0f - fNew) * s.sets[(size_t) (1 - s.cur)][(size_t) c].output (in);
        return y;
    }

    static void advanceFade (Slot& s) { if (s.fadeLeft > 0) --s.fadeLeft; }

    double sr = 48000.0;
    int blockSize = 64, fadeLen = 1440;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::array<ConvInput, 2> inputs;
    std::array<Slot, 2> slots;
    Glide gA, gB, gDry;
    float tA = 0, tB = 0, tDry = 1, wCoef = 0.01f;
    bool primed = false, historyRunning = true;
};

//==============================================================================
// Preparing an IR file's samples for a sample rate (message thread): windowed-sinc resampling,
// trimming to kMaxSeconds with a short fade, and normalising to unit energy so every IR plays at a
// similar level. 'samples' are at 'fromRate'; the result is at 'toRate'.
inline std::vector<float> prepareIR (const std::vector<float>& samples, double fromRate, double toRate)
{
    std::vector<float> in = samples;
    if (in.empty()) return {};
    // unit energy, measured at the source rate (so the level is the same at every target rate)
    double e = 0.0;
    for (float v : in) e += (double) v * v;
    if (e > 0.0) { const float g = (float) (1.0 / std::sqrt (e)); for (auto& v : in) v *= g; }

    std::vector<float> out;
    if (std::abs (fromRate - toRate) < 0.5)
        out = in;
    else
    {
        const double ratio = toRate / fromRate, cutoff = std::min (1.0, ratio);
        const int zeros = 24;
        const double halfWidth = zeros / cutoff;
        const size_t len = (size_t) std::ceil ((double) in.size() * ratio);
        out.assign (len, 0.0f);
        auto bessel0 = [] (double x) { double s = 1.0, t = 1.0; for (int k = 1; k < 40; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
        const double beta = 8.0, i0b = bessel0 (beta);
        for (size_t n = 0; n < len; ++n)
        {
            const double tpos = (double) n / ratio;
            const int m0 = (int) std::ceil (tpos - halfWidth), m1 = (int) std::floor (tpos + halfWidth);
            double acc = 0.0;
            for (int m = std::max (0, m0); m <= std::min ((int) in.size() - 1, m1); ++m)
            {
                const double d = tpos - m;
                const double xs = cutoff * d;
                const double sinc = std::abs (xs) < 1e-12 ? 1.0 : std::sin (kPiD * xs) / (kPiD * xs);
                const double u = d / halfWidth;
                const double win = std::abs (u) < 1.0 ? bessel0 (beta * std::sqrt (1.0 - u * u)) / i0b : 0.0;
                acc += in[(size_t) m] * cutoff * sinc * win;
            }
            out[n] = (float) acc;
        }
        // an impulse response at a higher rate has proportionally smaller taps for the same response
        const float g = (float) (fromRate / toRate);
        for (auto& v : out) v *= g;
    }
    const size_t maxLen = (size_t) (IRCab::kMaxSeconds * toRate);
    if (out.size() > maxLen)
    {
        out.resize (maxLen);
        const size_t fade = std::min (maxLen, (size_t) (0.010 * toRate));
        for (size_t i = 0; i < fade; ++i)
            out[maxLen - 1 - i] *= (float) (0.5 - 0.5 * std::cos (kPiD * (double) i / (double) fade));
    }
    return out;
}
//==============================================================================
// ALIGN: the minimum-phase version of an IR (message thread). Same frequency response, but all of it
// starts at once: no lead-in before the main peak (zero added delay), and two IRs blend without the
// low end cancelling (their phase now follows their frequency response, which is similar).
// Homomorphic method: real cepstrum of the log magnitude, folded to make it causal.
inline std::vector<float> minimumPhase (const std::vector<float>& h)
{
    if (h.size() < 2) return h;
    int order = 1;
    while ((1 << order) < 8 * (int) h.size() && order < 20) ++order;   // 8x padding keeps cepstral aliasing tiny
    const int N = 1 << order;
    juce::dsp::FFT fft (order);
    using C = juce::dsp::Complex<float>;
    std::vector<C> a ((size_t) N), b ((size_t) N);
    for (size_t i = 0; i < h.size(); ++i) a[i] = C (h[i], 0.0f);
    fft.perform (a.data(), b.data(), false);
    float peak = 0.0f;
    for (auto& z : b) peak = std::max (peak, std::abs (z));
    const float floorMag = std::max (peak * 1.0e-6f, 1.0e-30f);            // -120 dB: no log(0)
    for (int k = 0; k < N; ++k) a[(size_t) k] = C (std::log (std::max (std::abs (b[(size_t) k]), floorMag)), 0.0f);
    fft.perform (a.data(), b.data(), true);                                 // real cepstrum
    for (int n = 0; n < N; ++n)                                             // fold: keep the causal part
    {
        const float w = (n == 0 || n == N / 2) ? 1.0f : (n < N / 2 ? 2.0f : 0.0f);
        a[(size_t) n] = C (b[(size_t) n].real() * w, 0.0f);
    }
    fft.perform (a.data(), b.data(), false);
    for (int k = 0; k < N; ++k) a[(size_t) k] = std::exp (b[(size_t) k]);
    fft.perform (a.data(), b.data(), true);
    std::vector<float> out (h.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = b[i].real();
    return out;
}

} // namespace ct
