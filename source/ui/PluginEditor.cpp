#include "PluginEditor.h"

#include <algorithm>

namespace tl::ui
{
PluginEditor::PluginEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), spectrum (p), meterRail (p), inspector (p)
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

    latencyLabel.setFont (theme::font (12.0f));
    latencyLabel.setColour (juce::Label::textColourId, theme::textMid);
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    latencyLabel.setTooltip ("Delay the plugin reports to the host (the largest band lookahead)");
    addAndMakeVisible (latencyLabel);

    fixedLatency.setParameter (processor.getState(), schema::global::constantLatency, "Fixed Latency", "Constant Latency");
    fixedLatency.setTooltip ("Always report the maximum 10 ms, so that changing a lookahead never changes the latency");
    addAndMakeVisible (fixedLatency);

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
    undoButton.onClick = [this] { processor.getBandSlots().undoLast(); refreshFromProcessor(); };
    addAndMakeVisible (undoButton);

    redoButton.setButtonText ("Redo");
    redoButton.setComponentID ("action_redo");
    redoButton.setTooltip ("Redo the last undone band add or remove");
    redoButton.onClick = [this] { processor.getBandSlots().redoLast(); refreshFromProcessor(); };
    addAndMakeVisible (redoButton);

    // ---- Presets -------------------------------------------------------------------------------
    const auto setupAction = [this] (juce::TextButton& b, const char* text, const char* componentId, const char* title, std::function<void()> action)
    {
        b.setButtonText (text);
        b.setComponentID (componentId);
        b.setTitle (title);
        b.setTooltip (title);
        b.onClick = std::move (action);
        addAndMakeVisible (b);
    };

    setupAction (presetPrevious, "<", "action_presetPrevious", "Previous preset", [this]
    {
        const auto result = processor.getPresets().loadPrevious();
        if (result.failed()) showStatus (result.getErrorMessage());
        afterStateChange();
    });
    setupAction (presetNext, ">", "action_presetNext", "Next preset", [this]
    {
        const auto result = processor.getPresets().loadNext();
        if (result.failed()) showStatus (result.getErrorMessage());
        afterStateChange();
    });
    setupAction (presetName, "No preset", "action_presetMenu", "Presets", [this]
    {
        buildPresetMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetName),
                                         [safe = juce::Component::SafePointer<PluginEditor> (this)] (int id)
                                         {
                                             if (safe != nullptr && id != 0) safe->handlePresetMenu (id);
                                         });
    });
    setupAction (presetSave, "Save", "action_presetSave", "Save preset", [this]
    {
        // A user preset is saved in place; anything else asks for a name first.
        auto& presets = processor.getPresets();
        const auto current = presets.getCurrentName();
        for (auto& e : presets.list())
            if (! e.builtIn && e.name == current) { savePresetAs (current); return; }
        beginNamingPreset();
    });

    presetNameEditor.setComponentID ("action_presetNameEntry");
    presetNameEditor.setTitle ("Preset name");
    presetNameEditor.setFont (theme::font (13.0f));
    presetNameEditor.setJustification (juce::Justification::centredLeft);
    presetNameEditor.onReturnKey = [this] { const auto text = presetNameEditor.getText(); presetNameEditor.setVisible (false); savePresetAs (text); };
    presetNameEditor.onEscapeKey = [this] { presetNameEditor.setVisible (false); };
    presetNameEditor.onFocusLost = [this] { presetNameEditor.setVisible (false); };
    addChildComponent (presetNameEditor);

    // ---- A/B -----------------------------------------------------------------------------------
    setupAction (abA, "A", "action_abA", "Compare: state A", [this] { processor.getPresets().switchTo (0); afterStateChange(); });
    setupAction (abB, "B", "action_abB", "Compare: state B", [this] { processor.getPresets().switchTo (1); afterStateChange(); });
    setupAction (abCopy, "Copy", "action_abCopy", "Copy the active state to the other one", [this]
    {
        processor.getPresets().copyActiveToOther();
        showStatus (processor.getPresets().getActiveSlot() == 0 ? "Copied A to B" : "Copied B to A");
    });

    statusLabel.setFont (theme::font (12.0f));
    statusLabel.setColour (juce::Label::textColourId, theme::warning);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (statusLabel);

    bypass.setParameter (processor.getState(), schema::global::bypass, "Bypass", "Plugin Bypass");
    bypass.setColour (juce::TextButton::buttonOnColourId, theme::warning);
    addAndMakeVisible (bypass);

    emptyHint.setText ("No bands yet. Press \"+ Add Band\" or drag across the spectrum to create one - until then the plugin passes audio through unchanged.",
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

    spectrum.onSelect = [this] (int s) { refreshFromProcessor(); selectSlot (s); };
    addAndMakeVisible (spectrum);
    addAndMakeVisible (meterRail);
    addAndMakeVisible (inspector);

    auto& apvts = processor.getState();
    inputGain.setParameter (apvts, schema::global::inGain, "Input", "Input Gain", theme::accent);
    globalMix.setParameter (apvts, schema::global::mix, "Mix", "Global Mix", theme::accent);
    outputGain.setParameter (apvts, schema::global::outGain, "Output", "Output Gain", theme::accent);
    clipCeiling.setParameter (apvts, schema::global::clipCeiling, "Ceiling", "Clip Ceiling", theme::accent);
    for (auto* c : { &inputGain, &globalMix, &outputGain, &clipCeiling })
        addAndMakeVisible (*c);

    autoGain.setParameter (apvts, schema::global::autoGain, "Auto Gain", "Automatic Gain");
    autoGain.setTooltip ("Slowly matches the output loudness to the input loudness (at most 12 dB either way)");
    addAndMakeVisible (autoGain);
    autoGainValue.setFont (theme::font (theme::valueFontSize));
    autoGainValue.setColour (juce::Label::textColourId, theme::textHi);
    autoGainValue.setJustificationType (juce::Justification::centredLeft);
    autoGainValue.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (autoGainValue);

    clipMode.setParameter (apvts, schema::global::clipMode, "Clip", "Output Clip Mode");
    addAndMakeVisible (clipMode);
    clipModeValue = apvts.getRawParameterValue (schema::global::clipMode);
    autoGainOn = apvts.getRawParameterValue (schema::global::autoGain);

    setResizable (true, true);
    setResizeLimits (minWidth, minHeight, maxWidth, maxHeight);
    setSize (defaultWidth, defaultHeight);
    lastBlockCount = processor.getProcessedBlockCount();

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

void PluginEditor::afterStateChange()
{
    refreshFromProcessor();
    if (selectedSlot == 0 && shownCount > 0)
        selectSlot (shownOrder[0]);
}

void PluginEditor::showStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusUntilMs = juce::Time::getMillisecondCounterHiRes() + 4000.0;
}

