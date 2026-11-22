#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "BandSlots.h"

// Plugin state: root element attributes schemaVersion + pluginVersion, one
// <PARAM id value/> per parameter (schema order), <Topology>, <NonParam>, and a
// <Foreign> child carrying unknown nodes verbatim.
//
// Load pipeline (all stages except `load` are pure functions):
//   parse -> validate -> migrate (v(n)->v(n+1) chain) -> normalise -> replaceState
// Any failure before replaceState leaves the live state untouched.
// Message thread only (load/save touch the ValueTree).
namespace tl::state
{
inline constexpr int currentSchemaVersion = 1;

struct ParsedParam
{
    std::string id;
    float value = 0.0f;
    bool finite = false;
    std::shared_ptr<const juce::XmlElement> xml;   // verbatim, used when the id is unknown
};

struct Parsed
{
    int schemaVersion = 0;                 // 0 = attribute missing (legacy)
    juce::String pluginVersion;
    std::vector<ParsedParam> params;
    std::optional<Topology> topology;
    juce::ValueTree nonParam;              // invalid if absent
    juce::ValueTree foreign { "Foreign" }; // unknown nodes, verbatim
};

struct LoadResult
{
    bool ok = false;
    int fromVersion = -1;
    juce::String error;
    /** Parameters whose value the load changed. They were set WITHOUT telling the host
        (AudioProcessorParameter::setValue); the caller tells it, after releasing whatever lock
        it holds (PluginProcessor does this through StateGuard). */
    std::vector<juce::RangedAudioParameter*> changed;
};

std::optional<Parsed> parse (const void* data, size_t size, const juce::Identifier& rootType, juce::String& error);
bool validate (const Parsed&, juce::String& error);
void migrate (Parsed&);   // runs the chain up to currentSchemaVersion
/** Fills defaults, clamps, drops unknowns to Foreign, repairs scMode=Band references to unused slots. */
juce::ValueTree normalise (const Parsed&, const juce::Identifier& rootType);

/** v0 -> v1 documented stub. v0 = no schemaVersion attribute (pre-release provisional layout).
    Only structural work is done: if there is no Topology node, derive `used` from legacy b{N}_used
    PARAM values (consumed, not kept); otherwise topology stays default (all unused). */
void migrateV0toV1 (Parsed&);

juce::MemoryBlock save (juce::AudioProcessorValueTreeState&);
LoadResult load (juce::AudioProcessorValueTreeState&, const void* data, size_t size);
} // namespace tl::state
