#pragma once
// Plugin measurements in the spirit of Phil Speiser's THE_LAB (same approach as Mastering Studio's).
#include <juce_core/juce_core.h>
#include <vector>

namespace lab
{
struct Results
{
    float gateOpenThdDelta = 0.0f;                              // THD change from the gate when it's open (dB)
    float neutralResponseDev = 0.0f;                            // worst deviation of the neutral setting, 60 Hz - 10 kHz (dB)
    std::vector<std::pair<juce::String, float>> aliasing;       // worst alias below 15 kHz per configuration (dBc)
    int latencyError = 0;                                       // samples between reported and measured latency
    float cpu48 = 0.0f;                                         // whole plugin, % of one core at 48 kHz
    float silenceOutDb = 0.0f;                                  // output level after silence (dBFS)
    float warmthZeroThd = 0.0f;                                 // THD at -12 dBFS with Warmth at 0 (info)
};

// Runs all measurements, appends a Markdown report to md.
Results run (juce::String& md, bool verbose);
}
