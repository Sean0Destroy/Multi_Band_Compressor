# Tone Legion Multiband

A multiband compressor plugin for VST3, AU (Mac) and CLAP, built with JUCE. You place up to eight bands anywhere
on the spectrum, and each band compresses (or lifts) only its own part of the sound, so you can tame a boomy low
end, a harsh upper mid or sibilance without touching the rest. Around the bands there are sidechain options,
stereo and mid/side processing, lookahead, a saturation ("colour") stage, an output clipper, loudness metering,
presets and A/B compare.

> **Beta build.** It is built and checked by automated tests and plugin validators on Windows, macOS and Linux,
> but the formal listening pass and DAW checks in `docs/listening/PROTOCOL.md` have not been done yet. Settings,
> parameters and saved state can still change between builds, so don't rely on it for projects you need to keep.

![The Tone Legion Multiband editor (Soft Glass look) with three bands, rendered off-screen by the test suite from real analyzer and meter data](docs/images/soft-glass-ui.png)

*The editor with three bands, as rendered by `tl_tests "[.screenshot]"` (not a DAW screenshot).*

**License: none selected yet.** The repository is public, but no license has been granted: all rights are
reserved until the owner chooses one. Third-party code under `libs/` keeps its own licenses. The fonts embedded in
the editor (Marcellus, Jost, DM Mono, in `resources/fonts/`) are under the SIL Open Font License 1.1; their licence
texts sit next to them.

## Installing the beta

There are two installers. CI builds both on every run and keeps them for 30 days as workflow artifacts on the
run's Actions page: `ToneLegionMultiband-beta-windows-x64-setup` and `ToneLegionMultiband-beta-macos-arm64-pkg`.
A download from there is a `.zip`, so unzip it first.

**Neither installer is signed yet.** Windows SmartScreen and macOS Gatekeeper will warn that the app comes from an
unknown developer. That is expected for this beta; the steps below say what to click.

**Windows 10 or 11, 64-bit (VST3 and CLAP)**

1. Close your DAW.
2. Run `ToneLegionMultiband-<version>-beta-windows-x64-setup.exe`.
3. If Windows says **"Windows protected your PC"**, click **More info**, then **Run anyway**. If your browser says
   the file isn't commonly downloaded, choose **Keep**.
4. Allow the admin prompt (the plugins go into system folders).
5. Read the beta notice, tick the formats you want and click **Install**.
6. Open your DAW and rescan plugins if "Tone Legion Multiband" doesn't show up.

It installs to `C:\Program Files\Common Files\VST3` and `C:\Program Files\Common Files\CLAP`. To remove it, use
**Settings > Apps > Installed apps > Tone Legion Multiband beta > Uninstall**.

**Mac with Apple Silicon, M1 or newer (AU, VST3 and CLAP).** Intel Macs are not supported in this beta.

1. Quit your DAW.
2. Double-click `ToneLegionMultiband-<version>-beta-macos-arm64.pkg`. macOS will say it can't check the package.
   Click **Done** (or **OK**), open **System Settings > Privacy & Security**, click **Open Anyway** next to the
   installer's name and confirm with your password.
3. Read the beta notice, tick the formats you want on **Installation Type** and click **Install**.
4. Open your DAW. Logic and GarageBand use the AU; if Logic doesn't list it, rescan in
   **Logic Pro > Settings > Plug-in Manager**.

It installs to `/Library/Audio/Plug-Ins/Components`, `/Library/Audio/Plug-Ins/VST3` and
`/Library/Audio/Plug-Ins/CLAP`. To remove it, run **Uninstall Tone Legion Multiband.command** in
`/Library/Application Support/Tone Legion/Multiband`.

To update, quit your DAW and run the newer installer over the old one. Sessions saved with an older beta may not
reload exactly the same. The full tester instructions, including how to report a bug, are in
`docs/beta/TESTER-NOTES.md`.

## How it works, in one picture

```
input ─► Input gain ──┬──► bands ──► Colour ──► global Mix ──► Auto gain ──► Output gain ──► Clipper ──► output
                      │                             ▲
                      └───── dry, lined up in time ─┘

each band:  detector ──► compress or lift ──► Makeup ──► band Mix ──► Character

sidechain input ──► only the band detectors set to External (never the output, except through Listen)
```

- Each band is a filter region placed over part of the spectrum. Where no band is acting, the sound passes
  through untouched; a new instance has no bands and sounds the same as bypass.
- Every band has its own **detector** (what it listens to) and its own **compressor** (how it reacts). By default a
  band listens to its own frequency range of the main input.
- After the bands come the global stages: Colour, the global Mix, Auto gain, Output gain and the Clipper, in that
  order.

## Using the editor

The window has four parts: the header along the top, the spectrum display, a row of band strips with the band
inspector under it, and the master strip on the right.

### Adding and shaping bands in the spectrum display

The display shows the input spectrum (grey), the output spectrum (light blue, filled) and the combined response
of all the bands (light blue curve). Each band is one object you drag:

| To do this | Do this |
|---|---|
| Add a band | Drag across an empty part of the display over the range you want, or click **+ Add** on an empty strip. Up to eight bands. |
| Move a band | Drag its numbered dot left or right. |
| Set its Makeup (static boost or cut) | Drag the dot up or down. |
| Set how far it can compress or lift | Drag the arrow marker up or down. Below the dot the band compresses (Direction Down); above it, the band lifts (Direction Up). The distance is the Range. |
| Stretch or shrink it | Drag one of its edges (sets Low Freq or High Freq), or use the mouse wheel over the band to change its Width. |
| Undo / redo | Ctrl/Cmd+Z and Ctrl/Cmd+Shift+Z (band add / remove and knob edits). |

The shaded area of a band runs from its static curve out to its Range limit, and the live curve moves inside it as
the band works. The curve is a picture of one channel: it does not show the stereo mode or the global Mix.

### Band strip (one per band)

Each band has a strip with its name (SUB, LOW, LOW MID, MID, PRESENCE, UPPER or AIR, from where its centre sits),
its frequency range and a gain-reduction bar. Click a strip to select that band.

