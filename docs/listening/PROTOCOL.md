# Listening protocol (experimental band engines)

Purpose: decide by ear which of the two experimental band engines (if either) is acceptable, and collect the
observations the engine decision needs. This is a manual procedure. **Nothing in this document has been performed yet**:
no DAW has been tested, and the automated tests say nothing about how the plugin sounds.

## 1. Build and install

Build on the machine you will listen on (see `README.md`, "Build"). The plugin files end up in
`build/ToneLegionMultiband_artefacts/Release/`:

| Format | File | Where the host looks for it |
|---|---|---|
| VST3 | `VST3/Tone Legion Multiband.vst3` | Windows: `C:\Program Files\Common Files\VST3\` · macOS: `~/Library/Audio/Plug-Ins/VST3/` |
| AU (macOS) | `AU/Tone Legion Multiband.component` | `~/Library/Audio/Plug-Ins/Components/` |
| CLAP | `CLAP/Tone Legion Multiband.clap` | Windows: `C:\Program Files\Common Files\CLAP\` · macOS: `~/Library/Audio/Plug-Ins/CLAP/` · Linux: `~/.clap/` |

On Linux the VST3 bundle goes to `~/.vst3/`. `README.md` only spells out the Linux build commands; on Windows
(Visual Studio 2022) and macOS (Xcode) the same three `cmake` steps apply with the default generator, as the CI
workflow in `.github/workflows/ci.yml` does. Nobody has built or installed it by hand on Windows or macOS yet.

Rescan plugins in the host afterwards. The plugin codes are placeholders, so do not keep sessions that depend on
this build.

## 2. Session setup (REAPER is the reference host; any DAW works)

1. 48 kHz project, buffer 256 samples. Repeat the critical checks at 44.1 kHz and 96 kHz, and at buffers 64 and 1024.
2. Three stereo tracks with material you know well: a full mix, a drum bus, a vocal or solo instrument.
   Add a pink-noise track and a 1 kHz sine track for the technical checks.
3. Insert *Tone Legion Multiband* on each track. A new instance has no bands and must sound identical to bypass.
4. Level-match whenever you compare against bypass: use the band Makeup or the Output control and a loudness meter
   after the plugin, or switch Auto Gain on (it matches loudness slowly). The plugin's own LUFS read-out next to the meters (section 3)
   can be used as well; it measures the output only while the editor is open.

## 3. What the editor offers in this build

- Top row: preset browser (`<` `>`, click the name for the list; "Starter" presets are untested starting points),
  **Save**, **A / B / Copy** for comparing two settings, Undo / Redo, the window scale (75 to 150 %), Bypass.
- Second row: **EXPERIMENTAL ENGINE** chooses the band filter under test (see "Engine comparison").
- **+ Add Band** creates a band on the next free slot (up to 8). **X** on a column removes it. **Undo / Redo**
  (or Ctrl/Cmd+Z and Ctrl/Cmd+Shift+Z) step back through adding and removing bands and knob edits, one step per
  drag or click.
- Spectrum display: grey is the input spectrum, violet the output. Drag across empty space to create a band, drag
  a band's edge or body to move it. The shaded shapes and the white line show the response the plugin is applying
  at this moment, so you can see what the experimental engine really does to each range (compare with B1 and C1).
- Column: **BYP** (band bypass), gain-reduction bar and value (deepest value of the last second), Threshold,
  Ratio, Attack, Release.
- Click a column to select it. The inspector below has three tabs for that band. *Band*: Low Freq, High Freq, Knee,
  Range, Makeup, Mix, Stereo Mode, Link. *Detector*: Detector (Peak / RMS / Blend), RMS Blend, RMS Time, Source
  (Main / External), Channels, Listens To (Own / Band / Custom) with Follow or Det Low / Det High. *Timing*:
  Lookahead, Adaptive Rel, Freq Release, Transient.
- Header: the latency reported to the host, and Fixed Latency (always 10 ms, so that it never changes).
- Bottom bar: Input gain (before the compressor), Mix (dry/wet of the whole plugin), Output gain, Auto Gain
  (slow loudness match, shows the correction in dB); Colour (Clean / Warm FET) with Drive, OS (oversampling of the
  colour stage; adds about 1 ms of latency), Clip (Off / Soft / Hard) and its Ceiling.
- Double-click a knob to reset it. Click the value under a knob to type one ("1.2k", "250 ms", "inf").
- **Bypass** (top right) is the plugin's own bypass with a short fade (bands 10 ms; gains and clipper 20 ms).
- **Spectrum / Dynamics** switch the display: Dynamics shows the selected band's transfer curve with its current
  operating point and its gain reduction over the last 8 seconds.
- Right of the display: input, output and sidechain peak / RMS meters (dBFS) and the sidechain state.
- **Solo** and **Listen** in the inspector audition the selected band: Solo plays what the band lets through,
  Listen plays what its detector hears (6 dB down). The status line shows what is being auditioned.
- Next to the meters: output loudness in LUFS (M = momentary, S = short-term, I = integrated since Reset) and the
  true-peak maximum (TP). Use I or S to level-match against bypass.

## 4. Checks

Write down what you hear for each item; "nothing unusual" is a valid result. Note material, band settings and
sample rate with every observation so it can be reproduced.

### A. Baseline

| # | Step | Expected |
|---|---|---|
| A1 | New instance, no bands, toggle the host's plugin bypass | No audible difference |
| A2 | Add one band, threshold at maximum (+12 dB) | No audible difference, GR reads 0.0 |
| A3 | Add and remove bands while audio plays; repeat with Fixed Latency on | No clicks. With Fixed Latency (or lookahead) a new band is heard 10 ms after the click, a removed one fades out 10 ms late |
| A4 | Toggle band BYP and plugin Bypass during heavy gain reduction | Short fade, no click |
| A5 | Save the project, close, reopen | Same bands, values, preset name and sound |
| A6 | Save a preset, change things, load it again; switch A / B during playback | The preset comes back exactly; A/B switches without clicks louder than the difference between the two settings |

### B. One band (the A3 response shape)

A new instance starts on A-prime: select A3 in the engine selector for B1 - B3. Use pink noise, then music. Band 200 Hz - 2 kHz, ratio 4:1, threshold for about 6 dB of gain reduction.

| # | Listen for | Background |
|---|---|---|
| B1 | Does the band sound like "a region being turned down" or like "a dip in the middle"? | A3 is a bell: full depth only at the band centre, about 2.7 dB less at the nominal edges for 12 dB of reduction |
| B2 | Does the perceived width change as gain reduction gets deeper? | The bell narrows with depth |
| B3 | Narrow band (1/3 octave) and wide band (4+ octaves): is either unusable? | |
| B4 | Sweep Low Freq and High Freq while compressing | Smooth, no zipper noise or thumps (edges are smoothed over 20 ms) |
| B5 | Fast attack (0.01 - 1 ms) on drums; slow release (1 - 5 s) on a mix | No clicks; note distortion on low bass with fast times, which is expected from any compressor |
| B6 | Solo the band, sweep its edges; then Listen | Solo: only that range, with its gain reduction. Listen: the detector's view of the signal. Switching either is click-free |

### Engine comparison (the main question)

The selector "EXPERIMENTAL ENGINE" in the second header row switches between the two candidates as a short
crossfade, 10 to 30 ms after the click for ordinary bands and up to a quarter of a second for very narrow low bands
(the other engine first runs unheard while its band filters settle; the compressor's detector and envelope carry
over, so the amount of gain reduction does not restart). Auto Gain brings both engines to the loudness of the dry
signal, also across the switch: keep it on to compare the engines at equal loudness, and off to hear how much
they differ in level. Everything else (detector, timing, settings) is identical, so what you hear is the band filter only.
Repeat B1 - B4 and C1 - C4 on both, level-matched.

| # | Compare | Background |
|---|---|---|
| X1 | One band, 6 and 12 dB of reduction: which sounds like "this range turned down"? | A3 is full depth only at the centre and about 3 dB down at the edges; A-prime reaches half the reduction at the edges at any depth |
| X2 | Two adjacent bands compressing equally | Junction ripple: A3 2.4 dB at -6 dB (7 dB at -12 dB); A-prime about 1 dB (2 dB) |
| X3 | Several adjacent bands all compressing: overall level | With A-prime the skirts add up (eight bands at -12 dB reach about -18 dB); with A3 they under-reach. Repeat with Overlap on Blend: each band should land near its own command |
| X4 | Fast, deep gain reduction on a narrow low band (kick, bass note) | A-prime changes its shape over 50 - 500 ms after the gain moves; listen for a "swirl" or a thump. A3 has no such transition |
| X5 | Makeup +6 dB compared with -6 dB on the same band | A-prime: mirror images. A3: boost is wide, cut is narrow |

### C. Several bands

| # | Setup | Listen for |
|---|---|---|
| C1 | Two adjacent bands (120 - 400 Hz, 400 Hz - 1 kHz), same settings, 6 - 12 dB GR each | A bump or a hole around 400 Hz (measured: about 2.4 dB ripple at the junction) |
| C2 | Two overlapping bands, Overlap on Stack, then on Blend | Stack: gain reduction adds up in the overlap. Blend: about the deeper band's own reduction, not the sum |
| C5 | Four adjacent bands compressing a full mix; switch Overlap between Stack and Blend while it plays | No click on the switch; Blend should sound less carved-out between the bands, without the bands sounding weaker on their own |
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
| E4 | Range 6 dB with a low threshold and high ratio | Gain reduction stops at -6.0 on the read-out |

### F. Gain staging

| # | Setup | Listen for |
|---|---|---|
| F1 | Makeup +6 dB on a band that is not compressing | That region gets louder; the GR read-out stays at 0.0 |
| F2 | Band Mix from 100 % to 0 % while the band compresses hard | Smooth transition to the uncompressed sound, no phasing |
| F3 | Global Mix at 50 % with several bands | Parallel compression without comb filtering or hollowness |
| F4 | Move Input, Output, Makeup and both Mix controls quickly during playback | No clicks or zipper noise (changes are ramped over 20 ms) |
| F5 | Input +12 dB, Output -12 dB | More gain reduction at the same threshold; overall level roughly as before |
| F6 | Auto Gain on with several dB of gain reduction | Output about as loud as bypass after a second or two; no pumping, no jump after a pause in the music |
| F7 | Clip Hard and Soft with the Output pushed into the ceiling | Once the clipper has been on for 20 ms the output meter never exceeds the ceiling (switching it on, or leaving Bypass, fades it in over 20 ms, and an over passes during that time). Hard is audibly harsher than Soft; note any high-frequency "fizz" (the clipper is not oversampled yet) |

### Lookahead and latency

| # | Setup | Listen for |
|---|---|---|
| L1 | Drum bus, one band, attack 5 ms, compare Lookahead 0 and 5 ms | With lookahead the in-band part of each hit is caught from its start; the click of the hit itself is unchanged |
| L2 | Any lookahead: play the track against an untreated copy of itself | Still in time (the host compensates the latency shown in the header); check again after changing the value |
| L3 | Change Lookahead during playback, with Fixed Latency off and then on | Off: a short blend, and the host may glitch while it re-aligns. On: no change in timing at all |

### Timing character (first versions: say plainly if one of them is not useful)

| # | Setup | Listen for |
|---|---|---|
| T1 | Low band on bass with a short release, Freq Release 0 vs 100 % | Less low-frequency distortion at 100 % (the release becomes about 3 times longer at 100 Hz) |
| T2 | Full mix with loud and quiet sections, Adaptive Rel 0 vs 100 % | Less pumping after long loud passages; single hits still recover quickly. Is the slow recovery too slow? |
| T3 | Drum bus, fast attack, Transient 0 vs 100 % | Hits keep their attack while the body is still controlled. Any roughness on sustained notes would be a fault |

### Colour

| # | Setup | Listen for |
|---|---|---|
| K1 | Colour = Warm FET, Drive from 0 to 100 % on a bass, a vocal and a full mix | Does it add something you want (thickness, harmonics) or only distortion? At what drive does it become too much? |
| K2 | Bright material (cymbals, synths), high Drive, OS Off vs 4x | Off may sound grainy or add non-musical tones (aliasing); 4x should be clean |
| K3 | Switch Colour and OS during playback | A short crossfade, no click; the latency in the header changes with OS |
| K4 | Per-band Character (inspector, Band tab) from 0 to 100 % on one band: a bass band under a kick and bass, then a vocal band | Does it thicken that band without muddying the rest? At what amount is it too much? Default is 0 % (off) |
| K5 | Character on a band that is compressing hard, compared with the same amount on a band that is not compressing | It sits after the compression, so a band that is turned down is driven less: is that the order you want, or should it drive into the compressor instead? |
| K6 | Character at 100 % on a bright band (above 5 kHz) with cymbals or synths | Grain or tones that do not belong to the music (it is anti-aliased, not oversampled) |

### G. Sidechain and stereo modes

To feed the sidechain in REAPER: on the track with the plugin set the track channel count to 4, add a send from the
key track to channels 3/4, and check in the plugin's pin connector that 3/4 go to the sidechain input. Other hosts
have a sidechain selector in the plugin window header.

| # | Setup | Listen for |
|---|---|---|
| G1 | Band on a pad or bass, Source = External, kick on the sidechain | The band ducks with the kick; the kick itself is never heard through the plugin |
| G2 | Source = External with nothing routed to the sidechain | No gain reduction, and the amber "not connected" message in the Detector tab |
| G3 | High band Listens To = Band (the low band) on a full mix | The highs duck when the lows are loud (cross-band detection) |
| G4 | Listens To = Custom, sweep Det Low / Det High | The band reacts only to that range; no clicks while sweeping |
| G5 | Stereo Mode = Mid, then Side, on wide material | Only the centre, or only the sides, are compressed; the image stays stable |
| G6 | Stereo Mode = Mid/Side with Channels = L/R | Mid and side are controlled independently (the two GR values are mid and side) |
| G7 | Switch Stereo Mode and Source during playback; repeat with Fixed Latency on | A short dip of the band's effect (10 ms longer with Fixed Latency or lookahead), no click |
| G8 | Channels = Max vs L/R with Link 0 % on material that is loud on one side | Max keeps the image centred; L/R unlinked lets it move |

### H. Host behaviour

| # | Step | Expected |
|---|---|---|
| H1 | Automate Threshold and Low Freq with a host envelope | Controls follow; no clicks (automation is applied once per audio buffer) |
| H2 | Change buffer size and sample rate while the project is open | Plugin keeps working, state unchanged |
| H3 | Open and close the plugin window repeatedly during playback; resize it | No dropouts, no change in sound |
| H4 | Offline render vs realtime playback | No audible difference |
| H5 | Duplicate the track with the plugin; copy the plugin to another track | Settings and bands are copied |

## 5. Reporting

For each problem: host and version, OS, plugin format, sample rate and buffer size, the band settings (a screenshot
of the editor is enough), the material, and what you heard. A saved project or preset chunk that reproduces it is
the most useful attachment.

The decision this feeds: whether A-prime stays the default. It was picked on 2026-11-24 from measurements, and the
engine comparison (X1 - X5, above all X3 and X4) is what confirms it or sends it back to A3 or to a fix. That decision
is the owner's and is made by listening, not from response plots.
