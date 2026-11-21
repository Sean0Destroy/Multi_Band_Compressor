# Tone Legion Multiband

Multiband compressor plugin (JUCE 9). **Milestone P1-M2: experimental clean DSP engine + detector.**
P1-M1 infrastructure (schema, topology, state, buses) plus a clean feed-forward compressor per used band slot,
running on the **EXPERIMENTAL A3 region engine**. The A3 bell response is *not* accepted as final product
behaviour; the engine sits behind `dsp::IBandEngine` so that it can be replaced. The editor is JUCE's generic
editor. Latency is 0.

## Status

| Item | State |
|---|---|
| Parameter schema (230 parameters, one table) | **PROVISIONAL / NOT FROZEN** (`source/ParameterSchema.cpp`); threshold -80..+12 dB in 0.1 dB steps |
| Band engine | **EXPERIMENTAL** A3 regions (see below), clean compression, peak/RMS/blend detector, GR metering data |
| Plugin / manufacturer codes (`Tlmb` / `Tlgn`) | **PLACEHOLDER**, uniqueness and registration not checked |
| State serialization, band topology, slot allocation + undo | implemented, tested |
| Presets / A-B snapshots | out of scope |
| Builds and validators | see **CI and validation** below (Linux x64, macOS arm64, Windows x64) |
| Real DAW hosts | **not tested** on any platform |
| Usable in a host for auditioning | **no**: the generic editor has no control to create a band (band existence is state topology, not a parameter), so a fresh instance passes audio through unchanged |
| RTSan | `-DTL_ENABLE_RTSAN=ON` requires clang >= 20 and errors otherwise (never faked) |
| ASan + UBSan | `-DTL_ENABLE_ASAN_UBSAN=ON` (GCC/Clang): whole build instrumented, UB fatal; the test heap detector then counts `operator new/delete` only |

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
- `source/PluginProcessor.*` buses, pass-through, bypass, generic editor
- `source/rt/` real-time safe pieces (`FiniteGuard`)
- `source/dsp/` band engine: `IBandEngine` (interface), `A3RegionEngine`, `TptSvf`, `EdgeSmoother`, `GainComputer`,
  `BandDetector`, `GrSmoother` + `ActivityRamp`, `GrMeter`
- `bench/` CPU benchmark, `tools/plot_measurements.py` plots of the measurement CSVs
- `tests/` Catch2 suite, `tests/support/` offline renderer and audio-thread heap detector (`operator new`/`delete` override plus, on glibc, `malloc`/`calloc`/`realloc`/`free` interposition)

## DSP (P1-M2, experimental)

Per used slot, in slot order, per channel (`source/dsp/A3RegionEngine.cpp`):

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
- **Inert parameters (exist, no effect yet):** `makeup`, `mix`, `stereoMode` (only linked/unlinked stereo via `link`),
  `scSource`, `scMode`, `scRef`, `scLo`, `scHi`, `scChan`, `lookahead`, `adaptRel`, `freqRel`, `transient`,
  `character`, and the globals `inGain`, `outGain`, `mix`, `quality`, `lpLength`, `oversampling`, `constantLatency`,
  `overlapMode`, `charModel`, `charDrive`, `autoGain`, `clipMode`, `clipCeiling`. The sidechain bus is accepted but unused.
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
- New work happens on short-lived topic branches (for example `docs/...`, `l1/...`, `research/...`) and reaches
  `main` through a pull request with green CI. History on existing branches is not rewritten.
