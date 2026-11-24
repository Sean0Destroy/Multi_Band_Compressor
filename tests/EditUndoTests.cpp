// Knob-edit undo (ParameterUndo, one step per gesture in the band history) and the editor's
// window scale.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <vector>

#include "PluginProcessor.h"
#include "ui/PluginEditor.h"

using namespace tl;
using Catch::Matchers::WithinAbs;

namespace
{
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

std::string id (int slot, const char* suffix) { return schema::bandParamId (slot, suffix); }

juce::RangedAudioParameter& param (PluginProcessor& p, const std::string& paramId)
{
    auto* prm = p.getState().getParameter (paramId);
    REQUIRE (prm != nullptr);
    return *prm;
}

float plain (PluginProcessor& p, const std::string& paramId) { return p.getState().getRawParameterValue (paramId)->load(); }

/** What a knob drag, a typed value or a combo click does: begin, value(s), end. */
void userEdit (PluginProcessor& p, const std::string& paramId, float plainValue)
{
    auto& prm = param (p, paramId);
    prm.beginChangeGesture();
    prm.setValueNotifyingHost (prm.convertTo0to1 (plainValue));
    prm.endChangeGesture();
}

/** What host automation does: a value, no gesture. */
void hostEdit (PluginProcessor& p, const std::string& paramId, float plainValue)
{
    auto& prm = param (p, paramId);
    prm.setValueNotifyingHost (prm.convertTo0to1 (plainValue));
}

/** Counts what the "host" is told. */
struct HostSpy final : public juce::AudioProcessorListener
{
    explicit HostSpy (juce::AudioProcessor& proc) : p (proc) { p.addListener (this); }
    ~HostSpy() override { p.removeListener (this); }

    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float) override { changed.push_back (index); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { ++begins; }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override { ++ends; }

    juce::AudioProcessor& p;
    std::vector<int> changed;
    int begins = 0, ends = 0;
};
} // namespace

TEST_CASE ("Knob undo: one gesture is one step; undo and redo restore both values", "[undo]")
{
    PluginProcessor p;
    auto& slots = p.getBandSlots();
    REQUIRE (slots.createBand() == 1);
    const auto thresh = id (1, schema::band::thresh);
    const float before = plain (p, thresh);

    userEdit (p, thresh, -30.0f);
    CHECK_THAT (plain (p, thresh), WithinAbs (-30.0, 0.05));
    REQUIRE (slots.canUndo());

    REQUIRE (slots.undoLast());
    CHECK_THAT (plain (p, thresh), WithinAbs (before, 1.0e-4));
    CHECK (slots.isUsed (1));                        // only the knob step was undone
    REQUIRE (slots.canRedo());

    REQUIRE (slots.redoLast());
    CHECK_THAT (plain (p, thresh), WithinAbs (-30.0, 0.05));

    // Undo walks back through the band step too, in order.
    REQUIRE (slots.undoLast());
    REQUIRE (slots.undoLast());
    CHECK_FALSE (slots.isUsed (1));
    CHECK_FALSE (slots.canUndo());
}

TEST_CASE ("Knob undo: a multi-step drag and a two-parameter gesture each make one step", "[undo]")
{
    PluginProcessor p;
    auto& slots = p.getBandSlots();
    REQUIRE (slots.createBand() == 1);
    slots.clearHistory();

    // A drag with many intermediate values.
    const auto ratio = id (1, schema::band::ratio);
    const float ratioBefore = plain (p, ratio);
    auto& r = param (p, ratio);
    r.beginChangeGesture();
    for (float v : { 2.0f, 3.0f, 5.0f, 8.0f })
        r.setValueNotifyingHost (r.convertTo0to1 (v));
    r.endChangeGesture();

    // Moving a band in the spectrum: both edges in one gesture.
    const auto lo = id (1, schema::band::fLo), hi = id (1, schema::band::fHi);
    const float loBefore = plain (p, lo), hiBefore = plain (p, hi);
    auto& l = param (p, lo);
    auto& h = param (p, hi);
    l.beginChangeGesture();
    h.beginChangeGesture();
    l.setValueNotifyingHost (l.convertTo0to1 (300.0f));
    h.setValueNotifyingHost (h.convertTo0to1 (1200.0f));
    l.endChangeGesture();
    h.endChangeGesture();

    REQUIRE (slots.undoLast());                       // both edges at once
    CHECK_THAT (plain (p, lo), WithinAbs (loBefore, loBefore * 1.0e-4));
    CHECK_THAT (plain (p, hi), WithinAbs (hiBefore, hiBefore * 1.0e-4));
    CHECK_THAT (plain (p, ratio), WithinAbs (8.0, 0.01));

    REQUIRE (slots.undoLast());                       // the whole drag
    CHECK_THAT (plain (p, ratio), WithinAbs (ratioBefore, 1.0e-4));
    CHECK_FALSE (slots.canUndo());
}

