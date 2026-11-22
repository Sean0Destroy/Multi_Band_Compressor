#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

// One mutex for everything in the plugin state that is not a lock-free parameter value (the
// state tree with its Topology and NonParam nodes, the band undo history) and for the commit
// of changes that span several parameters (a band edit, a state or preset load), together with
// the rule for telling the host about such changes.
//
// The rule: THE HOST IS NEVER CALLED WHILE THE MUTEX IS HELD.
//
// A host may save the plugin state from a thread of its own while it holds a lock of its own,
// and take that same lock inside the notifications it receives from the plugin. If the plugin
// notified it while holding the state mutex, the two threads would wait for each other for
// ever. So a change is made in two steps:
//
//   1. Commit (mutex held). Parameter values are set quietly (AudioProcessorParameter::setValue,
//      which updates the value the audio thread reads but tells no host), the tree is edited,
//      and what the host has to be told is recorded as pending: per parameter "value changed"
//      (optionally as a begin / end gesture), "state that is not a parameter changed" and
//      "latency may have changed". Anyone who takes the mutex afterwards sees the whole change
//      or nothing of it.
//   2. Notify (mutex not held). The pending notifications are delivered by exactly one thread
//      at a time (whichever gets there first; others leave theirs to it and return, nobody
//      waits). Each one carries the value the parameter has when it is sent, not the value of
//      the commit that requested it, so a notification can never bring an old value back.
//      Whatever a host does from inside a notification (save, load, edit) is just another
//      commit; its notifications are delivered by the loop that is already running.
//
// What this guarantees: no host call under the mutex; a saved state is never half of an edit;
// when every caller has returned, the last value the host was told for a parameter is the
// value it has. What it does not guarantee: which thread delivers a notification when two
// threads change the state at the same moment, nor the order of notifications from different
// commits. Not for the audio thread.
namespace tl
{
class StateGuard
{
public:
    /** Construct after the processor's parameters exist (one pending flag per parameter). */
    explicit StateGuard (juce::AudioProcessor& processor);

    /** Holds the mutex (recursive) and nothing else: for readers, and for writes that the host
        is not told about. Nothing is delivered when it is released. */
    class Lock
    {
    public:
        explicit Lock (const StateGuard& g) noexcept : guard (g) { guard.enter(); }
        ~Lock() noexcept                                         { guard.exit(); }
    private:
        const StateGuard& guard;
        JUCE_DECLARE_NON_COPYABLE (Lock)
    };

    /** Holds the mutex (recursive). For code that changes state the host is told about: when the
        outermost scope on this thread is released, pending notifications are delivered (after
        the release). Message thread or a host's state thread; never the audio thread. */
    class Edit
    {
    public:
        explicit Edit (StateGuard& g) noexcept : guard (g) { guard.enter(); }
        ~Edit()
        {
            guard.exit();
            guard.deliverPending();
        }
    private:
        StateGuard& guard;
        JUCE_DECLARE_NON_COPYABLE (Edit)
    };

    // ---- Step 1, with the mutex held (or from any thread for the two flags) -------------------
    /** Sets a parameter without telling the host and records that the host has to be told.
        (The value the audio thread reads is updated at once, and listeners of the parameter
        tree are called at once, on this thread: they must not call the host.)
        `asGesture`: deliver it between a begin and an end gesture (an edit made by the user). */
    void setQuietly (juce::AudioProcessorParameter&, float normalisedValue, bool asGesture) noexcept;
    /** Records that the host has to be told about a parameter that was already set quietly. */
    void markParameter (const juce::AudioProcessorParameter&, bool asGesture) noexcept;
    /** State that is not a parameter changed (band set, engine, preset name ...). */
    void markStateChanged() noexcept      { stateChanged.store (true, std::memory_order_release); }
    /** The latency may have changed. */
    void markLatency() noexcept           { latencyChanged.store (true, std::memory_order_release); }

    // ---- Step 2 ---------------------------------------------------------------------------------
    /** Delivers what is pending, unless this thread holds the mutex (then the outermost Edit
        does it) or another thread is delivering (then that thread also delivers ours). */
    void deliverPending();

    /** How the latency is brought up to date (called without the mutex). */
    std::function<void()> onLatencyChanged;

    bool isHeldByCurrentThread() const noexcept { return owner.load (std::memory_order_acquire) == juce::Thread::getCurrentThreadId(); }
    bool hasPending() const noexcept;

private:
    void enter() const noexcept;
    void exit() const noexcept;
    bool deliverOnce();

    juce::AudioProcessor& processor;
    juce::CriticalSection mutex;
    mutable std::atomic<juce::Thread::ThreadID> owner { nullptr };   // the thread that holds the mutex, if any
    mutable int depth = 0;                                           // guarded by the mutex

    static constexpr std::uint8_t kValue = 1, kGesture = 2;
    const int numPending;
    const std::unique_ptr<std::atomic<std::uint8_t>[]> pending;      // one per parameter index
    std::atomic<bool> stateChanged { false }, latencyChanged { false };
    std::atomic<bool> delivering { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StateGuard)
};
} // namespace tl
