# Tone Legion Multiband

Multiband compressor plugin (JUCE 9). **Development build: feature-complete enough to audition, nothing in it has
been listened to or run in a DAW yet.** Up to eight freely placed bands with a feed-forward compressor each,
sidechain routing, stereo and mid/side modes, lookahead, timing options, a colour stage, metering, presets and an
editor. The band filters come from one of **two EXPERIMENTAL engines** behind `dsp::IBandEngine`, selectable in the
editor so that they can be compared by ear: *A3* (the original; its bell narrows with depth) and *A-prime*
(constant width). Neither is accepted as final product behaviour. Latency is 0 unless lookahead or oversampling is
in use.

**License: none selected yet.** The repository is public, but no license has been
granted: all rights are reserved until the owner chooses one. Third-party code under `libs/` keeps its own licenses.

## Status

| Item | State |
|---|---|
| Parameter schema (230 parameters, one table) | **PROVISIONAL / NOT FROZEN** (`source/ParameterSchema.cpp`); threshold -80..+12 dB in 0.1 dB steps |
| Band engine | **EXPERIMENTAL**, two candidates selectable for listening: A3 regions and A-prime regions (see "Band engines") |
| Plugin / manufacturer codes (`Tlmb` / `Tlgn`) | **PLACEHOLDER**, uniqueness and registration not checked |
| State serialization, band topology, slot allocation + undo | implemented, tested |
| Presets / A-B | starter presets (built in, not yet judged by ear), user preset files, A/B compare (`source/PresetManager.*`) |
| Builds and validators | see **CI and validation** below (Linux x64, macOS arm64, Windows x64) |
| Real DAW hosts | **not tested** on any platform |
| Editor | `source/ui/`: spectrum display with draggable band regions and the live response curve, add / remove bands with undo, per-band columns, two-tab inspector, global bar; level meters; preset browser and A/B, band solo and detector listen, LUFS and true-peak read-outs |
| Usable in a host for auditioning | **built for it, not yet tried**: bands can be created in the editor; see `docs/listening/PROTOCOL.md`. No DAW session has been run |
| RTSan | `-DTL_ENABLE_RTSAN=ON` requires clang >= 20 and errors otherwise (never faked) |
| ASan + UBSan | `-DTL_ENABLE_ASAN_UBSAN=ON` (GCC/Clang): whole build instrumented, UB fatal; the test heap detector then counts `operator new/delete` only |

## What is not done

- **Nobody has listened to this plugin and it has not been opened in a DAW.** Everything below "tested" means
  automated tests, format validators in CI and sanitizers. See `docs/listening/PROTOCOL.md` for the manual checks.
- The two band engines are experiments; the choice between them (or neither) is a listening decision.
- Designs chosen without a listening session: the timing options (adaptive release, frequency-dependent release,
  transient preservation), the colour curve, automatic gain, the starter presets. Expect retuning.
- Inert parameters with no control in the editor: `character` (per band), `quality`, `lpLength` (linear-phase
  mode: deliberately not started), `overlapMode` (overlapping bands always stack).
- The output clipper is not oversampled. The oversampler is not optimised for CPU.
- Editor: no UI scaling and no compact layout (the window needs at least 1136 x 772 points); no sidechain routing
  view, no band suggestions; undo covers adding and removing bands only, not knob movements; keyboard and
  screen-reader use has not been tried by a person.
- Automation is applied once per audio block (see "CI and validation").
- The loudness meter has not been run against the official EBU test files.
- The parameter schema is not frozen (IDs and ranges can still change, which would break saved projects), the
  plugin codes are placeholders, no license has been chosen, there is no installer, code signing or notarisation,
  and AAX is not built.
- RTSan has not been run; the evidence for real-time safety is the audio-thread heap detector in the tests plus
  ASan / UBSan runs.

## Dependencies (git submodules in `libs/`, pinned)

