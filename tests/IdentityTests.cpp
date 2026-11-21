#include <catch2/catch_test_macros.hpp>

#include "PluginIdentity.h"
#include "PluginProcessor.h"

using namespace tl;

namespace
{
int countUpper (std::string_view s) { int n = 0; for (char c : s) n += (c >= 'A' && c <= 'Z'); return n; }
bool restLower (std::string_view s) { for (size_t i = 1; i < s.size(); ++i) if (s[i] >= 'A' && s[i] <= 'Z') return false; return true; }
}

TEST_CASE ("Identity constants match JUCE macros and CMake", "[identity]")
{
    CHECK (identity::pluginName == JucePlugin_Name);
    CHECK (identity::companyName == JucePlugin_Manufacturer);
    CHECK (identity::versionString == JucePlugin_VersionString);
    CHECK (identity::manufacturerCode == (std::uint32_t) JucePlugin_ManufacturerCode);
    CHECK (identity::pluginCode == (std::uint32_t) JucePlugin_PluginCode);
    CHECK (identity::bundleId == JUCE_STRINGIFY (JucePlugin_CFBundleIdentifier));
    CHECK (std::string_view (JucePlugin_Vst3Category) == "Fx|Dynamics");
    CHECK (JucePlugin_IsSynth == 0);
    CHECK (JucePlugin_WantsMidiInput == 0);
    CHECK (JucePlugin_ProducesMidiOutput == 0);
    CHECK (JucePlugin_IsMidiEffect == 0);

    CHECK (identity::bundleId == TL_CMAKE_BUNDLE_ID);
    CHECK (identity::clapId == TL_CMAKE_CLAP_ID);
    CHECK (identity::manufacturerCodeString == TL_CMAKE_MANUFACTURER_CODE);
    CHECK (identity::pluginCodeString == TL_CMAKE_PLUGIN_CODE);
    CHECK (identity::fourCC (identity::manufacturerCodeString[0], identity::manufacturerCodeString[1],
                             identity::manufacturerCodeString[2], identity::manufacturerCodeString[3]) == identity::manufacturerCode);
    CHECK (identity::fourCC (identity::pluginCodeString[0], identity::pluginCodeString[1],
                             identity::pluginCodeString[2], identity::pluginCodeString[3]) == identity::pluginCode);

    PluginProcessor p;
    CHECK (p.getName().toStdString() == identity::pluginName);
    CHECK (p.getBypassParameter() != nullptr);
}

TEST_CASE ("Identity codes satisfy JUCE rules", "[identity]")
{
    const auto mfr = identity::manufacturerCodeString, plug = identity::pluginCodeString;
    CHECK (mfr.size() == 4);
    CHECK (plug.size() == 4);
    CHECK (countUpper (mfr) >= 1);
    CHECK (mfr[0] >= 'A'); CHECK (mfr[0] <= 'Z'); CHECK (restLower (mfr));   // GarageBand rule
    CHECK (countUpper (plug) == 1);                                          // AU: exactly one upper-case
    CHECK (plug[0] >= 'A'); CHECK (plug[0] <= 'Z'); CHECK (restLower (plug));
    CHECK (identity::stateSchemaVersion == 1);
}

TEST_CASE ("createEditor returns the generic editor", "[identity][editor]")
{
    PluginProcessor p;
    CHECK (p.hasEditor());
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    REQUIRE (ed != nullptr);
    CHECK (dynamic_cast<juce::GenericAudioProcessorEditor*> (ed.get()) != nullptr);
}
