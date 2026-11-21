#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
#include <string_view>
#include <vector>

// =============================================================================
//  PROVISIONAL / NOT FROZEN.
//  This is the ENTIRE parameter schema (one data table in ParameterSchema.cpp).
//  It is under independent identity review and may still change. Once frozen,
//  parameter IDs, ranges, choice orders and versionHint must never be edited
//  (only appended, with a higher versionHint). The golden manifest
//  tests/golden/parameter_ids.txt pins the whole table.
// =============================================================================
namespace tl::schema
{
inline constexpr int kActiveSlots   = 8;    // slots that exist as parameters today
inline constexpr int kTopologySlots = 16;   // topology / scRef are 16-slot ready
inline constexpr int kVersionHint   = 1;    // every parameter of schema v1

enum class Kind { Float, Bool, Choice, Int };
enum class Scale { Linear, Log, Skew };     // Skew: setSkewForCentre (ParamSpec::centre)

// scMode choice indices (b{N}_scMode). Appending is safe; reordering is not.
inline constexpr int scModeOwn = 0, scModeBand = 1, scModeCustom = 2;

struct ParamSpec
{
    std::string id;           // e.g. "b3_thresh", "inGain"
    std::string name;         // display name, e.g. "B3 Threshold"
    int slot = 0;             // 1..kActiveSlots for band parameters, 0 for global
    Kind kind = Kind::Float;
    float minValue = 0.0f, maxValue = 1.0f, defaultValue = 0.0f;
    Scale scale = Scale::Linear;
    float centre = 0.0f;      // only for Scale::Skew
    float interval = 0.0f;    // Scale::Linear snapping interval, 0 = continuous
    std::string label;        // unit
    std::vector<std::string> choices;   // Kind::Choice
    bool automatable = true;
};

// ---- Identifier helpers -----------------------------------------------------
namespace band   // per-band suffixes: id = "b{slot}_" + suffix
{
    inline constexpr const char* bypass = "bypass";     inline constexpr const char* fLo = "fLo";
    inline constexpr const char* fHi = "fHi";           inline constexpr const char* thresh = "thresh";
    inline constexpr const char* ratio = "ratio";       inline constexpr const char* knee = "knee";
    inline constexpr const char* attack = "attack";     inline constexpr const char* release = "release";
    inline constexpr const char* range = "range";       inline constexpr const char* makeup = "makeup";
    inline constexpr const char* mix = "mix";           inline constexpr const char* detMode = "detMode";
    inline constexpr const char* detBlend = "detBlend"; inline constexpr const char* rmsTime = "rmsTime";
    inline constexpr const char* stereoMode = "stereoMode"; inline constexpr const char* link = "link";
    inline constexpr const char* scSource = "scSource"; inline constexpr const char* scMode = "scMode";
    inline constexpr const char* scRef = "scRef";       inline constexpr const char* scLo = "scLo";
    inline constexpr const char* scHi = "scHi";         inline constexpr const char* scChan = "scChan";
    inline constexpr const char* lookahead = "lookahead"; inline constexpr const char* adaptRel = "adaptRel";
    inline constexpr const char* freqRel = "freqRel";   inline constexpr const char* transient = "transient";
    inline constexpr const char* character = "character";
}

namespace global
{
    inline constexpr const char* bypass = "bypass";
}

std::string bandParamId (int slot, std::string_view suffix);

/** Slot default crossover ranges (Hz), slot 1..8. */
float slotDefaultLo (int slot) noexcept;
float slotDefaultHi (int slot) noexcept;

/** Every parameter, in registration order (bands 1..8, then global). Built once. */
const std::vector<ParamSpec>& allParams();
const ParamSpec* findSpec (std::string_view id);
int bandParamsPerSlot();

/** Converts plain <-> normalised for a spec (same mapping the JUCE parameter uses). */
float clampToSpec (const ParamSpec&, float plain) noexcept;

juce::NormalisableRange<float> makeRange (const ParamSpec&);

/** Builds the APVTS layout from the table: one group per band plus "Global". */
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
} // namespace tl::schema