| Path | Source | Pin |
|---|---|---|
| `libs/JUCE` | juce-framework/JUCE | tag `9.0.6`, `654bdbc7a9db0dae90849b44abe91f339e9f0768` |
| `libs/clap-juce-extensions` | free-audio/clap-juce-extensions | `3837f4a6ed2ab0ba46aba69ae5b7041c7c64ce74` (+ its `clap`, `clap-helpers` submodules) |
| `libs/Catch2` | catchorg/Catch2 | tag `v3.16.1`, `08092139210881f01a34e60c5f2fd6afa0ea4024` |

```sh
git submodule update --init --recursive
```

## Build (Linux)

Needs cmake >= 3.22, a C++20 compiler, ninja, and dev packages for freetype2, fontconfig, x11, xext
(no ALSA / curl / Xrandr / Xinerama / Xcursor / GTK / WebKit: the corresponding JUCE features are compiled
out via definitions in `CMakeLists.txt`, and `juce_audio_devices` / `juce_audio_utils` are not linked).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ToneLegionMultiband_VST3 ToneLegionMultiband_CLAP tl_tests
ctest --test-dir build --output-on-failure
```

Options: `TL_BUILD_CLAP` (default ON; compatibility gate, the wrapper officially claims JUCE 6/7/8 only),
`TL_BUILD_TESTS` (ON), `TL_BUILD_BENCH` (ON), `TL_ENABLE_RTSAN` (OFF), `TL_ENABLE_ASAN_UBSAN` (OFF).
Keep the build directory outside the repo or under `build/`.

Measurements and benchmark (neither is part of `ctest`'s pass/fail beyond the assertions in the tests):

```sh
TL_MEASURE_DIR=/tmp/tl-measure build/tests/tl_tests "[dsp]"      # writes CSV (responses, phase, GR traces)
python3 tools/plot_measurements.py /tmp/tl-measure                 # numpy + matplotlib, writes PNGs there
build/bench/tl_bench [seconds]                                     # ns/frame and % real time, 48 kHz, block 128
```

Updating the parameter manifest after an intentional schema change:
`TL_UPDATE_GOLDEN=1 build/tests/tl_tests "[ids]"` then review the diff of `tests/golden/parameter_ids.txt`.

## Layout

- `source/PluginIdentity.h` identity constants (checked against JUCE macros and CMake by tests)
- `source/ParameterSchema.*` the whole schema as one table, builds the `ParameterLayout` (one group per band + Global)
- `source/BandSlots.*` band topology (`used[16]`, `everUsed[16]`, LRU counter), create/delete as `UndoableAction`s
- `source/StateSerialization.*` parse -> validate -> migrate -> normalise -> `replaceState`
- `source/PresetManager.*` starter presets, user preset files, A/B compare (message thread)
- `source/PluginProcessor.*` buses, pass-through, bypass, creates the editor
- `source/ui/` editor: `Theme.h` (colour and size tokens), `ValueFormat` (value text with units), `Controls`
  (look-and-feel, knob / drop-down / toggle bound to parameters, GR bar), `SpectrumView`, `DynamicsView`, `MeterRail`,
  `BandColumn`,
  `BandInspector`, `PluginEditor`
- `docs/listening/PROTOCOL.md` manual listening and host checks (not yet performed)
- `source/rt/` real-time safe pieces (`FiniteGuard`, `AnalyzerFifo`, `LevelMeter`, `LoudnessMeter`)
- `source/analysis/` editor-side spectrum analysis (`SpectrumAnalyzer`, small radix-2 FFT)
- `source/dsp/` band processing: `IBandEngine` (interface), `DynamicsCore` (engine-independent: edge smoothing,
  detector, gain computer, link, attack/release, activity ramp, band lifecycle -> per-sample band gains),
  `BandEngineBase` (chunking, guards, metering, lifecycle and every engine-independent stage), `A3RegionEngine` and
  `APrimeRegionEngine` (only the filter structures), `TptSvf`, `EdgeSmoother`, `GainComputer`, `BandDetector`, `GrSmoother` + `ActivityRamp`, `Ramp` (20 ms gain / mix ramps), `KWeighting`, `OutputStage`
  (automatic gain, clipper), `Lookahead`, `Oversampler`, `Character`, `GrMeter`
- `bench/` CPU benchmark, `tools/plot_measurements.py` plots of the measurement CSVs
- `tests/` Catch2 suite, `tests/support/` offline renderer and audio-thread heap detector (`operator new`/`delete` override plus, on glibc, `malloc`/`calloc`/`realloc`/`free` interposition)

## DSP (P1-M2, experimental)

Per used slot, in slot order, per channel (dynamics in `source/dsp/DynamicsCore.cpp`, filters in `source/dsp/A3RegionEngine.cpp`):

- **A3 region:** TPT SVF with prewarped edges, `g = sqrt(wl wh)`, `R = (wh - wl)/(2g)`, delta form
  `y = x + (gEff - 1) 2R bp`. At `gEff == 1` the output is bit-identical to the input. The response is a bell:
  full depth only at the (digital) centre, about -2.7 dB at the nominal edges for -12 dB, width shrinking with depth,
  2.4 dB ripple at the junction of two adjacent bands, up to ~37 deg interchannel phase when unlinked channels get
  different gain. These are the documented reasons it is experimental.
- **Edges:** one-pole smoothing (20 ms) in the log2 domain, ordering resolved in DSP, runtime clamp
  `fLo >= 10 Hz`, `fHi <= 0.45 fs`, `fHi >= fLo * 2^(1/24)` (never written back). Coefficients change only on an
  absolute 16-sample grid (sample counter reset in `prepareToPlay`).
- **Detector:** main input before compression, the band's own range only: Butterworth HP at `fLo` then LP at `fHi`;
  peak, RMS (`rmsTime`) or blend (`detBlend`); Giannoulis/Massberg/Reiss soft knee (`ratio` 100 = infinity);
  `range` limit; stereo `link`; decoupled attack/release smoothing in the GR domain; 10 ms activity ramp for
  used/unused, band bypass and global bypass. Host bypass (`processBlockBypassed`) uses the same 10 ms ramp to
  neutral, then outputs the input bit-exactly while the states keep running; leaving it ramps back in.
- **Gain staging:** signal path `input gain -> bands -> global mix -> automatic gain -> output gain -> clipper`. Per band, `makeup` is part of
  the wet band and `mix` scales the band's whole gain change, `gEff = 1 + activity * mix * (gComp * makeup - 1)`
  (for one A3 band this is an exact parallel blend). The global `mix` blends with the input after the input gain,
  which is exact only because the A3 engine has zero latency and passes the input unchanged when neutral. All five
  are ramped over 20 ms (gains linearly in dB) and land exactly on their targets; at the defaults every stage is
  skipped and the output is bit-identical to the build without them. Host bypass and the plugin bypass ramp the
  input and output gains to unity, so bypass returns the input exactly. The GR meter shows compression only.
- **Detector routing (per band, always feed-forward: every source is taken before any band processing):**
  - `scSource`: main input (after the input gain) or the external sidechain bus. A mono sidechain feeds both
    channels. With no sidechain connected an External band gets silence and does nothing; it never falls back to the
    main input. Switching the source crossfades the detector input over 5 ms. Sidechain audio is only read (through
    a guarded copy) and cannot reach the output.
  - `scMode`: the detector's frequency range is the band's own, another slot's (`scRef`, its `fLo`/`fHi`), or a
    custom one (`scLo`/`scHi`), smoothed like the band edges. A reference to a slot that does not exist falls back
    to the band's own range. Deleting a followed band converts its followers to that range as a custom range.
  - `scChan`: `L/R` (per lane), `Mid`, `Side`, `Sum` (L+R, 6 dB hotter than Mid for in-phase material) or `Max`
    (per lane, deeper reduction applied to both). With a mono main input a stereo sidechain is summed.
- **Stereo modes (`stereoMode`, stereo only):** `Stereo` (left/right linked by `link`), `Left/Right` (independent),
  `Mid` or `Side` (only that component of the band is processed, the other is untouched), `Mid/Side` (both,
  independently). Mid/side needs no global encode: the A3 region filter is the same linear system on both channels,
  so its band-pass output is split into M and S and the two gain changes are added back per channel. Changing the
  mode fades the band out over 10 ms, swaps, and fades back in. In the mid/side modes the two GR meter channels
  are mid and side.
- **Automatic gain (`autoGain`, v1):** a slow loudness match. The K-weighted mean square (BS.1770 pre-filter,
  re-derived per sample rate; the 48 kHz coefficients are checked against the published values) of the dry signal
  (after the input gain) and of the processed signal are integrated over 1 s; the processed signal is scaled by
  `sqrt (dry / processed)`, limited to +-12 dB and smoothed (250 ms). While the dry signal is below -70 dB the
  correction is held. It restores the average level and does not follow individual gain-reduction events; fast
  level changes are not compensated. The target is updated on the absolute 16-sample grid, so static settings stay
  bit-identical across block layouts. This is not a loudness meter: no gating, no LUFS read-out.
- **Clipper (`clipMode`, `clipCeiling`):** the very last stage. *Hard* clamps at the ceiling. *Soft* is exactly
  linear up to half the ceiling, then a tanh section that approaches the ceiling without exceeding it (value and
  slope continuous). Neither is oversampled yet, so both alias on strong high-frequency overs; `oversampling` is
  still inert. Bypass switches the clipper off (bypass returns the input even above the ceiling).
- **Lookahead (`lookahead` per band 0..10 ms, `constantLatency`):** the audio is delayed by the largest lookahead of
  the used bands (or by the full 10 ms with constant latency on) and that delay is reported to the host as latency;
  each band's gain is delayed by the difference, so it leads the audio by exactly its own lookahead. A neutral band
  with lookahead is an exact delay; bypass keeps the same delay. When a value changes while running, the delay taps
  crossfade over 10 ms and the new latency is reported from the message thread (hosts differ in how they react to a
  latency change during playback; constant latency avoids it). With all lookaheads at 0 nothing is delayed or
  copied. Note that an abrupt onset is largely broadband and therefore outside a band's region: lookahead removes
  the in-band overshoot, not the click.
- **Timing character (per band, 0..100 %, 0 = off and bit-identical; v1 designs, to be judged by ear):**
  - `freqRel`: the release is scaled by the band centre, `release * (1 kHz / sqrt(fLo fHi)) ^ (0.5 amount)`:
    3.16 times longer at 100 Hz and 3.16 times shorter at 10 kHz at 100 %.
  - `adaptRel`: a slow follower of the gain reduction (falls with `max (8 release, 400 ms)`, rises 2.5 times slower)
    sets a floor for the recovery, `reduction = max (smoothed, amount * slow)`. A brief peak recovers at the set
    release and leaves only a small remainder; sustained reduction recovers several times slower. It never produces
    more reduction than the plain compressor reached.
  - `transient`: an onset detector (5 ms peak follower against its own 30 ms average) scales the reduction command
    back during onsets, `gr *= 1 - amount * onset`. Steady signals are compressed exactly as before.
- **Audition (not parameters, never saved):** *Solo* outputs one band's own component after its gain (for A3:
  `gEff * 2R * bp`, mid/side decoded to left / right); *Listen* outputs that band's detector signal (after its
  detector filter, before the envelope) 6 dB down and delayed like the audio, which with an external source is the
  one way sidechain audio reaches the output. One at a time, 10 ms crossfades, off while bypassed, and cleared by a
  state or preset load, by A/B, by removing that band and by closing the editor.
- **Colour (`charModel`, `charDrive`) and `oversampling`:** a global coloration stage on the wet signal, after the
  bands and before the global mix. One original model, *Warm FET*: an asymmetric tanh curve with unity small-signal
  gain, `y = (tanh (k (x + b)) - tanh (k b)) / (k (1 - tanh^2 (k b)))`, `b = 0.15`, `k = 2.5 drive`, followed by a
  5 Hz DC blocker. The bias gives even harmonics, tanh the odd ones and gentle peak rounding. It is not a model of
  any hardware. Oversampling (2x / 4x / 8x) is linear-phase: three Kaiser half-band FIR stages (95 / 25 / 17 taps,
  about 90 dB stop band) with a round-trip latency of 47 / 53 / 55 samples, reported to the host whenever
  oversampling is on (whether or not the colour is in use, so that automating the colour never changes the
  latency); the clean path and the dry signal are delayed by the same amount, so the clean model with oversampling
  is an exact delay. Measured at 48 kHz with a 9 kHz tone at full drive, the folded 4th and 5th harmonics drop by
  more than 40 dB at 4x. The output clipper is not oversampled. Cost is not optimised (plain FIR at the high rate).
- **Inert parameters (exist, no effect yet):** `character` (per band), and the globals `quality`, `lpLength`,
  `overlapMode`.
- **Spec amendment (accepted in review):** "becoming used -> states reset, activity ramps from 0" applies to a band
  that was fully skipped. A slot that is used again while it is still fading out (e.g. undo right after delete)
  keeps its filter/detector/smoother states and edges and ramps up from its current activity, so there is no
  discontinuity. Pinned by the test "Band re-used during its fade-out continues without reset".
- **Real-time safety:** no allocation after construction (fixed 256-sample internal chunk, so any host block size
  works), no locks or I/O; input guard (non-finite -> 0), non-finite internal state -> band reset, output guard;
  `ScopedNoDenormals`. Parameters are read once per host block, so automation takes effect at block boundaries:
  static parameters are bit-identical for any block layout, automated ones differ slightly between layouts.
- **GR metering:** `PluginProcessor::getGainReductionMeter().getGainReductionDb (slot, channel)`, lock-free,
  the minimum applied band gain of the last block in dB; `readAndResetHeldGainReductionDb (slot, channel)` returns the
  deepest value since its previous call (min-hold until read, for UI polling), or the last block's value when no
  block was processed in between.

## Band engines (both experimental)

A numerical study (kept outside this repository) compared the current engine with four alternatives against hard
gates: exact null at unity gain, no response above the commanded range, no dynamic overshoot above +0.5 dB with
smoothed gains, stability, bounded cost. Results, in short:

| Candidate | Result |
|---|---|
| **A3** (current): `y = x + (G - 1) 2R bp(x)` | survives. Known weaknesses: the bell narrows with depth (a 1-octave band acts over 0.85 octave at -3 dB and 0.06 at -48 dB), the nominal edges never get deeper than about -3 dB, 2.4 / 7.1 dB ripple between adjacent bands at -6 / -12 dB, cut and boost differ |
| A-prime as first written (damping follows the gain every sample) | **fails**: +8.6 dB overshoot on 24 dB gain steps, +23 dB on 48 dB steps |
| **A-prime**, modified (below) | survives: constant width, half the command (in dB) at the nominal edges, cut and boost mirror images, junction ripple 0.95 / 2.1 dB, overshoot at most +0.26 dB, cost equal to A3 |
| Complementary tree | **fails**: up to +4.3 dB above unity with cut-only commands |
| A3 with normalised gain commands | no mapping is both exact and free of boosts on freely placed bands; not worth its cost |
| Conventional LR4 crossover | **fails** the exact-null gate (already known) |

`APrimeRegionEngine`: `R' = R / sqrt (Gw)`, `u = 2R' x`, `y = x + (G - 1) bp_R' (u)`, where `Gw` is a lagged copy of
the gain (one pole in dB, time constant `max (5 ms, 4 / (R w0))`) updated with the coefficients on the 16-sample
grid, and the output term uses the instantaneous gain. Its own weaknesses, measured: the constant-width shape is
only reached after that lag (5 ms for an octave at 1 kHz, hundreds of milliseconds for narrow low bands; until then
it behaves like A3); transient deviations of about -15 to -17 dB relative to an in-band tone on large cuts, which
A3 does not have; the skirts of adjacent bands add (eight adjacent bands at -12 dB reach -18 dB); no flat top; both
lanes of a band share one width. Graphs and gates eliminate candidates; which engine sounds right is the owner's
decision after listening.

