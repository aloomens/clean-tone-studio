#pragma once

// Tap tempo: the tempo from the time between taps (message thread, editor).
//  - Averages the last few intervals, so one uneven tap doesn't throw it off.
//  - A pause of more than kResetMs starts a fresh count.
//  - The result is limited to the Tempo control's range (30 - 300 BPM).
#include <array>
#include <optional>
#include <algorithm>

class TapTempo
{
public:
    static constexpr double kResetMs = 2000.0;
    static constexpr int kMaxIntervals = 4;

    // timeMs: a steadily rising clock in milliseconds. Returns the new tempo once there are two taps.
    std::optional<double> tap (double timeMs)
    {
        if (count > 0 && (timeMs - last > kResetMs || timeMs <= last))
            count = 0;                                   // a long pause (or a clock jump): start again
        if (count > 0)
        {
            intervals[(size_t) (filled % kMaxIntervals)] = timeMs - last;
            ++filled;
        }
        else
            filled = 0;
        last = timeMs;
        ++count;
        if (filled == 0) return std::nullopt;
        const int n = std::min (filled, kMaxIntervals);
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += intervals[(size_t) i];
        return std::clamp (60000.0 * n / sum, 30.0, 300.0);
    }

    // taps so far in the current count (0 after a reset), for the button's feedback
    int getCount (double nowMs) const { return (count > 0 && nowMs - last <= kResetMs) ? count : 0; }

private:
    std::array<double, kMaxIntervals> intervals {};
    int filled = 0, count = 0;
    double last = 0.0;
};
