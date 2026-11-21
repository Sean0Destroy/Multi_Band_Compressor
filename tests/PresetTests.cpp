// Presets (starter presets, user preset files) and the A/B comparison.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>

#include "PluginProcessor.h"
#include "PresetManager.h"
#include "support/DspTestSupport.h"

using namespace tl;
using Catch::Matchers::WithinAbs;
using Slots = std::vector<int>;

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("tl_preset_test_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    ~TempDir() { dir.deleteRecursively(); }
};

juce::MemoryBlock stateOf (PluginProcessor& p)
{
    juce::MemoryBlock b;
    p.getStateInformation (b);
    return b;
}

/** Loads the processor's own state once. Values set through the parameter API can differ by one
    ulp from what a load of the saved text gives (float normalisation, visible with FMA on
    arm64); after one load the state is a fixed point and save / load is byte-exact. A host
    project or a preset is always such a loaded state the second time round. */
void settle (PluginProcessor& p)
{
    const auto block = stateOf (p);
    p.setStateInformation (block.getData(), (int) block.getSize());
}

/** Byte comparison without printing two whole state documents when it fails. */
bool same (const juce::MemoryBlock& a, const juce::MemoryBlock& b) { return a == b; }

float plain (PluginProcessor& p, const std::string& id) { return p.getState().getRawParameterValue (id)->load(); }

int starterCount (const std::vector<PresetManager::Entry>& entries)
{
    int n = 0;
    for (auto& e : entries) n += e.builtIn ? 1 : 0;
    return n;
}
}

TEST_CASE ("Starter presets load as valid states", "[presets]")
{
    TempDir tmp;
    PluginProcessor p;
    PresetManager presets (p, tmp.dir);

    const auto entries = presets.list();
    REQUIRE (starterCount (entries) == 5);
    CHECK ((int) entries.size() == 5);                                    // no user presets yet
    CHECK_FALSE (tmp.dir.exists());                                       // listing creates nothing on disk

    const std::vector<int> expectedBands { 3, 1, 1, 4, 1 };
    for (size_t i = 0; i < entries.size(); ++i)
    {
        INFO (entries[i].name);
        CHECK (entries[i].builtIn);
        CHECK (entries[i].name.startsWith ("Starter - "));
        REQUIRE (presets.load (entries[i]).wasOk());
        CHECK ((int) p.getBandSlots().usedSlots().size() == expectedBands[i]);
        CHECK (presets.getCurrentName() == entries[i].name);
        CHECK_FALSE (p.getBandSlots().canUndo());

        // Every value landed inside its parameter range, and the edges are ordered.
        for (auto& spec : schema::allParams())
        {
            const float v = plain (p, spec.id);
            REQUIRE (v >= spec.minValue);
            REQUIRE (v <= spec.maxValue);
        }
        for (int slot : p.getBandSlots().usedSlots())
            CHECK (plain (p, schema::bandParamId (slot, "fLo")) < plain (p, schema::bandParamId (slot, "fHi")));

        // The state it produces survives a save / load round trip unchanged.
        const auto saved = stateOf (p);
        PluginProcessor other;
        other.setStateInformation (saved.getData(), (int) saved.getSize());
        CHECK (same (stateOf (other), saved));
    }

    // Spot checks of what the data says.
    REQUIRE (presets.loadByName ("Starter - De-Ess").wasOk());
    CHECK_THAT ((double) plain (p, "b1_fLo"), WithinAbs (5000.0, 0.5));
    CHECK_THAT ((double) plain (p, "b1_ratio"), WithinAbs (6.0, 1e-3));
    CHECK (p.getLatencySamples() == 48);                                  // 1 ms lookahead at the default 48 kHz
    REQUIRE (presets.loadByName ("Starter - External Sidechain Duck").wasOk());
    CHECK (plain (p, "b1_scSource") == 1.0f);
    CHECK (p.getLatencySamples() == 0);

    CHECK (presets.loadByName ("does not exist").failed());
}

