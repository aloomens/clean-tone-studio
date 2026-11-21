# Clean Tone Studio 1.6.2 - plugin measurements

Measured offline on the whole plugin (mono in, stereo out, left channel analyzed), with Auto Level off so levels are absolute. Unless a row names a preset it starts from the most neutral setting: Flat pickup, compressor off, Warmth 0, flat EQ, filters wide open, cabinet, gate, delay and reverb off. THD: harmonics 2 and up, relative to the fundamental. Aliasing: everything that is not DC, the tone or its harmonics (fold-back products plus noise), relative to the tone. Lower is better throughout. A typical DI guitar peaks around -12 dBFS.

## Harmonic distortion (THD) at 110 Hz (low A string)

| Configuration | -24 dBFS | -12 dBFS | -6 dBFS | -1 dBFS |
|---|---|---|---|---|
| Neutral (Flat pickup, compressor off, Warmth 0, flat EQ, cabinet off) | -147.4 dB | -148.2 dB | -148.1 dB | -147.9 dB |
| Warmth 5 % (fading in) | -51.4 dB | -39.4 dB | -33.6 dB | -29.1 dB |
| Warmth 30 % (default) | -36.6 dB | -24.6 dB | -19.1 dB | -15.4 dB |
| Warmth 60 % | -28.7 dB | -16.9 dB | -13.0 dB | -10.1 dB |
| Warmth 100 % | -20.5 dB | -11.1 dB | -8.2 dB | -6.3 dB |
| Compressor: Sustain 35 %, Attack 15 ms (default) | -83.8 dB | -81.3 dB | -84.4 dB | -86.3 dB |
| Compressor: Sustain 100 %, Attack 1 ms (fastest) | -83.6 dB | -84.3 dB | -84.5 dB | -84.5 dB |
| Noise gate 100 % (open) | -146.7 dB | -146.5 dB | -146.5 dB | -146.4 dB |
| Preset: Studio Clean (effects off) | -31.6 dB | -27.2 dB | -25.3 dB | -23.8 dB |
| Preset: Endless Sustain (effects off) | -26.3 dB | -25.2 dB | -24.7 dB | -24.2 dB |

## Harmonic distortion (THD) at 1 kHz

| Configuration | -24 dBFS | -12 dBFS | -6 dBFS | -1 dBFS |
|---|---|---|---|---|
| Neutral (Flat pickup, compressor off, Warmth 0, flat EQ, cabinet off) | -143.6 dB | -143.3 dB | -144.1 dB | -146.0 dB |
| Warmth 5 % (fading in) | -51.7 dB | -39.7 dB | -33.9 dB | -29.4 dB |
| Warmth 30 % (default) | -38.2 dB | -26.1 dB | -20.5 dB | -16.7 dB |
| Warmth 60 % | -31.8 dB | -19.7 dB | -15.4 dB | -12.6 dB |
| Warmth 100 % | -25.8 dB | -14.9 dB | -12.1 dB | -10.0 dB |
| Compressor: Sustain 35 %, Attack 15 ms (default) | -118.0 dB | -119.9 dB | -122.4 dB | -124.1 dB |
| Compressor: Sustain 100 %, Attack 1 ms (fastest) | -121.0 dB | -121.0 dB | -142.7 dB | -142.8 dB |
| Noise gate 100 % (open) | -142.9 dB | -143.4 dB | -144.7 dB | -142.2 dB |
| Preset: Studio Clean (effects off) | -32.0 dB | -27.7 dB | -25.7 dB | -24.1 dB |
| Preset: Endless Sustain (effects off) | -27.3 dB | -26.2 dB | -25.6 dB | -25.1 dB |

Warmth is a gentle asymmetric (mostly 2nd-harmonic) curve. The bottom 10 % of the knob fades it in from nothing, so at 0 % the path is clean; presets all use 15 % or more.

## Worst-case aliasing (sine sweep 2-19 kHz at -1 dBFS)

Each tone is pushed through the configuration with the cabinet off and High Cut fully open, so nothing hides the fold-back. The table shows the worst result over the sweep: total non-harmonic energy, the single largest fold-back product, and where it landed. Warmth runs at 4x oversampling.

| Configuration | Total aliasing + noise | Largest alias (anywhere) | Largest alias below 15 kHz |
|---|---|---|---|
| Neutral (Flat pickup, compressor off, Warmth 0, flat EQ, cabinet off) | -135.3 dB | -143.6 dB at 10.00 kHz (14.0 kHz tone) | -143.6 dB at 10.00 kHz (14.0 kHz tone) |
| Warmth 30 % (default) | -92.0 dB | -92.0 dB at 23.00 kHz (5.0 kHz tone) | -103.7 dB at 10.00 kHz (19.0 kHz tone) |
| Warmth 100 % | -57.7 dB | -59.9 dB at 12.00 kHz (17.0 kHz tone) | -59.9 dB at 12.00 kHz (17.0 kHz tone) |
| Warmth 100 % + Sustain 100 % (fastest) | -92.3 dB | -99.9 dB at 8.01 kHz (8.0 kHz tone) | -99.9 dB at 8.01 kHz (8.0 kHz tone) |

