#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <memory>
#include <set>

#include "PluginProcessor.h"
#include "support/OfflineRenderer.h"
#include "ui/PluginEditor.h"

using namespace tl;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
using Slots = std::vector<int>;

/** Mimics a plugin wrapper: the processor is told before its editor is deleted. */
struct EditorCloser
{
    void operator() (juce::AudioProcessorEditor* ed) const
    {
        if (ed == nullptr) return;
        ed->processor.editorBeingDeleted (ed);
        delete ed;
    }
};
using EditorHolder = std::unique_ptr<juce::AudioProcessorEditor, EditorCloser>;

EditorHolder openEditor (PluginProcessor& p) { return EditorHolder (p.createEditorAndMakeActive()); }

struct Rig
{
    PluginProcessor proc;
    EditorHolder holder { openEditor (proc) };
    ui::PluginEditor* editor = dynamic_cast<ui::PluginEditor*> (holder.get());
};

std::string id (int slot, const char* suffix) { return schema::bandParamId (slot, suffix); }

float plain (PluginProcessor& p, const std::string& paramId)
{
    return p.getState().getRawParameterValue (paramId)->load();
}

void setPlain (PluginProcessor& p, const std::string& paramId, float v)
{
    auto* prm = p.getState().getParameter (paramId);
    REQUIRE (prm != nullptr);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

template <typename T>
void collect (juce::Component& root, std::vector<T*>& out)
{
    for (auto* child : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*> (child)) out.push_back (t);
        collect (*child, out);
    }
}

juce::Component* findById (juce::Component& root, const juce::String& componentId)
{
    for (auto* child : root.getChildren())
    {
        if (child->getComponentID() == componentId) return child;
        if (auto* found = findById (*child, componentId)) return found;
    }
    return nullptr;
}

template <typename T>
T& control (juce::Component& root, const std::string& componentId)
{
    auto* c = dynamic_cast<T*> (findById (root, componentId));
    INFO ("control " << componentId);
    REQUIRE (c != nullptr);
    return *c;
}

void click (juce::Component& root, const std::string& componentId)
{
    auto& b = control<juce::Button> (root, componentId);
    REQUIRE (b.isEnabled());
    REQUIRE (b.onClick != nullptr);
    b.onClick();
}

/** Every interactive widget below `root`: componentID of each Slider / Button / ComboBox. */
std::vector<juce::String> interactiveIds (juce::Component& root)
{
    std::vector<juce::Slider*> sliders;     collect (root, sliders);
    std::vector<juce::Button*> buttons;     collect (root, buttons);
    std::vector<juce::ComboBox*> combos;    collect (root, combos);

    std::vector<juce::String> ids;
    for (auto* c : sliders) ids.push_back (c->getComponentID());
    for (auto* c : buttons) ids.push_back (c->getComponentID());
    for (auto* c : combos)  ids.push_back (c->getComponentID());
    return ids;
}

// The band parameters the engine implements today (PluginProcessor::readEngineSettings).
const std::vector<const char*> kColumnSuffixes { schema::band::bypass, schema::band::thresh, schema::band::ratio,
                                                 schema::band::attack, schema::band::release };
const std::vector<const char*> kInspectorSuffixes { schema::band::fLo, schema::band::fHi, schema::band::knee,
                                                    schema::band::range, schema::band::makeup, schema::band::mix,
                                                    schema::band::stereoMode, schema::band::link,
                                                    schema::band::detMode, schema::band::detBlend, schema::band::rmsTime,
                                                    schema::band::scSource, schema::band::scMode, schema::band::scRef,
                                                    schema::band::scLo, schema::band::scHi, schema::band::scChan };
const std::vector<const char*> kGlobalIds { schema::global::bypass, schema::global::inGain,
                                            schema::global::mix, schema::global::outGain,
                                            schema::global::autoGain, schema::global::clipMode,
                                            schema::global::clipCeiling };
}

TEST_CASE ("Editor: a new instance shows no bands and opening it does not change the state", "[ui][editor]")
{
    PluginProcessor p;
    juce::MemoryBlock before, after;
    p.getStateInformation (before);

    {
        auto holder = openEditor (p);
        auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
        REQUIRE (ed != nullptr);
        CHECK (ed->getColumnOrder().empty());
        CHECK (ed->getSelectedSlot() == 0);
        CHECK (ed->getWidth() == ui::PluginEditor::defaultWidth);
        CHECK (ed->getHeight() == ui::PluginEditor::defaultHeight);
        CHECK_FALSE (control<juce::Button> (*ed, "action_undo").isEnabled());
        CHECK_FALSE (control<juce::Button> (*ed, "action_redo").isEnabled());

        for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
            CHECK_FALSE (ed->getColumn (slot).isVisible());

        ed->refreshFromProcessor();
        ed->setSize (1400, ui::PluginEditor::defaultHeight);
    }

    p.getStateInformation (after);
    CHECK (before == after);
}

TEST_CASE ("Editor binding audit: every widget is a parameter or a documented action; no false controls", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    for (int i = 0; i < schema::kActiveSlots; ++i) r.proc.getBandSlots().createBand();
    r.editor->refreshFromProcessor();

    std::set<juce::String> documentedActions { "action_addBand", "action_undo", "action_redo",
                                               "action_inspectorTab_band", "action_inspectorTab_detector" };
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        documentedActions.insert ("action_removeBand_" + juce::String (slot));
        documentedActions.insert ("action_selectBand_" + juce::String (slot));
    }

    std::set<juce::String> seenActions, seenParams;

    for (int selected = 1; selected <= schema::kActiveSlots; ++selected)
    {
        r.editor->selectSlot (selected);
        REQUIRE (r.editor->getSelectedSlot() == selected);

        for (auto& cid : interactiveIds (*r.editor))
        {
            INFO ("widget id '" << cid << "'");
            REQUIRE (cid.isNotEmpty());   // an unmapped widget fails the audit

            if (cid.startsWith ("action_"))
            {
                CHECK (documentedActions.count (cid) == 1);
                seenActions.insert (cid);
            }
            else
            {
                CHECK (r.proc.getState().getParameter (cid) != nullptr);
                seenParams.insert (cid);
            }
        }
    }

    CHECK (seenActions == documentedActions);

    // Exactly the implemented parameters are exposed: 22 per band plus the implemented globals.
    std::set<juce::String> expected;
    for (auto* g : kGlobalIds) expected.insert (g);
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        for (auto* s : kColumnSuffixes)    expected.insert (id (slot, s));
        for (auto* s : kInspectorSuffixes) expected.insert (id (slot, s));
    }
    CHECK (seenParams == expected);
}