The engine choice is a listening switch in the editor: saved with the project, not a parameter, not changed by
presets or A/B. Switching while audio plays crossfades the two engines over 20 ms. The dynamics (detector, gain
computer, timing) are the same code for both engines, so a comparison isolates the filter structure.

## Editor

- Header, row 1: preset browser (previous / next, a menu with starter and user presets, Save, Save As, delete,
  new, show folder), **A / B / Copy**, **Undo / Redo** (band add / remove only) and plugin **Bypass**. Row 2: a
  the experimental-engine selector, the *Spectrum / Dynamics* view tabs, a status line, the reported latency with *Fixed Latency*, the band
  counter and **+ Add Band**.
- Presets are the plugin state as readable XML (`.tlmb`) in the user's application data folder
  (`Tone Legion/Multiband/Presets`), so they pass the same validation and migration as host state: a broken file or
  one from a newer schema changes nothing. The preset name is saved with the project. Five starter presets are
  built in as data; they were set by calculation and nobody has listened to them. A/B keeps two state snapshots in
  the processor (they survive closing the editor, they are not saved with the project); loading a preset or
  switching A/B clears the band undo history.
- Spectrum display (`SpectrumView`): input (grey) and output (violet) spectrum, 4096-point Hann FFT computed on the
  message thread from two lock-free FIFOs that the audio thread only fills while an editor is open (blocks are
  dropped when the editor falls behind; nothing is allocated). Each used band shows its nominal region
  (`fLo`..`fHi`): drag an edge, drag the body to move it, or drag across empty space to create a band. The shaded
  curve per band and the white combined curve are the response the engine applies right now, computed from the same
  closed-form A3 model the tests verify against the production engine (`dsp/A3Model.h`), with the current gain
  reduction, makeup and mix. The display takes any extra window height.
