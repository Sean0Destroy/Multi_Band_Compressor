#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "PluginProcessor.h"

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

std::string id (int slot, const char* suffix) { return schema::bandParamId (slot, suffix); }
using Slots = std::vector<int>;
}

TEST_CASE ("createBand fills slots in order and reports full", "[bandslots]")
{
    PluginProcessor p;
    auto& bs = p.getBandSlots();
    CHECK (bs.usedSlots().empty());
    CHECK (bs.usedMaskSnapshot() == 0);

    for (int i = 1; i <= 8; ++i)
    {
        CHECK (bs.createBand() == i);
        CHECK (bs.isUsed (i));
        CHECK (bs.usedMaskSnapshot() == ((1u << i) - 1u));
    }
    CHECK (bs.createBand() == 0);
    CHECK (bs.usedSlots() == Slots { 1, 2, 3, 4, 5, 6, 7, 8 });
    CHECK_FALSE (bs.deleteBand (0));
    CHECK_FALSE (bs.deleteBand (9));
}

TEST_CASE ("Slot allocation: never-used first, then least recently used", "[bandslots]")
{
    PluginProcessor p;
    auto& bs = p.getBandSlots();
    for (int i = 0; i < 3; ++i) bs.createBand();           // 1 2 3
    REQUIRE (bs.deleteBand (2));
    CHECK (bs.createBand() == 4);                          // never-used 4 beats freshly freed 2
    for (int expected : { 5, 6, 7, 8 }) CHECK (bs.createBand() == expected);
    CHECK (bs.createBand() == 2);                          // only free slot left
    CHECK (bs.createBand() == 0);

    // Everything has been used: pure LRU among free slots (order of deletion decides).
    REQUIRE (bs.deleteBand (6));
    REQUIRE (bs.deleteBand (3));
    REQUIRE (bs.deleteBand (8));
    CHECK (bs.createBand() == 6);
    CHECK (bs.createBand() == 3);
    CHECK (bs.createBand() == 8);
    CHECK_FALSE (bs.deleteBand (8) == false);              // deleting a used slot works
    CHECK_FALSE (bs.deleteBand (8));                       // double delete is rejected
}

TEST_CASE ("createBand resets the reused slot's parameters to defaults", "[bandslots]")
{
    PluginProcessor p;
    auto& bs = p.getBandSlots();
    REQUIRE (bs.createBand() == 1);
    setPlain (p, id (1, "thresh"), -40.0f);
    setPlain (p, id (1, "fLo"), 55.0f);
    setPlain (p, id (1, "scMode"), 2.0f);
    REQUIRE (bs.deleteBand (1));
    CHECK_THAT (val (p, id (1, "thresh")), WithinRel (-40.0f, 1e-5f));   // parameters persist after delete

    for (int i = 2; i <= 8; ++i) REQUIRE (bs.createBand() == i);
    REQUIRE (bs.createBand() == 1);                                       // reuse
    CHECK_THAT (val (p, id (1, "thresh")), WithinRel (-18.0f, 1e-5f));
    CHECK_THAT (val (p, id (1, "fLo")), WithinRel (20.0f, 1e-4f));
    CHECK (val (p, id (1, "scMode")) == 0.0f);
    CHECK (bs.isUsed (1));
}

TEST_CASE ("deleteBand converts dependents to Custom snapshots", "[bandslots]")
{
    PluginProcessor p;
    auto& bs = p.getBandSlots();
    for (int i = 0; i < 4; ++i) bs.createBand();

    setPlain (p, id (1, "fLo"), 150.0f);
    setPlain (p, id (1, "fHi"), 450.0f);
    setPlain (p, id (2, "scMode"), (float) schema::scModeBand); setPlain (p, id (2, "scRef"), 1.0f);
    setPlain (p, id (3, "scMode"), (float) schema::scModeBand); setPlain (p, id (3, "scRef"), 4.0f);   // other target
    setPlain (p, id (4, "scMode"), (float) schema::scModeOwn);  setPlain (p, id (4, "scRef"), 1.0f);   // Own: untouched

    REQUIRE (bs.deleteBand (1));
    CHECK (val (p, id (2, "scMode")) == (float) schema::scModeCustom);
    CHECK_THAT (val (p, id (2, "scLo")), WithinRel (150.0f, 1e-5f));
    CHECK_THAT (val (p, id (2, "scHi")), WithinRel (450.0f, 1e-5f));
    CHECK (val (p, id (3, "scMode")) == (float) schema::scModeBand);
    CHECK (val (p, id (4, "scMode")) == (float) schema::scModeOwn);
    CHECK (bs.usedSlots() == Slots { 2, 3, 4 });
}