TEST_CASE ("Editor controls and parameters stay in sync in both directions", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    for (int i = 0; i < schema::kActiveSlots; ++i) r.proc.getBandSlots().createBand();
    ed.refreshFromProcessor();

    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        ed.selectSlot (slot);

        for (auto* suffix : { schema::band::thresh, schema::band::ratio, schema::band::attack, schema::band::release,
                              schema::band::fLo, schema::band::fHi, schema::band::knee, schema::band::range,
                              schema::band::makeup, schema::band::mix, schema::band::scLo, schema::band::scHi,
                              schema::band::detBlend, schema::band::rmsTime, schema::band::link })
        {
            const auto pid = id (slot, suffix);
            INFO (pid);
            auto& slider = control<juce::Slider> (ed, pid);
            const auto* spec = schema::findSpec (pid);
            REQUIRE (spec != nullptr);
            const auto range = schema::makeRange (*spec);

            // Initial value and range come from the parameter.
            CHECK_THAT (slider.getValue(), WithinRel ((double) plain (r.proc, pid), 1e-6));
            CHECK_THAT (slider.getMinimum(), WithinRel ((double) spec->minValue, 1e-6));
            CHECK_THAT (slider.getMaximum(), WithinRel ((double) spec->maxValue, 1e-6));

            // Parameter -> control (host automation).
            const float a = range.snapToLegalValue (range.convertFrom0to1 (0.3f));
            setPlain (r.proc, pid, a);
            CHECK_THAT (slider.getValue(), WithinRel ((double) a, 1e-5));

            // Control -> parameter (user gesture).
            const float b = range.snapToLegalValue (range.convertFrom0to1 (0.7f));
            slider.setValue ((double) b, juce::sendNotificationSync);
            CHECK_THAT ((double) plain (r.proc, pid), WithinRel ((double) b, 1e-5));

            // Double-click resets to the parameter default (slot defaults for the edges).
            CHECK (slider.isDoubleClickReturnEnabled());
            CHECK_THAT (slider.getDoubleClickReturnValue(), WithinRel ((double) spec->defaultValue, 1e-6));
        }

        {
            const auto pid = id (slot, schema::band::detMode);
            auto& combo = control<juce::ComboBox> (ed, pid);
            CHECK (combo.getNumItems() == 3);
            setPlain (r.proc, pid, 2.0f);
            CHECK (combo.getSelectedItemIndex() == 2);
            combo.setSelectedItemIndex (1, juce::sendNotificationSync);
            CHECK (plain (r.proc, pid) == 1.0f);
        }

        for (auto* suffix : { schema::band::scSource, schema::band::scMode, schema::band::scChan, schema::band::stereoMode })
        {
            const auto pid = id (slot, suffix);
            INFO (pid);
            auto& combo = control<juce::ComboBox> (ed, pid);
            const auto* spec = schema::findSpec (pid);
            REQUIRE (spec != nullptr);
            const int last = (int) spec->choices.size() - 1;
            CHECK (combo.getNumItems() == last + 1);
            CHECK (combo.getItemText (last) == juce::String (spec->choices.back()));
            setPlain (r.proc, pid, (float) last);
            CHECK (combo.getSelectedItemIndex() == last);
            combo.setSelectedItemIndex (0, juce::sendNotificationSync);
            CHECK (plain (r.proc, pid) == 0.0f);
        }

        {
            // The band a detector follows: an integer slot parameter shown as a list of the other bands.
            const auto pid = id (slot, schema::band::scRef);
            auto& combo = control<juce::ComboBox> (ed, pid);
            ed.refreshFromProcessor();
            if (slot == 1) CHECK (combo.getText() == "This band");         // default reference of slot 1 is itself
            const int other = slot == 1 ? 2 : 1;
            setPlain (r.proc, pid, (float) other);
            CHECK (combo.getSelectedId() == other);
            CHECK (combo.getNumItems() == schema::kActiveSlots - 1);       // every used band except itself
            const int another = slot == 8 ? 7 : 8;
            combo.setSelectedId (another, juce::sendNotificationSync);
            CHECK (plain (r.proc, pid) == (float) another);
        }

        {
            const auto pid = id (slot, schema::band::bypass);
            auto& button = control<juce::Button> (ed, pid);
            CHECK_FALSE (button.getToggleState());
            setPlain (r.proc, pid, 1.0f);
            CHECK (button.getToggleState());
            button.setToggleState (false, juce::sendNotificationSync);
            CHECK (plain (r.proc, pid) == 0.0f);
        }
    }

    for (auto* gid : { schema::global::inGain, schema::global::mix, schema::global::outGain })
    {
        INFO (gid);
        auto& slider = control<juce::Slider> (ed, gid);
        const auto* spec = schema::findSpec (gid);
        REQUIRE (spec != nullptr);
        setPlain (r.proc, gid, spec->minValue + 0.25f * (spec->maxValue - spec->minValue));
        CHECK_THAT (slider.getValue(), WithinRel ((double) plain (r.proc, gid), 1e-5));
        slider.setValue ((double) spec->maxValue, juce::sendNotificationSync);
        CHECK_THAT ((double) plain (r.proc, gid), WithinRel ((double) spec->maxValue, 1e-5));
        CHECK_THAT (slider.getDoubleClickReturnValue(), WithinAbs ((double) spec->defaultValue, 1e-6));
    }
    CHECK (control<juce::Slider> (ed, schema::global::outGain).getTextFromValue (-3.0) == "-3.0 dB");
    CHECK (control<juce::Slider> (ed, schema::global::mix).getTextFromValue (50.0) == "50 %");

    auto& bypass = control<juce::Button> (ed, schema::global::bypass);
    setPlain (r.proc, schema::global::bypass, 1.0f);
    CHECK (bypass.getToggleState());
    bypass.setToggleState (false, juce::sendNotificationSync);
    CHECK (plain (r.proc, schema::global::bypass) == 0.0f);
}