- Band strip: one column per used slot, ordered by low edge. Number and colour belong to the slot and never change.
  Column controls: bypass, remove, GR bar with a 1 s hold read-out, threshold, ratio, attack, release.
- Inspector for the selected band, three tabs. *Band*: low / high frequency, knee, range, makeup, mix, stereo mode,
  link. *Detector*: detector type, RMS blend, RMS time, source (main / external), channels, range (own / band /
  custom) with the band to follow or the custom edges. *Timing*: lookahead, adaptive release, frequency-dependent
  release, transient preservation. *Solo* and *Listen* audition the selected band; the status line names what is
  being auditioned for as long as it is. The header shows the latency reported to the host
  and a *Fixed Latency* switch. Controls that do not apply in the current mode are disabled,
  and an amber message appears when a band listens to an external sidechain that the host has not connected.
- Global bar, two rows: input gain, mix, output gain, automatic gain (with the correction it currently applies);
  colour model and drive, oversampling, clip mode and ceiling.
- Only parameters the engine implements have a control (the inert ones listed above have none). A test walks the
  component tree and fails on any widget that is neither bound to a registered parameter nor a documented action.
- All controls use APVTS attachments; the editor polls topology and meters on a 30 Hz message-thread timer and
  touches nothing on the audio thread. A test renders audio with and without an open editor and requires
  bit-identical output.
