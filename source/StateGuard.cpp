#include "StateGuard.h"

namespace tl
{
StateGuard::StateGuard (juce::AudioProcessor& p)
    : processor (p),
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
        pending[(size_t) index].fetch_or ((std::uint8_t) (kValue | (asGesture ? kGesture : 0)), std::memory_order_release);
}

void StateGuard::setQuietly (juce::AudioProcessorParameter& parameter, float normalisedValue, bool asGesture) noexcept
{
    parameter.setValue (normalisedValue);      // updates the value; the host is not called
    markParameter (parameter, asGesture);
}

bool StateGuard::hasPending() const noexcept
{
    if (stateChanged.load (std::memory_order_acquire) || latencyChanged.load (std::memory_order_acquire))
        return true;
    for (int i = 0; i < numPending; ++i)
        if (pending[(size_t) i].load (std::memory_order_acquire) != 0)
            return true;
    return false;
}

bool StateGuard::deliverOnce()
{
    bool any = false;
    auto& parameters = processor.getParameters();

    for (int i = 0; i < numPending; ++i)
    {
        const auto flags = pending[(size_t) i].exchange (0, std::memory_order_acq_rel);
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

    if (stateChanged.exchange (false, std::memory_order_acq_rel))
    {
        any = true;
        processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    }

    if (latencyChanged.exchange (false, std::memory_order_acq_rel))
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
        if (delivering.exchange (true, std::memory_order_acq_rel))
            return;

        while (deliverOnce()) {}

        delivering.store (false, std::memory_order_release);

        // Something recorded between the last pass and the release above would otherwise be
        // left behind: look again, and try to become the delivering thread again if so.
        if (! hasPending())
            return;
    }
}
} // namespace tl