TEST_CASE ("Editor value fields show units and accept typed values", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    REQUIRE (r.proc.getBandSlots().createBand() == 1);
    ed.refreshFromProcessor();
    ed.selectSlot (1);

    auto& thresh = control<juce::Slider> (ed, id (1, schema::band::thresh));
    CHECK (thresh.getTextFromValue (-18.0) == "-18.0 dB");
    CHECK_THAT (thresh.getValueFromText ("-30"), WithinRel (-30.0, 1e-9));

    auto& hi = control<juce::Slider> (ed, id (1, schema::band::fHi));
    CHECK (hi.getTextFromValue (2500.0) == "2.50 kHz");
    CHECK_THAT (hi.getValueFromText ("3.2k"), WithinRel (3200.0, 1e-9));

    auto& ratio = control<juce::Slider> (ed, id (1, schema::band::ratio));
    CHECK (ratio.getTextFromValue (100.0) == "inf:1");
    CHECK_THAT (ratio.getValueFromText ("inf"), WithinRel (100.0, 1e-9));

    auto& release = control<juce::Slider> (ed, id (1, schema::band::release));
    CHECK (release.getTextFromValue (1500.0) == "1.50 s");
    CHECK_THAT (release.getValueFromText ("0.5 s"), WithinRel (500.0, 1e-9));

    // Screen-reader titles name the band and the control.
    CHECK (thresh.getTitle() == "Band 1 Threshold");
    CHECK (control<juce::Button> (ed, "action_removeBand_1").getTitle() == "Remove Band 1");
}

TEST_CASE ("Editor: add, remove, undo and redo through the buttons", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    auto& counter = [&]() -> juce::Label&
    {
        std::vector<juce::Label*> labels; collect (ed, labels);
        for (auto* l : labels) if (l->getText().contains (" / ")) return *l;
        FAIL ("band counter label not found");
        return *labels.front();
    }();

    CHECK (counter.getText() == "0 / 8");

    click (ed, "action_addBand");
    CHECK (ed.getColumnOrder() == Slots { 1 });
    CHECK (ed.getSelectedSlot() == 1);                  // a new band is selected
    CHECK (ed.getInspector().getSlot() == 1);
    CHECK (r.proc.getBandSlots().usedSlots() == Slots { 1 });
    CHECK (counter.getText() == "1 / 8");

    click (ed, "action_addBand");
    CHECK (ed.getColumnOrder() == Slots { 1, 2 });
    CHECK (ed.getSelectedSlot() == 2);

    click (ed, "action_removeBand_2");                  // removing the selected band moves the selection
    CHECK (ed.getColumnOrder() == Slots { 1 });
    CHECK (ed.getSelectedSlot() == 1);
    CHECK_FALSE (ed.getColumn (2).isVisible());

    click (ed, "action_undo");
    CHECK (ed.getColumnOrder() == Slots { 1, 2 });
    click (ed, "action_redo");
    CHECK (ed.getColumnOrder() == Slots { 1 });
    CHECK_FALSE (control<juce::Button> (ed, "action_redo").isEnabled());

    click (ed, "action_removeBand_1");
    CHECK (ed.getColumnOrder().empty());
    CHECK (ed.getSelectedSlot() == 0);
    CHECK (ed.getInspector().getSlot() == 0);

    while (ed.getColumnOrder().size() < (size_t) schema::kActiveSlots)
        click (ed, "action_addBand");

    CHECK (counter.getText() == "8 / 8");
    CHECK_FALSE (control<juce::Button> (ed, "action_addBand").isEnabled());
    CHECK (r.proc.getBandSlots().usedSlots().size() == (size_t) schema::kActiveSlots);
}

TEST_CASE ("Editor columns are ordered by frequency while identity stays with the slot", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    for (int i = 0; i < 3; ++i) r.proc.getBandSlots().createBand();   // defaults: 20, 120, 400 Hz
    ed.refreshFromProcessor();
    CHECK (ed.getColumnOrder() == Slots { 1, 2, 3 });

    setPlain (r.proc, id (1, schema::band::fLo), 5000.0f);
    setPlain (r.proc, id (1, schema::band::fHi), 9000.0f);
    ed.refreshFromProcessor();
    CHECK (ed.getColumnOrder() == Slots { 2, 3, 1 });

    // Geometry follows the order; widgets keep their slot's parameters.
    CHECK (ed.getColumn (2).getX() < ed.getColumn (3).getX());
    CHECK (ed.getColumn (3).getX() < ed.getColumn (1).getX());
    CHECK (findById (ed.getColumn (1), juce::String (id (1, schema::band::thresh))) != nullptr);
    CHECK (findById (ed.getColumn (2), juce::String (id (2, schema::band::thresh))) != nullptr);
    for (int slot : { 1, 2, 3 })
        CHECK (findById (ed.getColumn (slot), "action_removeBand_" + juce::String (slot)) != nullptr);
}

TEST_CASE ("Editor: every control is labelled for the parameter it changes", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    for (int i = 0; i < 2; ++i) r.proc.getBandSlots().createBand();
    ed.refreshFromProcessor();
    ed.selectSlot (2);

    // Accessible title of each control: "Band N <what it is>" (a swapped binding would show here).
    const std::vector<std::pair<const char*, const char*>> sliders {
        { schema::band::thresh, "Threshold" }, { schema::band::ratio, "Ratio" }, { schema::band::attack, "Attack" },
        { schema::band::release, "Release" }, { schema::band::fLo, "Low Frequency" }, { schema::band::fHi, "High Frequency" },
        { schema::band::knee, "Knee" }, { schema::band::range, "Range" }, { schema::band::makeup, "Makeup" },
        { schema::band::mix, "Mix" }, { schema::band::link, "Stereo Link" }, { schema::band::detBlend, "Detector RMS Blend" },
        { schema::band::rmsTime, "RMS Time" }, { schema::band::scLo, "Detector Low Frequency" },
        { schema::band::scHi, "Detector High Frequency" } };
    for (auto& [suffix, what] : sliders)
        CHECK (control<juce::Slider> (ed, id (2, suffix)).getTitle() == juce::String ("Band 2 ") + what);

    const std::vector<std::pair<const char*, const char*>> combos {
        { schema::band::detMode, "Detector" }, { schema::band::stereoMode, "Stereo Mode" }, { schema::band::scSource, "Detector Source" },
        { schema::band::scMode, "Detector Range" }, { schema::band::scChan, "Detector Channels" },
        { schema::band::scRef, "Detector Follows Band" } };
    for (auto& [suffix, what] : combos)
        CHECK (control<juce::ComboBox> (ed, id (2, suffix)).getTitle() == juce::String ("Band 2 ") + what);

    CHECK (control<juce::Button> (ed, id (2, schema::band::bypass)).getTitle() == "Band 2 Bypass");
    CHECK (control<juce::Button> (ed, "action_selectBand_2").getTitle() == "Select Band 2");
    CHECK (control<juce::Slider> (ed, schema::global::inGain).getTitle() == "Input Gain");
    CHECK (control<juce::Slider> (ed, schema::global::outGain).getTitle() == "Output Gain");
    CHECK (control<juce::Slider> (ed, schema::global::mix).getTitle() == "Global Mix");
    CHECK (control<juce::Button> (ed, schema::global::bypass).getTitle() == "Plugin Bypass");

    // The visible caption next to a knob says the same thing as its parameter name.
    for (auto* suffix : { schema::band::thresh, schema::band::ratio, schema::band::attack, schema::band::release })
    {
        auto& slider = control<juce::Slider> (ed, id (1, suffix));
        std::vector<juce::Label*> labels;
        collect (*slider.getParentComponent(), labels);
        const auto* spec = schema::findSpec (id (1, suffix));
        bool captionFound = false;
        for (auto* l : labels) captionFound = captionFound || juce::String (spec->name).endsWith (l->getText());
        INFO (suffix);
        CHECK (captionFound);
    }

    // The custom look is applied to controls that were built before they had a parent.
    auto& thresh = control<juce::Slider> (ed, id (1, schema::band::thresh));
    std::vector<juce::Label*> valueBoxes;
    collect (thresh, valueBoxes);
    REQUIRE (valueBoxes.size() == 1u);
    CHECK_THAT ((double) valueBoxes[0]->getFont().getHeight(), WithinAbs ((double) ui::theme::valueFontSize + 1.0, 0.01));
    CHECK (valueBoxes[0]->findColour (juce::Label::outlineColourId).isTransparent());

    // Unreadable typed text keeps the current value instead of jumping to zero.
    thresh.setValue (-24.0, juce::sendNotificationSync);
    CHECK_THAT (thresh.getValueFromText ("abc"), WithinAbs (-24.0, 1e-6));
    CHECK_THAT (thresh.getValueFromText (""), WithinAbs (-24.0, 1e-6));
}