- Dynamics view (`DynamicsView`, switched with the *Spectrum / Dynamics* tabs): the selected band's static transfer
  curve (threshold, ratio, knee, range, makeup) with a dot at the current operating point (derived from the gain
  reduction by inverting the curve), and its gain-reduction history of the last ~8 s. The history of every band is
  recorded all the time.
- Meter rail (`MeterRail`): input, output and sidechain level in dBFS (peak bar with max-hold between reads,
  300 ms RMS marker, numeric peak held 2 s, amber above 0 dBFS). The sidechain shows *off* (the host has not
  enabled the bus), *silent* (300 ms below -90 dBFS) or *live*; the plugin cannot know which track feeds the bus.
  Next to them the output loudness: momentary (400 ms), short-term (3 s) and gated integrated LUFS and the
  true-peak maximum in dBTP (4x interpolation), with a reset button. The meter follows the commonly published
  BS.1770 / R 128 algorithm and reads the EBU Tech 3341 style reference cases in the tests within 0.1 LU; it has
  not been run against the official test files, and it is not a certified meter. Everything in the rail is
  measured on the audio thread only while an editor is open, so "integrated" covers the time the editor was open.
- The window is resizable (the display takes the extra height); the size the user chose is saved with the project
  and is not changed by loading a preset or switching A/B.
