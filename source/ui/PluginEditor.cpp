#include "PluginEditor.h"

#include <algorithm>

namespace tl::ui
{
PluginEditor::PluginEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), inspector (p)
{
    setLookAndFeel (&lookAndFeel);

    productName.setText ("TONE LEGION  Multiband", juce::dontSendNotification);
    productName.setFont (theme::font (17.0f, true));
    productName.setColour (juce::Label::textColourId, theme::textHi);
    addAndMakeVisible (productName);

    // Honest labelling: the band engine is an experiment, not the final design.
    engineTag.setText ("EXPERIMENTAL A3 ENGINE - LISTENING BUILD", juce::dontSendNotification);
    engineTag.setFont (theme::font (11.0f, true));
    engineTag.setColour (juce::Label::textColourId, theme::warning);
    engineTag.setTooltip ("The band engine in this build is an experimental design under evaluation. "
                          "Sound and behaviour may change.");
    addAndMakeVisible (engineTag);

    bandCounter.setFont (theme::font (13.0f));
    bandCounter.setColour (juce::Label::textColourId, theme::textMid);
    bandCounter.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (bandCounter);

    addButton.setButtonText ("+ Add Band");
    addButton.setTitle ("Add band");
    addButton.setComponentID ("action_addBand");
    addButton.onClick = [this] { addBand(); };
    addAndMakeVisible (addButton);

    undoButton.setButtonText ("Undo");
    undoButton.setComponentID ("action_undo");
    undoButton.setTooltip ("Undo the last band add or remove");
    undoButton.onClick = [this] { processor.getUndoManager().undo(); refreshFromProcessor(); };
    addAndMakeVisible (undoButton);

    redoButton.setButtonText ("Redo");
    redoButton.setComponentID ("action_redo");
    redoButton.setTooltip ("Redo the last undone band add or remove");
    redoButton.onClick = [this] { processor.getUndoManager().redo(); refreshFromProcessor(); };
    addAndMakeVisible (redoButton);

    bypass.setParameter (processor.getState(), schema::global::bypass, "Bypass", "Plugin Bypass");
    bypass.setColour (juce::TextButton::buttonOnColourId, theme::warning);
    addAndMakeVisible (bypass);

    emptyHint.setText ("No bands yet. Press \"+ Add Band\" to create one - until then the plugin passes audio through unchanged.",
                       juce::dontSendNotification);
    emptyHint.setFont (theme::font (14.0f));
    emptyHint.setColour (juce::Label::textColourId, theme::textLow);
    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setInterceptsMouseClicks (false, false);
    addChildComponent (emptyHint);

    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        auto column = std::make_unique<BandColumn> (processor, slot);
        column->onSelect = [this] (int s) { selectSlot (s); };
        column->onRemove = [this] (int s) { removeBand (s); };
        addChildComponent (*column);
        columns[(size_t) slot - 1] = std::move (column);
    }

    addAndMakeVisible (inspector);

    setResizable (true, true);
    setResizeLimits (minWidth, minHeight, maxWidth, maxHeight);
    setSize (defaultWidth, defaultHeight);

    // Children built before they had a parent picked up the default look: re-resolve everything.
    sendLookAndFeelChange();

    refreshFromProcessor();
    startTimerHz (timerHz);
}

PluginEditor::~PluginEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

std::vector<int> PluginEditor::getColumnOrder() const
{
    return { shownOrder.begin(), shownOrder.begin() + shownCount };
}

void PluginEditor::addBand()
{
    const int slot = processor.getBandSlots().createBand();
    refreshFromProcessor();
    if (slot > 0) selectSlot (slot);
}

void PluginEditor::removeBand (int slot)
{
    processor.getBandSlots().deleteBand (slot);
    refreshFromProcessor();
}

void PluginEditor::selectSlot (int slot)
{
    if (slot < 1 || slot > schema::kActiveSlots || ! processor.getBandSlots().isUsed (slot))
        slot = 0;

    if (slot == selectedSlot && inspector.getSlot() == slot)
        return;

    selectedSlot = slot;
    for (auto& c : columns)
        c->setSelected (c->getSlot() == slot);
    inspector.setSlot (slot);
}

void PluginEditor::refreshFromProcessor()
{
    const auto mask = processor.getBandSlots().usedMaskSnapshot();

    // Used slots ordered by low edge (slot number breaks ties): identity stays with the slot.
    std::array<int, schema::kActiveSlots> order {};
    int count = 0;
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
        if ((mask & (1u << (slot - 1))) != 0)
            order[(size_t) count++] = slot;

    std::stable_sort (order.begin(), order.begin() + count, [this] (int a, int b)
    {
        return columns[(size_t) a - 1]->getLowHz() < columns[(size_t) b - 1]->getLowHz();
    });

    if (mask != shownMask || count != shownCount || order != shownOrder)
    {
        shownMask = mask;
        shownOrder = order;
        shownCount = count;

        for (auto& c : columns)
            c->setVisible ((mask & (1u << (c->getSlot() - 1))) != 0);

        emptyHint.setVisible (count == 0);
        bandCounter.setText (juce::String (count) + " / " + juce::String (schema::kActiveSlots), juce::dontSendNotification);
        addButton.setEnabled (count < schema::kActiveSlots);
        layoutColumns();

        if (selectedSlot != 0 && (mask & (1u << (selectedSlot - 1))) == 0)
            selectSlot (count > 0 ? order[0] : 0);
    }

    auto& undo = processor.getUndoManager();
    undoButton.setEnabled (undo.canUndo());
    redoButton.setEnabled (undo.canRedo());

    const double now = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < shownCount; ++i)
        columns[(size_t) shownOrder[(size_t) i] - 1]->refresh (now);

    inspector.refresh();
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    g.setColour (theme::panel);
    g.fillRect (getLocalBounds().removeFromTop (theme::headerHeight));
    g.setColour (theme::border);
    g.fillRect (0, theme::headerHeight - 1, getWidth(), 1);
}

void PluginEditor::layoutColumns()
{
    emptyHint.setBounds (stripBounds);
    if (shownCount == 0 || stripBounds.isEmpty()) return;

    const int gap = theme::pad;
    const int available = stripBounds.getWidth() - gap * (schema::kActiveSlots - 1);
    const int width = juce::jlimit (theme::columnMinWidth, theme::columnMaxWidth, available / schema::kActiveSlots);

    int x = stripBounds.getX();
    for (int i = 0; i < shownCount; ++i)
    {
        columns[(size_t) shownOrder[(size_t) i] - 1]->setBounds (x, stripBounds.getY(), width, stripBounds.getHeight());
        x += width + gap;
    }
}

void PluginEditor::resized()
{
    auto r = getLocalBounds();

    auto header = r.removeFromTop (theme::headerHeight).reduced (theme::pad + 4, theme::pad);
    productName.setBounds (header.removeFromLeft (220));
    engineTag.setBounds (header.removeFromLeft (290));

    bypass.setBounds (header.removeFromRight (72));
    header.removeFromRight (theme::pad * 2);
    redoButton.setBounds (header.removeFromRight (56));
    header.removeFromRight (4);
    undoButton.setBounds (header.removeFromRight (56));
    header.removeFromRight (theme::pad * 2);
    addButton.setBounds (header.removeFromRight (96));
    header.removeFromRight (theme::pad);
    bandCounter.setBounds (header.removeFromRight (56));

    r = r.reduced (theme::pad + 4, theme::pad);
    inspector.setBounds (r.removeFromBottom (theme::inspectorHeight));
    r.removeFromBottom (theme::pad);
    stripBounds = r;
    layoutColumns();
}
} // namespace tl::ui