## Frequency response of the voicings (small signal, -30 dBFS)

Level relative to the neutral setting at each frequency. Pickup rows include the pickup's level normalisation (+3 dB single coil, -2.5 dB humbucker). Cabinet rows include the cabinets' fixed -1.4 dB trim.

| Voicing | 60 | 100 | 200 | 400 | 800 | 1.6k | 3.2k | 6.4k | 10.0k |
|---|---|---|---|---|---|---|---|---|---|
| Neutral setting vs input | -0.1 | -0.0 | -0.0 | -0.0 | 0.0 | 0.0 | 0.0 | 0.0 | -0.0 |
| Pickup: Single Coil | 5.0 | 4.9 | 4.1 | 4.1 | 3.3 | 2.6 | 0.5 | 2.3 | 2.2 |
| Pickup: Humbucker | -3.5 | -3.4 | -4.0 | -4.6 | -2.7 | -1.4 | -0.6 | -1.2 | -1.0 |
| Cabinet: Open 1x12 | -6.1 | -0.0 | -0.9 | -3.0 | -2.8 | -1.0 | -1.5 | -8.7 | -21.8 |
| Cabinet: Classic 2x12 | -4.2 | 1.4 | -1.0 | -3.6 | -2.1 | -0.9 | -0.6 | -10.3 | -25.0 |
| Cabinet: Closed 4x12 | -0.8 | 3.7 | -0.1 | -2.0 | -2.9 | -1.2 | 0.6 | -12.2 | -28.5 |
| Warmth 30 % (fullness curve) | 0.1 | 0.3 | 0.7 | 0.4 | 0.1 | 0.0 | -0.0 | -0.2 | -0.6 |
| Preset: Studio Clean (effects off) | -2.5 | 7.2 | 6.6 | 4.2 | 3.3 | 4.5 | 2.0 | -3.8 | -18.5 |

Values in dB. The neutral row shows the plugin's own coloration with everything off (the 20 Hz low cut, the 20 kHz high cut and the oversampling filters); ideally 0.0.

## Latency: reported vs measured

| Sample rate | Reported to host | Measured (impulse, neutral) | Measured (noise cross-correlation, neutral) |
|---|---|---|---|
| 44100 Hz | 5 samples (0.113 ms) | 5 | 5 |
| 48000 Hz | 5 samples (0.104 ms) | 5 | 5 |
| 96000 Hz | 6 samples (0.062 ms) | 6 | 6 |

The pickup, EQ and cabinet filters are minimum-phase, like analogue ones: they shift the phase of some frequencies but add no latency, so latency is measured on the neutral path.

## Silence in, silence out

Ambient Swell (7 s reverb, 55 % delay feedback), 2 s of tone then 37 s of digital silence: output in the last second peaks at -345.8 dBFS. The plugin adds no noise of its own; the delay and reverb switch themselves off once their echoes have faded.

## Reverb decay: the Decay knob vs the measured RT60

Impulse into the reverb alone (Size 50 %, Tone 1 kHz), decay measured by Schroeder integration (-5 to -25 dB, x3) below 300 Hz and above 2.5 kHz.

| Decay knob | RT60 lows | RT60 highs |
|---|---|---|
| 0.5 s | 0.52 s | 0.22 s |
| 1.0 s | 0.95 s | 0.36 s |
| 2.5 s | 2.45 s | 0.74 s |
| 6.0 s | 5.67 s | 1.60 s |
| 10.0 s | 9.49 s | 2.58 s |

Every delay line has a calibrated absorption filter, so the lows ring for the time on the knob and the highs die away about four times faster, like a real room.

## CPU cost (single core, 256-sample blocks)

| Module | 48 kHz | 96 kHz |
|---|---|---|
| Noise gate | 0.02 % | 0.03 % |
| Pickup voicing | 0.03 % | 0.05 % |
| Sustain compressor | 0.14 % | 0.29 % |
| Warmth (4x oversampled) | 0.88 % | 1.82 % |
| Tone stack | 0.05 % | 0.10 % |
| Cabinet | 0.05 % | 0.10 % |
| Delay (stereo) | 0.32 % | 0.63 % |
| Reverb (stereo) | 1.46 % | 2.93 % |
| IR loader (two 0.3 s IRs blended, stereo) | 2.27 % | 5.19 % |
| Clarity (dynamic low-mid cut, 100 %) | 0.07 % | 0.13 % |
| Pick (transient shaper, +100 %) | 0.40 % | 0.80 % |
| Whole plugin (processBlock, Ambient Swell: everything on) | 3.03 % | 6.08 % |
| Auto Level measurement, per new setting (total ms, spread over ~0.5 s of audio) | 89.09 ms | 182.74 ms |

Switched-off delay and reverb rest completely once faded out. The compressor's detector keeps running while it's switched off so it can come back in at exactly the right level.
