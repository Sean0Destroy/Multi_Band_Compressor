#include "PluginEditor.h"

#include <algorithm>

namespace tl::ui
{
PluginEditor::PluginEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), spectrum (p), dynamics (p), routing (p), meterRail (p), inspector (p)
{
    setLookAndFeel (&lookAndFeel);

    // Everything is laid out at 100 % inside `content`; the window shows it scaled (setUiScale).
    content.painter = [this] (juce::Graphics& g) { paintContent (g); };
    content.onResized = [this] { layoutContent(); };
    addAndMakeVisible (content);

    productName.setText ("TONE LEGION  Multiband", juce::dontSendNotification);
    productName.setFont (theme::font (17.0f, true));
    productName.setColour (juce::Label::textColourId, theme::textHi);
    content.addAndMakeVisible (productName);

    // Honest labelling: the band engine is an experiment, not the final design.
    engineTag.setText ("EXPERIMENTAL ENGINE", juce::dontSendNotification);
    engineTag.setFont (theme::font (11.0f, true));
    engineTag.setColour (juce::Label::textColourId, theme::warning);
    engineTag.setTooltip ("The band engine in this build is an experimental design under evaluation. "
                          "Sound and behaviour may change.");
    content.addAndMakeVisible (engineTag);

    // Listening switch between the experimental band engines (saved with the project, not a
    // parameter and not part of a preset).
    for (int k = 0; k < dsp::kNumEngineKinds; ++k)
        engineSelect.addItem (dsp::engineName ((dsp::EngineKind) k), k + 1);
    engineSelect.setComponentID ("action_engineSelect");
    engineSelect.setTitle ("Experimental band engine");
    engineSelect.setTooltip ("Two candidate band engines to compare by ear. Switching crossfades over 20 ms");
    engineSelect.setSelectedId ((int) processor.getEngineKind() + 1, juce::dontSendNotification);
    engineSelect.onChange = [this]
    {
        processor.setEngineKind ((dsp::EngineKind) (engineSelect.getSelectedId() - 1));
    };
    content.addAndMakeVisible (engineSelect);

    latencyLabel.setFont (theme::font (12.0f));
    latencyLabel.setColour (juce::Label::textColourId, theme::textMid);
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    latencyLabel.setTooltip ("Delay the plugin reports to the host (the largest band lookahead, plus the oversampling filters while oversampling is on)");
    content.addAndMakeVisible (latencyLabel);

    fixedLatency.setParameter (processor.getState(), schema::global::constantLatency, "Fixed Latency", "Constant Latency");
    fixedLatency.setTooltip ("Always report the maximum 10 ms, so that changing a lookahead never changes the latency");
    content.addAndMakeVisible (fixedLatency);

    bandCounter.setFont (theme::font (13.0f));
    bandCounter.setColour (juce::Label::textColourId, theme::textMid);
    bandCounter.setJustificationType (juce::Justification::centredRight);
    content.addAndMakeVisible (bandCounter);

    addButton.setButtonText ("+ Add Band");
    addButton.setTitle ("Add band");
    addButton.setComponentID ("action_addBand");
    addButton.onClick = [this] { addBand(); };
    content.addAndMakeVisible (addButton);

    undoButton.setButtonText ("Undo");
    undoButton.setComponentID ("action_undo");
    undoButton.setTooltip ("Undo the last change: a knob, a switch, or adding or removing a band (Ctrl/Cmd+Z)");
    undoButton.onClick = [this] { undoLast(); };
    content.addAndMakeVisible (undoButton);

    redoButton.setButtonText ("Redo");
    redoButton.setComponentID ("action_redo");
    redoButton.setTooltip ("Redo the last undone change (Ctrl/Cmd+Shift+Z)");
    redoButton.onClick = [this] { redoLast(); };
    content.addAndMakeVisible (redoButton);

    // ---- Presets -------------------------------------------------------------------------------
    const auto setupAction = [this] (juce::TextButton& b, const char* text, const char* componentId, const char* title, std::function<void()> action)
    {
        b.setButtonText (text);
        b.setComponentID (componentId);
        b.setTitle (title);
        b.setTooltip (title);
        b.onClick = std::move (action);
        content.addAndMakeVisible (b);
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
    content.addChildComponent (presetNameEditor);

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
    content.addAndMakeVisible (statusLabel);

    bypass.setParameter (processor.getState(), schema::global::bypass, "Bypass", "Plugin Bypass");
    bypass.setColour (juce::TextButton::buttonOnColourId, theme::warning);
    content.addAndMakeVisible (bypass);

    // Window scale: the same layout, drawn larger or smaller. Saved with the project (NonParam),
    // not a parameter and not part of a preset.
    for (int i = 0; i < (int) uiScaleSteps.size(); ++i)
        scaleSelect.addItem (juce::String (uiScaleSteps[(size_t) i]) + "%", uiScaleSteps[(size_t) i]);
    scaleSelect.setComponentID ("action_uiScale");
    scaleSelect.setTitle ("Window scale");
    scaleSelect.setTooltip ("Window scale: the same layout, larger or smaller");
    scaleSelect.onChange = [this] { setUiScalePercent (scaleSelect.getSelectedId()); };
    content.addAndMakeVisible (scaleSelect);

    emptyHint.setText ("No bands yet. Press \"+ Add Band\" or drag across the spectrum to create one - until then the plugin passes audio through unchanged.",
                       juce::dontSendNotification);
    emptyHint.setFont (theme::font (14.0f));
    emptyHint.setColour (juce::Label::textColourId, theme::textLow);
    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setInterceptsMouseClicks (false, false);
    content.addChildComponent (emptyHint);

    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        auto column = std::make_unique<BandColumn> (processor, slot);
        column->onSelect = [this] (int s) { selectSlot (s); };
        column->onRemove = [this] (int s) { removeBand (s); };
        content.addChildComponent (*column);
        columns[(size_t) slot - 1] = std::move (column);
    }

    spectrum.onSelect = [this] (int s) { refreshFromProcessor(); selectSlot (s); };
    content.addAndMakeVisible (spectrum);
    content.addChildComponent (dynamics);
    content.addChildComponent (routing);
    // A click on a band in the routing view opens its routing controls.
    routing.onSelect = [this] (int s) { selectSlot (s); inspector.setTab (BandInspector::Tab::Detector); };

    const auto setupView = [this] (juce::TextButton& b, const char* text, const char* componentId, View v)
    {
        b.setButtonText (text);
        b.setComponentID (componentId);
        b.setTitle (juce::String (text) + " view");
        b.onClick = [this, v] { setView (v); };
        content.addAndMakeVisible (b);
    };
    setupView (spectrumTab, "Spectrum", "action_viewSpectrum", View::Spectrum);
    setupView (dynamicsTab, "Dynamics", "action_viewDynamics", View::Dynamics);
    setupView (routingTab, "Routing", "action_viewRouting", View::Routing);
    routingTab.setTooltip ("What each band's detector listens to: main input or sidechain, frequency range and channel");
    spectrumTab.setToggleState (true, juce::dontSendNotification);
    content.addAndMakeVisible (meterRail);
    content.addAndMakeVisible (inspector);

    auto& apvts = processor.getState();
    inputGain.setParameter (apvts, schema::global::inGain, "Input", "Input Gain", theme::accent);
    globalMix.setParameter (apvts, schema::global::mix, "Mix", "Global Mix", theme::accent);
    outputGain.setParameter (apvts, schema::global::outGain, "Output", "Output Gain", theme::accent);
    clipCeiling.setParameter (apvts, schema::global::clipCeiling, "Ceiling", "Clip Ceiling", theme::accent);
    for (auto* c : { &inputGain, &globalMix, &outputGain, &clipCeiling })
        content.addAndMakeVisible (*c);

    autoGain.setParameter (apvts, schema::global::autoGain, "Auto Gain", "Automatic Gain");
    autoGain.setTooltip ("Slowly matches the output loudness to the input loudness (at most 12 dB either way)");
    content.addAndMakeVisible (autoGain);
    autoGainValue.setFont (theme::font (theme::valueFontSize));
    autoGainValue.setColour (juce::Label::textColourId, theme::textHi);
    autoGainValue.setJustificationType (juce::Justification::centredLeft);
    autoGainValue.setInterceptsMouseClicks (false, false);
    content.addAndMakeVisible (autoGainValue);

    overlapMode.setParameter (apvts, schema::global::overlapMode, "Overlap", "Band Overlap");
    overlapMode.getCombo().setTooltip ("How neighbouring bands share the range where they overlap. Stack: their gains add up, "
                                       "so two bands compressing together cut deeper than either asks. Blend: each band is "
                                       "eased back by its neighbours, so the cut stays near what each band asks for");
    overlapMode.getCombo().changeItemText (3, "Split (not built yet)");
    overlapMode.getCombo().setItemEnabled (3, false);
    content.addAndMakeVisible (overlapMode);

    quality.setParameter (apvts, schema::global::quality, "Quality", "Quality");
    quality.getCombo().setTooltip ("Zero Latency: the band filters shift phase around each band, with no delay. "
                                   "Linear Phase: the same curve with no phase shift, at the cost of the LP Length's latency "
                                   "(switching dips the sound for that long)");
    content.addAndMakeVisible (quality);

    lpLength.setParameter (apvts, schema::global::lpLength, "LP", "Linear Phase Length");
    lpLength.getCombo().setTooltip ("Kernel length in Linear Phase: Short 43 ms, Medium 85 ms, Long 171 ms of latency. "
                                    "Longer is more exact for narrow and low bands");
    content.addAndMakeVisible (lpLength);

    clipMode.setParameter (apvts, schema::global::clipMode, "Clip", "Output Clip Mode");
    content.addAndMakeVisible (clipMode);
    clipOversampling.setParameter (apvts, schema::global::clipOversampling, "Clip OS", "Clip Oversampling");
    clipOversampling.getCombo().setTooltip ("Oversampling of the output clipper: less aliasing on hard overs. "
                                            "Adds latency (47 to 55 samples) while it is on, whether or not the clipper is");
    content.addAndMakeVisible (clipOversampling);

    characterModel.setParameter (apvts, schema::global::charModel, "Colour", "Character Model");
    characterDrive.setParameter (apvts, schema::global::charDrive, "Drive", "Character Drive", theme::accent);
    oversampling.setParameter (apvts, schema::global::oversampling, "OS", "Oversampling");
    oversampling.getCombo().setTooltip ("Oversampling of the colour stage. Adds latency (47 to 55 samples) while it is on");
    for (auto* c : std::initializer_list<juce::Component*> { &characterModel, &characterDrive, &oversampling })
        content.addAndMakeVisible (*c);
    characterModelValue = apvts.getRawParameterValue (schema::global::charModel);
    clipModeValue = apvts.getRawParameterValue (schema::global::clipMode);
    autoGainOn = apvts.getRawParameterValue (schema::global::autoGain);

    setResizable (true, true);
    setWantsKeyboardFocus (true);         // Ctrl/Cmd+Z reaches the editor when no child takes the key
    // The window size and scale the user last chose are part of the saved state (NonParam); a
    // state without them opens at the default size and 100 %. The size is the window's, the
    // limits scale with it.
    {
        int w = defaultWidth, h = defaultHeight, percent = 100;
        {
            const StateGuard::Lock lock (processor.getStateGuard());     // a host may be loading on another thread
            const auto nonParam = processor.getState().state.getChildWithName ("NonParam");
            percent = validScalePercent ((int) nonParam.getProperty ("editorScale", 100));
            w = (int) nonParam.getProperty ("editorWidth", scaled (defaultWidth, percent));
            h = (int) nonParam.getProperty ("editorHeight", scaled (defaultHeight, percent));
        }
        uiScalePercent = percent;
        scaleSelect.setSelectedId (percent, juce::dontSendNotification);
        applyResizeLimits();
        setSize (juce::jlimit (scaled (minWidth, percent), scaled (maxWidth, percent), w),
                 juce::jlimit (scaled (minHeight, percent), scaled (maxHeight, percent), h));
    }
    constructed = true;
    lastBlockCount = processor.getProcessedBlockCount();

    // Children built before they had a parent picked up the default look: re-resolve everything.
    sendLookAndFeelChange();

    refreshFromProcessor();
    startTimerHz (timerHz);
}