TEST_CASE ("Undo/redo of create and delete restores everything in one step", "[bandslots]")
{
    PluginProcessor p;
    auto& bs = p.getBandSlots();
    auto& um = p.getUndoManager();

    for (int i = 0; i < 3; ++i) bs.createBand();
    um.clearUndoHistory();

    setPlain (p, id (1, "fLo"), 150.0f);
    setPlain (p, id (1, "fHi"), 450.0f);
    setPlain (p, id (2, "scMode"), (float) schema::scModeBand); setPlain (p, id (2, "scRef"), 1.0f);
    setPlain (p, id (2, "scLo"), 999.0f);
    setPlain (p, id (3, "scMode"), (float) schema::scModeBand); setPlain (p, id (3, "scRef"), 1.0f);
    const auto topoBefore = bs.topology();

    REQUIRE (bs.deleteBand (1));
    const auto topoAfter = bs.topology();
    REQUIRE (val (p, id (2, "scMode")) == (float) schema::scModeCustom);
    REQUIRE (val (p, id (3, "scMode")) == (float) schema::scModeCustom);

    REQUIRE (um.undo());                                             // ONE undo reverts the whole delete
    CHECK (bs.topology() == topoBefore);
    CHECK (bs.usedSlots() == Slots { 1, 2, 3 });
    CHECK (bs.usedMaskSnapshot() == 0b111u);
    CHECK (val (p, id (2, "scMode")) == (float) schema::scModeBand);
    CHECK (val (p, id (2, "scRef")) == 1.0f);
    CHECK_THAT (val (p, id (2, "scLo")), WithinRel (999.0f, 1e-5f));
    CHECK (val (p, id (3, "scMode")) == (float) schema::scModeBand);

    REQUIRE (um.redo());
    CHECK (bs.topology() == topoAfter);
    CHECK (bs.usedSlots() == Slots { 2, 3 });
    CHECK (bs.usedMaskSnapshot() == 0b110u);
    CHECK (val (p, id (2, "scMode")) == (float) schema::scModeCustom);
    CHECK_THAT (val (p, id (2, "scLo")), WithinRel (150.0f, 1e-5f));
    CHECK_THAT (val (p, id (2, "scHi")), WithinRel (450.0f, 1e-5f));

    // Create: undo restores the slot's previous parameter values and used flag.
    REQUIRE (um.undo());                                             // back to 3 bands
    REQUIRE (bs.deleteBand (3));
    setPlain (p, id (3, "thresh"), -44.0f);
    um.clearUndoHistory();
    for (int i = 4; i <= 8; ++i) bs.createBand();                    // use 4..8 (never used)
    REQUIRE (bs.createBand() == 3);                                  // reuse -> resets thresh
    CHECK_THAT (val (p, id (3, "thresh")), WithinRel (-18.0f, 1e-5f));
    REQUIRE (um.undo());
    CHECK_FALSE (bs.isUsed (3));
    CHECK_THAT (val (p, id (3, "thresh")), WithinRel (-44.0f, 1e-5f));
    CHECK ((bs.usedMaskSnapshot() & 0b100u) == 0);
    REQUIRE (um.redo());
    CHECK (bs.isUsed (3));
    CHECK_THAT (val (p, id (3, "thresh")), WithinRel (-18.0f, 1e-5f));
}

TEST_CASE ("Topology survives state save/load including allocation history", "[bandslots]")
{
    PluginProcessor a;
    for (int i = 0; i < 4; ++i) a.getBandSlots().createBand();
    a.getBandSlots().deleteBand (2);
    juce::MemoryBlock blob;
    a.getStateInformation (blob);

    PluginProcessor b;
    b.setStateInformation (blob.getData(), (int) blob.getSize());
    CHECK (b.getBandSlots().topology() == a.getBandSlots().topology());
    CHECK (b.getBandSlots().createBand() == 5);                      // never-used still preferred after reload
}