TEST_CASE ("Editor: selection by mouse and keyboard button; removing another band keeps it", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    for (int i = 0; i < 3; ++i) r.proc.getBandSlots().createBand();
    ed.refreshFromProcessor();
    CHECK (ed.isRefreshing());                                   // the periodic refresh runs while the editor exists

    ed.selectSlot (5);                                           // not in use: nothing is selected
    CHECK (ed.getSelectedSlot() == 0);
    ed.selectSlot (99);
    CHECK (ed.getSelectedSlot() == 0);

    click (ed, "action_selectBand_2");                           // the slot chip (also reachable by keyboard)
    CHECK (ed.getSelectedSlot() == 2);
    CHECK (ed.getColumn (2).isSelected());
    CHECK_FALSE (ed.getColumn (1).isSelected());

    const auto mouseDownOn = [] (ui::BandColumn& column, juce::Component& target)
    {
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), { 2.0f, 2.0f }, juce::ModifierKeys::leftButtonModifier,
                                  juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                  juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                  juce::MouseInputSource::defaultTiltY, &target, &target, now, { 2.0f, 2.0f }, now, 1, false);
        column.mouseDown (e);
    };

    // A click on a knob of another column selects that band ...
    mouseDownOn (ed.getColumn (3), control<juce::Slider> (ed, id (3, schema::band::ratio)));
    CHECK (ed.getSelectedSlot() == 3);
    mouseDownOn (ed.getColumn (1), ed.getColumn (1));
    CHECK (ed.getSelectedSlot() == 1);

    // ... but a click on a remove button does not: removing band 2 leaves band 3 selected.
    ed.selectSlot (3);
    mouseDownOn (ed.getColumn (2), control<juce::Button> (ed, "action_removeBand_2"));
    CHECK (ed.getSelectedSlot() == 3);
    click (ed, "action_removeBand_2");
    CHECK (ed.getSelectedSlot() == 3);
    CHECK (ed.getColumnOrder() == Slots { 1, 3 });
}

TEST_CASE ("Editor: column order uses the lower edge, crossed edges are shown sorted", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    for (int i = 0; i < 3; ++i) r.proc.getBandSlots().createBand();

    // Lower edges 100 / 200 / 200 Hz, upper edges 5000 / 300 / 250 Hz: ordering by the upper edge
    // would give 3, 2, 1; equal lower edges keep slot order.
    setPlain (r.proc, id (1, schema::band::fLo), 100.0f); setPlain (r.proc, id (1, schema::band::fHi), 5000.0f);
    setPlain (r.proc, id (2, schema::band::fLo), 200.0f); setPlain (r.proc, id (2, schema::band::fHi), 300.0f);
    setPlain (r.proc, id (3, schema::band::fLo), 200.0f); setPlain (r.proc, id (3, schema::band::fHi), 250.0f);
    ed.refreshFromProcessor();
    CHECK (ed.getColumnOrder() == Slots { 1, 2, 3 });

    // Crossed parameters (low 8 kHz, high 50 Hz): the engine uses 50 Hz .. 8 kHz, and so does the UI.
    setPlain (r.proc, id (3, schema::band::fLo), 8000.0f);
    setPlain (r.proc, id (3, schema::band::fHi), 50.0f);
    ed.refreshFromProcessor();
    CHECK (ed.getColumnOrder() == Slots { 3, 1, 2 });
    CHECK_THAT ((double) ed.getColumn (3).getLowHz(), WithinRel (50.0, 1e-5));
    CHECK_THAT ((double) ed.getColumn (3).getHighHz(), WithinRel (8000.0, 1e-5));

    ed.selectSlot (3);
    ed.refreshFromProcessor();
    std::vector<juce::Label*> labels;
    collect (ed.getInspector(), labels);
    bool titleFound = false;
    for (auto* l : labels) titleFound = titleFound || l->getText() == "BAND 3   50.0 Hz - 8.00 kHz";
    CHECK (titleFound);
}