| Control | What it does | Tips |
|---|---|---|
| **Threshold** (-80 to +12 dB, default -18) | The level at which the band starts to act. | Lower it until the band reacts only to the peaks you want to control. |
| **Ratio** (1:1 to inf, default 2:1) | How hard the band pushes back once the level is over the threshold. 4:1 means 4 dB over becomes 1 dB over. At the top it reads "inf" and acts like a limiter. | 2:1 to 4:1 for gentle control, 8:1 and above to hold a range firmly. |
| **Attack** (0.01 to 300 ms, default 10) | How fast the band reacts when the level goes over. | Faster catches more of a transient; slower lets the punch through. |
| **Release** (5 to 5000 ms, default 150) | How fast the band lets go once the level drops. | Short releases can pump or distort on low bands; longer ones sound smoother. |
| **Byp** | Bypasses this band only (with a short fade). | |
| **×** | Removes the band. Undo brings it back. | |

The needle meter next to the inspector shows how much the selected band is reducing (or, for an upward band, how
far it is lifting), on a 0 to 20 dB scale.

### Band inspector: Band tab

| Control | What it does | Tips and catches |
|---|---|---|
| **Low Freq / High Freq** | The edges of the band's range. | The detector listens to these edges as set, even when Width changes the filter. |
| **Width** (25 to 400 %, default 100) | Widens or narrows the band's filter around its centre without moving it. 100 % follows the edges; 200 % covers twice as many octaves, 50 % half. | Use it when a band reaches too far into its neighbours, or not far enough. |
| **Knee** (0 to 24 dB, default 6) | How gradually the band starts to act around the threshold. 0 is a hard knee; higher values ease in. | Soft knees sound more natural on full mixes. |
| **Range** (0 to 48 dB, default 24) | The most the band will ever reduce (or lift). At 0 the band does nothing dynamic. | Keep it small for gentle "safety" control; large ranges let the band clamp hard. |
| **Direction** (Down / Up) | Down compresses above the threshold. Up lifts the band above the threshold instead, by the same amount Down would cut, at most Range. Dragging the arrow marker sets both Direction and Range. | Up brings out detail that crosses the threshold, for example the attack of a snare. |
| **Makeup** (-24 to +24 dB) | A fixed boost or cut of the band, after its compression. With no compression happening it works like a static EQ boost or cut of that range. | Use it to win back the level the band took away. |
| **Mix** (0 to 100 %, default 100) | How much of the band's change you hear. 0 % means the band changes nothing. | Lower it for parallel-style compression of one range. |
| **Character** (0 to 100 %, default 0 = off) | Warm saturation of this band alone, after its compression: it adds harmonics of what is in the band and leaves the rest of the spectrum alone. No latency. | Adds weight or bite to one range. Very bright bands at high settings can sound grainy. |
| **Stereo Mode** | **Stereo**: both sides together (see Link). **Left/Right**: each side on its own. **Mid**: only the centre of the band is processed. **Side**: only the sides. **Mid/Side**: centre and sides each on their own. | Mid to control a centred vocal or bass, Side to tame wide cymbals or reverb. Greyed on a mono track. |
| **Link** (0 to 100 %, default 100) | In Stereo mode, how much left and right share one gain change. At 100 % both sides get the deeper of the two reductions, so the stereo image stays steady; at 0 % they act independently. | Only active in Stereo mode with Channels set to L/R. |

### Band inspector: Detector tab

The detector decides *what* the band reacts to. It always listens before any band processing.

| Control | What it does | Tips and catches |
|---|---|---|
| **Detector** (Peak / RMS / Blend) | Peak reacts to the instantaneous level, RMS to the average level over the RMS Time, Blend mixes the two. | Peak for transients, RMS for smoother, more "musical" control. |
| **RMS Blend** (0 to 1, default 0.5) | In Blend mode, how much RMS is in the mix: 0 is all peak, 1 is all RMS. | Only active in Blend mode. |
| **RMS Time** (1 to 300 ms, default 30) | The averaging window for RMS. | Longer is smoother and slower. Not used in Peak mode. |
| **Source** (Main / External) | Main listens to the plugin's own input. External listens to the sidechain input your DAW connects to the plugin. | External for ducking, for example a bass band that dips when the kick hits. With nothing connected the band does nothing, and an amber warning says so. |
| **Listens To** (Own / Band / Custom) | Which frequency range the detector hears. Own: the band's own range. Band: another band's range (pick it in **Follow**). Custom: your own range in **Det Low / Det High**. | Custom lets a band react to one range and act on another, for example cut a broad upper-mid band only when sibilance at 6 to 9 kHz is loud. |
| **Channels** (L/R, Mid, Side, Sum, Max) | Which part of a stereo signal the detector hears. L/R: each side on its own. Mid or Side: that component. Sum: left plus right (6 dB hotter than Mid for centred material). Max: each side on its own, and the deeper reduction is applied to both. | On a mono input every choice except Sum uses the mono signal. |

### Band inspector: Timing tab

Attack and Release are on the band strip. The Timing tab holds the extras:

| Control | What it does | Tips and catches |
|---|---|---|
| **Lookahead** (0 to 10 ms, default 0) | Lets the band see the signal coming and react before it arrives, so it catches the start of a peak in its range. | **Adds latency**: the whole plugin is delayed by the largest lookahead of any band. Lookahead removes the in-band overshoot, not the broadband click at an onset. Not automatable. Off while Low Latency is on. |
| **Adaptive Rel** (0 to 100 %, default 0 = off) | Short peaks recover at the set release, while long stretches of reduction recover more slowly. | Smooth, less pumpy release on sustained material. |
| **Freq Release** (0 to 100 %, default 0 = off) | Scales the release by where the band sits: longer for low bands, shorter for high bands (at 100 %, about 3 times longer at 100 Hz and 3 times shorter at 10 kHz). | Avoids distortion on bass bands and dullness on high bands with one release setting. |
| **Transient** (0 to 100 %, default 0 = off) | Backs the band off during onsets, so attacks pass through while the sustain is still compressed. Steady sounds are compressed as before. | Keeps drums punchy under heavier compression. |
| **Fixed Latency** (off by default, whole plugin) | Always delays by the full 10 ms lookahead, so the latency reported to your DAW never changes when you change lookahead or add and remove bands. | Use it if your DAW glitches or drifts when the plugin's latency changes. It is one setting for the whole plugin, so it shows the same on every band. Not automatable. Off while Low Latency is on. |

These timing designs were chosen by measurement, not yet by ear, and may be retuned.

### Solo and Listen

Under the inspector tabs are two audition buttons for the selected band. They are never saved, only one band can
be auditioned at a time, and the title strip of the display says what you are hearing.

- **Solo** plays only what this band contributes, after its compression.
- **Listen** plays what this band's detector hears (6 dB down). With an External source this is the one way to hear
  the sidechain signal.

Both skip the colour stage, the global Mix, Auto gain and the Output gain, but still pass through the clipper.

### Master strip (right side)

