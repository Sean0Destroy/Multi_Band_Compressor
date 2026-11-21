# Tone Legion Multiband

Multiband compressor plugin (JUCE 9). **Milestone P1-M1: engine-independent plugin infrastructure only.**
There is no compression engine, detector, routing, linear-phase, saturation or custom UI yet. The audio path is a
bit-exact pass-through of the main bus (plus an output NaN/Inf guard); the editor is JUCE's generic editor.

## Status

| Item | State |
|---|---|
| Parameter schema (230 parameters, one table) | **PROVISIONAL / NOT FROZEN** (`source/ParameterSchema.cpp`) |
| Plugin / manufacturer codes (`Tlmb` / `Tlgn`) | **PLACEHOLDER**, uniqueness and registration not checked |
| State serialization, band topology, slot allocation + undo | implemented, tested |
| Presets / A-B snapshots | out of scope |
| VST3 (Linux) | builds, pluginval strictness 10 passes |
| CLAP (Linux) | builds against JUCE 9.0.6, clap-validator 0.3.2 passes (see below) |
| AU | macOS only, **not built or tested here** |
| macOS / Windows | **untested** |
| RTSan | `-DTL_ENABLE_RTSAN=ON` requires clang >= 20 and errors otherwise (never faked) |

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
`TL_BUILD_TESTS` (ON), `TL_ENABLE_RTSAN` (OFF). Keep the build directory outside the repo or under `build/`.

Updating the parameter manifest after an intentional schema change:
`TL_UPDATE_GOLDEN=1 build/tests/tl_tests "[ids]"` then review the diff of `tests/golden/parameter_ids.txt`.

## Layout

- `source/PluginIdentity.h` identity constants (checked against JUCE macros and CMake by tests)
- `source/ParameterSchema.*` the whole schema as one table, builds the `ParameterLayout` (one group per band + Global)
- `source/BandSlots.*` band topology (`used[16]`, `everUsed[16]`, LRU counter), create/delete as `UndoableAction`s
- `source/StateSerialization.*` parse -> validate -> migrate -> normalise -> `replaceState`
- `source/PluginProcessor.*` buses, pass-through, bypass, generic editor
- `source/rt/` real-time safe pieces (`FiniteGuard`)
- `tests/` Catch2 suite, `tests/support/` offline renderer and audio-thread heap detector (`operator new`/`delete` override plus, on glibc, `malloc`/`calloc`/`realloc`/`free` interposition)

## Notes on design choices

- Band identity is its slot; `b{N}_used` is not a parameter. Topology lives in the state tree (`<Topology>`) and the audio
  thread reads an atomic bitmask snapshot (`BandSlots::usedMaskSnapshot`).
- The APVTS is created without an `UndoManager`; band create/delete use their own undo manager with explicit
  before/after snapshots. Knob-edit undo is not part of this milestone.
- Plain parameter values pass through JUCE's float-normalised representation, so values can differ from the table
  by a few ULP after a round trip (tests use relative tolerances; save-after-load is byte-stable).
- State from a newer `schemaVersion` is rejected (current state untouched); unknown nodes/parameters inside a
  supported version are preserved verbatim under `<Foreign>` and written back on save.

## Not validated here

macOS, Windows, AU, real DAW hosts. Linux validators were run on Release builds in a scratch directory; they are
external tools and are not part of the repository.