TEST_CASE ("Editor meters: both channels, nothing stale after hide / show or when audio stops", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    REQUIRE (r.proc.getBandSlots().createBand() == 1);
    setPlain (r.proc, id (1, schema::band::fLo), 200.0f);
    setPlain (r.proc, id (1, schema::band::fHi), 5000.0f);
    setPlain (r.proc, id (1, schema::band::thresh), -40.0f);
    setPlain (r.proc, id (1, schema::band::ratio), 8.0f);
    setPlain (r.proc, id (1, schema::band::link), 0.0f);
    ed.refreshFromProcessor();
    CHECK_FALSE (ed.isAudioRunning());                           // nothing processed yet

    test::OfflineRenderer renderer (r.proc);
    REQUIRE (renderer.prepare (48000.0, 512, {}));
    auto rightOnly = test::makeNoise (2, 24000, 3u, 0.7f);
    rightOnly.clear (0, 0, rightOnly.getNumSamples());

    // Only the right channel is loud and the band is unlinked: the bar shows the deeper channel.
    renderer.render (rightOnly, nullptr, { 512 });
    ed.refreshFromProcessor();
    CHECK (ed.isAudioRunning());
    CHECK (r.proc.getGainReductionMeter().getGainReductionDb (1, 0) == 0.0f);
    CHECK (ed.getColumn (1).getGrBar().getCurrentDb() < -3.0f);

    // Bypass dims the column's controls.
    CHECK (control<juce::Slider> (ed, id (1, schema::band::thresh)).getParentComponent()->getAlpha() == 1.0f);
    setPlain (r.proc, id (1, schema::band::bypass), 1.0f);
    ed.refreshFromProcessor();
    CHECK (control<juce::Slider> (ed, id (1, schema::band::thresh)).getParentComponent()->getAlpha() < 0.6f);
    setPlain (r.proc, id (1, schema::band::bypass), 0.0f);

    // Remove while compressing, keep playing, undo: the old reduction must not reappear.
    renderer.render (rightOnly, nullptr, { 512 });
    ed.refreshFromProcessor();
    click (ed, "action_removeBand_1");
    renderer.render (rightOnly, nullptr, { 512 });               // fades out; the hidden column polls nothing
    click (ed, "action_undo");
    CHECK (ed.getColumn (1).isVisible());
    CHECK (ed.getColumn (1).getGrBar().getHeldDb() == 0.0f);
    CHECK (ed.getColumn (1).getGrBar().getCurrentDb() == 0.0f);

    // Playing again shows reduction; when the host stops processing, the meters and the response
    // curve return to "no reduction" instead of keeping the last value.
    renderer.render (rightOnly, nullptr, { 512 });
    ed.refreshFromProcessor();
    CHECK (ed.getColumn (1).getGrBar().getCurrentDb() < -3.0f);
    CHECK (ed.getSpectrum().bandGain (1) < 0.8);
    for (int i = 0; i < ui::PluginEditor::idleTicksBeforeMetersClear + 1; ++i)
        ed.refreshFromProcessor();
    CHECK_FALSE (ed.isAudioRunning());
    CHECK (ed.getColumn (1).getGrBar().getCurrentDb() == 0.0f);
    CHECK (ed.getSpectrum().bandGain (1) == 1.0);
}

TEST_CASE ("Band edits tell the host that non-parameter state changed", "[ui][editor][state]")
{
    struct Listener : juce::AudioProcessorListener
    {
        int nonParameterChanges = 0;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override
        {
            if (d.nonParameterStateChanged) ++nonParameterChanges;
        }
    };

    Rig r;
    REQUIRE (r.editor != nullptr);
    Listener listener;
    r.proc.addListener (&listener);

    click (*r.editor, "action_addBand");
    CHECK (listener.nonParameterChanges == 1);
    click (*r.editor, "action_removeBand_1");
    CHECK (listener.nonParameterChanges == 2);
    click (*r.editor, "action_undo");
    CHECK (listener.nonParameterChanges == 3);
    click (*r.editor, "action_redo");
    CHECK (listener.nonParameterChanges == 4);

    // Creating a band by dragging in the spectrum counts too.
    auto& v = r.editor->getSpectrum();
    const float y = v.plotBounds().getCentreY();
    v.beginDrag ({ v.xForFrequency (1000.0), y });
    v.continueDrag ({ v.xForFrequency (4000.0), y });
    v.endDrag();
    CHECK (listener.nonParameterChanges == 5);

    r.proc.removeListener (&listener);
}

TEST_CASE ("Editor follows a state reload while open", "[ui][editor]")
{
    juce::MemoryBlock saved;
    {
        PluginProcessor src;
        REQUIRE (src.getBandSlots().createBand() == 1);
        REQUIRE (src.getBandSlots().createBand() == 2);
        REQUIRE (src.getBandSlots().createBand() == 3);
        REQUIRE (src.getBandSlots().deleteBand (2));
        setPlain (src, id (3, schema::band::thresh), -33.0f);
        setPlain (src, id (3, schema::band::fLo), 700.0f);
        setPlain (src, id (3, schema::band::detMode), 1.0f);
        setPlain (src, id (1, schema::band::bypass), 1.0f);
        src.getStateInformation (saved);
    }

    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    for (int i = 0; i < 2; ++i) r.proc.getBandSlots().createBand();
    ed.refreshFromProcessor();
    ed.selectSlot (2);
    REQUIRE (ed.getSelectedSlot() == 2);

    r.proc.setStateInformation (saved.getData(), (int) saved.getSize());
    ed.refreshFromProcessor();

    CHECK (ed.getColumnOrder() == Slots { 1, 3 });
    CHECK (ed.getSelectedSlot() == 1);                          // slot 2 no longer exists
    CHECK_FALSE (control<juce::Button> (ed, "action_undo").isEnabled());   // history cleared by the load
    CHECK (control<juce::Button> (ed, id (1, schema::band::bypass)).getToggleState());
    CHECK_THAT (control<juce::Slider> (ed, id (3, schema::band::thresh)).getValue(), WithinAbs (-33.0, 1e-4));

    ed.selectSlot (3);
    CHECK_THAT (control<juce::Slider> (ed, id (3, schema::band::fLo)).getValue(), WithinRel (700.0, 1e-5));
    CHECK (control<juce::ComboBox> (ed, id (3, schema::band::detMode)).getSelectedItemIndex() == 1);

    // Detector-dependent enablement: RMS mode uses the RMS window but not the blend amount.
    CHECK (control<juce::Slider> (ed, id (3, schema::band::rmsTime)).isEnabled());
    CHECK_FALSE (control<juce::Slider> (ed, id (3, schema::band::detBlend)).isEnabled());
    setPlain (r.proc, id (3, schema::band::detMode), 0.0f);
    ed.refreshFromProcessor();
    CHECK_FALSE (control<juce::Slider> (ed, id (3, schema::band::rmsTime)).isEnabled());
    setPlain (r.proc, id (3, schema::band::detMode), 2.0f);
    ed.refreshFromProcessor();
    CHECK (control<juce::Slider> (ed, id (3, schema::band::rmsTime)).isEnabled());
    CHECK (control<juce::Slider> (ed, id (3, schema::band::detBlend)).isEnabled());
}