- The meters and the response curve return to "no reduction" when the host stops calling the processor, and a band
  that was hidden shows nothing stale when it comes back.
- Band edits (add, remove, undo, redo) notify the host that non-parameter state changed. `BandSlots` and the state
  functions share a lock that the audio thread never takes, so a host that loads state from another thread cannot
  interleave with an edit; JUCE's own parameter attachments are outside that guarantee.
- Not covered by automated tests: real mouse / keyboard dispatch (tests call the handlers directly), screen
  readers, HiDPI behaviour, repaint efficiency, and any host.
  `TL_SCREENSHOT_DIR=/some/dir build/tests/tl_tests "[.screenshot]"` renders the editor off-screen to PNG files.

## Notes on design choices

- Band identity is its slot; `b{N}_used` is not a parameter. Topology lives in the state tree (`<Topology>`) and the audio
  thread reads an atomic bitmask snapshot (`BandSlots::usedMaskSnapshot`).
- The APVTS is created without an `UndoManager`; band create/delete use their own undo manager with explicit
  before/after snapshots. Knob-edit undo is not part of this milestone.
- Plain parameter values pass through JUCE's float-normalised representation, so values can differ from the table
  by a few ULP after a round trip (tests use relative tolerances; save-after-load is byte-stable).
- State from a newer `schemaVersion` is rejected (current state untouched); unknown nodes/parameters inside a
  supported version are preserved verbatim under `<Foreign>` and written back on save.

