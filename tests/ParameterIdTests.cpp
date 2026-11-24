#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "PluginProcessor.h"

using namespace tl;

namespace
{
std::string num (float v)
{
    char b[40];
    std::snprintf (b, sizeof b, "%.6g", (double) v);
    return b;
}

std::string num4 (float v)
{
    char b[40];
    std::snprintf (b, sizeof b, "%.4g", (double) v);
    return b;
}

// One line per parameter, produced from the LIVE processor (not from the schema table):
// index|id|name|type|min|max|interval|default|mid(norm 0.5)|choices|automatable|versionHint|vst3Id
std::string manifest (PluginProcessor& p)
{
    std::ostringstream o;
    o << "# PROVISIONAL / NOT FROZEN parameter manifest (generated from the live processor).\n"
      << "# index|id|name|type|min|max|interval|default|mid@0.5|choices|automatable|versionHint|vst3ParamId\n";

    int index = 0;
    for (auto* param : p.getParameters())
    {
        auto* r = dynamic_cast<juce::RangedAudioParameter*> (param);
        REQUIRE (r != nullptr);
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (param);
        REQUIRE (withId != nullptr);

        std::string type, choices;
        if (dynamic_cast<juce::AudioParameterFloat*> (param))       type = "float";
        else if (dynamic_cast<juce::AudioParameterBool*> (param))   type = "bool";
        else if (dynamic_cast<juce::AudioParameterInt*> (param))    type = "int";
        else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (param))
        {
            type = "choice";
            for (auto& s : c->choices) choices += (choices.empty() ? "" : ";") + s.toStdString();
        }
        else FAIL ("unknown parameter type");

        const auto range = r->getNormalisableRange();
        o << index++ << '|' << withId->paramID << '|' << withId->getName (64) << '|' << type << '|'
          << num (range.start) << '|' << num (range.end) << '|' << num (range.interval) << '|'
          << num (r->convertFrom0to1 (r->getDefaultValue())) << '|' << num4 (r->convertFrom0to1 (0.5f)) << '|'
          << choices << '|' << (param->isAutomatable() ? 1 : 0) << '|' << withId->getVersionHint() << '|'
          << juce::VST3ClientExtensions::convertJuceParameterId (withId->paramID, true) << '\n';
    }
    return o.str();
}
}

TEST_CASE ("Parameter manifest matches the golden file", "[ids]")
{
    PluginProcessor p;
    const auto actual = manifest (p);
    const std::string path = std::string (TL_GOLDEN_DIR) + "/parameter_ids.txt";

    if (std::getenv ("TL_UPDATE_GOLDEN") != nullptr)
        std::ofstream (path, std::ios::binary) << actual;

    std::ifstream f (path, std::ios::binary);
    REQUIRE (f.good());
    std::stringstream ss;
    ss << f.rdbuf();

    // The comparison is on content, not on the checkout's line-ending convention
    // (a Windows checkout with core.autocrlf turns the golden file into CRLF).
    auto expected = ss.str();
    expected.erase (std::remove (expected.begin(), expected.end(), '\r'), expected.end());

    // Report the first differing line so a CI failure is diagnosable from the log alone.
    std::istringstream expLines (expected), actLines (actual);
    std::string e, a;
    for (int line = 1;; ++line)
    {
        const bool haveE = static_cast<bool> (std::getline (expLines, e));
        const bool haveA = static_cast<bool> (std::getline (actLines, a));
        if (! haveE && ! haveA) break;
        if (haveE != haveA || e != a)
        {
            FAIL_CHECK ("manifest line " << line << "\n  golden: " << (haveE ? e : "<missing>")
                                         << "\n  actual: " << (haveA ? a : "<missing>"));
            break;
        }
    }
    CHECK (expected == actual);   // order, ids, types, ranges, intervals, choices, automatable, versionHint, vst3 ids
}

TEST_CASE ("Parameter count, uniqueness and id hygiene", "[ids]")
{
    PluginProcessor p;
    constexpr int expectedCount = 231;   // 8 bands x 27 + 15 global
    CHECK ((int) p.getParameters().size() == expectedCount);
    CHECK ((int) schema::allParams().size() == expectedCount);
    CHECK (schema::bandParamsPerSlot() == 27);

    const std::regex pattern ("^[A-Za-z0-9_]{1,32}$");
    std::set<std::string> ids, lower;
    std::set<juce::uint32> vst3;

    int i = 0;
    for (auto* param : p.getParameters())
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (param);
        REQUIRE (withId != nullptr);
        const auto id = withId->paramID.toStdString();
        INFO (id);

        CHECK (std::regex_match (id, pattern));
        CHECK (ids.insert (id).second);
        auto l = id;
        for (auto& c : l) c = (char) std::tolower ((unsigned char) c);
        CHECK (lower.insert (l).second);                                   // no case-only collisions
        CHECK (vst3.insert (juce::VST3ClientExtensions::convertJuceParameterId (withId->paramID, true)).second);
        CHECK (withId->getVersionHint() == 1);
        CHECK (param->getParameterIndex() == i++);
        CHECK (id == schema::allParams()[(size_t) param->getParameterIndex()].id);
    }

    CHECK (p.getBypassParameter() == p.getState().getParameter ("bypass"));
    CHECK (p.getBypassParameter()->getParameterIndex() == expectedCount - 1);
}