| Control | What it does | Tips and catches |
|---|---|---|
| **Meters** | Input, output and sidechain level in dBFS (amber above 0 dBFS). The sidechain shows *off* (no sidechain enabled in the DAW), *silent* or *live*. | They only measure while the editor is open. |
| **Loudness** | Momentary, short-term and integrated LUFS and the true-peak maximum (dBTP) of the output. **Reset** starts integrated and true peak again. | Integrated covers only the time the editor was open. Not a certified meter. |
| **Input** (-24 to +24 dB) | Gain into the plugin, before the bands and their detectors. | Changing it changes how hard every band works. |
| **Mix** (0 to 100 %, default 100) | Blends the processed sound with the dry input (after Input gain), lined up in time. | Parallel compression of the whole plugin. |
| **Output** (-24 to +24 dB) | Gain after everything except the clipper. | |
| **Overlap** (Stack / Blend) | What happens where neighbouring bands overlap. **Stack** (default): their gain changes add up, so two bands compressing together cut deeper than either asks. **Blend**: each band is eased back by what its neighbours already do, so the cut stays close to what each band asks for. | Blend when adjacent bands compress at the same time and the result is too deep. No latency. **Split** is listed but not built yet: it can't be picked, and a session that sets it gets Stack. |
| **Auto gain** (On / Off) | Slowly matches the output loudness to the input loudness, by at most 12 dB either way, and shows the correction it applies. | Useful for fair before/after comparisons. It follows the average level over about a second, not individual peaks. |
| **Colour** (Clean / Warm FET) and **Drive** | A saturation stage on the whole processed signal, after the bands. Warm FET adds gentle even and odd harmonics and rounds peaks; Clean leaves the sound alone. Drive sets how much. | Drive is greyed while Colour is Clean. |
| **Colour OS** (Off / 2x / 4x / 8x) | Runs the colour stage at a higher sample rate to reduce aliasing (harsh, inharmonic artefacts) at high drive. | **Adds latency** (47 / 53 / 55 samples, about 1 ms at 48 kHz) whenever it is on, even with Colour set to Clean, so switching the colour never changes the latency. More CPU. Not automatable. Off while Low Latency is on. |
| **Clip** (Off / Soft / Hard) and **Ceiling** (-6 to 0 dBFS, default -0.3) | The very last stage: keeps sample peaks from going over the ceiling. Soft rounds peaks off as they approach it, Hard cuts them flat. | A safety stage for peaks; heavy clipping adds distortion. The view next to it shows what is being shaved off. Ceiling is greyed while Clip is Off. |
| **Clip OS** (Off / 2x / 4x / 8x) | Runs the clipper oversampled, so hard overs alias less. A final check at the normal rate keeps the ceiling exact. | **Adds latency** (47 / 53 / 55 samples) whenever it is on, even with Clip Off, and on top of Colour OS. Not automatable. Off while Low Latency is on. |

### Header (top row)

| Control | What it does | Tips and catches |
|---|---|---|
| **Presets** | Previous / next arrows, **Save**, and a menu (click the name) with the starter presets and your own, plus Save As, Delete, New (no bands) and Show preset folder. | The five starter presets (Gentle 3 Band, Low End Control, De-Ess, Drum Bus 4 Band, External Sidechain Duck) are starting points set by calculation; nobody has judged them by ear yet. Loading a preset clears the undo history. |
| **A / B** | Two complete settings you can switch between to compare. | They survive closing the editor but are not saved with the project. Switching clears the undo history. |
| **Low Latency** | One switch that makes the plugin add no delay: it turns off every band's lookahead, both oversampling stages and Fixed Latency. Those controls are greyed and come back as they were when you switch it off. Its tooltip shows the latency the plugin currently reports. | For tracking and live monitoring. Without lookahead, fast peaks get through a little before the bands react; without oversampling, the colour stage and the clipper can alias more at high settings. Not automatable. |
| **Window scale** (75 to 150 %) | Draws the whole window larger or smaller. | Saved with the project, not changed by presets or A/B. You can also resize the window; the display takes the extra height. |
| **Bypass** | Bypasses the whole plugin, with a short fade. | |

## Latency at a glance

| What | Latency it adds |
|---|---|
| Nothing below turned on | 0 |
| Lookahead | the largest lookahead of any band (up to 10 ms) |
| Fixed Latency | always the full 10 ms for lookahead, whatever the bands are set to |
| Colour OS 2x / 4x / 8x | 47 / 53 / 55 samples, while it is on |
| Clip OS 2x / 4x / 8x | 47 / 53 / 55 samples, while it is on, added to Colour OS |
| **Low Latency** on | 0, whatever the settings above are |

The plugin reports its latency to the DAW, which compensates for it on playback. Hosts differ in how they handle a
latency change during playback; Fixed Latency avoids that for lookahead changes.

## Coming later and known limits

- **Not yet heard properly.** The automated tests, validators and sanitizers say nothing about how it sounds. The
  formal listening pass and DAW checks in `docs/listening/PROTOCOL.md` are still to do.
- **Linear phase** is coming: it is in draft (PR 11) and not on `main`. The `quality` and `lpLength` parameters
  exist for it but do nothing yet and have no control.
- **Split** overlap mode is listed but not built.
- Designs chosen without a listening session and likely to be retuned: the timing options (adaptive release,
  frequency-dependent release, transient preservation), the colour curve, per-band Character, Auto gain and the
  starter presets.
- Per-band Character runs at the normal sample rate (anti-aliased, not oversampled).
- Automation is applied once per audio block, not sample-accurately.
- The loudness meter has not been checked against the official EBU test files.
- Editor: no compact layout (at 100 % the window needs at least 1136 x 772 points, 852 x 579 at 75 %), no band
  suggestions, and keyboard and screen-reader use has not been tried by a person.
- Release work still open: the parameter set is not frozen (IDs and ranges can change, which would break saved
  projects), the plugin codes (`Tlmb` / `Tlgn`) are placeholders, no license is chosen, the installers are
  unsigned and not notarised, Intel Macs are not built, and AAX is not built.
- RTSan has not been run; the evidence for real-time safety is the audio-thread heap detector in the tests plus
  ASan / UBSan runs.

---

# For developers

## Status