TEST_CASE ("Knob undo: what is not recorded", "[undo]")
{
    PluginProcessor p;
    auto& slots = p.getBandSlots();
    REQUIRE (slots.createBand() == 1);
    slots.clearHistory();

    SECTION ("a gesture that ends where it started")
    {
        const auto thresh = id (1, schema::band::thresh);
        const float v = plain (p, thresh);
        userEdit (p, thresh, -40.0f);
        userEdit (p, thresh, v);                      // two steps; then one that changes nothing
        auto& prm = param (p, thresh);
        prm.beginChangeGesture();
        prm.endChangeGesture();
        REQUIRE (slots.undoLast());
        REQUIRE (slots.undoLast());
        CHECK_FALSE (slots.canUndo());
    }

    SECTION ("host automation (no gesture)")
    {
        hostEdit (p, id (1, schema::band::attack), 50.0f);
        hostEdit (p, schema::global::outGain, -3.0f);
        CHECK_FALSE (slots.canUndo());
    }

    SECTION ("the host bypass")
    {
        userEdit (p, schema::global::bypass, 1.0f);
        CHECK_FALSE (slots.canUndo());
    }

    SECTION ("a gesture that a state load interrupts")
    {
        juce::MemoryBlock state;
        p.getStateInformation (state);

        auto& prm = param (p, id (1, schema::band::release));
        prm.beginChangeGesture();
        prm.setValueNotifyingHost (prm.convertTo0to1 (400.0f));
        p.setStateInformation (state.getData(), (int) state.getSize());
        prm.endChangeGesture();
        CHECK_FALSE (slots.canUndo());
    }
}

TEST_CASE ("Knob undo: an undo does not record itself and keeps the redo step", "[undo]")
{
    PluginProcessor p;
    auto& slots = p.getBandSlots();
    REQUIRE (slots.createBand() == 1);
    slots.clearHistory();

    const auto makeup = id (1, schema::band::makeup);
    userEdit (p, makeup, 4.0f);
    userEdit (p, makeup, 6.0f);

    HostSpy host (p);
    REQUIRE (slots.undoLast());
    CHECK_THAT (plain (p, makeup), WithinAbs (4.0, 0.01));

    // The host is told, as a gesture (a user edit), after the mutex was released ...
    CHECK (host.begins == 1);
    CHECK (host.ends == 1);
    REQUIRE (host.changed.size() == 1);
    CHECK (host.changed.front() == param (p, makeup).getParameterIndex());

    // ... and that gesture did not become a new step (it would have cleared the redo step).
    p.getParameterUndo().flush();
    REQUIRE (slots.canRedo());
    REQUIRE (slots.redoLast());
    CHECK_THAT (plain (p, makeup), WithinAbs (6.0, 0.01));
    REQUIRE (slots.undoLast());
    REQUIRE (slots.undoLast());
    CHECK_THAT (plain (p, makeup), WithinAbs (0.0, 0.01));
    CHECK_FALSE (slots.canUndo());
}

TEST_CASE ("Knob undo: a finished edit takes its place before a band is added", "[undo]")
{
    PluginProcessor p;
    auto& slots = p.getBandSlots();
    REQUIRE (slots.createBand() == 1);
    slots.clearHistory();

    // No message loop runs here: the step is committed by the history access, not the queue.
    userEdit (p, schema::global::inGain, 5.0f);
    REQUIRE (slots.createBand() == 2);

    REQUIRE (slots.undoLast());                       // the band first ...
    CHECK_FALSE (slots.isUsed (2));
    CHECK_THAT (plain (p, schema::global::inGain), WithinAbs (5.0, 0.01));
    REQUIRE (slots.undoLast());                       // ... then the knob
    CHECK_THAT (plain (p, schema::global::inGain), WithinAbs (0.0, 0.01));
}