PluginEditor::~PluginEditor()
{
    stopTimer();
    processor.setAudition (dsp::AuditionMode::None, 0);   // nobody could switch it off without the editor
    setLookAndFeel (nullptr);
}

std::vector<int> PluginEditor::getColumnOrder() const
{
    return { shownOrder.begin(), shownOrder.begin() + shownCount };
}

void PluginEditor::setView (View v)
{
    view = v;
    spectrum.setVisible (view == View::Spectrum);
    dynamics.setVisible (view == View::Dynamics);
    routing.setVisible (view == View::Routing);
    spectrumTab.setToggleState (view == View::Spectrum, juce::dontSendNotification);
    dynamicsTab.setToggleState (view == View::Dynamics, juce::dontSendNotification);
    routingTab.setToggleState (view == View::Routing, juce::dontSendNotification);
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
    dynamics.setSelectedSlot (slot);
    routing.setSelectedSlot (slot);
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
    {
        auto& column = *columns[(size_t) shownOrder[(size_t) i] - 1];
        column.refresh (now, running);
        dynamics.pushGainReduction (column.getSlot(), column.getGrBar().getCurrentDb());
    }
    dynamics.endTick();

    // Header: preset name, A/B state, status line.
    {
        const auto name = processor.getPresets().getCurrentName();
        const auto text = name.isEmpty() ? juce::String ("No preset") : name;
        if (text != presetName.getButtonText())
            presetName.setButtonText (text);

        const int active = processor.getPresets().getActiveSlot();
        abA.setToggleState (active == 0, juce::dontSendNotification);
        abB.setToggleState (active == 1, juce::dontSendNotification);

        // The status line carries short messages; otherwise it says what is being auditioned,
        // for as long as it is, so that a soloed or listened band is never a mystery.
        if (juce::Time::getMillisecondCounterHiRes() > statusUntilMs)
        {
            juce::String text;
            const int slot = processor.getAuditionSlot();
            if (processor.getAuditionMode() == dsp::AuditionMode::Solo)
                text = "SOLO: Band " + juce::String (slot);
            else if (processor.getAuditionMode() == dsp::AuditionMode::Listen)
            {
                const bool external = processor.getState().getRawParameterValue (schema::bandParamId (slot, schema::band::scSource))->load() >= 0.5f;
                text = "LISTENING: Band " + juce::String (slot) + " detector (" + (external ? "External" : "Main") + ")";
            }

            if (text != statusLabel.getText())
                statusLabel.setText (text, juce::dontSendNotification);
        }
    }

    if (engineSelect.getSelectedId() != (int) processor.getEngineKind() + 1)      // e.g. after a project load
        engineSelect.setSelectedId ((int) processor.getEngineKind() + 1, juce::dontSendNotification);

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
    characterDrive.setEnabled (juce::roundToInt (characterModelValue->load (std::memory_order_relaxed)) != 0);
    const bool autoOn = autoGainOn->load (std::memory_order_relaxed) >= 0.5f;
    const float correction = processor.getEngine().getAutoGainDb();
    const auto autoText = ! autoOn ? juce::String()
                        : (correction >= 0.05f ? "+" : "") + juce::String (std::abs (correction) < 0.05f ? 0.0f : correction, 1) + " dB";
    if (autoText != autoGainValue.getText())
        autoGainValue.setText (autoText, juce::dontSendNotification);

    inspector.refresh();
    routing.refresh (getColumnOrder());
    spectrum.refresh (running);
    meterRail.refresh (now, running);
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);      // under the content while it is being rescaled
}