| Item | State |
|---|---|
| Parameter schema (248 host parameters, one table) | **PROVISIONAL / NOT FROZEN** (`source/ParameterSchema.cpp`) |
| Band engine | A-prime regions; A3 kept only as a test reference (see "Band engines") |
| Plugin / manufacturer codes (`Tlmb` / `Tlgn`) | **PLACEHOLDER**, uniqueness and registration not checked |
| State serialization, band topology, slot allocation + undo | implemented, tested |
| Presets / A-B | starter presets (built in, not yet judged by ear), user preset files, A/B compare (`source/PresetManager.*`) |
| Builds and validators | see **CI and validation** below (Linux x64, macOS arm64, Windows x64) |
| Installers | Windows (Inno Setup) and macOS (`.pkg`) betas, unsigned, built and install-tested in CI |
| Real DAW hosts | not yet checked against `docs/listening/PROTOCOL.md` |
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
- `source/ParameterUndo.*` records knob edits (one step per gesture) into the same undo history
- `source/StateSerialization.*` parse -> validate -> migrate -> normalise -> `replaceState`
- `source/PresetManager.*` starter presets, user preset files, A/B compare (message thread)
- `source/PluginProcessor.*` buses, pass-through, bypass, creates the editor
- `source/ui/` editor: `Theme.h` (Soft Glass colour, radius and size tokens, panel and card painters), `ValueFormat`
  (value text with units), `Controls` (look-and-feel, knob / drop-down / toggle bound to parameters), `SpectrumView`,
  `ConsoleMeters` (gain-reduction needle, clipper view), `MeterRail`, `BandColumn`, `BandInspector`, `PluginEditor`
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

## CI and validation

GitHub Actions (`.github/workflows/ci.yml`) runs on every pull request, on pushes to `main`, and by hand. It builds
every supported format, runs the test suite, runs the format validators, then builds the beta installers and
installs and removes them on the runner. The table is the run for the pull request that brought `develop` to
`main` at `8972208` (develop commit `9153690`): all four jobs passed. The Actions page is the record for any later
commit.

| Job | Toolchain | Formats built | Tests (`ctest`) | Validators and installer |
|---|---|---|---|---|
| `linux-x64` (ubuntu-24.04) | GCC 13.3.0 | VST3, CLAP | 257 of 257 | pluginval 1.0.4 strictness 10 (VST3): success; clap-validator 0.3.2: 16 passed, 0 failed, 5 skipped |
| `macos-arm64` (macos-15, Apple Silicon runner) | AppleClang 17.0.0 | VST3, AU, CLAP | 256 of 256 | pluginval strictness 10 (VST3): success; `auval -strict` (AU): succeeded; clap-validator: 16 passed, 0 failed, 5 skipped; `.pkg` built, install / uninstall smoke test passed |
| `windows-x64` (windows-2022) | MSVC 19.44 | VST3, CLAP | 256 of 256 | pluginval strictness 10 (VST3): success; clap-validator: 15 passed, 0 failed, 6 skipped; Inno Setup installer built, silent install / uninstall smoke test passed |
| `linux-x64-asan-ubsan` (ubuntu-24.04) | GCC 13.3.0, `-fsanitize=address,undefined` | tests only | 256 of 256, no sanitizer report | - |

Why one more test on the Linux job: one test case, "Allocation counter detects malloc and free (positive
control)", exists only where the test binary interposes glibc's `malloc` / `free` (`TL_TEST_MALLOC_INTERPOSED` in
`tests/support/AllocationCounter.h`), which is plain Linux. Under AddressSanitizer the sanitizer owns the
allocator, and macOS and Windows have no glibc, so there the case is not compiled and the audio-thread heap
detector counts `operator new` / `delete` only. Every other test runs on all four jobs. The consequence: the
"no `malloc` on the audio thread" evidence comes from the Linux job alone; on macOS and Windows only C++
allocations are detected.

What this does and does not establish:

- A green macOS job means the plugin builds, passes its tests and the three validators on GitHub's Apple Silicon
  runner. It is not a test in Logic or any other host, or on Intel Macs (not built).
- Validators exercise the plugin through generic host-like harnesses. They are **not** DAW tests and are not
  evidence of DAW compatibility.
- Sanitizer and heap-detector runs are evidence, not proof, of memory and real-time safety. RTSan has not been run.
- Validator binaries are downloaded by CI and checked against pinned SHA-256 digests; they are not part of the
  repository.
- **Automation is block-rate.** Parameters are read once per host block and smoothed. The pinned JUCE wrappers do not
  deliver sample offsets for parameter changes on VST3 (only the last point of each queue is applied) or AU
  (`CanScheduleParameters` is false); the CLAP wrapper can split blocks at parameter events only when
  `CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES` is set, which this build does not do. Sample-accurate automation is not
  claimed.
- This is a beta, **not a release**: the parameter schema is not frozen and the plugin codes are placeholders.

## Building the beta installers

Both are built by CI and uploaded as workflow artifacts (kept 30 days). Tester-facing instructions are in
`docs/beta/TESTER-NOTES.md`; the beta notice both installers show first is `packaging/beta-notice.txt`. The
version comes from `project(... VERSION ...)` in `CMakeLists.txt`. Neither installer is signed, so Windows
SmartScreen and macOS Gatekeeper warn testers about an unknown developer.

- **Windows** (`packaging/windows/`, Inno Setup 6.3+): VST3 to `C:\Program Files\Common Files\VST3`, CLAP to
  `C:\Program Files\Common Files\CLAP`, a format picker and an uninstaller under Installed apps. The
  `windows-x64` job builds it after the validators, installs and uninstalls it silently on the runner
  (`smoke-test-installer.ps1`) and uploads `ToneLegionMultiband-beta-windows-x64-setup`. Locally, after a
  Release build: `./packaging/windows/build-installer.ps1`.
- **macOS** (`packaging/macos/`, `pkgbuild` / `productbuild`): AU, VST3 and CLAP to `/Library/Audio/Plug-Ins/...`
  as three ticked choices, plus notes and an uninstaller in `/Library/Application Support/Tone Legion/Multiband`.
  Apple Silicon only, because CI builds arm64. The `macos-arm64` job builds it, installs and removes it on the
  runner (`smoke-test-pkg.sh`) and uploads `ToneLegionMultiband-beta-macos-arm64-pkg`. Locally, after a Release
  build: `bash packaging/macos/build-pkg.sh`.

## Branches

- `main`: accepted baselines only. It was created at the accepted P1-M2 commit `fed99f2`.
- `claude/sleepy-newton-8vp783`: the P1 development history, preserved as is.
- `develop`: integration branch for work after P1-M2 (starting with the L1 editor). It reaches `main` through a
  pull request with green CI that the owner merges.
- History on existing branches is not rewritten.

---

# Technical reference

## Signal processing in detail