TEST_CASE ("Non-automatable parameters are exactly the schema's", "[ids]")
{
    PluginProcessor p;
    std::set<std::string> nonAuto;
    for (auto* param : p.getParameters())
        if (! param->isAutomatable())
            nonAuto.insert (dynamic_cast<juce::AudioProcessorParameterWithID*> (param)->paramID.toStdString());

    // per band: scMode, scRef, lookahead; global: quality, lpLength, oversampling, constantLatency, clipOversampling
    CHECK (nonAuto.size() == 8 * 3 + 5);
    for (int s = 1; s <= 8; ++s)
    {
        CHECK (nonAuto.count (schema::bandParamId (s, "scMode")) == 1);
        CHECK (nonAuto.count (schema::bandParamId (s, "scRef")) == 1);
        CHECK (nonAuto.count (schema::bandParamId (s, "lookahead")) == 1);
    }
    for (auto id : { "quality", "lpLength", "oversampling", "constantLatency", "clipOversampling" })
        CHECK (nonAuto.count (id) == 1);
    CHECK (nonAuto.count ("b1_used") == 0);   // removed: topology lives in state
}

TEST_CASE ("Defaults follow the schema", "[ids]")
{
    PluginProcessor p;
    for (auto& s : schema::allParams())
    {
        INFO (s.id);
        // Values travel through a float-normalised representation, so allow a few ULP (documented JUCE behaviour).
        CHECK_THAT (p.getState().getRawParameterValue (s.id)->load(), Catch::Matchers::WithinRel (s.defaultValue, 1e-5f) || Catch::Matchers::WithinAbs (s.defaultValue, 1e-6));
    }
    CHECK_THAT (p.getState().getRawParameterValue ("b3_fLo")->load(), Catch::Matchers::WithinRel (400.0f, 1e-5f));
    CHECK_THAT (p.getState().getRawParameterValue ("b3_scHi")->load(), Catch::Matchers::WithinRel (1000.0f, 1e-5f));
}

TEST_CASE ("Parameter text conversion round trips consistently", "[ids]")
{
    PluginProcessor p;
    for (auto* param : p.getParameters())
        for (float n : { 0.0f, 0.1234f, 0.4166f, 0.5f, 0.9f, 1.0f })
        {
            const auto t1 = param->getText (n, 0);
            const auto back = param->getValueForText (t1);
            const auto t2 = param->getText (back, 0);
            INFO (param->getName (64) << " " << n << " '" << t1 << "' -> '" << t2 << "'");
            CHECK (t1 == t2);
        }
}

TEST_CASE ("Provisional dynamics ranges", "[ids]")
{
    PluginProcessor p;

    auto* thresh = dynamic_cast<juce::AudioParameterFloat*> (p.getState().getParameter ("b1_thresh"));
    REQUIRE (thresh != nullptr);
    const auto tr = thresh->getNormalisableRange();
    CHECK (tr.start == -80.0f);
    CHECK (tr.end == 12.0f);
    CHECK_THAT (tr.interval, Catch::Matchers::WithinRel (0.1f, 1e-6f));
    CHECK_THAT (thresh->get(), Catch::Matchers::WithinAbs (-18.0, 1e-5));
    CHECK_THAT (tr.snapToLegalValue (-17.96f), Catch::Matchers::WithinAbs (-18.0, 1e-4));   // 0.1 dB steps
    CHECK_THAT (tr.snapToLegalValue (11.98f), Catch::Matchers::WithinAbs (12.0, 1e-4));

    auto* release = dynamic_cast<juce::AudioParameterFloat*> (p.getState().getParameter ("b1_release"));
    REQUIRE (release != nullptr);
    CHECK (release->getNormalisableRange().start == 5.0f);
    CHECK (release->getNormalisableRange().end == 5000.0f);
    CHECK_THAT (release->get(), Catch::Matchers::WithinRel (150.0f, 1e-5f));
    CHECK_THAT (release->convertFrom0to1 (0.5f), Catch::Matchers::WithinRel (158.11f, 1e-3f));   // log mapping

    auto* range = dynamic_cast<juce::AudioParameterFloat*> (p.getState().getParameter ("b1_range"));
    REQUIRE (range != nullptr);
    CHECK (range->getNormalisableRange().start == 0.0f);
    CHECK (range->getNormalisableRange().end == 48.0f);
    CHECK_THAT (range->get(), Catch::Matchers::WithinRel (24.0f, 1e-6f));
}