TEST_CASE ("Editor inspector: tabs, routing enablement and the sidechain warning", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    auto& insp = ed.getInspector();
    for (int i = 0; i < 3; ++i) r.proc.getBandSlots().createBand();
    ed.refreshFromProcessor();
    ed.selectSlot (2);

    // Tabs switch which controls are shown.
    auto& lowFreq = control<juce::Slider> (ed, id (2, schema::band::fLo));
    auto& source = control<juce::ComboBox> (ed, id (2, schema::band::scSource));
    CHECK (insp.getTab() == ui::BandInspector::Tab::Band);
    CHECK (lowFreq.getParentComponent()->isVisible());
    CHECK_FALSE (source.getParentComponent()->isVisible());
    click (ed, "action_inspectorTab_detector");
    CHECK (insp.getTab() == ui::BandInspector::Tab::Detector);
    CHECK_FALSE (lowFreq.getParentComponent()->isVisible());
    CHECK (source.getParentComponent()->isVisible());
    ed.selectSlot (3);                                           // the tab is kept when the selection changes
    CHECK (insp.getTab() == ui::BandInspector::Tab::Detector);
    ed.selectSlot (2);

    // Detector range: follow-band list and custom edges are enabled by the mode.
    auto& follow = control<juce::ComboBox> (ed, id (2, schema::band::scRef));
    auto& detLow = control<juce::Slider> (ed, id (2, schema::band::scLo));
    CHECK_FALSE (follow.isEnabled());
    CHECK_FALSE (detLow.isEnabled());
    setPlain (r.proc, id (2, schema::band::scMode), (float) schema::scModeBand);
    ed.refreshFromProcessor();
    CHECK (follow.isEnabled());
    CHECK_FALSE (detLow.isEnabled());
    CHECK (follow.getNumItems() == 2);                           // bands 1 and 3
    CHECK (follow.getItemText (0) == "Band 1");
    CHECK (follow.getItemText (1) == "Band 3");
    setPlain (r.proc, id (2, schema::band::scMode), (float) schema::scModeCustom);
    ed.refreshFromProcessor();
    CHECK_FALSE (follow.isEnabled());
    CHECK (detLow.isEnabled());

    // A reference to a band that is not in use is shown as such instead of being hidden.
    setPlain (r.proc, id (2, schema::band::scMode), (float) schema::scModeBand);
    setPlain (r.proc, id (2, schema::band::scRef), 6.0f);
    ed.refreshFromProcessor();
    CHECK (follow.getText() == "Band 6 (unused)");

    // Removing a followed band converts the follower to a custom range, and the UI follows.
    setPlain (r.proc, id (2, schema::band::scRef), 1.0f);
    ed.refreshFromProcessor();
    CHECK (follow.getText() == "Band 1");
    REQUIRE (r.proc.getBandSlots().deleteBand (1));
    ed.refreshFromProcessor();
    CHECK (control<juce::ComboBox> (ed, id (2, schema::band::scMode)).getSelectedItemIndex() == schema::scModeCustom);
    CHECK (detLow.isEnabled());
    CHECK_THAT (detLow.getValue(), WithinRel ((double) schema::slotDefaultLo (1), 1e-5));

    // External source without a connected sidechain bus: a visible warning, not a silent no-op.
    CHECK_FALSE (insp.isSidechainWarningShown());
    CHECK (r.proc.getSidechainChannelCount() == 0);
    setPlain (r.proc, id (2, schema::band::scSource), 1.0f);
    ed.refreshFromProcessor();
    CHECK (insp.isSidechainWarningShown());
    {
        test::OfflineRenderer renderer (r.proc);
        REQUIRE (renderer.prepare (48000.0, 256, { 2, 2 }));     // the host connects a stereo sidechain
        ed.refreshFromProcessor();
        CHECK (r.proc.getSidechainChannelCount() == 2);
        CHECK_FALSE (insp.isSidechainWarningShown());
        REQUIRE (renderer.prepare (48000.0, 256, { 1, 0 }));     // mono track, no sidechain
        ed.refreshFromProcessor();
        CHECK (insp.isSidechainWarningShown());
    }

    // Mono: stereo mode and link do not apply. Stereo: the link only acts in Stereo mode.
    auto& stereoMode = control<juce::ComboBox> (ed, id (2, schema::band::stereoMode));
    auto& link = control<juce::Slider> (ed, id (2, schema::band::link));
    CHECK_FALSE (stereoMode.isEnabled());
    CHECK_FALSE (link.isEnabled());
    {
        test::OfflineRenderer renderer (r.proc);
        REQUIRE (renderer.prepare (48000.0, 256, { 2, 0 }));
    }
    ed.refreshFromProcessor();
    CHECK (stereoMode.isEnabled());
    CHECK (link.isEnabled());
    setPlain (r.proc, id (2, schema::band::stereoMode), 4.0f);   // Mid/Side
    ed.refreshFromProcessor();
    CHECK_FALSE (link.isEnabled());

    // Nothing selected: no warning, no tabs.
    ed.selectSlot (0);
    CHECK_FALSE (insp.isSidechainWarningShown());
    CHECK_FALSE (control<juce::Button> (ed, "action_inspectorTab_band").isVisible());
}

TEST_CASE ("Editor global bar: automatic gain read-out and clipper controls", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;

    auto& autoButton = control<juce::Button> (ed, schema::global::autoGain);
    auto& clip = control<juce::ComboBox> (ed, schema::global::clipMode);
    auto& ceiling = control<juce::Slider> (ed, schema::global::clipCeiling);
    CHECK (autoButton.getTitle() == "Automatic Gain");
    CHECK (clip.getTitle() == "Output Clip Mode");
    CHECK (ceiling.getTitle() == "Clip Ceiling");

    // Clip: three modes; the ceiling is only enabled while the clipper is on.
    CHECK (clip.getNumItems() == 3);
    CHECK (clip.getItemText (0) == "Off");
    CHECK (clip.getSelectedItemIndex() == 0);
    CHECK_FALSE (ceiling.isEnabled());
    clip.setSelectedItemIndex (2, juce::sendNotificationSync);
    CHECK (plain (r.proc, schema::global::clipMode) == 2.0f);
    ed.refreshFromProcessor();
    CHECK (ceiling.isEnabled());
    CHECK (ceiling.getTextFromValue (-0.3) == "-0.3 dB");
    ceiling.setValue (-3.0, juce::sendNotificationSync);
    CHECK_THAT ((double) plain (r.proc, schema::global::clipCeiling), WithinAbs (-3.0, 1e-5));

    // Automatic gain: no read-out while off; the applied correction while on.
    CHECK (ed.getAutoGainText().isEmpty());
    r.proc.getBandSlots().createBand();
    setPlain (r.proc, id (1, schema::band::fLo), 20.0f);
    setPlain (r.proc, id (1, schema::band::fHi), 20000.0f);
    setPlain (r.proc, id (1, schema::band::thresh), -50.0f);
    setPlain (r.proc, id (1, schema::band::ratio), 4.0f);
    setPlain (r.proc, schema::global::clipMode, 0.0f);
    autoButton.setToggleState (true, juce::sendNotificationSync);
    CHECK (plain (r.proc, schema::global::autoGain) == 1.0f);

    test::OfflineRenderer renderer (r.proc);
    REQUIRE (renderer.prepare (48000.0, 512, {}));
    renderer.render (test::makeNoise (2, 4 * 48000, 17u, 0.3f), nullptr, { 512 });
    ed.refreshFromProcessor();
    const float correction = r.proc.getEngine().getAutoGainDb();
    REQUIRE (correction > 3.0f);
    CHECK (ed.getAutoGainText() == "+" + juce::String (correction, 1) + " dB");

    autoButton.setToggleState (false, juce::sendNotificationSync);
    ed.refreshFromProcessor();
    CHECK (ed.getAutoGainText().isEmpty());
}