The exact behaviour behind the controls above, for anyone changing the code. Per used slot, in slot order, per
channel (dynamics in `source/dsp/DynamicsCore.cpp`, filters in `source/dsp/APrimeRegionEngine.cpp`, the default,
and `source/dsp/A3RegionEngine.cpp`, the test reference; several notes below were written for A3 and say so):

- **A3 region (test reference; A-prime is described under "Band engines"):** TPT SVF with prewarped edges, `g = sqrt(wl wh)`, `R = (wh - wl)/(2g)`, delta form
  `y = x + (gEff - 1) 2R bp`. At `gEff == 1` the output is bit-identical to the input. The response is a bell:
  full depth only at the (digital) centre, about -2.7 dB at the nominal edges for -12 dB, width shrinking with depth,
  2.4 dB ripple at the junction of two adjacent bands, up to ~37 deg interchannel phase when unlinked channels get
  different gain. These are the documented reasons it is experimental.
- **Edges:** one-pole smoothing (20 ms) in the log2 domain, ordering resolved in DSP, runtime clamp
  `fLo >= 10 Hz`, `fHi <= 0.45 fs`, `fHi >= fLo * 2^(1/24)` (never written back). Coefficients change only on an
  absolute 16-sample grid (sample counter reset in `prepareToPlay`).
- **Detector:** main input before compression, by default the band's own range (see detector routing below):
  Butterworth HP at `fLo` then LP at `fHi`;
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
  It is edited on the inspector's Detector tab; a band that listens to an External sidechain the host has not
  connected shows an amber warning there, and the master strip shows the sidechain's state (off, silent, live).
  - `scSource`: main input (after the input gain) or the external sidechain bus. A mono sidechain feeds both
    channels. With no sidechain connected an External band gets silence and does nothing; it never falls back to the
    main input. Switching the source crossfades the detector input over 5 ms. Sidechain audio is only read (through
    a guarded copy) and cannot reach the output.
  - `scMode`: the detector's frequency range is the band's own, another slot's (`scRef`, its `fLo`/`fHi`), or a
    custom one (`scLo`/`scHi`), smoothed like the band edges. A reference to a slot that does not exist falls back
    to the band's own range. Deleting a followed band converts its followers to that range as a custom range.
  - `scChan`: `L/R` (per lane), `Mid`, `Side`, `Sum` (L+R, 6 dB hotter than Mid for in-phase material) or `Max`
    (per lane, deeper reduction applied to both). With a mono main input every choice except `Sum` uses the mono
    sum (a stereo sidechain is summed; `Side`, which a mono signal does not have, means the same as `Mid`).
- **Stereo modes (`stereoMode`, stereo only):** `Stereo` (left/right linked by `link`; the link only acts with
  `L/R` detection, the other detector channel choices already give both lanes the same signal), `Left/Right` (independent),
  `Mid` or `Side` (only that component of the band is processed, the other is untouched), `Mid/Side` (both,
  independently). Mid/side needs no global encode: the A3 region filter is the same linear system on both channels,
  so its band-pass output is split into M and S and the two gain changes are added back per channel. Changing the
  mode fades the band out over 10 ms, swaps, and fades back in; while any lookahead delay is running the swap also
  waits another 10 ms, until every gain still in the delay lines is neutral. In the mid/side modes the two GR meter
  channels are mid and side.
- **Automatic gain (`autoGain`, v1):** a slow loudness match. The K-weighted mean square (BS.1770 pre-filter,
  re-derived per sample rate; the 48 kHz coefficients are checked against the published values) of the dry signal
  (after the input gain) and of the processed signal are integrated over 1 s; the processed signal is scaled by
  `sqrt (dry / processed)`, limited to +-12 dB and smoothed (250 ms). While the dry signal is below -70 dB the
  correction is held. It restores the average level and does not follow individual gain-reduction events; fast
  level changes are not compensated. The target is updated on the absolute 16-sample grid, so static settings stay
  bit-identical across block layouts. This is not a loudness meter: no gating, no LUFS read-out.
- **Clipper (`clipMode`, `clipCeiling`):** the very last stage. *Hard* clamps at the ceiling. *Soft* is exactly
  linear up to half the ceiling, then a tanh section that approaches the ceiling without exceeding it (value and
  slope continuous). At the base rate both alias on strong high-frequency overs. **`clipOversampling`** (Off / 2x /
  4x / 8x, not automatable, Off by default) runs the clipper at 2x / 4x / 8x with the same linear-phase half-band
  filters as the colour stage. Its own latency (47 / 53 / 55 samples) is reported, and added to the colour's,
  whenever it is on, whether or not the clipper is, so switching the clipper never changes the latency (with the
  clipper off the output is then an exact delay). The decimation filter rings a little past the ceiling on clipped
  peaks (about 1.1 dB at most, measured), so a final clamp at the base rate keeps the ceiling exact (owner's
  decision, 2026-11-24: a control named Ceiling must hold); that clamp brings part of the aliasing back. Measured with a 5 kHz tone 12 dB over a -6 dB ceiling (folds at 3, 7 and
  13 kHz): about 10 dB less aliasing with the clamp, 25 to 60 dB less without it. A factor change crossfades the
  oversampled clip with the base-rate clip of the delayed signal and the delay to its new length (10 ms each), so the
  ceiling holds throughout. Bypass, and switching the clipper off, fade it out over 20 ms (bypass then
  returns the input even above the ceiling); switching it on fades it in over 20 ms, so the ceiling holds from then
  on. Changing between *Soft* and *Hard* crossfades the outputs of the two curves, so the ceiling holds throughout.
  Solo and Listen pass through the clipper as well.
- **Lookahead (`lookahead` per band 0..10 ms, `constantLatency`):** the audio is delayed by the largest lookahead of
  the used bands (or by the full 10 ms with constant latency on) and that delay is reported to the host as latency;
  each band's gain is delayed by the difference, so it leads the audio by exactly its own lookahead. A neutral band
  with lookahead is an exact delay; bypass keeps the same delay. When a value changes while running, the delay taps
  crossfade over 10 ms and the new latency is reported from the message thread (hosts differ in how they react to a
  latency change during playback; constant latency avoids it). A tap is only faded in once that much history has
  been collected, however quickly the value is changed again. Every slot has its gain delay whether it is used or
  not, and a band that is not running has a history of unity gains: adding a band never replays anything, and a
  removed band is retired only after its last delayed gain has been applied. With all lookaheads at 0 nothing is
  delayed or copied. Note that an abrupt onset is largely broadband and therefore outside a band's region: lookahead removes
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
  detector filter, before the envelope) 6 dB down, which with an external source is the one way sidechain audio
  reaches the output. Both replace the signal before the clipper (not affected by the colour, the global mix,
  automatic gain or the output gain) and are delayed to line up with the normal output (lookahead and oversampling
  latency); after switching, the audition fades in once its delayed signal has arrived. One at a time, 10 ms
  crossfades, off while bypassed, and cleared by a state or preset load, by A/B, by removing that band and by
  closing the editor.