TEST_CASE ("Knob undo: a latency parameter brings the reported latency back", "[undo]")
{
    PluginProcessor p;
    p.setPlayConfigDetails (2, 2, 48000.0, 256);
    p.prepareToPlay (48000.0, 256);
    auto& slots = p.getBandSlots();
    REQUIRE (slots.createBand() == 1);
    slots.clearHistory();
    const int latencyBefore = p.getLatencySamples();

    userEdit (p, id (1, schema::band::lookahead), 5.0f);
    const int latencyWith = p.getLatencySamples();
    CHECK (latencyWith > latencyBefore);

    REQUIRE (slots.undoLast());
    CHECK (p.getLatencySamples() == latencyBefore);
    REQUIRE (slots.redoLast());
    CHECK (p.getLatencySamples() == latencyWith);
    p.releaseResources();
}

TEST_CASE ("Editor: knob edits through a control and the Undo button", "[undo][ui][editor]")
{
    PluginProcessor p;
    REQUIRE (p.getBandSlots().createBand() == 1);
    p.getBandSlots().clearHistory();
    auto holder = openEditor (p);
    auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
    REQUIRE (ed != nullptr);
    ed->refreshFromProcessor();

    // The column's threshold knob, as the user would turn it (a drag notification around it).
    juce::Slider* knob = nullptr;
    std::function<void (juce::Component&)> find = [&] (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (child->getComponentID() == juce::String (id (1, schema::band::thresh)))
                if (auto* s = dynamic_cast<juce::Slider*> (child)) knob = s;
            find (*child);
        }
    };
    find (*ed);
    REQUIRE (knob != nullptr);

    const double before = knob->getValue();
    {
        const juce::Slider::ScopedDragNotification drag (*knob);
        knob->setValue (-35.0, juce::sendNotificationSync);
    }
    CHECK_THAT (plain (p, id (1, schema::band::thresh)), WithinAbs (-35.0, 0.05));

    ed->refreshFromProcessor();
    CHECK (p.getBandSlots().canUndo());

    // Ctrl/Cmd+Z, then the button for redo.
    CHECK (ed->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)));
    CHECK_THAT (plain (p, id (1, schema::band::thresh)), WithinAbs (before, 0.05));
    CHECK_THAT (knob->getValue(), WithinAbs (before, 0.05));        // the control follows

    CHECK (ed->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)));
    CHECK_THAT (knob->getValue(), WithinAbs (-35.0, 0.05));
    CHECK_FALSE (ed->keyPressed (juce::KeyPress ('z', juce::ModifierKeys(), 0)));
}