void PluginEditor::beginNamingPreset()
{
    const auto current = processor.getPresets().getCurrentName();
    presetNameEditor.setText (current.isEmpty() || current.startsWith ("Starter - ") ? juce::String ("My Preset") : current, false);
    presetNameEditor.setVisible (true);
    presetNameEditor.toFront (false);
    presetNameEditor.selectAll();
    if (isShowing()) presetNameEditor.grabKeyboardFocus();
}

void PluginEditor::savePresetAs (const juce::String& name)
{
    const auto result = processor.getPresets().saveUser (name);
    showStatus (result.wasOk() ? "Saved \"" + processor.getPresets().getCurrentName() + "\"" : result.getErrorMessage());
    refreshFromProcessor();
}

juce::PopupMenu PluginEditor::buildPresetMenu() const
{
    juce::PopupMenu menu;
    auto& presets = processor.getPresets();
    const auto entries = presets.list();
    const auto current = presets.getCurrentName();

    bool userHeader = false, currentIsUser = false;
    menu.addSectionHeader ("Starter presets");
    for (int i = 0; i < (int) entries.size(); ++i)
    {
        const auto& e = entries[(size_t) i];
        if (! e.builtIn && ! userHeader) { menu.addSectionHeader ("Your presets"); userHeader = true; }
        menu.addItem (i + 1, e.name, true, e.name == current);
        currentIsUser = currentIsUser || (! e.builtIn && e.name == current);
    }

    menu.addSeparator();
    menu.addItem (menuSaveAs, "Save As...");
    menu.addItem (menuDeleteCurrent, currentIsUser ? "Delete \"" + current + "\"" : juce::String ("Delete preset"), currentIsUser);
    menu.addItem (menuNewEmpty, "New (no bands)");
    menu.addItem (menuOpenFolder, "Show preset folder");
    return menu;
}

