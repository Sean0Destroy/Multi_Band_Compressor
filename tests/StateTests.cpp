#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <fstream>
#include <random>
#include <sstream>

#include "PluginIdentity.h"
#include "PluginProcessor.h"
#include "StateSerialization.h"

using namespace tl;
using Catch::Matchers::WithinRel;

namespace
{
float val (PluginProcessor& p, const std::string& id) { return p.getState().getRawParameterValue (id)->load(); }

void setPlain (PluginProcessor& p, const std::string& id, float v)
{
    auto* prm = p.getState().getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

juce::MemoryBlock blobFromXml (const juce::String& text)
{
    auto xml = juce::XmlDocument::parse (text);
    REQUIRE (xml != nullptr);
    juce::MemoryBlock mb;
    juce::AudioProcessor::copyXmlToBinary (*xml, mb);
    return mb;
}

juce::MemoryBlock save (PluginProcessor& p) { juce::MemoryBlock mb; p.getStateInformation (mb); return mb; }
std::unique_ptr<juce::XmlElement> xmlOf (const juce::MemoryBlock& mb);

// Byte comparison of two saved states. On mismatch, reports the first differing line of the
// XML so a failure on a platform we cannot debug locally is diagnosable from the CI log.
bool same (const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    if (a == b) return true;
    auto xa = xmlOf (a), xb = xmlOf (b);
    if (xa != nullptr && xb != nullptr)
    {
        juce::StringArray la, lb;
        la.addLines (xa->toString());
        lb.addLines (xb->toString());
        for (int i = 0; i < juce::jmax (la.size(), lb.size()); ++i)
            if (la[i] != lb[i])
            {
                UNSCOPED_INFO ("first state difference at line " << i << "\n  a: " << la[i].toStdString()
                                                                 << "\n  b: " << lb[i].toStdString());
                break;
            }
    }
    return false;
}

std::string readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    REQUIRE (f.good());
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::unique_ptr<juce::XmlElement> xmlOf (const juce::MemoryBlock& b)
{
    return juce::AudioProcessor::getXmlFromBinary (b.getData(), (int) b.getSize());
}
}

TEST_CASE ("State round trip over all parameters", "[state]")
{
    PluginProcessor a;
    for (int i = 0; i < 8; ++i) REQUIRE (a.getBandSlots().createBand() == i + 1);

    std::mt19937 rng (12345);
    std::uniform_real_distribution<float> dist (0.0f, 1.0f);
    for (auto* prm : a.getParameters())
        prm->setValueNotifyingHost (dist (rng));

    // scRef outside the active slots is normalised on load (covered by its own test); keep refs valid here.
    for (int s = 1; s <= 8; ++s)
        setPlain (a, schema::bandParamId (s, "scRef"), (float) (1 + (s % 8)));

    a.getState().state.getChildWithName ("NonParam").setProperty ("uiWidth", 777, nullptr);

    const auto blobA = save (a);
    REQUIRE (blobA.getSize() > 0);

    PluginProcessor b;
    b.setStateInformation (blobA.getData(), (int) blobA.getSize());

    int maxUlpish = 0;
    for (auto& s : schema::allParams())
    {
        INFO (s.id);
        const float va = val (a, s.id), vb = val (b, s.id);
        if (s.kind == schema::Kind::Float)
            CHECK_THAT (vb, WithinRel (va, 1e-5f) || Catch::Matchers::WithinAbs (va, 1e-6));
        else
            CHECK (va == vb);
        (void) maxUlpish;
    }

    CHECK (b.getBandSlots().topology() == a.getBandSlots().topology());
    CHECK (b.getBandSlots().usedMaskSnapshot() == 0xffu);
    CHECK ((int) b.getState().state.getChildWithName ("NonParam").getProperty ("uiWidth") == 777);

    const auto blobB1 = save (b);
    const auto blobB2 = save (b);
    CHECK (same (blobB1, blobB2));                       // saving twice is byte-identical

    PluginProcessor c;                                   // and stable through another generation
    c.setStateInformation (blobB1.getData(), (int) blobB1.getSize());
    CHECK (same (blobB1, save (c)));

    auto xml = xmlOf (blobA);
    REQUIRE (xml != nullptr);
    CHECK (xml->getIntAttribute ("schemaVersion", -1) == 1);
    CHECK (xml->getStringAttribute ("pluginVersion") == "0.1.0");
    CHECK (xml->getChildByName ("Topology") != nullptr);
    CHECK (xml->getChildByName ("NonParam") != nullptr);
}

TEST_CASE ("v0 fixture (no schemaVersion) loads, migrates and preserves foreign nodes", "[state]")
{
    const auto text = readFile (std::string (TL_GOLDEN_DIR) + "/state_v0.xml");
    const auto blob = blobFromXml (text);

    PluginProcessor p;
    setPlain (p, "b4_thresh", -50.0f);   // must be reset to default: missing in the file
    setPlain (p, "outGain", 3.0f);

    auto r = state::load (p.getState(), blob.getData(), blob.getSize());
    REQUIRE (r.ok);
    CHECK (r.fromVersion == 0);
    p.getBandSlots().refresh();

    CHECK_THAT (val (p, "b1_thresh"), WithinRel (-30.0f, 1e-5f));
    CHECK_THAT (val (p, "b2_ratio"), WithinRel (8.0f, 1e-5f));
    CHECK_THAT (val (p, "inGain"), WithinRel (6.0f, 1e-5f));
    CHECK_THAT (val (p, "b4_thresh"), WithinRel (-18.0f, 1e-5f));      // missing -> default
    CHECK (val (p, "outGain") == 0.0f);
    CHECK (p.getBandSlots().usedSlots() == std::vector<int> { 1, 2 }); // derived from legacy b{N}_used
    CHECK ((int) p.getState().state.getChildWithName ("NonParam").getProperty ("uiWidth") == 900);

    const auto out = save (p);
    auto xml = xmlOf (out);
    REQUIRE (xml != nullptr);
    CHECK (xml->getIntAttribute ("schemaVersion") == 1);
    CHECK (xml->getChildByName ("b1_used") == nullptr);

    bool foundParam = false;
    for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
        if (e->getStringAttribute ("id") == "zz_future_param")
        {
            foundParam = true;
            CHECK (e->getStringAttribute ("note") == "kept");
            CHECK (e->getStringAttribute ("value") == "3");
        }
    CHECK (foundParam);
    auto* fut = xml->getChildByName ("FutureNode");
    REQUIRE (fut != nullptr);
    CHECK (fut->getStringAttribute ("kind") == "x");
    CHECK (fut->getChildByName ("Inner") != nullptr);

    // Round trip keeps them, byte-identically on the second generation.
    PluginProcessor q;
    q.setStateInformation (out.getData(), (int) out.getSize());
    CHECK (same (out, save (q)));
}

TEST_CASE ("Loaded state does not depend on the instance's previous parameter values", "[state]")
{
    PluginProcessor src;
    setPlain (src, "inGain", 6.0f);
    setPlain (src, "b2_fLo", 333.0f);
    setPlain (src, "b3_attack", 12.5f);
    const auto blob = save (src);

    PluginProcessor fresh;
    fresh.setStateInformation (blob.getData(), (int) blob.getSize());

    // Put every float parameter a few ulps away from the value in the blob. JUCE treats such
    // values as "approximately equal" and would not re-apply them, leaving state that depends
    // on the instance's history (seen as a one-ulp difference on Apple Silicon).
    PluginProcessor dirty;
    int perturbed = 0;
    for (auto& spec : schema::allParams())
    {
        const float target = val (src, spec.id);
        for (int ulps = 1; ulps <= 4 && juce::exactlyEqual (val (dirty, spec.id), target); ++ulps)
        {
            float v = target;
            for (int i = 0; i < ulps; ++i) v = std::nextafter (v, 1.0e9f);
            setPlain (dirty, spec.id, v);
        }
        if (! juce::exactlyEqual (val (dirty, spec.id), target)) ++perturbed;
    }
    REQUIRE (perturbed > 20);   // the test must not be vacuous

    dirty.setStateInformation (blob.getData(), (int) blob.getSize());
    CHECK (same (save (fresh), save (dirty)));

    // Restored values are exactly the stored ones, parameter by parameter...
    for (auto& spec : schema::allParams())
        CHECK (juce::exactlyEqual (val (fresh, spec.id), val (dirty, spec.id)));

    // ...and a further generation reproduces the same bytes.
    PluginProcessor again;
    const auto gen1 = save (fresh);
    again.setStateInformation (gen1.getData(), (int) gen1.getSize());
    CHECK (same (gen1, save (again)));
}

namespace
{
struct ChangeCounter final : juce::AudioProcessorParameter::Listener
{
    explicit ChangeCounter (PluginProcessor& proc) : p (proc) { for (auto* prm : p.getParameters()) prm->addListener (this); }
    ~ChangeCounter() override { for (auto* prm : p.getParameters()) prm->removeListener (this); }
    void parameterValueChanged (int, float) override { ++count; }
    void parameterGestureChanged (int, bool) override {}
    PluginProcessor& p;
    int count = 0;
};
}

TEST_CASE ("Loading state notifies the host only for parameters that change", "[state]")
{
    PluginProcessor src;
    setPlain (src, "inGain", 6.0f);
    setPlain (src, "b2_fLo", 333.0f);
    const auto blobA = save (src);
    setPlain (src, "b5_ratio", 7.0f);
    setPlain (src, "b6_release", 420.0f);
    const auto blobB = save (src);

    PluginProcessor p;
    p.setStateInformation (blobA.getData(), (int) blobA.getSize());

    {
        ChangeCounter counter (p);
        p.setStateInformation (blobA.getData(), (int) blobA.getSize());   // nothing changes
        CHECK (counter.count == 0);
    }
    {
        ChangeCounter counter (p);
        p.setStateInformation (blobB.getData(), (int) blobB.getSize());   // exactly two parameters change
        CHECK (counter.count == 2);
    }
    // Compare with another instance that loaded the same state (a never-loaded instance may
    // differ from its own reloaded state by one ulp where constructor defaults are off-grid).
    PluginProcessor q;
    q.setStateInformation (blobB.getData(), (int) blobB.getSize());
    CHECK (same (save (q), save (p)));
}

TEST_CASE ("Corrupt and unknown data leaves state untouched", "[state]")
{
    PluginProcessor p;
    setPlain (p, "b1_thresh", -33.0f);
    p.getBandSlots().createBand();
    const auto before = save (p);

    auto expectRejected = [&] (const juce::MemoryBlock& mb)
    {
        auto r = state::load (p.getState(), mb.getData(), mb.getSize());
        CHECK_FALSE (r.ok);
        CHECK (r.error.isNotEmpty());
        CHECK (same (before, save (p)));
    };

    expectRejected ({});                                                          // empty
    { juce::MemoryBlock junk (4096); std::mt19937 g (1); for (size_t i = 0; i < junk.getSize(); ++i) junk[i] = (char) g(); expectRejected (junk); }
    { auto t = before; t.setSize (t.getSize() / 2); expectRejected (t); }         // truncated
    { juce::MemoryBlock z (64); z.fillWith (0); expectRejected (z); }
    expectRejected (blobFromXml ("<OTHER_ROOT schemaVersion=\"1\"/>"));
    expectRejected (blobFromXml ("<TL_STATE schemaVersion=\"abc\"/>"));
    expectRejected (blobFromXml ("<TL_STATE schemaVersion=\"-1\"/>"));
    expectRejected (blobFromXml ("<TL_STATE schemaVersion=\"99\"><PARAM id=\"inGain\" value=\"5\"/></TL_STATE>"));   // newer

    // Garbage values inside an otherwise valid file are survivable (defaults / clamping), never a crash.
    auto blob = blobFromXml ("<TL_STATE schemaVersion=\"1\"><PARAM id=\"inGain\" value=\"nan\"/><PARAM id=\"outGain\" value=\"1e30\"/><PARAM id=\"mix\" value=\"1e99\"/>"
                             "<PARAM id=\"b1_thresh\"/><PARAM value=\"1\"/></TL_STATE>");
    p.setStateInformation (blob.getData(), (int) blob.getSize());
    CHECK (val (p, "inGain") == 0.0f);
    CHECK (val (p, "outGain") == 24.0f);   // finite but out of range: clamped
    CHECK (val (p, "mix") == 100.0f);       // overflows float: treated as invalid -> default
    CHECK_THAT (val (p, "b1_thresh"), WithinRel (-18.0f, 1e-5f));
}

TEST_CASE ("NonParam child is preserved and defaults to present", "[state]")
{
    PluginProcessor a;
    auto np = a.getState().state.getChildWithName ("NonParam");
    REQUIRE (np.isValid());
    np.setProperty ("selectedBand", 3, nullptr);
    np.setProperty ("analyzer", "fast", nullptr);
    juce::ValueTree sub ("View"); sub.setProperty ("zoom", 2.5, nullptr); np.appendChild (sub, nullptr);

    const auto blob = save (a);
    PluginProcessor b;
    b.setStateInformation (blob.getData(), (int) blob.getSize());
    auto nb = b.getState().state.getChildWithName ("NonParam");
    CHECK ((int) nb.getProperty ("selectedBand") == 3);
    CHECK (nb.getProperty ("analyzer").toString() == "fast");
    CHECK ((double) nb.getChildWithName ("View").getProperty ("zoom") == 2.5);
}

TEST_CASE ("Reference to an unused slot becomes a Custom snapshot on load", "[state]")
{
    const auto blob = blobFromXml (
        "<TL_STATE schemaVersion=\"1\">"
        "<PARAM id=\"b3_scMode\" value=\"1\"/><PARAM id=\"b3_scRef\" value=\"5\"/>"      // -> unused slot 5
        "<PARAM id=\"b6_scMode\" value=\"1\"/><PARAM id=\"b6_scRef\" value=\"2\"/>"      // -> used slot 2
        "<PARAM id=\"b7_scMode\" value=\"1\"/><PARAM id=\"b7_scRef\" value=\"12\"/>"     // -> out of active range
        "<PARAM id=\"b5_fLo\" value=\"300\"/><PARAM id=\"b5_fHi\" value=\"700\"/>"
        "<Topology schemaVersion=\"1\" used=\"0110010000000000\" everUsed=\"0110010000000000\""
        " lastUse=\"0 1 2 0 0 3 0 0 0 0 0 0 0 0 0 0\" counter=\"3\"/>"
        "</TL_STATE>");

    PluginProcessor p;
    p.setStateInformation (blob.getData(), (int) blob.getSize());

    CHECK (val (p, "b3_scMode") == (float) schema::scModeCustom);
    CHECK_THAT (val (p, "b3_scLo"), WithinRel (300.0f, 1e-5f));
    CHECK_THAT (val (p, "b3_scHi"), WithinRel (700.0f, 1e-5f));
    CHECK (val (p, "b6_scMode") == (float) schema::scModeBand);      // slot 2 is used: reference kept
    CHECK (val (p, "b6_scRef") == 2.0f);
    CHECK (val (p, "b7_scMode") == (float) schema::scModeCustom);    // invalid reference: own range snapshot
    CHECK_THAT (val (p, "b7_scLo"), WithinRel (schema::slotDefaultLo (7), 1e-5f));
    CHECK (p.getBandSlots().usedSlots() == std::vector<int> { 2, 3, 6 });
}

TEST_CASE ("Missing parameters are filled with defaults", "[state]")
{
    PluginProcessor p;
    for (auto* prm : p.getParameters()) prm->setValueNotifyingHost (0.9f);

    const auto blob = blobFromXml ("<TL_STATE schemaVersion=\"1\"><PARAM id=\"inGain\" value=\"-6\"/></TL_STATE>");
    p.setStateInformation (blob.getData(), (int) blob.getSize());

    for (auto& s : schema::allParams())
    {
        INFO (s.id);
        if (s.id == "inGain") CHECK_THAT (val (p, s.id), WithinRel (-6.0f, 1e-5f));
        else if (s.kind == schema::Kind::Float) CHECK_THAT (val (p, s.id), WithinRel (s.defaultValue, 1e-5f) || Catch::Matchers::WithinAbs (s.defaultValue, 1e-6));
        else CHECK (val (p, s.id) == s.defaultValue);
    }
    CHECK (p.getBandSlots().usedSlots().empty());
}

TEST_CASE ("Foreign nodes survive a round trip verbatim", "[state]")
{
    const auto blob = blobFromXml ("<TL_STATE schemaVersion=\"1\">"
                                   "<Experimental flag=\"yes\" n=\"7\"><Deep><Deeper z=\"1\"/></Deep></Experimental>"
                                   "<PARAM id=\"unknown_param\" value=\"0.25\" tag=\"t\"/>"
                                   "</TL_STATE>");
    PluginProcessor a;
    a.setStateInformation (blob.getData(), (int) blob.getSize());
    const auto s1 = save (a);

    PluginProcessor b;
    b.setStateInformation (s1.getData(), (int) s1.getSize());
    const auto s2 = save (b);
    CHECK (same (s1, s2));

    auto xml = xmlOf (s2);
    auto* e = xml->getChildByName ("Experimental");
    REQUIRE (e != nullptr);
    CHECK (e->toString() == juce::XmlDocument::parse ("<Experimental flag=\"yes\" n=\"7\"><Deep><Deeper z=\"1\"/></Deep></Experimental>")->toString());

    bool found = false;
    for (auto* pe : xml->getChildWithTagNameIterator ("PARAM"))
        if (pe->getStringAttribute ("id") == "unknown_param")
            found = pe->getStringAttribute ("tag") == "t" && pe->getStringAttribute ("value") == "0.25";
    CHECK (found);
}

TEST_CASE ("Pure pipeline stages", "[state]")
{
    juce::String err;
    CHECK_FALSE (state::parse (nullptr, 0, "TL_STATE", err).has_value());

    auto blob = blobFromXml ("<TL_STATE/>");
    auto parsed = state::parse (blob.getData(), blob.getSize(), "TL_STATE", err);
    REQUIRE (parsed.has_value());
    CHECK (parsed->schemaVersion == 0);
    CHECK (state::validate (*parsed, err));
    state::migrate (*parsed);
    CHECK (parsed->schemaVersion == state::currentSchemaVersion);
    auto tree = state::normalise (*parsed, "TL_STATE");
    int params = 0;
    for (auto c : tree) params += c.hasType ("PARAM");
    CHECK (params == (int) schema::allParams().size());
}

// What a host's state check does (clap-validator's state-reproducibility tests): set every
// parameter to a random normalised value, save, load that into a new instance, save again, and
// expect the same bytes. The first save must already be a fixed point of a load.
TEST_CASE ("Saving random parameter values, reloading and saving again gives the same bytes", "[state]")
{
    for (unsigned seed = 1; seed <= 64; ++seed)
    {
        INFO ("seed " << seed);
        PluginProcessor a;
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> dist (0.0f, 1.0f);
        for (auto* prm : a.getParameters())
            prm->setValueNotifyingHost (dist (rng));
        // A sidechain reference to a band that is not in use becomes a Custom range on load (its
        // own test covers that); keep the references here pointing at bands that are.
        for (int s = 1; s <= 8; ++s)
            if (juce::exactlyEqual (val (a, schema::bandParamId (s, "scMode")), (float) schema::scModeBand))
                setPlain (a, schema::bandParamId (s, "scMode"), (float) schema::scModeCustom);

        const auto first = save (a);
        PluginProcessor b;
        b.setStateInformation (first.getData(), (int) first.getSize());
        CHECK (same (first, save (b)));
    }
}

// The reason the test above holds on every platform: each value a host can set (the plain value
// of some normalised value) maps back to a normalised value that gives it exactly, whatever the
// last bit of the maths library's exp, log and pow.
TEST_CASE ("Every plain value a host can set survives plain -> normalised -> plain exactly", "[state]")
{
    PluginProcessor p;
    std::mt19937 rng (2026);
    std::uniform_real_distribution<float> dist (0.0f, 1.0f);

    for (auto& spec : schema::allParams())
    {
        if (spec.kind != schema::Kind::Float || spec.slot > 1) continue;   // the slots share their ranges
        auto* prm = p.getState().getParameter (spec.id);
        REQUIRE (prm != nullptr);

        int misses = 0;
        float firstMiss = 0.0f;
        const auto check = [&] (float normalised)
        {
            const float plain = prm->convertFrom0to1 (normalised);
            if (! juce::exactlyEqual (prm->convertFrom0to1 (prm->convertTo0to1 (plain)), plain) && misses++ == 0)
                firstMiss = plain;
        };
        check (0.0f);
        check (1.0f);
        for (int i = 0; i < 20000; ++i) check (dist (rng));

        INFO (spec.id << ": " << misses << " misses, first at " << firstMiss);
        CHECK (misses == 0);
    }
}