void PluginEditor::paintContent (juce::Graphics& g)
{
    g.fillAll (theme::background);

    const int headerBottom = theme::headerHeight + theme::toolbarHeight;
    g.setColour (theme::panel);
    g.fillRect (content.getLocalBounds().removeFromTop (headerBottom));
    g.setColour (theme::border);
    g.fillRect (0, headerBottom - 1, content.getWidth(), 1);

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
    // The content keeps the 100 % layout of the window size divided by the scale.
    const float scale = getUiScale();
    content.setTransform (juce::AffineTransform::scale (scale));
    content.setBounds (0, 0, juce::roundToInt ((float) getWidth() / scale), juce::roundToInt ((float) getHeight() / scale));

    // Remember a size the user chose (not the size set while constructing).
    if (constructed)
    {
        const StateGuard::Lock lock (processor.getStateGuard());          // a host may be saving on another thread
        auto nonParam = processor.getState().state.getChildWithName ("NonParam");
        if (nonParam.isValid()
            && ((int) nonParam.getProperty ("editorWidth", defaultWidth) != getWidth()
                || (int) nonParam.getProperty ("editorHeight", defaultHeight) != getHeight()))
        {
            nonParam.setProperty ("editorWidth", getWidth(), nullptr);
            nonParam.setProperty ("editorHeight", getHeight(), nullptr);
        }
    }
}