- **Colour (`charModel`, `charDrive`) and `oversampling`:** a global coloration stage on the wet signal, after the
  bands and before the global mix. One original model, *Warm FET*: an asymmetric tanh curve with unity small-signal
  gain, `y = (tanh (k (x + b)) - tanh (k b)) / (k (1 - tanh^2 (k b)))`, `b = 0.15`, `k = 2.5 drive`, followed by a
  5 Hz DC blocker. The bias gives even harmonics, tanh the odd ones and gentle peak rounding. It is not a model of
  any hardware. Oversampling (2x / 4x / 8x) is linear-phase: three Kaiser half-band FIR stages (95 / 25 / 17 taps,
  stop band about 84 dB for the first stage from 0.56 fs and about 88 dB for the other two, as measured from the
  coefficients in review) with a round-trip latency of 47 / 53 / 55 samples, reported to the host whenever
  oversampling is on (whether or not the colour is in use, so that automating the colour never changes the
  latency); the clean path and the dry signal are delayed by the same amount, so the clean model with oversampling
  is an exact delay. Measured at 48 kHz with a 9 kHz tone at full drive, the folded 4th and 5th harmonics drop by
  more than 40 dB at 4x. Switching the colour on while oversampling runs dips the level by about 10 % for a
  millisecond (measured in review; no step). The output clipper has its own oversampling (see Clipper). Each half-band
  stage runs in polyphase form: it skips the zero taps and adds the mirrored taps before multiplying, the same
  output as the full FIR (tested against a direct-form reference). Measured with `tl_bench` (Release, GCC, stereo,
  48 kHz, block 128, ns per frame): colour at 2x / 4x / 8x costs about 144 / 219 / 339, the hard clipper about
  99 / 139 / 210 (before: about 345 / 520 / 790 and 338 / 518 / 777).
- **Per-band character (`character`, "Character" on the inspector's Band tab, 0..100 %, 0 = off and
  bit-identical):** the Warm FET curve of the colour stage (same curve, 100 % = the same full drive), applied to the
  band's own component after its gain, so after its compression, makeup and band mix. What it adds is the curve's
  residue `f(u) - u`: the curve has unity small-signal gain, so the band's linear response is unchanged and only
  harmonics of what is in the band are added; what lies outside the band is not saturated (a tone outside the band
  stays within 0.1 dB, measured). The residue is anti-aliased with the first-order antiderivative method
  (`(H(u) - H(u_prev)) / (u - u_prev)`, `H' = f - id`, in double precision; this puts it half a sample behind the
  band, an offset of about 10 us at 48 kHz), has its DC removed (5 Hz) and its drive ramped per sample over 20 ms.
  No latency, no oversampling. On a Mid band the side is left alone (and the mid on a Side band); band bypass and
  host bypass take it out. Measured: a 100 Hz tone at 0.7 in a 60 - 300 Hz band at 100 % gets its 2nd harmonic at
  -18 dB and its 3rd at -20 dB; a quiet signal (-60 dBFS) changes by less than -50 dB of itself. Aliasing (offline
  check of the curve on a 7 kHz tone at 0.7, full drive): about -35 dB relative to the tone, 8 dB less than without
  the anti-aliasing; bright bands at high drive may sound grainy. Cost (`tl_bench`, 8 bands compressing, all at 50 %,
  stereo): about 920 ns per frame against 570 without.
- **Inert parameters (exist, no effect yet):** the globals `quality`, `lpLength`.
  `overlapMode` works for Stack and Blend; its third choice, Split, acts as Stack until it is built.
- **Spec amendment (accepted in review):** "becoming used -> states reset, activity ramps from 0" applies to a band
  that was fully skipped. A slot that is used again while it is still fading out (e.g. undo right after delete)
  keeps its filter/detector/smoother states and edges and ramps up from its current activity, so there is no
  discontinuity. Pinned by the test "Band re-used during its fade-out continues without reset".
- **Real-time safety:** no allocation after construction (fixed 256-sample internal chunk, so any host block size
  works), no locks or I/O; input guard (non-finite -> 0), non-finite internal state -> band reset, output guard;
  `ScopedNoDenormals`. A finite but absurd input (around 3e38, or less with a large input gain) overflows inside:
  the output stays finite, at the cost of 10 - 25 ms of silence per event while the affected states are reset
  (measured in review). Parameters are read once per host block, so automation takes effect at block boundaries:
  static parameters are bit-identical for any block layout, automated ones differ slightly between layouts.
- **GR metering:** `PluginProcessor::getGainReductionMeter().getGainReductionDb (slot, channel)`, lock-free,
  the minimum applied band gain of the last block in dB; `readAndResetHeldGainReductionDb (slot, channel)` returns the
  deepest value since its previous call (min-hold until read, for UI polling), or the last block's value when no
  block was processed in between.

## Band engines (both experimental)

A numerical study (kept outside this repository) compared the original engine (A3) with four alternatives against hard
gates: exact null at unity gain, no response above the commanded range, no dynamic overshoot above +0.5 dB with
smoothed gains, stability, bounded cost. Results, in short:

| Candidate | Result |
|---|---|
| **A3** (the original engine): `y = x + (G - 1) 2R bp(x)` | survives. Known weaknesses: the bell narrows with depth (a 1-octave band acts over 0.85 octave at -3 dB and 0.06 at -48 dB), the nominal edges never get deeper than about -3 dB, 2.4 / 7.1 dB ripple between adjacent bands at -6 / -12 dB, cut and boost differ |
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

**Default: A-prime** (the owner's pick, 2026-11-24, made on the measurements above and below; the listening pass in
`docs/listening/PROTOCOL.md` is still to confirm it). A new instance starts on A-prime and its project saves the
choice. **Since the owner's call of 2026-11-24 the editor has no engine switch**: every project opens on A-prime,
including one saved with A3 selected or from before A-prime was the default (which may sound slightly different
from how it was made). Use the band's Width control to widen or narrow a band instead. Measured differences to know about: where several
adjacent bands compress together, A-prime goes deeper than each band's command (four adjacent bands each at -6 dB
reach -6.3 to -8.4 dB between 200 Hz and 3.5 kHz, A3 -4.3 to -7.4 dB); a band's Mix is an exact parallel blend
only with A3; with Mid/Side or a stereo link below 100 % the two lanes of an A-prime band share one width. CPU is
the same (8 bands, stereo: about 590 ns per frame for either engine).

