#include "ParameterUndo.h"

#include <algorithm>
#include <utility>

namespace tl
{
namespace
{
/** One recorded gesture. It is recorded after the user made it, so the first perform() (from
    UndoManager::perform) changes nothing; later ones (redo) set the values again. */
class ParameterAction final : public juce::UndoableAction
{
public:
    ParameterAction (juce::AudioProcessor& p, StateGuard& g, std::vector<ParameterUndo::Change> c)
        : processor (p), guard (g), changes (std::move (c)) {}

    bool perform() override
    {
        if (alreadyDone) { alreadyDone = false; return true; }
        apply (true);
        return true;
    }

    bool undo() override { apply (false); return true; }

    int getSizeInUnits() override { return 1 + (int) changes.size(); }

private:
    void apply (bool forward)
    {
        // One commit with the state mutex held; the host hears of it when the outermost scope
        // is released (as a gesture: it is a user edit, like the original one).
        const StateGuard::Edit edit (guard);
        auto& parameters = processor.getParameters();

        for (auto& c : changes)
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameters[c.index]))
                guard.setQuietly (*ranged, forward ? c.after : c.before, true);

        guard.markLatency();     // a lookahead, the constant-latency switch or the oversampling may have changed
    }

    juce::AudioProcessor& processor;
    StateGuard& guard;
    std::vector<ParameterUndo::Change> changes;
    bool alreadyDone = true;
};

juce::String describe (juce::AudioProcessor& processor, const std::vector<ParameterUndo::Change>& changes)
{
    if (changes.size() == 1)
        if (auto* p = processor.getParameters()[changes.front().index])
            return "Change " + p->getName (64);
    return "Change " + juce::String ((int) changes.size()) + " controls";
}
} // namespace

ParameterUndo::ParameterUndo (juce::AudioProcessor& p, juce::UndoManager& u, StateGuard& g, const juce::StringArray& excludedIds)
    : processor (p), undo (u), guard (g)
{
    auto& parameters = processor.getParameters();
    watched.assign ((size_t) parameters.size(), nullptr);
    open.assign ((size_t) parameters.size(), false);
    pending.reserve (8);

    for (auto* parameter : parameters)
    {
        const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*> (parameter);
        if (hosted != nullptr && excludedIds.contains (hosted->getParameterID()))
            continue;

        const int index = parameter->getParameterIndex();
        if (index < 0 || index >= (int) watched.size()) continue;
        watched[(size_t) index] = parameter;
        parameter->addListener (this);
    }
}

ParameterUndo::~ParameterUndo()
{
    cancelPendingUpdate();
    for (auto* parameter : watched)
        if (parameter != nullptr)
            parameter->removeListener (this);
}

bool ParameterUndo::isRecorded (int index) const noexcept
{
    return index >= 0 && index < (int) watched.size() && watched[(size_t) index] != nullptr;
}

void ParameterUndo::parameterGestureChanged (int index, bool starting)
{
    // Called with the parameter's listener lock held: note it, nothing else (no state mutex).
    if (! isRecorded (index)
        || ! juce::MessageManager::existsAndIsCurrentThread()
        || guard.isDeliveringOnCurrentThread())
        return;

    auto* parameter = watched[(size_t) index];
    const auto entry = std::find_if (pending.begin(), pending.end(), [index] (const Change& c) { return c.index == index; });

    if (starting)
    {
        if (open[(size_t) index]) return;                         // already open (nested begin)

        if (openCount == 0)
        {
            finishPending();                                      // the previous gesture is a step of its own
            pendingEpoch = epoch.load (std::memory_order_acquire);
        }

        open[(size_t) index] = true;
        ++openCount;
        if (entry == pending.end())
            pending.push_back ({ index, parameter->getValue(), parameter->getValue() });
    }
    else
    {
        if (! open[(size_t) index]) return;                       // an end without a begin we saw

        open[(size_t) index] = false;
        --openCount;
        if (entry != pending.end())
            entry->after = parameter->getValue();

        if (openCount == 0)
        {
            finishPending();
            triggerAsyncUpdate();
        }
    }
}

void ParameterUndo::finishPending()
{
    if (pending.empty()) return;

    Step step;
    step.epoch = pendingEpoch;
    for (auto& c : pending)
        if (! juce::exactlyEqual (c.before, c.after))   // unchanged means exactly the same value
            step.changes.push_back (c);
    pending.clear();

    if (! step.changes.empty())
        finished.push_back (std::move (step));
}

void ParameterUndo::flush()
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        handleUpdateNowIfNeeded();
}

void ParameterUndo::commit()
{
    if (finished.empty()) return;

    auto steps = std::move (finished);
    finished.clear();

    // The history is state that the mutex guards (a host thread may be loading and clearing it).
    // A step from before the latest load is dropped: the load cleared the history it belonged to.
    const StateGuard::Lock lock (guard);
    const auto current = epoch.load (std::memory_order_acquire);

    for (auto& step : steps)
    {
        if (step.epoch != current) continue;
        undo.beginNewTransaction (describe (processor, step.changes));
        undo.perform (new ParameterAction (processor, guard, std::move (step.changes)));
    }
}
} // namespace tl