TEST_CASE ("Editor lifecycle: repeated open / close around processing and topology changes", "[ui][editor]")
{
    PluginProcessor p;
    test::OfflineRenderer renderer (p);
    REQUIRE (renderer.prepare (48000.0, 256, {}));
    const auto noise = test::makeNoise (2, 1024, 7u, 0.5f);

    for (int i = 0; i < 60; ++i)
    {
        auto holder = openEditor (p);
        auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
        REQUIRE (ed != nullptr);

        if (i % 3 == 0) click (*ed, p.getBandSlots().usedSlots().size() < 8 ? "action_addBand" : "action_removeBand_1");
        const auto used = p.getBandSlots().usedSlots();
        if (! used.empty()) ed->selectSlot (used[(size_t) i % used.size()]);

        renderer.render (noise, nullptr, { 64, 256, 17 });
        ed->refreshFromProcessor();
        ed->setSize (ui::PluginEditor::minWidth + (i * 13) % 400, ui::PluginEditor::defaultHeight);

        if (i % 7 == 0 && ! used.empty())
        {
            // Remove the selected band while its inspector is bound, then close immediately.
            REQUIRE (p.getBandSlots().deleteBand (ed->getSelectedSlot()));
            ed->refreshFromProcessor();
        }

        // The editor agrees with the processor at every step ...
        auto shown = ed->getColumnOrder();
        std::sort (shown.begin(), shown.end());
        REQUIRE (shown == p.getBandSlots().usedSlots());
        REQUIRE ((ed->getSelectedSlot() == 0 || p.getBandSlots().isUsed (ed->getSelectedSlot())));
        REQUIRE (ed->getInspector().getSlot() == ed->getSelectedSlot());
        REQUIRE (p.getActiveEditor() == ed);
        REQUIRE (p.isAnalyzerActive());

        // ... and closing it leaves nothing behind.
        holder.reset();
        REQUIRE (p.getActiveEditor() == nullptr);
        REQUIRE_FALSE (p.isAnalyzerActive());
    }

    // Closing the editor before or after a state change leaves the processor usable.
    juce::MemoryBlock state;
    p.getStateInformation (state);
    {
        auto holder = openEditor (p);
        p.setStateInformation (state.getData(), (int) state.getSize());
    }
    const auto out = renderer.render (noise, nullptr, { 256 });
    CHECK (out.getNumSamples() == noise.getNumSamples());
    CHECK (p.getActiveEditor() == nullptr);
}

TEST_CASE ("Audio is bit-identical with and without an open editor", "[ui][editor]")
{
    const auto configure = [] (PluginProcessor& p)
    {
        for (int i = 0; i < 3; ++i) p.getBandSlots().createBand();
        setPlain (p, id (1, schema::band::thresh), -40.0f);
        setPlain (p, id (1, schema::band::ratio), 8.0f);
        setPlain (p, id (2, schema::band::thresh), -35.0f);
        setPlain (p, id (2, schema::band::detMode), 1.0f);
        setPlain (p, id (3, schema::band::thresh), -30.0f);
        setPlain (p, id (3, schema::band::link), 0.0f);
    };

    const auto noise = test::makeNoise (2, 48000, 99u, 0.7f);
    const std::vector<int> blocks { 64, 480, 1, 512, 33 };

    PluginProcessor headless;
    configure (headless);
    test::OfflineRenderer r1 (headless);
    REQUIRE (r1.prepare (48000.0, 512, {}));
    const auto reference = r1.render (noise, nullptr, blocks);

    PluginProcessor withEditor;
    configure (withEditor);
    auto holder = openEditor (withEditor);
    auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
    REQUIRE (ed != nullptr);
    test::OfflineRenderer r2 (withEditor);
    REQUIRE (r2.prepare (48000.0, 512, {}));

    const auto out = r2.render (noise, nullptr, blocks, false, [ed] (int block, PluginProcessor&)
    {
        ed->refreshFromProcessor();                       // meter polling between blocks
        ed->selectSlot (1 + block % 3);                   // inspector re-binding
        if (block % 5 == 0) ed->setSize (ui::PluginEditor::minWidth + (block * 7) % 500, ui::PluginEditor::defaultHeight);
    });

    CHECK (test::bitIdentical (reference, out));

    // The compressor really was working (otherwise the comparison proves nothing).
    CHECK_FALSE (test::bitIdentical (reference, noise));
}

TEST_CASE ("Editor GR read-out follows the meter and holds the deepest value", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    REQUIRE (r.proc.getBandSlots().createBand() == 1);
    setPlain (r.proc, id (1, schema::band::fLo), 200.0f);
    setPlain (r.proc, id (1, schema::band::fHi), 5000.0f);
    setPlain (r.proc, id (1, schema::band::thresh), -40.0f);
    setPlain (r.proc, id (1, schema::band::ratio), 8.0f);
    ed.refreshFromProcessor();
    CHECK (ed.getColumn (1).getGrBar().getCurrentDb() == 0.0f);

    test::OfflineRenderer renderer (r.proc);
    REQUIRE (renderer.prepare (48000.0, 512, {}));
    renderer.render (test::makeNoise (2, 24000, 3u, 0.7f), nullptr, { 512 });
    ed.refreshFromProcessor();

    const float shown = ed.getColumn (1).getGrBar().getCurrentDb();
    CHECK (shown < -3.0f);
    CHECK (shown >= -24.0f);                                // Range default
    CHECK (ed.getColumn (1).getGrBar().getHeldDb() <= shown);

    // The bar itself: deepest value is held for one second, then follows the current value.
    ui::GrBar bar (juce::Colours::white);
    bar.update (-6.0f, 0.0);
    bar.update (-2.0f, 500.0);
    CHECK (bar.getCurrentDb() == -2.0f);
    CHECK (bar.getHeldDb() == -6.0f);
    bar.update (-2.0f, 1001.0);
    CHECK (bar.getHeldDb() == -2.0f);
    bar.update (0.5f, 1002.0);                              // positive input is clamped to 0
    CHECK (bar.getCurrentDb() == 0.0f);
}