**Band overlap (`overlapMode`, "Overlap" in the master strip).** Each band's filter has skirts that reach into its
neighbours, so with **Stack** (the default, and how every earlier version sounds) the gain reductions of bands that
compress together add up where they meet. **Blend** eases each band back by what its neighbours' skirts already do at
its centre, so each band lands on its own command there. How: at each of 16 points per block, per lane, it solves
"applied gain + neighbours' skirts at my centre = my command" (dB) with exact closed-form models of each pair of
bands (A3's or A-prime's, whichever engine runs), linearised at the last solution and refined with a few Gauss-Seidel
sweeps. A band is never pushed past its own command or to the other side of 0 dB, so a cut never becomes a boost and a
band that is not compressing is never touched; a cut next to a boost is left as Stack has it. One band alone, or Stack,
is bit-identical to before. Switching fades over 20 ms; no latency. Measured (four adjacent bands, 120 Hz - 5 kHz,
level between 200 Hz and 3.5 kHz):

| Each band at -6 dB | Stack | Blend |
|---|---|---|
| A-prime | -8.4 to -6.3 dB | -6.1 to -4.9 dB |
| A3 | -7.4 to -4.3 dB | -6.0 to -3.9 dB |

With A-prime at deeper settings Blend gives -12.1 to -9.8 dB (bands at -12 dB) and -24.1 to -19.2 dB (bands at
-24 dB); the tests check that Blend is never more than 0.1 dB deeper than the command at -3, -6, -12 and -24 dB, for
both engines, while Stack is at least 0.5 dB deeper.

Two bands over the same range at -6 dB each: Stack -12 dB, Blend -6 dB. Between band centres Blend sits a little
short of the command (up to about 1 dB at -6 dB, 5 dB at -24 dB with four bands), because the correction is solved
at the centres. Known limits: when Mid/Side bands sit next to Left/Right bands the lanes are paired by index, which is
an approximation; the gain-reduction meters still show what each band's compressor asks for, not what Blend applies.
Cost (`tl_bench`, 8 bands compressing, stereo): about 770 ns per frame against 610 for Stack.

The engine choice was a listening switch in the editor (now removed; the internal switch remains for the tests):
saved with the project, not a parameter, not changed by presets or A/B. The dynamics (detector, gain computer, timing) are the same code for both engines, so a
comparison isolates the filter structure, and a changeover while audio plays is built on that:

- The engine that is switched in takes over everything the two share from the one that is running
  (`IBandEngine::continueFrom`: detector and envelope states, gain ramps, lookahead history, automatic gain, the
  position on the coefficient grid). From its first sample it computes the same band gains as the running engine;
  a test requires them to be bit-identical. Before this, the new engine started from a clean state and was heard
  settling: +5.0 dB with a 100 ms attack, +8.5 dB with 300 ms, +4.2 dB with a 300 ms RMS window, -6.8 dB with
  automatic gain (125 ms after the click). Now: within 0.01 dB for a tone at the band centre in all settings
  measured (`tests/RepairMeasurements.cpp`).