## CI and validation

GitHub Actions (`.github/workflows/ci.yml`) builds every supported format, runs the test suite and then the format
validators. Final P1-M2 result at commit `fed99f2` (CI run 8): all jobs passed.

| Platform | Formats built | Tests | Validators |
|---|---|---|---|
| Linux x64 (ubuntu-24.04, GCC) | VST3, CLAP | pass | pluginval 1.0.4 strictness 10 (VST3), clap-validator 0.3.2 (CLAP) |
| macOS arm64 (macos-15) | VST3, AU, CLAP | pass | pluginval strictness 10 (VST3), `auval -strict` (AU), clap-validator (CLAP) |
| Windows x64 (windows-2022, MSVC) | VST3, CLAP | pass | pluginval strictness 10 (VST3), clap-validator (CLAP) |
| Linux x64, ASan + UBSan | tests only | pass | - |

What this does and does not establish:

- Validators exercise the plugin through generic host-like harnesses. They are **not** DAW tests: no real DAW host
  has been tested on any platform, and nothing here is evidence of DAW compatibility.
- Sanitizer and heap-detector runs are evidence, not proof, of memory and real-time safety. RTSan has not been run.
- Validator binaries are downloaded by CI and checked against pinned SHA-256 digests; they are not part of the
  repository.
- **Automation is block-rate.** Parameters are read once per host block and smoothed. The pinned JUCE wrappers do not
  deliver sample offsets for parameter changes on VST3 (only the last point of each queue is applied) or AU
  (`CanScheduleParameters` is false); the CLAP wrapper can split blocks at parameter events only when
  `CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES` is set, which this build does not do. Sample-accurate automation is not
  claimed.
- This is an experimental engineering milestone, **not a release**: the band engine is provisional, the parameter
  schema is not frozen and the plugin codes are placeholders.

## Branches

- `main`: accepted baselines only. It was created at the accepted P1-M2 commit `fed99f2`.
- `claude/sleepy-newton-8vp783`: the P1 development history, preserved as is.
- `develop`: integration branch for work after P1-M2 (starting with the L1 editor). It reaches `main` through a
  pull request with green CI that the owner merges.
- History on existing branches is not rewritten.