void PluginEditor::layoutContent()
{
    auto r = content.getLocalBounds();

    // Row 1: name | presets | A/B | undo / redo | scale | bypass.
    auto header = r.removeFromTop (theme::headerHeight).reduced (theme::pad + 4, theme::pad);
    productName.setBounds (header.removeFromLeft (220));

    bypass.setBounds (header.removeFromRight (72));
    header.removeFromRight (theme::pad);
    scaleSelect.setBounds (header.removeFromRight (68));
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
    engineTag.setBounds (toolbar.removeFromLeft (146));
    engineSelect.setBounds (toolbar.removeFromLeft (220));
    toolbar.removeFromLeft (theme::pad * 2);
    spectrumTab.setBounds (toolbar.removeFromLeft (84));
    toolbar.removeFromLeft (4);
    dynamicsTab.setBounds (toolbar.removeFromLeft (84));
    toolbar.removeFromLeft (4);
    routingTab.setBounds (toolbar.removeFromLeft (84));
    toolbar.removeFromLeft (theme::pad);
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
        auto top = bar.removeFromTop (28);
        bar.removeFromTop (8);
        auto bottom = bar.removeFromTop (28);

        // Row 1: input, mix, output, band overlap, quality and its length, automatic gain.
        autoGainValue.setBounds (top.removeFromRight (58));
        top.removeFromRight (4);
        autoGain.setBounds (top.removeFromRight (84));
        top.removeFromRight (theme::pad * 2);
        lpLength.setBounds (top.removeFromRight (110));
        top.removeFromRight (theme::pad);
        quality.setBounds (top.removeFromRight (160));
        top.removeFromRight (theme::pad * 2);
        overlapMode.setBounds (top.removeFromRight (150));
        top.removeFromRight (theme::pad * 2);
        const int cell = juce::jmin (300, (top.getWidth() - 2 * theme::pad) / 3);
        inputGain.setBounds (top.removeFromLeft (cell));
        top.removeFromLeft (theme::pad);
        globalMix.setBounds (top.removeFromLeft (cell));
        top.removeFromLeft (theme::pad);
        outputGain.setBounds (top.removeFromLeft (cell));

        // Row 2: colour (model, drive), its oversampling, clipper (mode, its oversampling, ceiling).
        clipCeiling.setBounds (bottom.removeFromRight (220));
        bottom.removeFromRight (theme::pad);
        clipOversampling.setBounds (bottom.removeFromRight (140));
        bottom.removeFromRight (theme::pad);
        clipMode.setBounds (bottom.removeFromRight (128));
        bottom.removeFromRight (theme::pad * 2);
        characterModel.setBounds (bottom.removeFromLeft (170));
        bottom.removeFromLeft (theme::pad);
        const int driveWidth = juce::jmax (0, juce::jmin (300, cell, bottom.getWidth() - theme::pad * 2 - 120));
        characterDrive.setBounds (bottom.removeFromLeft (driveWidth));
        bottom.removeFromLeft (theme::pad * 2);
        oversampling.setBounds (bottom.removeFromLeft (120));
    }

    inspector.setBounds (r.removeFromBottom (theme::inspectorHeight));
    r.removeFromBottom (theme::pad);
    stripBounds = r.removeFromBottom (theme::columnStripHeight);
    r.removeFromBottom (theme::pad);
    meterRail.setBounds (r.removeFromRight (MeterRail::preferredWidth));
    r.removeFromRight (theme::pad);
    spectrum.setBounds (r);
    dynamics.setBounds (r);
    routing.setBounds (r);
    layoutColumns();
}

