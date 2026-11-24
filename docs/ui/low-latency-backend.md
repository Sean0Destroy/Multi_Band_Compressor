# Low Latency mode: backend work still to do

The Soft Glass header has one **Low Latency** button where the latency read-out and the
*Fixed* switch used to be. Today it is the look only: it lights when pressed, but it is not
bound to a parameter, changes nothing in the audio and is not saved. This note is the hand-off
for the engine work that makes it real. Nothing here is built.

## What Sean asked for

One press turns off lookahead and oversampling, so the plugin adds no delay (0 ms).

## Where latency comes from today

`PluginProcessor::refreshLatency()` reports
`engine latency + dsp::processingLatencySamples (settings, fs)`, which is

- the largest band `lookahead` (0 to 10 ms), or the full 10 ms while `constantLatency` is on;
- the colour stage's `oversampling` filters (Off / 2x / 4x / 8x);
- the clipper's own `clipOversampling` filters (47 / 53 / 55 samples).

Both engines in this branch report 0 themselves. Linear phase (draft PR 11, unmerged) would add
its own latency and would need to be covered too if it lands first.

## Proposed design (needs approval before anyone builds it)

1. **A new global parameter** `lowLatency` (Bool, default off, automatable, name
   "Low Latency"). A new ID only, so no existing ID, range or default changes. Old sessions
   and presets have no value for it and load with it off, so they sound exactly as before.
   Check that the state loader fills a missing parameter with its default; if it needs a
   `schemaVersion` step, add one.
2. **An override, not a rewrite.** While it is on, the engine treats every band's lookahead as
   0, both oversampling factors as Off and `constantLatency` as off. The stored values of those
   parameters stay as they are, so switching Low Latency off brings the old settings back.
   The editor greys those controls while it is on.
3. **Latency** is then 0 samples, reported through the existing `refreshLatency()` path (message
   thread, never from the audio callback).
4. **Switching during playback** reuses the existing crossfades (lookahead taps, oversampling
   change), so no new allocation or locking in `processBlock`.
5. **Tests:** latency 0 with every source turned up and Low Latency on; settings restored when it
   goes off; an old state loads with it off and bit-identical output; automation of the new ID.

## Sound consequences to tell users

- Without lookahead the compressor reacts after a transient starts, so fast peaks pass a little.
- Without oversampling the colour stage and the clipper can alias more at high drive.
- Check whether the clipper's exact ceiling still holds at 1x (sample peaks should; inter-sample
  peaks may not).

## Decisions for Sean

- Override (recommended, above) or change the stored settings for real?
- Should Low Latency also win over a hand-set Fixed Latency, as proposed?
- Fixed Latency now has no editor control. Keep it automation-only, or give it a place back
  (for example in a settings menu)?

## Hooking up the button once the parameter exists

In `PluginEditor`, change `lowLatency` from a `juce::TextButton` to a `ParamToggle` bound to
`lowLatency`, drop the "Not connected to the audio yet" line from its tooltip, and add the ID
to `kGlobalIds` in `tests/EditorTests.cpp` (and remove `action_lowLatency` from the action list).
