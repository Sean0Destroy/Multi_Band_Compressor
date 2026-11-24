#include "BandSlots.h"

#include <algorithm>
#include <utility>

namespace tl
{
namespace
{
const juce::Identifier kTopologyId { "Topology" };

juce::String flagsToString (const std::array<bool, schema::kTopologySlots>& f)
{
    juce::String s;
    for (bool b : f) s << (b ? '1' : '0');
    return s;
}

bool flagsFromString (const juce::String& s, std::array<bool, schema::kTopologySlots>& f)
{
    if (s.length() != schema::kTopologySlots) return false;
    for (int i = 0; i < schema::kTopologySlots; ++i)
    {
        auto c = s[i];
        if (c != '0' && c != '1') return false;
        f[(size_t) i] = c == '1';
    }
    return true;
}
} // namespace

std::uint32_t usedMask (const Topology& t) noexcept
{
    std::uint32_t m = 0;
    for (int i = 0; i < schema::kTopologySlots; ++i)
        if (t.used[(size_t) i]) m |= (1u << i);
    return m;
}

juce::ValueTree topologyToTree (const Topology& t)
{
    juce::ValueTree v (kTopologyId);
    v.setProperty ("schemaVersion", kTopologyVersion, nullptr);
    v.setProperty ("used", flagsToString (t.used), nullptr);
    v.setProperty ("everUsed", flagsToString (t.everUsed), nullptr);

    juce::StringArray lu;
    for (auto x : t.lastUse) lu.add (juce::String ((juce::int64) x));
    v.setProperty ("lastUse", lu.joinIntoString (" "), nullptr);
    v.setProperty ("counter", (juce::int64) t.counter, nullptr);
    return v;
}

bool topologyFromTree (const juce::ValueTree& v, Topology& out)
{
    Topology t;

    if (! v.isValid() || ! v.hasType (kTopologyId)
        || (int) v.getProperty ("schemaVersion", 0) != kTopologyVersion
        || ! flagsFromString (v.getProperty ("used").toString(), t.used)
        || ! flagsFromString (v.getProperty ("everUsed").toString(), t.everUsed))
        return false;

    auto lu = juce::StringArray::fromTokens (v.getProperty ("lastUse").toString(), " ", "");
    lu.removeEmptyStrings();
    if (lu.size() != schema::kTopologySlots) return false;

    for (int i = 0; i < schema::kTopologySlots; ++i)
        t.lastUse[(size_t) i] = (std::uint32_t) lu[i].getLargeIntValue();

    t.counter = (std::uint32_t) (juce::int64) v.getProperty ("counter", 0);

    for (int i = 0; i < schema::kTopologySlots; ++i)   // used implies everUsed
        if (t.used[(size_t) i]) t.everUsed[(size_t) i] = true;

    out = t;
    return true;
}

int pickSlotForCreate (const Topology& t) noexcept
{
    for (int s = 1; s <= schema::kActiveSlots; ++s)
        if (! t.everUsed[(size_t) s - 1] && ! t.used[(size_t) s - 1])
            return s;

    int best = 0;
    for (int s = 1; s <= schema::kActiveSlots; ++s)
    {
        if (t.used[(size_t) s - 1]) continue;
        if (best == 0 || t.lastUse[(size_t) s - 1] < t.lastUse[(size_t) best - 1])
            best = s;
    }
    return best;
}

//==============================================================================
struct Change { std::string id; float before = 0, after = 0; };

class SlotAction final : public juce::UndoableAction
{
public:
    SlotAction (BandSlots& o, std::vector<Change> c, Topology b, Topology a)
        : owner (o), changes (std::move (c)), before (b), after (a) {}

    bool perform() override { apply (true);  return true; }
    bool undo() override    { apply (false); return true; }

private:
    void apply (bool forward)
    {
        // One commit: every parameter of the edit and the topology, with the state mutex held.
        // The host is told when the outermost scope is released (this one, if the undo manager
        // was used directly).
        const StateGuard::Edit edit (owner.guard);

        for (auto& c : changes)
            owner.setPlain (c.id, forward ? c.after : c.before);

        owner.write (forward ? after : before);
        owner.guard.markStateChanged();
        owner.guard.markLatency();          // the largest lookahead of the used bands may have changed
        if (owner.onTopologyCommitted) owner.onTopologyCommitted();
    }