- The band filters are its own, but they do not start from silence either: each band's filter memory is carried
  over in a form both kinds of filter share (the state of the band's unity-peak band-pass). That is exact for a
  band at unity gain (a bypassed band, a band below threshold) and, for a band that is applying gain, exact for
  what lies at the band centre and close for the rest of the band, where the two engines differ in damping.
- For that remaining difference the new engine first runs unheard on a copy of the input, for four time constants
  of the narrowest active band, `4 / (pi * bandwidth)` (longer by the square root of a makeup boost), at least
  10 ms and at most 250 ms; then the two outputs are crossfaded over 20 ms. For ordinary bands the change is heard
  10 to 30 ms after the click; for a 60 - 70 Hz band after about 150 ms. Measured with a tone at the band centre,
  worst level error after the click: 0.00 dB for bands from 20 - 21 Hz upwards, compressing or with +24 dB makeup,
  and 0.2 to 0.4 dB for a 10 - 10.3 Hz band, whose filter rings for seconds (before the carry-over: up to 17 dB
  for such bands, and 6 to 8 dB when a bypass was released just after the click).
- Where the engines differ (away from the band centre, between adjacent bands) the level moves from one engine's
  to the other's during the crossfade and no further.
- Automatic gain: with it on, both engines settle at the loudness of the dry signal. The new engine inherits the
  running engine's correction, which is wrong for its own output by however much the two differ in level, so the
  two outputs are measured side by side during the unheard run (which is then at least 100 ms) and the new
  engine's correction is set for its own output before it is faded in. Measured where the engines differ by
  3.3 dB: level error at the click within 0.01 dB for tones and 0.2 dB for noise (before the match: 3.2 dB,
  taking 2 to 4 s to go away, as a boost in one direction). Automatic gain by its nature removes the level
  difference between the engines: keep it on to compare them at equal loudness, off to hear the level difference.
- Host blocks of any size, also larger than the host announced, take this same path (the engines are run on
  slices of at most the prepared size). Nothing is allocated; the hand-over copies about 300 KB once.

## Editor internals

What each control does is under "Using the editor" above. These are the implementation details.

- Presets are the plugin state as readable XML (`.tlmb`) in the user's application data folder
  (`Tone Legion/Multiband/Presets`), so they pass the same validation and migration as host state: a broken file or
  one from a newer schema changes nothing. The preset name is saved with the project. Five starter presets are
  built in as data; they were set by calculation and nobody has listened to them. A/B keeps two state snapshots in
  the processor (they survive closing the editor, they are not saved with the project); loading a preset or
  switching A/B clears the undo history.
- Undo / Redo (`source/ParameterUndo.*`): one history for adding and removing bands and for knob edits, in the
  order they happened. One step is one gesture: a knob drag, a typed value, a double-click reset, a combo or switch
  click, or moving a band in the spectrum (both edges in one step). A gesture that ends where it started records
  nothing. Not recorded: host automation and a host's own parameter controls (no gesture), the plugin Bypass, and
  the gestures the plugin itself sends to the host when it delivers an edit or an undo. The gesture callbacks only
  take notes (JUCE calls them with the parameter's listener lock held); the step is committed from the message
  queue, or earlier, whenever the history is read or changed. A state load drops a gesture that was open across it.
- Spectrum display (`SpectrumView`): input (grey) and output (ice, with a soft fill) spectrum, 4096-point Hann FFT computed on the
  message thread from two lock-free FIFOs that the audio thread only fills while an editor is open (blocks are
  dropped when the editor falls behind; nothing is allocated). Each used band shows its nominal region
  (`fLo`..`fHi`) and is drawn as one object: its static curve (Makeup), a shaded dynamic area out to the Range limit
  and the live curve inside it. Drag the dot on the static curve to move the band and set its Makeup, drag the
  arrow marker at the Range limit up or down to set the dynamics (below compresses, above expands upward), drag an
  edge to stretch the band, use the mouse wheel over a band for its Width, or drag across empty space to create a
  band. The shaded curve per band and the ice combined curve are the response the selected engine applies right now, computed
  from the closed-form models the tests verify against the production engines (`dsp/A3Model.h`: A3, or the settled
  A-prime shape), with the current gain reduction, makeup and band mix. The curve is a per-channel picture: it does
  not show the stereo mode, the global mix, or the time A-prime takes to reach its settled width. The display takes
  any extra window height.
- Only parameters the engine implements have a control (the inert ones listed above have none). A test walks the
  component tree and fails on any widget that is neither bound to a registered parameter nor a documented action.
- All controls use APVTS attachments; the editor polls topology and meters on a 30 Hz message-thread timer and
  touches nothing on the audio thread. A test renders audio with and without an open editor and requires
  bit-identical output.
- Meters (`MeterRail`, top of the master strip): input, output and sidechain level in dBFS (peak bar with max-hold between reads,
  300 ms RMS marker, numeric peak held 2 s, amber above 0 dBFS). The sidechain shows *off* (the host has not
  enabled the bus), *silent* (300 ms below -90 dBFS) or *live*; the plugin cannot know which track feeds the bus.
  Next to them the output loudness: momentary (400 ms), short-term (3 s) and gated integrated LUFS and the
  true-peak maximum in dBTP (4x interpolation), with a reset button (the strip's **Reset**). The meter follows the commonly published
  BS.1770 / R 128 algorithm and reads the EBU Tech 3341 style reference cases in the tests within 0.1 LU; it has
  not been run against the official test files, and it is not a certified meter. Everything in the rail is
  measured on the audio thread only while an editor is open, so "integrated" covers the time an editor was open;
  when an editor is opened again the levels, momentary and short-term loudness start afresh, while the integrated
  loudness and the true-peak maximum continue until they are reset.
- The window is resizable (the display takes the extra height); the size the user chose is saved with the project
  and is not changed by loading a preset or switching A/B. The window scale (75, 90, 100, 110, 125, 150 %, the
  selector next to Bypass) draws the same layout larger or smaller: every control sits in one content component
  laid out at 100 % with the scale as its transform (the editor's own transform stays the host's display scale),
  and the size limits scale with it. Every scale draws from the same Soft Glass theme tokens (`Theme.h`); the static
  faceplate is cached as an image, so the 30 Hz meter and spectrum repaints do not redraw it. The scale is saved with the project (`editorScale`, absent at 100 %) and, like
  the size, not changed by presets or A/B.
- The meters and the response curve return to "no reduction" when the host stops calling the processor, and a band
  that was hidden shows nothing stale when it comes back.
- State and the host (`source/StateGuard.h`). One mutex, never taken by the audio thread, covers everything in the
  state that is not a lock-free parameter value (the tree with the band set, window size, engine choice and
  preset name; the undo history) and the commit of changes that span several parameters (band add / remove / undo
  / redo, state and preset loads, A/B). **The host is never called while it is held.** A change is committed
  under the mutex (parameters are set without telling anyone, what the host must hear is recorded) and delivered
  after it is released, by one thread at a time, each notification carrying the value the parameter has when it is
  sent. A saved state is therefore never half of an edit, a host that saves from another thread under a lock of
  its own cannot deadlock with the plugin through this mutex (this did happen before, and is reproduced in
  `tests/RepairMeasurements.cpp`), and a host may call back into the plugin from inside a notification.
  "Never while it is held" is meant per thread: no thread calls the host while it holds the mutex itself; a
  notification queued by one thread may be sent by another that happens to be delivering.
  What the host is told: band edits exactly as before (per changed parameter a begin / value / end gesture, then
  "state changed" and the latency). A state or preset load reports each parameter that really changed once; it
  used to report some unchanged ones as well, and some twice (in one recorded sequence of loads, 490 value
  notifications before and 193 now, every real change among them).
  Not guaranteed: which thread delivers when two threads change the state at the same moment, and the order of
  notifications from different changes.
  **A deadlock is still possible in one constellation**, as in any plugin built on JUCE's parameter classes: JUCE
  holds a lock on a parameter's listener list while it calls the host. If a host loads state under a lock of its
  own on one thread while a control of the plugin is moved on the message thread, and that host takes the same
  lock inside the parameter notification, the two wait for each other. The repair does not change this (it was so
  before, measured on both versions); delivering notifications on the message thread only would remove it, at the
  price of telling the host later than it is told today, which is a decision for the owner.
  Also outside the protocol: parameter changes made directly by the host or by the editor's controls, which are
  single lock-free values. Tested with threads (`tests/RepairTests.cpp`) and run under ThreadSanitizer on Linux
  (no data race reported; it does report the lock-order cycle just described). No real host has been tried.
- Not covered by automated tests: real mouse / keyboard dispatch (tests call the handlers directly), screen
  readers, HiDPI behaviour, repaint efficiency, and any host.
  `TL_SCREENSHOT_DIR=/some/dir build/tests/tl_tests "[.screenshot]"` renders the editor off-screen to PNG files.

## Notes on design choices

- Band identity is its slot; `b{N}_used` is not a parameter. Topology lives in the state tree (`<Topology>`) and the audio
  thread reads an atomic bitmask snapshot (`BandSlots::usedMaskSnapshot`).
- The APVTS is created without an `UndoManager`; band create/delete use their own undo manager with explicit
  before/after snapshots, and `ParameterUndo` adds knob edits to the same one, one action per gesture.
- Plain parameter values pass through JUCE's float-normalised representation, so values can differ from the table
  by a few ULP after a round trip (tests use relative tolerances; save-after-load is byte-stable).
- State from a newer `schemaVersion` is rejected (current state untouched); unknown nodes/parameters inside a
  supported version are preserved verbatim under `<Foreign>` and written back on save.
