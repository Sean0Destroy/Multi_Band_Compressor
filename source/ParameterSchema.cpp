#include "ParameterSchema.h"

#include <array>
#include <cmath>

namespace tl::schema
{
// ---- Named ranges / skews (single place for every number) -------------------
namespace rng
{
    constexpr float freqMin = 10.0f, freqMax = 30000.0f;
    constexpr float threshMin = -80.0f, threshMax = 12.0f, threshDef = -18.0f, threshInterval = 0.1f;
    constexpr float ratioMin = 1.0f, ratioMax = 100.0f, ratioDef = 2.0f, ratioCentre = 6.0f;   // 100 shows "inf"
    constexpr float kneeMin = 0.0f, kneeMax = 24.0f, kneeDef = 6.0f;
    constexpr float attackMin = 0.01f, attackMax = 300.0f, attackDef = 10.0f;     // log
    constexpr float releaseMin = 5.0f, releaseMax = 5000.0f, releaseDef = 150.0f; // log
    constexpr float rangeMin = 0.0f, rangeMax = 48.0f, rangeDef = 24.0f;
    constexpr float makeupMin = -24.0f, makeupMax = 24.0f;
    constexpr float percentMin = 0.0f, percentMax = 100.0f;
    constexpr float unitMin = 0.0f, unitMax = 1.0f, detBlendDef = 0.5f;
    constexpr float rmsTimeMin = 1.0f, rmsTimeMax = 300.0f, rmsTimeDef = 30.0f;   // log
    constexpr float lookaheadMin = 0.0f, lookaheadMax = 10.0f;
    constexpr float gainMin = -24.0f, gainMax = 24.0f;
    constexpr float clipCeilMin = -6.0f, clipCeilMax = 0.0f, clipCeilDef = -0.3f;
    constexpr int   scRefMin = 1, scRefMax = kTopologySlots, scRefDef = 1;
}

namespace choice
{
    const std::vector<std::string> detMode     { "Peak", "RMS", "Blend" };
    const std::vector<std::string> stereoMode  { "Stereo", "Left/Right", "Mid", "Side", "Mid/Side" };
    const std::vector<std::string> scSource    { "Main", "External" };
    const std::vector<std::string> scMode      { "Own", "Band", "Custom" };
    const std::vector<std::string> scChan      { "L/R", "Mid", "Side", "Max", "Sum" };
    const std::vector<std::string> quality     { "Zero Latency", "Linear Phase" };
    const std::vector<std::string> lpLength    { "Short", "Medium", "Long" };
    const std::vector<std::string> oversampling{ "Off", "2x", "4x", "8x" };
    const std::vector<std::string> overlapMode { "Stack", "Blend", "Split" };
    const std::vector<std::string> charModel   { "Clean", "Warm FET" };
    const std::vector<std::string> clipMode    { "Off", "Soft", "Hard" };
}

namespace
{
constexpr std::array<float, kActiveSlots> kSlotLo { 20.f, 120.f, 400.f, 1000.f, 2500.f, 5000.f, 8000.f, 12000.f };
constexpr std::array<float, kActiveSlots> kSlotHi { 120.f, 400.f, 1000.f, 2500.f, 5000.f, 8000.f, 12000.f, 20000.f };

enum class SlotDefault { None, Lo, Hi };

struct Row
{
    const char* idSuffix;
    const char* name;
    Kind kind;
    float minValue, maxValue, defaultValue;
    Scale scale;
    float centre;
    const char* label;
    const std::vector<std::string>* choices;
    bool automatable;
    SlotDefault slotDefault;
    float interval = 0.0f;    // optional trailing column (linear snapping), 0 = continuous
};

using K = Kind;
using S = Scale;
using SD = SlotDefault;

// ----------------------------- THE TABLE (per band) --------------------------
std::vector<Row> bandRows()
{
    return {
        // suffix       name              kind     min               max               default           scale     centre              label  choices            auto   slotDefault
        { band::bypass,   "Bypass",       K::Bool,   0, 1, 0,                                             S::Linear, 0,                   "",    nullptr,           true,  SD::None },
        { band::fLo,      "Low Freq",     K::Float,  rng::freqMin, rng::freqMax, 0,                       S::Log,    0,                   "Hz",  nullptr,           true,  SD::Lo },
        { band::fHi,      "High Freq",    K::Float,  rng::freqMin, rng::freqMax, 0,                       S::Log,    0,                   "Hz",  nullptr,           true,  SD::Hi },
        { band::thresh,   "Threshold",    K::Float,  rng::threshMin, rng::threshMax, rng::threshDef,      S::Linear, 0,                   "dB",  nullptr,           true,  SD::None, rng::threshInterval },
        { band::ratio,    "Ratio",        K::Float,  rng::ratioMin, rng::ratioMax, rng::ratioDef,         S::Skew,   rng::ratioCentre,    ":1",  nullptr,           true,  SD::None },
        { band::knee,     "Knee",         K::Float,  rng::kneeMin, rng::kneeMax, rng::kneeDef,            S::Linear, 0,                   "dB",  nullptr,           true,  SD::None },
        { band::attack,   "Attack",       K::Float,  rng::attackMin, rng::attackMax, rng::attackDef,      S::Log,    0,                   "ms",  nullptr,           true,  SD::None },
        { band::release,  "Release",      K::Float,  rng::releaseMin, rng::releaseMax, rng::releaseDef,   S::Log,    0,                   "ms",  nullptr,           true,  SD::None },
        { band::range,    "Range",        K::Float,  rng::rangeMin, rng::rangeMax, rng::rangeDef,         S::Linear, 0,                   "dB",  nullptr,           true,  SD::None },
        { band::makeup,   "Makeup",       K::Float,  rng::makeupMin, rng::makeupMax, 0,                   S::Linear, 0,                   "dB",  nullptr,           true,  SD::None },
        { band::mix,      "Mix",          K::Float,  rng::percentMin, rng::percentMax, 100,               S::Linear, 0,                   "%",   nullptr,           true,  SD::None },
        { band::detMode,  "Detector",     K::Choice, 0, 0, 0,                                             S::Linear, 0,                   "",    &choice::detMode,  true,  SD::None },
        { band::detBlend, "Det Blend",    K::Float,  rng::unitMin, rng::unitMax, rng::detBlendDef,        S::Linear, 0,                   "",    nullptr,           true,  SD::None },
        { band::rmsTime,  "RMS Time",     K::Float,  rng::rmsTimeMin, rng::rmsTimeMax, rng::rmsTimeDef,   S::Log,    0,                   "ms",  nullptr,           true,  SD::None },
        { band::stereoMode,"Stereo Mode", K::Choice, 0, 0, 0,                                             S::Linear, 0,                   "",    &choice::stereoMode, true, SD::None },
        { band::link,     "Link",         K::Float,  rng::percentMin, rng::percentMax, 100,               S::Linear, 0,                   "%",   nullptr,           true,  SD::None },
        { band::scSource, "SC Source",    K::Choice, 0, 0, 0,                                             S::Linear, 0,                   "",    &choice::scSource, true,  SD::None },
        { band::scMode,   "SC Range Mode",K::Choice, 0, 0, scModeOwn,                                    S::Linear, 0,                   "",    &choice::scMode,   false, SD::None },
        { band::scRef,    "SC Range Band",K::Int,    (float) rng::scRefMin, (float) rng::scRefMax, (float) rng::scRefDef, S::Linear, 0,   "",    nullptr,           false, SD::None },
        { band::scLo,     "SC Low Freq",  K::Float,  rng::freqMin, rng::freqMax, 0,                       S::Log,    0,                   "Hz",  nullptr,           true,  SD::Lo },
        { band::scHi,     "SC High Freq", K::Float,  rng::freqMin, rng::freqMax, 0,                       S::Log,    0,                   "Hz",  nullptr,           true,  SD::Hi },
        { band::scChan,   "SC Channel",   K::Choice, 0, 0, 0,                                             S::Linear, 0,                   "",    &choice::scChan,   true,  SD::None },
        { band::lookahead,"Lookahead",    K::Float,  rng::lookaheadMin, rng::lookaheadMax, 0,             S::Linear, 0,                   "ms",  nullptr,           false, SD::None },
        { band::adaptRel, "Adaptive Rel", K::Float,  rng::percentMin, rng::percentMax, 0,                 S::Linear, 0,                   "%",   nullptr,           true,  SD::None },
        { band::freqRel,  "Freq Rel",     K::Float,  rng::percentMin, rng::percentMax, 0,                 S::Linear, 0,                   "%",   nullptr,           true,  SD::None },
        { band::transient,"Transient",    K::Float,  rng::percentMin, rng::percentMax, 0,                 S::Linear, 0,                   "%",   nullptr,           true,  SD::None },
        { band::character,"Character",    K::Float,  rng::percentMin, rng::percentMax, 0,                 S::Linear, 0,                   "%",   nullptr,           true,  SD::None },
    };
}

// ----------------------------- THE TABLE (global) ----------------------------
std::vector<Row> globalRows()
{
    return {
        { "inGain",         "Input Gain",      K::Float,  rng::gainMin, rng::gainMax, 0,          S::Linear, 0, "dB",  nullptr,              true,  SD::None },
        { "outGain",        "Output Gain",     K::Float,  rng::gainMin, rng::gainMax, 0,          S::Linear, 0, "dB",  nullptr,              true,  SD::None },
        { "mix",            "Mix",             K::Float,  rng::percentMin, rng::percentMax, 100,  S::Linear, 0, "%",   nullptr,              true,  SD::None },
        { "quality",        "Quality",         K::Choice, 0, 0, 0,                                S::Linear, 0, "",    &choice::quality,     false, SD::None },
        { "lpLength",       "LP Length",       K::Choice, 0, 0, 1,                                S::Linear, 0, "",    &choice::lpLength,    false, SD::None },
        { "oversampling",   "Oversampling",    K::Choice, 0, 0, 0,                                S::Linear, 0, "",    &choice::oversampling,false, SD::None },
        { "constantLatency","Constant Latency",K::Bool,   0, 1, 0,                                S::Linear, 0, "",    nullptr,              false, SD::None },
        { "overlapMode",    "Overlap Mode",    K::Choice, 0, 0, 0,                                S::Linear, 0, "",    &choice::overlapMode, true,  SD::None },
        { "charModel",      "Character Model", K::Choice, 0, 0, 0,                                S::Linear, 0, "",    &choice::charModel,   true,  SD::None },
        { "charDrive",      "Character Drive", K::Float,  rng::percentMin, rng::percentMax, 0,    S::Linear, 0, "%",   nullptr,              true,  SD::None },
        { "autoGain",       "Auto Gain",       K::Bool,   0, 1, 0,                                S::Linear, 0, "",    nullptr,              true,  SD::None },
        { "clipMode",       "Clip Mode",       K::Choice, 0, 0, 0,                                S::Linear, 0, "",    &choice::clipMode,    true,  SD::None },
        { "clipCeiling",    "Clip Ceiling",    K::Float,  rng::clipCeilMin, rng::clipCeilMax, rng::clipCeilDef, S::Linear, 0, "dBFS", nullptr, true,  SD::None },
        { global::clipOversampling, "Clip Oversampling", K::Choice, 0, 0, 0,                      S::Linear, 0, "",    &choice::oversampling,false, SD::None },
        // The bypass stays the last parameter (the host's bypass).
        { global::bypass,   "Bypass",          K::Bool,   0, 1, 0,                                S::Linear, 0, "",    nullptr,              true,  SD::None },
    };
}

ParamSpec fromRow (const Row& r, int slot)
{
    ParamSpec p;
    p.slot = slot;
    p.id = slot > 0 ? bandParamId (slot, r.idSuffix) : std::string (r.idSuffix);
    p.name = slot > 0 ? "B" + std::to_string (slot) + " " + r.name : std::string (r.name);
    p.kind = r.kind;
    p.minValue = r.minValue;
    p.maxValue = r.maxValue;
    p.defaultValue = r.defaultValue;
    p.scale = r.scale;
    p.centre = r.centre;
    p.interval = r.interval;
    p.label = r.label;
    p.automatable = r.automatable;

    if (r.kind == Kind::Choice)
    {
        p.choices = *r.choices;
        p.minValue = 0.0f;
        p.maxValue = float (p.choices.size() - 1);
    }

    if (slot > 0 && r.slotDefault == SlotDefault::Lo) p.defaultValue = slotDefaultLo (slot);
    if (slot > 0 && r.slotDefault == SlotDefault::Hi) p.defaultValue = slotDefaultHi (slot);
    return p;
}

std::vector<ParamSpec> build()
{
    std::vector<ParamSpec> out;
    const auto brow = bandRows();

    for (int slot = 1; slot <= kActiveSlots; ++slot)
        for (auto& r : brow)
            out.push_back (fromRow (r, slot));

    for (auto& r : globalRows())
        out.push_back (fromRow (r, 0));

    return out;
}

juce::String text (const ParamSpec& s, float v)
{
    if (juce::String (s.id).endsWith ("_ratio") && v >= 99.5f)
        return "inf";

    if (s.kind == Kind::Float)
    {
        // Fixed decimals per parameter: the text -> value -> text round trip must be idempotent.
        const int decimals = (s.maxValue - s.minValue <= 1.5f || s.label == "ms") ? 2 : 1;
        return juce::String (v, decimals);
    }
        return juce::String (v, std::abs (v) < 10.0f ? 2 : (std::abs (v) < 100.0f ? 1 : 0));

    return juce::String (juce::roundToInt (v));
}
} // namespace

std::string bandParamId (int slot, std::string_view suffix)
{
    return "b" + std::to_string (slot) + "_" + std::string (suffix);
}

float slotDefaultLo (int slot) noexcept { return kSlotLo[(size_t) juce::jlimit (1, kActiveSlots, slot) - 1]; }
float slotDefaultHi (int slot) noexcept { return kSlotHi[(size_t) juce::jlimit (1, kActiveSlots, slot) - 1]; }

const std::vector<ParamSpec>& allParams()
{
    static const std::vector<ParamSpec> params = build();
    return params;
}

const ParamSpec* findSpec (std::string_view id)
{
    static const auto index = []
    {
        std::vector<std::pair<std::string_view, const ParamSpec*>> v;
        for (auto& p : allParams()) v.emplace_back (p.id, &p);
        return v;
    }();

    for (auto& e : index)
        if (e.first == id)
            return e.second;

    return nullptr;
}

int bandParamsPerSlot() { return (int) bandRows().size(); }

float clampToSpec (const ParamSpec& s, float v) noexcept
{
    if (! std::isfinite (v))
        return s.defaultValue;

    v = juce::jlimit (s.minValue, s.maxValue, v);
    return s.kind == Kind::Float ? v : std::round (v);
}

juce::NormalisableRange<float> makeRange (const ParamSpec& s)
{
    // Custom mappings use double precision internally so that plain -> normalised -> plain
    // round-trips return the original float for defaults and typical values.
    const double lo = (double) s.minValue, hi = (double) s.maxValue;
    const auto snap = [lo, hi] (float, float, float v) { return (float) juce::jlimit (lo, hi, (double) v); };

    if (s.scale == Scale::Log)
    {
        const double lr = std::log (hi / lo);
        return { s.minValue, s.maxValue,
                 [=] (float, float, float p) { return (float) (lo * std::exp (lr * juce::jlimit (0.0, 1.0, (double) p))); },
                 [=] (float, float, float v) { return (float) juce::jlimit (0.0, 1.0, std::log (juce::jmax ((double) v, lo) / lo) / lr); },
                 snap };
    }

    if (s.scale == Scale::Skew)
    {
        const double skew = std::log (0.5) / std::log (((double) s.centre - lo) / (hi - lo));
        return { s.minValue, s.maxValue,
                 [=] (float, float, float p) { return (float) (lo + (hi - lo) * std::pow (juce::jlimit (0.0, 1.0, (double) p), 1.0 / skew)); },
                 [=] (float, float, float v) { return (float) std::pow (juce::jlimit (0.0, 1.0, ((double) v - lo) / (hi - lo)), skew); },
                 snap };
    }

    return { s.minValue, s.maxValue, s.interval };
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    std::unique_ptr<juce::AudioProcessorParameterGroup> group;
    int currentSlot = -1;

    auto flush = [&] { if (group != nullptr) layout.add (std::move (group)); };

    for (auto& s : allParams())
    {
        if (s.slot != currentSlot || group == nullptr)
        {
            flush();
            currentSlot = s.slot;
            group = s.slot > 0
                ? std::make_unique<juce::AudioProcessorParameterGroup> ("band" + std::to_string (s.slot), "Band " + std::to_string (s.slot), "|")
                : std::make_unique<juce::AudioProcessorParameterGroup> ("global", "Global", "|");
        }

        const juce::ParameterID pid { s.id, kVersionHint };
        const juce::String name = s.name;
        std::unique_ptr<juce::RangedAudioParameter> p;

        switch (s.kind)
        {
            case Kind::Float:
                p = std::make_unique<juce::AudioParameterFloat> (pid, name, makeRange (s), s.defaultValue,
                        juce::AudioParameterFloatAttributes().withLabel (s.label)
                                                             .withAutomatable (s.automatable)
                                                             .withStringFromValueFunction ([spec = &s] (float v, int) { return text (*spec, v); }));
                break;
            case Kind::Bool:
                p = std::make_unique<juce::AudioParameterBool> (pid, name, s.defaultValue >= 0.5f,
                        juce::AudioParameterBoolAttributes().withAutomatable (s.automatable));
                break;
            case Kind::Choice:
            {
                juce::StringArray items;
                for (auto& c : s.choices) items.add (c);
                p = std::make_unique<juce::AudioParameterChoice> (pid, name, items, (int) s.defaultValue,
                        juce::AudioParameterChoiceAttributes().withAutomatable (s.automatable));
                break;
            }
            case Kind::Int:
                p = std::make_unique<juce::AudioParameterInt> (pid, name, (int) s.minValue, (int) s.maxValue, (int) s.defaultValue,
                        juce::AudioParameterIntAttributes().withAutomatable (s.automatable));
                break;
        }

        group->addChild (std::move (p));
    }

    flush();
    return layout;
}
} // namespace tl::schema