    BandSlots& owner;
    std::vector<Change> changes;
    Topology before, after;
};

//==============================================================================
BandSlots::BandSlots (juce::AudioProcessorValueTreeState& a, juce::UndoManager& u, StateGuard& g) : apvts (a), undo (u), guard (g)
{
    ensureNode();
    refresh();
}

void BandSlots::ensureNode()
{
    const StateGuard::Lock sl (guard);
    if (! apvts.state.getChildWithName (kTopologyId).isValid())
        apvts.state.appendChild (topologyToTree ({}), nullptr);
}

juce::ValueTree BandSlots::node() const { return apvts.state.getChildWithName (kTopologyId); }

Topology BandSlots::topology() const
{
    const StateGuard::Lock sl (guard);
    Topology t;
    topologyFromTree (node(), t);
    return t;
}

void BandSlots::write (const Topology& t)
{
    auto n = node();
    if (! n.isValid())
    {
        apvts.state.appendChild (topologyToTree (t), nullptr);
    }
    else
    {
        auto fresh = topologyToTree (t);
        n.copyPropertiesFrom (fresh, nullptr);
    }

    published.store (usedMask (t) & ((1u << schema::kActiveSlots) - 1u), std::memory_order_release);
}

void BandSlots::refresh()
{
    const StateGuard::Lock sl (guard);
    ensureNode();
    published.store (usedMask (topology()) & ((1u << schema::kActiveSlots) - 1u), std::memory_order_release);
}

std::vector<int> BandSlots::usedSlots() const
{
    std::vector<int> r;
    auto t = topology();
    for (int s = 1; s <= schema::kActiveSlots; ++s)
        if (t.used[(size_t) s - 1]) r.push_back (s);
    return r;
}

bool BandSlots::isUsed (int slot) const
{
    return slot >= 1 && slot <= schema::kActiveSlots && topology().used[(size_t) slot - 1];
}

float BandSlots::plain (const std::string& id) const
{
    if (auto* p = apvts.getRawParameterValue (id)) return p->load();
    jassertfalse;
    return 0.0f;
}

void BandSlots::setPlain (const std::string& id, float value)
{
    if (auto* p = apvts.getParameter (id))
    {
        // Set now, without the host; the host is told (as a begin / value / end gesture) once
        // the state mutex is released.
        guard.setQuietly (*p, p->convertTo0to1 (value), true);
    }
}

bool BandSlots::undoLast()       { settleHistory(); const StateGuard::Edit edit (guard); return undo.undo(); }
bool BandSlots::redoLast()       { settleHistory(); const StateGuard::Edit edit (guard); return undo.redo(); }
bool BandSlots::canUndo() const  { settleHistory(); const StateGuard::Lock sl (guard); return undo.canUndo(); }
bool BandSlots::canRedo() const  { settleHistory(); const StateGuard::Lock sl (guard); return undo.canRedo(); }

void BandSlots::settleHistory() const
{
    if (beforeHistoryAccess != nullptr && ! guard.isHeldByCurrentThread())
        beforeHistoryAccess();
}
void BandSlots::clearHistory()   { const StateGuard::Lock sl (guard); undo.clearUndoHistory(); }

int BandSlots::createBand()
{
    settleHistory();
    const StateGuard::Edit edit (guard);
    const auto before = topology();
    const auto slot = pickSlotForCreate (before);
    if (slot == 0) return 0;

    std::vector<Change> changes;
    for (auto& spec : schema::allParams())
        if (spec.slot == slot)
        {
            const auto cur = plain (spec.id);
            if (! juce::approximatelyEqual (cur, spec.defaultValue))
                changes.push_back ({ spec.id, cur, spec.defaultValue });
        }

    auto after = before;
    after.used[(size_t) slot - 1] = true;
    after.everUsed[(size_t) slot - 1] = true;
    after.lastUse[(size_t) slot - 1] = ++after.counter;

    undo.beginNewTransaction ("Create band");
    undo.perform (new SlotAction (*this, std::move (changes), before, after));
    return slot;
}

bool BandSlots::deleteBand (int slot)
{
    settleHistory();
    const StateGuard::Edit edit (guard);
    const auto before = topology();
    if (slot < 1 || slot > schema::kActiveSlots || ! before.used[(size_t) slot - 1]) return false;

    std::vector<Change> changes;
    const auto fLo = plain (schema::bandParamId (slot, schema::band::fLo));
    const auto fHi = plain (schema::bandParamId (slot, schema::band::fHi));

    for (int k = 1; k <= schema::kActiveSlots; ++k)
    {
        if (k == slot) continue;

        const auto modeId = schema::bandParamId (k, schema::band::scMode);
        const auto refId  = schema::bandParamId (k, schema::band::scRef);

        if ((int) plain (modeId) == schema::scModeBand && (int) plain (refId) == slot)
        {
            changes.push_back ({ modeId, plain (modeId), (float) schema::scModeCustom });
            const auto loId = schema::bandParamId (k, schema::band::scLo);
            const auto hiId = schema::bandParamId (k, schema::band::scHi);
            changes.push_back ({ loId, plain (loId), fLo });
            changes.push_back ({ hiId, plain (hiId), fHi });
        }
    }

    auto after = before;
    after.used[(size_t) slot - 1] = false;
    after.lastUse[(size_t) slot - 1] = ++after.counter;

    undo.beginNewTransaction ("Delete band");
    undo.perform (new SlotAction (*this, std::move (changes), before, after));
    return true;
}
} // namespace tl