//==============================================================================
int PluginEditor::validScalePercent (int percent) noexcept
{
    for (auto step : uiScaleSteps)
        if (step == percent) return percent;
    return 100;
}

void PluginEditor::applyResizeLimits()
{
    setResizeLimits (scaled (minWidth, uiScalePercent), scaled (minHeight, uiScalePercent),
                     scaled (maxWidth, uiScalePercent), scaled (maxHeight, uiScalePercent));
}

void PluginEditor::setUiScalePercent (int percent)
{
    percent = validScalePercent (percent);
    scaleSelect.setSelectedId (percent, juce::dontSendNotification);
    if (percent == uiScalePercent) return;

    // The layout stays what it is; the window grows or shrinks with the scale.
    const auto logicalWidth = (double) getWidth() * 100.0 / uiScalePercent;
    const auto logicalHeight = (double) getHeight() * 100.0 / uiScalePercent;
    uiScalePercent = percent;

    {
        const StateGuard::Lock lock (processor.getStateGuard());          // a host may be saving on another thread
        auto nonParam = processor.getState().state.getChildWithName ("NonParam");
        if (nonParam.isValid())
        {
            if (percent == 100) nonParam.removeProperty ("editorScale", nullptr);
            else                nonParam.setProperty ("editorScale", percent, nullptr);
        }
    }

    applyResizeLimits();
    const int w = juce::jlimit (scaled (minWidth, percent), scaled (maxWidth, percent), (int) std::lround (logicalWidth * percent / 100.0));
    const int h = juce::jlimit (scaled (minHeight, percent), scaled (maxHeight, percent), (int) std::lround (logicalHeight * percent / 100.0));
    if (w == getWidth() && h == getHeight()) resized();      // same window, new scale
    else                                     setSize (w, h);
}

//==============================================================================
void PluginEditor::undoLast()
{
    processor.getBandSlots().undoLast();
    refreshFromProcessor();
}

void PluginEditor::redoLast()
{
    processor.getBandSlots().redoLast();
    refreshFromProcessor();
}

bool PluginEditor::keyPressed (const juce::KeyPress& key)
{
    const bool command = key.getModifiers().isCommandDown();
    const auto code = key.getKeyCode();

    if (command && (code == 'Z' || code == 'z'))
    {
        if (key.getModifiers().isShiftDown()) redoLast();
        else                                  undoLast();
        return true;
    }

    if (command && (code == 'Y' || code == 'y'))
    {
        redoLast();
        return true;
    }

    return false;
}
} // namespace tl::ui
