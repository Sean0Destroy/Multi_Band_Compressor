#include "StateGuard.h"

#include "StateSerialization.h"

namespace tl
{
StateGuard::StateGuard (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& parameters)
    : processor (p),
      apvts (parameters),
      numPending (p.getParameters().size()),
      pending (std::make_unique<std::atomic<std::uint8_t>[]> ((size_t) juce::jmax (1, p.getParameters().size())))
{
    jassert (numPending > 0);   // construct after the parameters
    for (int i = 0; i < numPending; ++i)
        pending[(size_t) i].store (0, std::memory_order_relaxed);
}

void StateGuard::enter() const noexcept
{
    mutex.enter();
    if (depth++ == 0)
        owner.store (juce::Thread::getCurrentThreadId(), std::memory_order_release);
}

void StateGuard::exit() const noexcept
{
    if (--depth == 0)
        owner.store (nullptr, std::memory_order_release);
    mutex.exit();
}

void StateGuard::markParameter (const juce::AudioProcessorParameter& parameter, bool asGesture) noexcept
{
    const int index = parameter.getParameterIndex();
    if (index >= 0 && index < numPending)
        pending[(size_t) index].fetch_or ((std::uint8_t) (kValue | (asGesture ? kGesture : 0)));      // seq_cst, see deliverPending
}

void StateGuard::setQuietly (juce::RangedAudioParameter& parameter, float normalisedValue, bool asGesture) noexcept
{
    // The parameter object itself; neither the host nor any listener is called by this.
    static_cast<juce::AudioProcessorParameter&> (parameter).setValue (normalisedValue);

    // The parameter tree keeps its own copy of the plain value, which is what the audio thread
    // and the state save read. It normally follows through a parameter listener, i.e. only when
    // the host is told; here it has to be whole as soon as the commit is, so it is set directly
    // to exactly what the parameter now holds.
    if (auto* raw = apvts.getRawParameterValue (parameter.getParameterID()))
        raw->store (state::plainValueOf (parameter), std::memory_order_relaxed);

    markParameter (parameter, asGesture);
}

bool StateGuard::hasPending() const noexcept
{
    if (stateChanged.load() || latencyChanged.load())
        return true;
    for (int i = 0; i < numPending; ++i)
        if (pending[(size_t) i].load() != 0)
            return true;
    return false;
}

bool StateGuard::deliverOnce()
{
    bool any = false;
    auto& parameters = processor.getParameters();

    for (int i = 0; i < numPending; ++i)
    {
        const auto flags = pending[(size_t) i].exchange (0);
        if (flags == 0) continue;
        any = true;

        auto* parameter = parameters[i];
        if (parameter == nullptr) continue;

        // The value it has NOW: a later commit may have changed it again, and then this is
        // already the newer value (and that commit's own notification repeats it).
        if ((flags & kGesture) != 0) parameter->beginChangeGesture();
        parameter->sendValueChangedMessageToListeners (parameter->getValue());
        if ((flags & kGesture) != 0) parameter->endChangeGesture();
    }

    if (stateChanged.exchange (false))
    {
        any = true;
        processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    }

    if (latencyChanged.exchange (false))
    {
        any = true;
        if (onLatencyChanged) onLatencyChanged();
    }

    return any;
}

void StateGuard::deliverPending()
{
    // Never with the mutex held: that is the whole point. An inner scope leaves it to the
    // outermost Edit of this thread.
    if (isHeldByCurrentThread())
        return;

    for (;;)
    {
        // One thread delivers at a time. If another one is at it (or this thread is, further up
        // the stack: a host called back into the plugin from inside a notification), it will
        // pick up what was just recorded; do not wait for it.
        if (delivering.exchange (true))
            return;

        while (deliverOnce()) {}

        // Giving up the role and looking once more, against a thread that records something and
        // then looks whether somebody is delivering: each side writes one flag and then reads
        // the other's. That only works if neither can read before its own write is visible to
        // the other, i.e. with sequentially consistent operations on both flags (with weaker
        // orders the write may still sit in a store buffer: measured, 10 notifications left
        // behind in 9 million simultaneous commits).
        delivering.store (false);

        if (! hasPending())
            return;
    }
}
} // namespace tl