TEST_CASE ("Editor layout: eight bands fit without overlap and controls keep their minimum sizes", "[ui][editor]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& ed = *r.editor;
    for (int i = 0; i < schema::kActiveSlots; ++i) r.proc.getBandSlots().createBand();
    ed.refreshFromProcessor();
    ed.selectSlot (4);

    for (int width : { ui::PluginEditor::minWidth, ui::PluginEditor::defaultWidth, ui::PluginEditor::maxWidth })
    {
        INFO ("editor width " << width);
        ed.setSize (width, ui::PluginEditor::defaultHeight);

        juce::Rectangle<int> previous;
        for (int slot : ed.getColumnOrder())
        {
            auto& col = ed.getColumn (slot);
            INFO ("slot " << slot);
            CHECK (col.isVisible());
            CHECK (ed.getLocalBounds().contains (col.getBounds()));
            CHECK (col.getWidth() >= ui::theme::columnMinWidth);
            CHECK_FALSE (col.getBounds().intersects (previous));
            CHECK_FALSE (col.getBounds().intersects (ed.getInspector().getBounds()));
            previous = col.getBounds();

            for (auto* suffix : { schema::band::thresh, schema::band::ratio })
            {
                auto& s = control<juce::Slider> (col, id (slot, suffix));
                CHECK (juce::jmin (s.getWidth(), s.getHeight() - 16) >= ui::theme::knobPrimary);
            }
            for (auto* suffix : { schema::band::attack, schema::band::release })
            {
                auto& s = control<juce::Slider> (col, id (slot, suffix));
                CHECK (juce::jmin (s.getWidth(), s.getHeight() - 16) >= ui::theme::knobSecondary);
            }
        }

        // WCAG 2.2 target size: every button is at least 24 x 24.
        std::vector<juce::Button*> buttons; collect (ed, buttons);
        for (auto* b : buttons)
        {
            if (! b->isShowing() && ! b->isVisible()) continue;
            INFO ("button '" << b->getComponentID() << "'");
            CHECK (b->getWidth() >= 24);
            CHECK (b->getHeight() >= 24);
        }

        for (auto* suffix : kInspectorSuffixes)
        {
            auto* c = findById (ed.getInspector(), juce::String (id (4, suffix)));
            INFO ("inspector control " << suffix);
            REQUIRE (c != nullptr);
            CHECK (c->getWidth() >= 32);
            CHECK (c->getHeight() >= 20);
            CHECK (ed.getInspector().getLocalBounds().contains (c->getBoundsInParent().translated (c->getParentComponent()->getX(), c->getParentComponent()->getY())));
        }
    }
}

TEST_CASE ("Theme tokens meet the contrast targets", "[ui][theme]")
{
    using namespace ui::theme;

    // Text: at least 4.5:1 on every surface it is drawn on.
    for (auto bg : { background, panel, surface, elevated })
    {
        CHECK (contrastRatio (textHi, bg) >= 4.5);
        CHECK (contrastRatio (textMid, bg) >= 4.5);
    }
    for (auto bg : { background, panel, surface })
        CHECK (contrastRatio (textLow, bg) >= 4.5);

    CHECK (contrastRatio (warning, panel) >= 4.5);          // engine tag
    CHECK (contrastRatio (background, textMid) >= 4.5);     // lit toggle: dark text on light fill
    CHECK (contrastRatio (background, warning) >= 4.5);     // lit plugin bypass

    // Band colours: fills and outlines (>= 3:1), and the slot number drawn on the chip (>= 4.5:1).
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        INFO ("slot " << slot);
        CHECK (contrastRatio (slotColour (slot), panel) >= 3.0);
        CHECK (contrastRatio (slotColour (slot), surface) >= 3.0);
        CHECK (contrastRatio (slotColour (slot), background) >= 3.0);
        CHECK (contrastRatio (background, slotColour (slot)) >= 4.5);
    }

    // Slot colours are pairwise distinct.
    for (int a = 1; a <= schema::kActiveSlots; ++a)
        for (int b = a + 1; b <= schema::kActiveSlots; ++b)
            CHECK (slotColour (a) != slotColour (b));
}

// Hidden (not run by ctest): renders the editor off-screen to PNG files for visual inspection.
//   TL_SCREENSHOT_DIR=/some/dir tl_tests "[.screenshot]"
TEST_CASE ("Editor screenshots", "[.screenshot]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("TL_SCREENSHOT_DIR", {});
    if (dir.isEmpty()) SKIP ("TL_SCREENSHOT_DIR is not set");

    const auto save = [&dir] (juce::Component& c, const juce::String& name)
    {
        const auto image = c.createComponentSnapshot (c.getLocalBounds(), false, 2.0f);
        juce::File file (juce::File (dir).getChildFile (name + ".png"));
        file.deleteFile();
        juce::FileOutputStream out (file);
        REQUIRE (out.openedOk());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, out));
    };

    {
        Rig r;
        REQUIRE (r.editor != nullptr);
        save (*r.editor, "editor_empty");
    }

    for (int bands : { 3, 8 })
    {
        Rig r;
        REQUIRE (r.editor != nullptr);
        for (int i = 0; i < bands; ++i) r.proc.getBandSlots().createBand();
        setPlain (r.proc, id (1, schema::band::thresh), -40.0f);
        setPlain (r.proc, id (1, schema::band::ratio), 8.0f);
        setPlain (r.proc, id (2, schema::band::thresh), -32.0f);
        setPlain (r.proc, id (3, schema::band::bypass), 1.0f);
        setPlain (r.proc, id (2, schema::band::detMode), 2.0f);

        test::OfflineRenderer renderer (r.proc);
        REQUIRE (renderer.prepare (48000.0, 512, {}));
        renderer.render (test::makeNoise (2, 24000, 3u, 0.7f), nullptr, { 512 });

        r.editor->refreshFromProcessor();
        r.editor->selectSlot (2);
        r.editor->refreshFromProcessor();
        save (*r.editor, "editor_" + juce::String (bands) + "_bands");

        if (bands == 3)
        {
            setPlain (r.proc, id (2, schema::band::scSource), 1.0f);
            setPlain (r.proc, id (2, schema::band::scMode), (float) schema::scModeBand);
            r.editor->getInspector().setTab (ui::BandInspector::Tab::Detector);
            r.editor->refreshFromProcessor();
            save (*r.editor, "editor_3_bands_detector_tab");
        }

        if (bands == 8)
        {
            r.editor->setSize (ui::PluginEditor::minWidth, ui::PluginEditor::defaultHeight);
            save (*r.editor, "editor_8_bands_min_width");
        }
    }
}
