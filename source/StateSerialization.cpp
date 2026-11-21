#include "StateSerialization.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "ParameterSchema.h"
#include "PluginIdentity.h"

namespace tl::state
{
namespace
{
const juce::Identifier kParam { "PARAM" }, kNonParam { "NonParam" }, kTopology { "Topology" }, kForeign { "Foreign" };

juce::String fmt (float v)
{
    char buf[48];
    std::snprintf (buf, sizeof buf, "%.9g", (double) v);
    return buf;
}

bool parseFloat (const juce::String& text, float& out)
{
    auto utf8 = text.trim().toStdString();
    if (utf8.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod (utf8.c_str(), &end);
    if (end == nullptr || *end != '\0' || ! std::isfinite (d)) return false;
    out = (float) d;
    return std::isfinite (out);
}

bool isDigits (const juce::String& s)
{
    return s.isNotEmpty() && s.length() <= 9 && s.containsOnly ("0123456789");
}
} // namespace

std::optional<Parsed> parse (const void* data, size_t size, const juce::Identifier& rootType, juce::String& error)
{
    if (data == nullptr || size == 0 || size > (64u << 20)) { error = "empty or oversized blob"; return std::nullopt; }

    auto xml = juce::AudioProcessor::getXmlFromBinary (data, (int) size);
    if (xml == nullptr) { error = "not a valid state blob"; return std::nullopt; }
    if (! xml->hasTagName (rootType.toString())) { error = "unexpected root element"; return std::nullopt; }

    Parsed p;

    if (xml->hasAttribute ("schemaVersion"))
    {
        auto t = xml->getStringAttribute ("schemaVersion").trim();
        if (! isDigits (t)) { error = "malformed schemaVersion"; return std::nullopt; }
        p.schemaVersion = t.getIntValue();
    }

    p.pluginVersion = xml->getStringAttribute ("pluginVersion");

    for (auto* child : xml->getChildIterator())
    {
        if (child->hasTagName (kParam.toString()))
        {
            ParsedParam pp;
            pp.id = child->getStringAttribute ("id").toStdString();
            pp.finite = parseFloat (child->getStringAttribute ("value"), pp.value);
            pp.xml = std::make_shared<juce::XmlElement> (*child);
            if (pp.id.empty()) continue;   // nameless node: noise
            p.params.push_back (std::move (pp));
        }
        else if (child->hasTagName (kNonParam.toString()))
        {
            if (! p.nonParam.isValid())
                p.nonParam = juce::ValueTree::fromXml (*child);
        }
        else if (child->hasTagName (kTopology.toString()))
        {
            Topology t;
            auto tree = juce::ValueTree::fromXml (*child);
            if (! p.topology.has_value() && topologyFromTree (tree, t))
                p.topology = t;
            else if (! p.topology.has_value() && (int) tree.getProperty ("schemaVersion", 0) != kTopologyVersion)
                p.foreign.appendChild (tree, nullptr);   // newer/other-version topology: keep verbatim
        }
        else if (! child->hasTagName (kForeign.toString()))
        {
            p.foreign.appendChild (juce::ValueTree::fromXml (*child), nullptr);
        }
        else   // previously saved <Foreign>: flatten its children back
        {
            for (auto* f : child->getChildIterator())
                p.foreign.appendChild (juce::ValueTree::fromXml (*f), nullptr);
        }
    }

    return p;
}

bool validate (const Parsed& p, juce::String& error)
{
    if (p.schemaVersion < 0) { error = "negative schemaVersion"; return false; }
    if (p.schemaVersion > currentSchemaVersion)
    {
        error = "state was written by a newer schema (" + juce::String (p.schemaVersion) + ")";
        return false;
    }
    return true;
}

void migrateV0toV1 (Parsed& p)
{
    if (! p.topology.has_value())
    {
        Topology t;
        for (auto& pp : p.params)
            for (int s = 1; s <= schema::kActiveSlots; ++s)
                if (pp.id == schema::bandParamId (s, "used") && pp.finite && pp.value >= 0.5f)
                {
                    t.used[(size_t) s - 1] = t.everUsed[(size_t) s - 1] = true;
                    t.lastUse[(size_t) s - 1] = ++t.counter;
                }
        p.topology = t;
    }

    // Consume legacy `used` params so they do not end up in Foreign.
    std::erase_if (p.params, [] (const ParsedParam& pp)
    {
        for (int s = 1; s <= schema::kActiveSlots; ++s)
            if (pp.id == schema::bandParamId (s, "used")) return true;
        return false;
    });
}

void migrate (Parsed& p)
{
    using Step = void (*) (Parsed&);
    static constexpr Step steps[] = { &migrateV0toV1 };   // steps[v] migrates v -> v+1

    while (p.schemaVersion < currentSchemaVersion)
    {
        steps[p.schemaVersion] (p);
        ++p.schemaVersion;
    }
}

juce::ValueTree normalise (const Parsed& p, const juce::Identifier& rootType)
{
    auto& specs = schema::allParams();
    std::map<std::string, float> values;      // known ids only; first occurrence wins
    juce::ValueTree foreign = p.foreign.createCopy();

    for (auto& pp : p.params)
    {
        if (auto* spec = schema::findSpec (pp.id))
        {
            if (! values.count (pp.id))
                values[pp.id] = pp.finite ? schema::clampToSpec (*spec, pp.value) : spec->defaultValue;
        }
        else if (pp.xml != nullptr)
            foreign.appendChild (juce::ValueTree::fromXml (*pp.xml), nullptr);
    }

    auto valueOf = [&] (const schema::ParamSpec& s)
    {
        auto it = values.find (s.id);
        return it != values.end() ? it->second : s.defaultValue;   // missing -> default
    };

    Topology topo = p.topology.value_or (Topology {});

    // Band references to unused slots become Custom snapshots of the referenced slot's stored range.
    for (int k = 1; k <= schema::kActiveSlots; ++k)
    {
        auto* mode = schema::findSpec (schema::bandParamId (k, schema::band::scMode));
        auto* ref  = schema::findSpec (schema::bandParamId (k, schema::band::scRef));
        if ((int) valueOf (*mode) != schema::scModeBand) continue;

        const int r = (int) valueOf (*ref);
        const bool live = r >= 1 && r <= schema::kActiveSlots && topo.used[(size_t) r - 1];
        if (live) continue;

        const int src = (r >= 1 && r <= schema::kActiveSlots) ? r : k;
        values[mode->id] = (float) schema::scModeCustom;
        values[schema::bandParamId (k, schema::band::scLo)] = valueOf (*schema::findSpec (schema::bandParamId (src, schema::band::fLo)));
        values[schema::bandParamId (k, schema::band::scHi)] = valueOf (*schema::findSpec (schema::bandParamId (src, schema::band::fHi)));
    }

    juce::ValueTree root (rootType);
    root.setProperty ("schemaVersion", currentSchemaVersion, nullptr);
    root.setProperty ("pluginVersion", juce::String (identity::versionString.data(), identity::versionString.size()), nullptr);

    for (auto& s : specs)
    {
        juce::ValueTree v (kParam);
        v.setProperty ("id", juce::String (s.id), nullptr);
        v.setProperty ("value", valueOf (s), nullptr);
        root.appendChild (v, nullptr);
    }

    root.appendChild (topologyToTree (topo), nullptr);
    root.appendChild (p.nonParam.isValid() ? p.nonParam.createCopy() : juce::ValueTree (kNonParam), nullptr);

    if (foreign.getNumChildren() > 0)
        root.appendChild (foreign, nullptr);

    return root;
}

juce::MemoryBlock save (juce::AudioProcessorValueTreeState& apvts)
{
    juce::XmlElement root (apvts.state.getType().toString());
    root.setAttribute ("schemaVersion", currentSchemaVersion);
    root.setAttribute ("pluginVersion", juce::String (identity::versionString.data(), identity::versionString.size()));

    for (auto& s : schema::allParams())
    {
        auto* e = root.createNewChildElement (kParam.toString());
        e->setAttribute ("id", juce::String (s.id));
        auto* raw = apvts.getRawParameterValue (s.id);
        e->setAttribute ("value", fmt (raw != nullptr ? raw->load() : s.defaultValue));
    }

    Topology topo;
    topologyFromTree (apvts.state.getChildWithName (kTopology), topo);
    if (auto x = topologyToTree (topo).createXml()) root.addChildElement (x.release());

    auto np = apvts.state.getChildWithName (kNonParam);
    if (auto x = (np.isValid() ? np : juce::ValueTree (kNonParam)).createXml()) root.addChildElement (x.release());

    auto foreign = apvts.state.getChildWithName (kForeign);
    if (foreign.isValid())
        for (auto child : foreign)
            if (auto x = child.createXml()) root.addChildElement (x.release());   // written back at root level

    juce::MemoryBlock mb;
    juce::AudioProcessor::copyXmlToBinary (root, mb);
    return mb;
}

LoadResult load (juce::AudioProcessorValueTreeState& apvts, const void* data, size_t size)
{
    LoadResult r;
    try
    {
        auto parsed = parse (data, size, apvts.state.getType(), r.error);
        if (! parsed.has_value()) return r;

        r.fromVersion = parsed->schemaVersion;
        if (! validate (*parsed, r.error)) return r;

        migrate (*parsed);
        auto tree = normalise (*parsed, apvts.state.getType());

        // Foreign nodes are stored as a <Foreign> child inside the state; they are
        // written back at root level on save.
        apvts.replaceState (tree);
        r.ok = true;
    }
    catch (...)
    {
        r.ok = false;
        r.error = "exception while loading state";
    }
    return r;
}
} // namespace tl::state
