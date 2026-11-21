# Listening protocol (experimental A3 engine)

Purpose: decide by ear whether the experimental A3 band engine is acceptable, and collect the observations the
next engine decision needs. This is a manual procedure. **Nothing in this document has been performed yet**:
no DAW has been tested, and the automated tests say nothing about how the plugin sounds.

## 1. Build and install

Build on the machine you will listen on (see `README.md`, "Build"). The plugin files end up in
`build/ToneLegionMultiband_artefacts/Release/`:

| Format | File | Where the host looks for it |
|---|---|---|
| VST3 | `VST3/Tone Legion Multiband.vst3` | Windows: `C:\Program Files\Common Files\VST3\` · macOS: `~/Library/Audio/Plug-Ins/VST3/` |
| AU (macOS) | `AU/Tone Legion Multiband.component` | `~/Library/Audio/Plug-Ins/Components/` |
| CLAP | `CLAP/Tone Legion Multiband.clap` | Windows: `C:\Program Files\Common Files\CLAP\` · macOS: `~/Library/Audio/Plug-Ins/CLAP/` |

Rescan plugins in the host afterwards. The plugin codes are placeholders, so do not keep sessions that depend on
this build.

## 2. Session setup (REAPER is the reference host; any DAW works)

1. 48 kHz project, buffer 256 samples. Repeat the critical checks at 44.1 kHz and 96 kHz, and at buffers 64 and 1024.
2. Three stereo tracks with material you know well: a full mix, a drum bus, a vocal or solo instrument.
   Add a pink-noise track and a 1 kHz sine track for the technical checks.
3. Insert *Tone Legion Multiband* on each track. A new instance has no bands and must sound identical to bypass.
4. Level-match by ear or with a meter whenever you compare against bypass: this build has no makeup gain.

## 3. What the editor offers in this build

- **+ Add Band** creates a band on the next free slot (up to 8). **X** on a column removes it. **Undo / Redo**
  cover adding and removing bands only, not knob movements.
- Column: **BYP** (band bypass), gain-reduction bar and value (deepest value of the last second), Threshold,
  Ratio, Attack, Release.
- Click a column to select it. The inspector below then shows Low Freq, High Freq, Knee, Range, Detector
  (Peak / RMS / Blend), RMS Blend, RMS Time and Link for that band.
- Double-click a knob to reset it. Click the value under a knob to type one ("1.2k", "250 ms", "inf").
- **Bypass** (top right) is the plugin's own bypass with a 10 ms fade.
- Not in this build: spectrum display, solo, sidechain, makeup gain, mix, presets, output metering.

## 4. Checks

Write down what you hear for each item; "nothing unusual" is a valid result. Note material, band settings and
sample rate with every observation so it can be reproduced.

### A. Baseline

| # | Step | Expected |
|---|---|---|
| A1 | New instance, no bands, toggle the host's plugin bypass | No audible difference |
| A2 | Add one band, threshold at maximum (+12 dB) | No audible difference, GR reads 0.0 |
| A3 | Add and remove bands while audio plays | No clicks |
| A4 | Toggle band BYP and plugin Bypass during heavy gain reduction | Short fade, no click |
| A5 | Save the project, close, reopen | Same bands, values and sound |

### B. One band (the A3 response shape)

Use pink noise, then music. Band 200 Hz - 2 kHz, ratio 4:1, threshold for about 6 dB of gain reduction.

| # | Listen for | Background |
|---|---|---|
| B1 | Does the band sound like "a region being turned down" or like "a dip in the middle"? | A3 is a bell: full depth only at the band centre, about 2.7 dB less at the nominal edges for 12 dB of reduction |
| B2 | Does the perceived width change as gain reduction gets deeper? | The bell narrows with depth |
| B3 | Narrow band (1/3 octave) and wide band (4+ octaves): is either unusable? | |
| B4 | Sweep Low Freq and High Freq while compressing | Smooth, no zipper noise or thumps (edges are smoothed over 20 ms) |
| B5 | Fast attack (0.01 - 1 ms) on drums; slow release (1 - 5 s) on a mix | No clicks; note distortion on low bass with fast times, which is expected from any compressor |

### C. Several bands

| # | Setup | Listen for |
|---|---|---|
| C1 | Two adjacent bands (120 - 400 Hz, 400 Hz - 1 kHz), same settings, 6 - 12 dB GR each | A bump or a hole around 400 Hz (measured: about 2.4 dB ripple at the junction) |
| C2 | Two overlapping bands | Gain reduction adds up in the overlap (the "Stack" behaviour) |
| C3 | Four bands across the spectrum on a full mix, moderate settings | Overall tone compared with bypass (level-matched): colouration, phasiness, loss of punch |
| C4 | Eight bands, all active | Same as C3; note CPU load shown by the host |

### D. Stereo

| # | Setup | Listen for |
|---|---|---|
| D1 | Wide stereo material, Link 100 % | Stable image |
| D2 | Link 0 % with a loud element on one side only | Image shift is expected; listen for smearing or "phasey" width (measured: up to about 37 degrees of interchannel phase when the channels get different gain) |
| D3 | Sum the output to mono with Link 0 % | Hollow or comb-like sound would be a problem |

### E. Detector

| # | Setup | Listen for |
|---|---|---|
| E1 | Peak vs RMS vs Blend on drums and on a vocal | Peak reacts faster; RMS smoother; Blend in between |
| E2 | RMS Time 1 ms vs 300 ms | Short behaves like peak; long ignores transients |
| E3 | Knee 0 dB vs 24 dB at a low ratio | Hard knee has an audible onset; soft knee is gradual |
| E4 | Range 6 dB with a low threshold and high ratio | Gain reduction stops at 6.0 on the read-out |

### F. Host behaviour

| # | Step | Expected |
|---|---|---|
| F1 | Automate Threshold and Low Freq with a host envelope | Controls follow; no clicks (automation is applied once per audio buffer) |
| F2 | Change buffer size and sample rate while the project is open | Plugin keeps working, state unchanged |
| F3 | Open and close the plugin window repeatedly during playback; resize it | No dropouts, no change in sound |
| F4 | Offline render vs realtime playback | No audible difference |
| F5 | Duplicate the track with the plugin; copy the plugin to another track | Settings and bands are copied |

## 5. Reporting

For each problem: host and version, OS, plugin format, sample rate and buffer size, the band settings (a screenshot
of the editor is enough), the material, and what you heard. A saved project or preset chunk that reproduces it is
the most useful attachment.

The decision this feeds: keep A3, correct it (gain-dependent width), or replace it with one of the other candidate
engines. That decision is the owner's and is made by listening, not from response plots.