TEST_CASE ("User presets: save, list, load, overwrite, delete", "[presets]")
{
    TempDir tmp;
    PluginProcessor p;
    PresetManager presets (p, tmp.dir);

    p.getBandSlots().createBand();
    p.getBandSlots().createBand();
    test::setBand (p, 2, "thresh", -33.3f);
    test::setBand (p, 2, "stereoMode", 4.0f);
    test::setPlain (p, "outGain", 2.5f);
    settle (p);

    REQUIRE (presets.saveUser ("  My / Vocal: Chain?  ").wasOk());
    const auto name = presets.getCurrentName();
    CHECK (name == PresetManager::cleanName ("  My / Vocal: Chain?  "));
    CHECK_FALSE (name.containsAnyOf ("/:?"));
    CHECK (tmp.dir.getChildFile (name + ".tlmb").existsAsFile());
    const auto savedState = stateOf (p);

    // The file is readable XML with the schema version.
    const auto xml = juce::parseXML (tmp.dir.getChildFile (name + ".tlmb"));
    REQUIRE (xml != nullptr);
    CHECK (xml->hasTagName ("TL_STATE"));
    CHECK (xml->getIntAttribute ("schemaVersion") == 1);

    // Listed after the starters, in natural name order.
    REQUIRE (presets.saveUser ("Zebra 10").wasOk());
    REQUIRE (presets.saveUser ("Zebra 9").wasOk());
    const auto entries = presets.list();
    REQUIRE (entries.size() == 8u);
    CHECK (entries[5].name == name);
    CHECK (entries[6].name == "Zebra 9");
    CHECK (entries[7].name == "Zebra 10");
    CHECK_FALSE (entries[5].builtIn);

    // Change everything, load it back: exactly the saved state.
    presets.loadEmpty();
    CHECK (p.getBandSlots().usedSlots().empty());
    CHECK (presets.getCurrentName().isEmpty());
    CHECK (plain (p, "outGain") == 0.0f);
    REQUIRE (presets.loadByName (name).wasOk());
    CHECK (same (stateOf (p), savedState));
    CHECK (p.getBandSlots().usedSlots() == Slots { 1, 2 });
    CHECK_THAT ((double) plain (p, "b2_thresh"), WithinAbs (-33.3, 1e-4));

    // Saving under the same name overwrites.
    test::setBand (p, 2, "thresh", -10.0f);
    REQUIRE (presets.saveUser (name).wasOk());
    CHECK (presets.list().size() == 8u);
    presets.loadEmpty();
    REQUIRE (presets.loadByName (name).wasOk());
    CHECK_THAT ((double) plain (p, "b2_thresh"), WithinAbs (-10.0, 1e-4));

    // Names that cannot be used.
    CHECK (presets.saveUser ("   ").failed());
    CHECK (presets.saveUser ("///").failed());
    CHECK (presets.saveUser ("starter - de-ess").failed());               // belongs to a starter preset
    CHECK (PresetManager::cleanName (juce::String::repeatedString ("x", 200)).length() <= 64);

    // Delete.
    REQUIRE (presets.deleteUser (name).wasOk());
    CHECK (presets.getCurrentName().isEmpty());
    CHECK (presets.list().size() == 7u);
    CHECK (presets.deleteUser (name).failed());
    CHECK (presets.deleteUser ("Starter - De-Ess").failed());
}

TEST_CASE ("Presets: stepping through the list wraps around", "[presets]")
{
    TempDir tmp;
    PluginProcessor p;
    PresetManager presets (p, tmp.dir);
    p.getBandSlots().createBand();
    REQUIRE (presets.saveUser ("User One").wasOk());
    presets.loadEmpty();

    const auto entries = presets.list();
    REQUIRE (entries.size() == 6u);

    REQUIRE (presets.loadNext().wasOk());                                 // nothing current: the first
    CHECK (presets.getCurrentName() == entries[0].name);
    REQUIRE (presets.loadPrevious().wasOk());                             // wraps to the last
    CHECK (presets.getCurrentName() == "User One");
    REQUIRE (presets.loadNext().wasOk());
    CHECK (presets.getCurrentName() == entries[0].name);
    for (int i = 1; i < 6; ++i)
    {
        REQUIRE (presets.loadNext().wasOk());
        CHECK (presets.getCurrentName() == entries[(size_t) i].name);
    }

    presets.loadEmpty();
    REQUIRE (presets.loadPrevious().wasOk());                             // nothing current: the last
    CHECK (presets.getCurrentName() == "User One");
}

