#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include "ParameterSchema.h"
#include "StateGuard.h"

// Band topology + slot lifecycle. Meant for the message thread; every method takes the state
// mutex (StateGuard), so that a host loading state from another thread (which the processor also
// does under that mutex) cannot interleave with an edit. An edit commits under the mutex and the
// host is told afterwards (see StateGuard). The audio thread never takes the mutex: it only
// reads usedMaskSnapshot(), a lock-free atomic.
// PROVISIONAL together with the parameter schema.
namespace tl
{
inline constexpr int kTopologyVersion = 1;

/** Which of the 16 topology slots are in use (only 1..schema::kActiveSlots are allocatable now). */
struct Topology
{
    std::array<bool, schema::kTopologySlots> used {};
    std::array<bool, schema::kTopologySlots> everUsed {};
    std::array<std::uint32_t, schema::kTopologySlots> lastUse {};   // value of `counter` at last create/delete
    std::uint32_t counter = 0;

    bool operator== (const Topology&) const = default;
};

std::uint32_t usedMask (const Topology&) noexcept;                       // bit (slot-1), 16 bits
juce::ValueTree topologyToTree (const Topology&);                        // <Topology schemaVersion="1" .../>
/** Tolerant reader; returns false (and leaves `out` default) if the node is malformed or of another version. */
bool topologyFromTree (const juce::ValueTree&, Topology& out);

/** Applies the slot allocation policy: never-used slot first (lowest), else least-recently-used free slot. 0 if none. */
int pickSlotForCreate (const Topology&) noexcept;

class BandSlots
{
public:
    BandSlots (juce::AudioProcessorValueTreeState&, juce::UndoManager&, StateGuard&);

    /** Allocates a slot, resets all its parameters to defaults, marks it used. One undo transaction.
        Returns the 1-based slot, or 0 if all slots are in use. */
    int createBand();

    /** Marks the slot unused; every band referencing it (scMode=Band, scRef=slot) becomes Custom with
        scLo/scHi := slot's current fLo/fHi. One undo transaction. False if the slot is not in use. */
    bool deleteBand (int slot);

    std::vector<int> usedSlots() const;
    bool isUsed (int slot) const;
    Topology topology() const;

    /** Re-reads topology from the state tree (call after replaceState) and republishes. */
    void refresh();

    /** Undo / redo of the edit history: band create and delete, and the knob edits that
        ParameterUndo records into the same UndoManager. */
    bool undoLast();
    bool redoLast();
    bool canUndo() const;
    bool canRedo() const;
    void clearHistory();

    /** Called with the state mutex held, as part of a create, delete, undo or redo, after the
        topology was written. It must not call the host (which is told after the mutex is
        released: "state that is not a parameter changed" and "latency may have changed"). */
    std::function<void()> onTopologyCommitted;

    /** Called (without the state mutex) before the history is read or changed, so that a knob
        edit that has finished but is not committed yet takes its place first (ParameterUndo::flush). */
    std::function<void()> beforeHistoryAccess;

    /** Audio-thread safe: bit (slot-1) set for used slots. */
    std::uint32_t usedMaskSnapshot() const noexcept     { return published.load (std::memory_order_acquire); }

    /** Ensures a Topology child exists in the APVTS state. */
    void ensureNode();

private:
    friend class SlotAction;

    juce::ValueTree node() const;
    void settleHistory() const;
    void write (const Topology&);
    float plain (const std::string& id) const;
    void setPlain (const std::string& id, float value);

    juce::AudioProcessorValueTreeState& apvts;
    juce::UndoManager& undo;
    StateGuard& guard;
    std::atomic<std::uint32_t> published { 0 };
    static_assert (std::atomic<std::uint32_t>::is_always_lock_free);
};
} // namespace tl