TEST_CASE ("Editor window scale: same layout, scaled window, saved with the project", "[ui][editor][scale]")
{
    PluginProcessor p;
    {
        auto holder = openEditor (p);
        auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
        REQUIRE (ed != nullptr);
        CHECK (ed->getUiScalePercent() == 100);
        CHECK (ed->getContent().getBounds() == ed->getLocalBounds());

        ed->setUiScalePercent (75);
        CHECK (ed->getUiScalePercent() == 75);
        CHECK (ed->getWidth() == ui::PluginEditor::scaled (ui::PluginEditor::defaultWidth, 75));
        CHECK (ed->getHeight() == ui::PluginEditor::scaled (ui::PluginEditor::defaultHeight, 75));
        // The content keeps the 100 % layout (within rounding of the window size).
        CHECK (std::abs (ed->getContent().getWidth() - ui::PluginEditor::defaultWidth) <= 1);
        CHECK (std::abs (ed->getContent().getHeight() - ui::PluginEditor::defaultHeight) <= 1);
        CHECK (ed->getContent().getTransform().mat00 == 0.75f);

        // The limits scale too: at 75 % the window can be smaller than the 100 % minimum.
        auto* constrainer = ed->getConstrainer();
        REQUIRE (constrainer != nullptr);
        CHECK (constrainer->getMinimumWidth() == ui::PluginEditor::scaled (ui::PluginEditor::minWidth, 75));
        CHECK (constrainer->getMinimumWidth() < ui::PluginEditor::minWidth);
        ed->setBoundsConstrained ({ 0, 0, 1, 1 });
        CHECK (ed->getWidth() == ui::PluginEditor::scaled (ui::PluginEditor::minWidth, 75));

        // A size the user drags to at this scale is the window size that is saved.
        ed->setSize (1000, 700);
        CHECK (std::abs (ed->getContent().getWidth() - 1333) <= 1);

        ed->setUiScalePercent (12345);                 // not offered: 100 %
        CHECK (ed->getUiScalePercent() == 100);
        CHECK (ed->getContent().getTransform().isIdentity());
        CHECK (std::abs (ed->getWidth() - 1333) <= 1);  // same layout, so the window grew back

        ed->setUiScalePercent (150);
        CHECK (ed->getWidth() == juce::jlimit (ui::PluginEditor::scaled (ui::PluginEditor::minWidth, 150),
                                               ui::PluginEditor::scaled (ui::PluginEditor::maxWidth, 150),
                                               (int) std::lround (ed->getContent().getWidth() * 1.5)));
    }

    const auto nonParam = [] (PluginProcessor& proc) { return proc.getState().state.getChildWithName ("NonParam"); };
    CHECK ((int) nonParam (p).getProperty ("editorScale") == 150);
    const int savedWidth = (int) nonParam (p).getProperty ("editorWidth");

    // Reopening the project.
    juce::MemoryBlock saved;
    p.getStateInformation (saved);
    PluginProcessor reopened;
    reopened.setStateInformation (saved.getData(), (int) saved.getSize());
    {
        auto holder = openEditor (reopened);
        auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
        REQUIRE (ed != nullptr);
        CHECK (ed->getUiScalePercent() == 150);
        CHECK (ed->getWidth() == savedWidth);
    }

    // A preset or an A/B switch does not change it (it belongs to the session).
    PluginProcessor other;
    juce::MemoryBlock plainState;
    other.getStateInformation (plainState);
    reopened.setStateKeepingSession (plainState.getData(), (int) plainState.getSize());
    CHECK ((int) nonParam (reopened).getProperty ("editorScale") == 150);

    // A stored value that is not offered opens at 100 %; back at 100 % nothing extra is stored.
    nonParam (reopened).setProperty ("editorScale", 33, nullptr);
    {
        auto holder = openEditor (reopened);
        auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
        REQUIRE (ed != nullptr);
        CHECK (ed->getUiScalePercent() == 100);
        ed->setUiScalePercent (90);
        ed->setUiScalePercent (100);
    }
    CHECK_FALSE (nonParam (reopened).hasProperty ("editorScale"));
}

TEST_CASE ("Editor window scale: eight bands keep their layout at every scale", "[ui][editor][scale]")
{
    PluginProcessor p;
    for (int i = 0; i < schema::kActiveSlots; ++i) p.getBandSlots().createBand();
    auto holder = openEditor (p);
    auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
    REQUIRE (ed != nullptr);
    ed->refreshFromProcessor();

    for (int percent : ui::PluginEditor::uiScaleSteps)
    {
        INFO ("scale " << percent);
        ed->setUiScalePercent (percent);
        ed->setBoundsConstrained ({ 0, 0, 1, 1 });                           // the smallest window this scale allows
        ed->refreshFromProcessor();
        auto& content = ed->getContent();
        CHECK (content.getWidth() >= ui::PluginEditor::minWidth - 2);
        CHECK (content.getHeight() >= ui::PluginEditor::minHeight - 2);

        juce::Rectangle<int> previous;
        for (int slot : ed->getColumnOrder())
        {
            auto& col = ed->getColumn (slot);
            CHECK (col.isVisible());
            CHECK (content.getLocalBounds().contains (col.getBounds()));
            CHECK_FALSE (col.getBounds().intersects (previous));
            previous = col.getBounds();
        }
        // The content fills the window exactly (within a pixel).
        const auto shown = content.getBoundsInParent();
        CHECK (std::abs (shown.getWidth() - ed->getWidth()) <= 1);
        CHECK (std::abs (shown.getHeight() - ed->getHeight()) <= 1);
    }
}