TEST_CASE ("Presets: a broken or newer file changes nothing", "[presets]")
{
    TempDir tmp;
    PluginProcessor p;
    PresetManager presets (p, tmp.dir);
    p.getBandSlots().createBand();
    test::setBand (p, 1, "ratio", 7.0f);
    settle (p);
    REQUIRE (presets.saveUser ("Good").wasOk());
    const auto before = stateOf (p);

    tmp.dir.getChildFile ("Garbage.tlmb").replaceWithText ("this is not xml <<<");
    tmp.dir.getChildFile ("Wrong Root.tlmb").replaceWithText ("<SomethingElse schemaVersion=\"1\"/>");
    tmp.dir.getChildFile ("From The Future.tlmb").replaceWithText ("<TL_STATE schemaVersion=\"99\"><PARAM id=\"b1_ratio\" value=\"2\"/></TL_STATE>");
    tmp.dir.getChildFile ("notes.txt").replaceWithText ("not a preset");

    CHECK (presets.list().size() == 5u + 4u);                             // the .txt file is not listed
    for (auto* name : { "Garbage", "Wrong Root", "From The Future" })
    {
        INFO (name);
        const auto result = presets.loadByName (name);
        CHECK (result.failed());
        CHECK (result.getErrorMessage().isNotEmpty());
        CHECK (same (stateOf (p), before));
        CHECK (presets.getCurrentName() == "Good");
    }

    // A file from this version with unknown extra content still loads (unknowns are kept aside).
    tmp.dir.getChildFile ("Partial.tlmb").replaceWithText ("<TL_STATE schemaVersion=\"1\"><PARAM id=\"b1_ratio\" value=\"3\"/><PARAM id=\"zz_new\" value=\"1\"/></TL_STATE>");
    REQUIRE (presets.loadByName ("Partial").wasOk());
    CHECK_THAT ((double) plain (p, "b1_ratio"), WithinAbs (3.0, 1e-4));
    CHECK (p.getBandSlots().usedSlots().empty());                         // no topology in the file: no bands
}

TEST_CASE ("Presets: the name travels with the plugin state and the host is told about loads", "[presets]")
{
    struct Listener : juce::AudioProcessorListener
    {
        int nonParameterChanges = 0;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override { if (d.nonParameterStateChanged) ++nonParameterChanges; }
    };

    TempDir tmp;
    PluginProcessor p;
    PresetManager presets (p, tmp.dir);
    Listener listener;
    p.addListener (&listener);

    REQUIRE (presets.loadByName ("Starter - Gentle 3 Band").wasOk());
    CHECK (listener.nonParameterChanges >= 1);
    const int afterLoad = listener.nonParameterChanges;
    REQUIRE (presets.saveUser ("Mine").wasOk());
    CHECK (listener.nonParameterChanges > afterLoad);
    p.removeListener (&listener);

    // A project saved now reopens with the preset name.
    const auto project = stateOf (p);
    PluginProcessor reopened;
    reopened.setStateInformation (project.getData(), (int) project.getSize());
    CHECK (reopened.getPresets().getCurrentName() == "Mine");
    CHECK (reopened.getBandSlots().usedSlots() == Slots { 1, 2, 3 });
}

TEST_CASE ("A/B comparison", "[presets][ab]")
{
    PluginProcessor p;
    auto& ab = p.getPresets();
    CHECK (ab.getActiveSlot() == 0);

    p.getBandSlots().createBand();
    test::setBand (p, 1, "thresh", -30.0f);
    settle (p);
    const auto stateA = stateOf (p);

    ab.switchTo (0);                                                      // already there: nothing happens
    CHECK (same (stateOf (p), stateA));

    ab.switchTo (1);                                                      // first visit: B starts as a copy of A
    CHECK (ab.getActiveSlot() == 1);
    CHECK (same (stateOf (p), stateA));

    p.getBandSlots().createBand();                                        // edit B
    test::setBand (p, 1, "thresh", -12.0f);
    settle (p);
    const auto stateB = stateOf (p);
    CHECK_FALSE (same (stateB, stateA));

    ab.switchTo (0);
    CHECK (same (stateOf (p), stateA));
    CHECK (p.getBandSlots().usedSlots() == Slots { 1 });
    ab.switchTo (1);
    CHECK (same (stateOf (p), stateB));
    CHECK (p.getBandSlots().usedSlots() == Slots { 1, 2 });

    // The window size belongs to the session: neither A/B nor a preset load changes it.
    p.getState().state.getChildWithName ("NonParam").setProperty ("editorWidth", 1500, nullptr);
    ab.switchTo (0);
    CHECK ((int) p.getState().state.getChildWithName ("NonParam").getProperty ("editorWidth") == 1500);
    REQUIRE (ab.loadByName ("Starter - De-Ess").wasOk());
    CHECK ((int) p.getState().state.getChildWithName ("NonParam").getProperty ("editorWidth") == 1500);
    ab.loadEmpty();
    CHECK ((int) p.getState().state.getChildWithName ("NonParam").getProperty ("editorWidth") == 1500);
    p.getState().state.getChildWithName ("NonParam").removeProperty ("editorWidth", nullptr);
    ab.switchTo (1);
    ab.switchTo (0);
    ab.switchTo (1);
    p.setStateInformation (stateB.getData(), (int) stateB.getSize());

    // Copy the active slot over the other one.
    ab.copyActiveToOther();
    ab.switchTo (0);
    CHECK (same (stateOf (p), stateB));
    ab.switchTo (7);                                                      // out of range: clamped to B
    CHECK (ab.getActiveSlot() == 1);
    CHECK (same (stateOf (p), stateB));
}
