#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "StateGuard.h"

// Undo for knob edits: every parameter change the user makes in the editor becomes one step in
// the same history as adding and removing bands (the processor's UndoManager), so Undo / Redo
// walk back through both in the order they happened.
//
// What counts as one step: one gesture. A gesture starts when the first parameter begins a
// change gesture (a knob drag, a typed value, a double-click reset, a combo or toggle click,
// dragging a band in the spectrum, which moves two edges together) and ends when the last open
// one ends. Parameters whose value is the same at the end as at the start are left out; a
// gesture that changed nothing records nothing.
//
// What is not recorded:
//   - changes without a gesture (host automation, a host's own parameter controls, state and
//     preset loads),
//   - the gestures the plugin itself sends to the host when it delivers an edit or an undo
//     (StateGuard::deliverPending): an undo must not record itself,
//   - anything that does not happen on the message thread,
//   - the parameters named as excluded (the host bypass parameter).
//
// Threads and locks. The gesture callbacks come from JUCE with the parameter's listener lock
// held, so they only note what happened (message thread data, no state mutex) and the step is
// committed afterwards, from the message queue or from flush(). flush() is called before the
// history is read or changed (BandSlots does it), so a step is always in its place when Undo is
// pressed. A state load (any thread) calls invalidate(): a gesture that was open across a load
// is dropped, as the load clears the history.
//
// Undoing / redoing a step sets the values quietly under the state mutex and tells the host
// afterwards as a begin / value / end gesture, exactly like the band edits (see StateGuard).
namespace tl
{
class ParameterUndo final : private juce::AudioProcessorParameter::Listener,
                            private juce::AsyncUpdater
{
public:
    ParameterUndo (juce::AudioProcessor&, juce::UndoManager&, StateGuard&, const juce::StringArray& excludedIds);
    ~ParameterUndo() override;

    /** Message thread: commits a finished gesture now (no-op elsewhere or while one is open). */
    void flush();

    /** Any thread: forget the gesture in progress (a state load replaced everything). */
    void invalidate() noexcept { epoch.fetch_add (1, std::memory_order_acq_rel); }

    /** True while a gesture is open (message thread). */
    bool isGestureOpen() const noexcept { return openCount > 0; }

    /** Whether edits of this parameter are recorded. */
    bool isRecorded (int parameterIndex) const noexcept;

    struct Change { int index = -1; float before = 0.0f, after = 0.0f; };   // normalised values

private:
    void parameterValueChanged (int, float) override {}          // may come from the audio thread: nothing to do
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;
    void handleAsyncUpdate() override { commit(); }
    void commit();
    void finishPending();

    juce::AudioProcessor& processor;
    juce::UndoManager& undo;
    StateGuard& guard;
    std::vector<juce::AudioProcessorParameter*> watched;         // one entry per parameter index (nullptr = excluded)
    std::vector<bool> open;                                      // message thread
    std::vector<Change> pending;                                 // message thread: the gesture being collected
    struct Step { std::vector<Change> changes; std::uint32_t epoch = 0; };
    std::vector<Step> finished;                                  // message thread: gestures done, not committed yet (in order)
    int openCount = 0;
    std::uint32_t pendingEpoch = 0;
    std::atomic<std::uint32_t> epoch { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterUndo)
};
} // namespace tl