void PluginEditor::handlePresetMenu (int id)
{
    auto& presets = processor.getPresets();

    if (id == menuNewEmpty)
    {
        presets.loadEmpty();
    }
    else if (id == menuSaveAs)
    {
        beginNamingPreset();
        return;
    }
    else if (id == menuDeleteCurrent)
    {
        const auto name = presets.getCurrentName();
        const auto result = presets.deleteUser (name);
        showStatus (result.wasOk() ? "Deleted \"" + name + "\"" : result.getErrorMessage());
    }
    else if (id == menuOpenFolder)
    {
        auto dir = presets.getUserDirectory();
        if (! dir.isDirectory()) dir.createDirectory();
        dir.revealToUser();
        return;
    }
    else
    {
        const auto entries = presets.list();
        if (id >= 1 && id <= (int) entries.size())
        {
            const auto result = presets.load (entries[(size_t) id - 1]);
            if (result.failed()) showStatus (result.getErrorMessage());
        }
    }

    afterStateChange();
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
    spectrum.setSelectedSlot (slot);
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
        {
            const bool show = (mask & (1u << (c->getSlot() - 1))) != 0;
            if (show && ! c->isVisible())
                c->resetMeter();          // nothing collected while hidden may show up now
            c->setVisible (show);
        }

        emptyHint.setVisible (count == 0);
        bandCounter.setText (juce::String (count) + " / " + juce::String (schema::kActiveSlots), juce::dontSendNotification);
        addButton.setEnabled (count < schema::kActiveSlots);
        layoutColumns();

        if (selectedSlot != 0 && (mask & (1u << (selectedSlot - 1))) == 0)
            selectSlot (count > 0 ? order[0] : 0);
    }

    undoButton.setEnabled (processor.getBandSlots().canUndo());
    redoButton.setEnabled (processor.getBandSlots().canRedo());

    // Meters keep their last value when the host stops processing: detect that and clear them.
    const auto blocks = processor.getProcessedBlockCount();
    if (blocks != lastBlockCount) { lastBlockCount = blocks; idleTicks = 0; }
    else if (idleTicks < idleTicksBeforeMetersClear) ++idleTicks;
    const bool running = isAudioRunning();

    const double now = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < shownCount; ++i)
        columns[(size_t) shownOrder[(size_t) i] - 1]->refresh (now, running);

    // Header: preset name, A/B state, status line.
    {
        const auto name = processor.getPresets().getCurrentName();
        const auto text = name.isEmpty() ? juce::String ("No preset") : name;
        if (text != presetName.getButtonText())
            presetName.setButtonText (text);

        const int active = processor.getPresets().getActiveSlot();
        abA.setToggleState (active == 0, juce::dontSendNotification);
        abB.setToggleState (active == 1, juce::dontSendNotification);

        if (statusLabel.getText().isNotEmpty() && juce::Time::getMillisecondCounterHiRes() > statusUntilMs)
            statusLabel.setText ({}, juce::dontSendNotification);
    }

    // Header: the latency reported to the host.
    {
        const double fs = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        const int samples = processor.getLatencySamples();
        const auto text = samples == 0 ? juce::String ("Latency 0 ms")
                                       : "Latency " + juce::String (1000.0 * samples / fs, 1) + " ms";
        if (text != latencyLabel.getText())
            latencyLabel.setText (text, juce::dontSendNotification);
    }

    // Global bar: the ceiling only matters while the clipper is on; show the automatic gain.
    clipCeiling.setEnabled (juce::roundToInt (clipModeValue->load (std::memory_order_relaxed)) != 0);
    const bool autoOn = autoGainOn->load (std::memory_order_relaxed) >= 0.5f;
    const float correction = processor.getEngine().getAutoGainDb();
    const auto autoText = ! autoOn ? juce::String()
                        : (correction >= 0.05f ? "+" : "") + juce::String (std::abs (correction) < 0.05f ? 0.0f : correction, 1) + " dB";
    if (autoText != autoGainValue.getText())
        autoGainValue.setText (autoText, juce::dontSendNotification);

    inspector.refresh();
    spectrum.refresh (running);
    meterRail.refresh (now, running);
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    const int headerBottom = theme::headerHeight + theme::toolbarHeight;
    g.setColour (theme::panel);
    g.fillRect (getLocalBounds().removeFromTop (headerBottom));
    g.setColour (theme::border);
    g.fillRect (0, headerBottom - 1, getWidth(), 1);

    const auto bar = globalBarBounds.toFloat().reduced (0.5f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (bar, 6.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (bar, 6.0f, 1.0f);
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

    // Row 1: name | presets | A/B | undo / redo | bypass.
    auto header = r.removeFromTop (theme::headerHeight).reduced (theme::pad + 4, theme::pad);
    productName.setBounds (header.removeFromLeft (220));

    bypass.setBounds (header.removeFromRight (72));
    header.removeFromRight (theme::pad * 2);
    redoButton.setBounds (header.removeFromRight (56));
    header.removeFromRight (4);
    undoButton.setBounds (header.removeFromRight (56));
    header.removeFromRight (theme::pad * 2);
    abCopy.setBounds (header.removeFromRight (52));
    header.removeFromRight (4);
    abB.setBounds (header.removeFromRight (32));
    header.removeFromRight (2);
    abA.setBounds (header.removeFromRight (32));
    header.removeFromRight (theme::pad * 2);

    header.removeFromLeft (theme::pad);
    presetPrevious.setBounds (header.removeFromLeft (30));
    header.removeFromLeft (2);
    presetSave.setBounds (header.removeFromRight (56));
    header.removeFromRight (4);
    presetNext.setBounds (header.removeFromRight (30));
    header.removeFromRight (2);
    presetName.setBounds (header);
    presetNameEditor.setBounds (header);

    // Row 2: engine tag | status | latency | band counter and add.
    auto toolbar = r.removeFromTop (theme::toolbarHeight).reduced (theme::pad + 4, 4);
    toolbar.removeFromBottom (4);
    engineTag.setBounds (toolbar.removeFromLeft (290));
    addButton.setBounds (toolbar.removeFromRight (96));
    toolbar.removeFromRight (theme::pad);
    bandCounter.setBounds (toolbar.removeFromRight (56));
    toolbar.removeFromRight (theme::pad);
    fixedLatency.setBounds (toolbar.removeFromRight (100));
    toolbar.removeFromRight (4);
    latencyLabel.setBounds (toolbar.removeFromRight (juce::jmin (110, toolbar.getWidth())));
    toolbar.removeFromRight (theme::pad);
    statusLabel.setBounds (toolbar);

    r = r.reduced (theme::pad + 4, theme::pad);

    globalBarBounds = r.removeFromBottom (theme::globalBarHeight);
    r.removeFromBottom (theme::pad);
    {
        auto bar = globalBarBounds.reduced (theme::pad, 8);

        // Right to left: clip ceiling, clip mode, automatic gain; the three sliders share the rest.
        clipCeiling.setBounds (bar.removeFromRight (190));
        bar.removeFromRight (theme::pad);
        clipMode.setBounds (bar.removeFromRight (128));
        bar.removeFromRight (theme::pad * 2);
        autoGainValue.setBounds (bar.removeFromRight (58));
        bar.removeFromRight (4);
        autoGain.setBounds (bar.removeFromRight (84));
        bar.removeFromRight (theme::pad * 2);

        const int cell = juce::jmin (260, (bar.getWidth() - 2 * theme::pad) / 3);
        inputGain.setBounds (bar.removeFromLeft (cell));
        bar.removeFromLeft (theme::pad);
        globalMix.setBounds (bar.removeFromLeft (cell));
        bar.removeFromLeft (theme::pad);
        outputGain.setBounds (bar.removeFromLeft (cell));
    }

    inspector.setBounds (r.removeFromBottom (theme::inspectorHeight));
    r.removeFromBottom (theme::pad);
    stripBounds = r.removeFromBottom (theme::columnStripHeight);
    r.removeFromBottom (theme::pad);
    meterRail.setBounds (r.removeFromRight (MeterRail::preferredWidth));
    r.removeFromRight (theme::pad);
    spectrum.setBounds (r);
    layoutColumns();
}
} // namespace tl::ui
