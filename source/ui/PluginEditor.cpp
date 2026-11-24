#include "PluginEditor.h"

#include <algorithm>

namespace tl::ui
{
namespace
{
constexpr int headerButtonHeight = 32, toolHeight = 28, sectionPadX = 12;
constexpr float brandPx = 17.0f, brandTrackEm = 0.3f;
const juce::String starterPrefix = "Starter - ";

/** Width of a text button for `text` (upper case, Jost 11 with 0.2 em tracking, 10 px padding). */
int buttonWidth (const juce::String& text)
{
    return juce::jmax (32, (int) std::ceil (theme::textWidth (theme::label (11.0f, 0.2f), text.toUpperCase())) + 22);
}

/** Height of the capitals of `f`: the outline of a "T", from the baseline up. (The glyph's own
    bounds would give the font's ascent, which is taller than the capitals.) */
float capHeight (const juce::Font& f)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (f, "T", 0.0f, 0.0f);
    juce::Path outline;
    ga.createPath (outline);
    return -outline.getBounds().getY();
}

void drawAtBaseline (juce::Graphics& g, const juce::String& text, float x, float baseline, const juce::Font& f)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (f, text, x, baseline);
    ga.draw (g);
}

/** The emblem's hexagon, stroke included, spans 61.5 of its 64 units (from 1.25 to 62.75). */
constexpr float emblemOuterUnits = 61.5f, emblemOuterTopUnits = 1.25f;

/** Widths in the header lockup: "T", the emblem standing in for the O, "NE LEGION", the rule and
    MULTIBAND, all at one size on one baseline. */
struct Lockup
{
    juce::Font font = theme::display (brandPx, brandTrackEm);
    float tracking = brandPx * brandTrackEm;
    float cap = capHeight (font);
    float emblem = cap * 64.0f / emblemOuterUnits;      // the hexagon's outer edge spans the cap height
    float tWidth = theme::textWidth (font, "T");
    float tailWidth = theme::textWidth (font, "NE LEGION");
    float productWidth = theme::textWidth (font, "MULTIBAND");

    float wordWidth() const noexcept { return tWidth + tracking + emblem + tracking + tailWidth; }
    float totalWidth() const noexcept { return 30.0f + 14.0f + wordWidth() + 14.0f + 1.0f + 14.0f + productWidth; }
};
} // namespace

PluginEditor::PluginEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), spectrum (p), inspector (p), meterRail (p), clipView (p)
{
    setLookAndFeel (&lookAndFeel);

    // Everything is laid out at 100 % inside `content`; the window shows it scaled (setUiScale).
    content.painter = [this] (juce::Graphics& g) { paintContent (g); };
    content.onResized = [this] { layoutContent(); };
    addAndMakeVisible (content);

    // ---- Header: presets ---------------------------------------------------------------------
    const auto setupAction = [this] (juce::Button& b, const char* text, const char* componentId, const char* title, std::function<void()> action)
    {
        b.setButtonText (text);
        b.setComponentID (componentId);
        b.setTitle (title);
        b.setTooltip (title);
        b.onClick = std::move (action);
        content.addAndMakeVisible (b);
    };

    setupAction (presetPrevious, "", "action_presetPrevious", "Previous preset", [this]
    {
        const auto result = processor.getPresets().loadPrevious();
        if (result.failed()) showStatus (result.getErrorMessage());
        afterStateChange();
    });
    setupAction (presetNext, "", "action_presetNext", "Next preset", [this]
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
    presetNameEditor.setFont (theme::label (13.0f, 0.06f));
    presetNameEditor.setJustification (juce::Justification::centredLeft);
    presetNameEditor.setIndents (12, 0);
    presetNameEditor.setColour (juce::TextEditor::backgroundColourId, theme::background);
    presetNameEditor.setColour (juce::TextEditor::textColourId, theme::textHi);
    presetNameEditor.setColour (juce::TextEditor::outlineColourId, theme::accent);
    presetNameEditor.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    presetNameEditor.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    presetNameEditor.setColour (juce::CaretComponent::caretColourId, theme::accent);
    presetNameEditor.onReturnKey = [this] { const auto text = presetNameEditor.getText(); presetNameEditor.setVisible (false); savePresetAs (text); };
    presetNameEditor.onEscapeKey = [this] { presetNameEditor.setVisible (false); };
    presetNameEditor.onFocusLost = [this] { presetNameEditor.setVisible (false); };
    content.addChildComponent (presetNameEditor);

    // ---- Header: A/B, undo / redo, scale, bypass ----------------------------------------------
    setupAction (abA, "A", "action_abA", "Compare: state A", [this] { processor.getPresets().switchTo (0); afterStateChange(); });
    setupAction (abB, "B", "action_abB", "Compare: state B", [this] { processor.getPresets().switchTo (1); afterStateChange(); });
    setupAction (abCopy, "Copy", "action_abCopy", "Copy the active state to the other one", [this]
    {
        processor.getPresets().copyActiveToOther();
        showStatus (processor.getPresets().getActiveSlot() == 0 ? "Copied A to B" : "Copied B to A");
    });

    setupAction (undoButton, "Undo", "action_undo", "Undo", [this] { undoLast(); });
    undoButton.setTooltip ("Undo the last change: a knob, a switch, or adding or removing a band (Ctrl/Cmd+Z)");
    setupAction (redoButton, "Redo", "action_redo", "Redo", [this] { redoLast(); });
    redoButton.setTooltip ("Redo the last undone change (Ctrl/Cmd+Shift+Z)");

    // Window scale: the same layout, drawn larger or smaller. Saved with the project (NonParam),
    // not a parameter and not part of a preset.
    for (int i = 0; i < (int) uiScaleSteps.size(); ++i)
        scaleSelect.addItem (juce::String (uiScaleSteps[(size_t) i]) + "%", uiScaleSteps[(size_t) i]);
    scaleSelect.setComponentID ("action_uiScale");
    scaleSelect.setTitle ("Window scale");
    scaleSelect.setTooltip ("Window scale: the same layout, larger or smaller");
    scaleSelect.onChange = [this] { setUiScalePercent (scaleSelect.getSelectedId()); };
    content.addAndMakeVisible (scaleSelect);

    bypass.setParameter (processor.getState(), schema::global::bypass, "Bypass", "Plugin Bypass");
    bypass.setColour (juce::TextButton::buttonOnColourId, theme::warning);
    content.addAndMakeVisible (bypass);

    // ---- Toolbar ------------------------------------------------------------------------------
    statusLabel.setFont (theme::label (11.0f, 0.12f));
    statusLabel.setColour (juce::Label::textColourId, theme::warning);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setInterceptsMouseClicks (false, false);
    content.addAndMakeVisible (statusLabel);

    latencyLabel.setTooltip ("Delay the plugin reports to the host (the largest band lookahead, plus the oversampling filters while oversampling is on)");
    latencyLabel.setInterceptsMouseClicks (true, false);       // for the tooltip
    content.addAndMakeVisible (latencyLabel);

    fixedLatency.setParameter (processor.getState(), schema::global::constantLatency, "Fixed", "Constant Latency");
    fixedLatency.setTooltip ("Always report the maximum 10 ms, so that changing a lookahead never changes the latency");
    content.addAndMakeVisible (fixedLatency);

    bandCounter.setTitle ("Bands in use");
    content.addAndMakeVisible (bandCounter);

    setupAction (addButton, "+ Band", "action_addBand", "Add band", [this] { addBand(); });

    // ---- Left column: display, strips, needle and inspector ----------------------------------
    spectrum.onSelect = [this] (int s) { refreshFromProcessor(); selectSlot (s); };
    content.addAndMakeVisible (spectrum);

    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        auto column = std::make_unique<BandColumn> (processor, slot);
        column->onSelect = [this] (int s) { selectSlot (s); };
        column->onRemove = [this] (int s) { removeBand (s); };
        content.addChildComponent (*column);
        columns[(size_t) slot - 1] = std::move (column);

        auto empty = std::make_unique<EmptyStrip>();
        empty->onAdd = [this] { addBand(); };
        content.addChildComponent (*empty);
        placeholders[(size_t) slot - 1] = std::move (empty);
    }

    content.addAndMakeVisible (needle);
    content.addAndMakeVisible (inspector);

    // ---- Master strip ---------------------------------------------------------------------------
    content.addAndMakeVisible (meterRail);

    auto& apvts = processor.getState();
    inputGain.setParameter (apvts, schema::global::inGain, "Input", "Input Gain", theme::accent);
    globalMix.setParameter (apvts, schema::global::mix, "Mix", "Global Mix", theme::accent);
    outputGain.setParameter (apvts, schema::global::outGain, "Output", "Output Gain", theme::accent);
    for (auto* c : { &inputGain, &globalMix, &outputGain })
        content.addAndMakeVisible (*c);

    overlapLegend = &makeLegend ("Overlap");
    overlapMode.setParameter (apvts, schema::global::overlapMode, "Band Overlap");
    overlapMode.setTooltip ("How neighbouring bands share the range where they overlap. Stack: their gains add up, "
                            "so two bands compressing together cut deeper than either asks. Blend: each band is "
                            "eased back by its neighbours, so the cut stays near what each band asks for");
    if (overlapMode.getNumItems() > 2)
    {
        overlapMode.setItemEnabled (2, false);
        overlapMode.setItemTooltip (2, "Not built yet");
    }
    content.addAndMakeVisible (overlapMode);

    autoGainLegend = &makeLegend ("Auto gain");
    autoGain.setParameter (apvts, schema::global::autoGain, "Off", "Automatic Gain");
    autoGain.setTooltip ("Slowly matches the output loudness to the input loudness (at most 12 dB either way)");
    content.addAndMakeVisible (autoGain);
    autoGainValue.setFont (theme::mono (12.0f));
    autoGainValue.setJustificationType (juce::Justification::centredRight);
    autoGainValue.setInterceptsMouseClicks (false, false);
    content.addAndMakeVisible (autoGainValue);

    colourLegend = &makeLegend ("Colour");
    characterModel.setParameter (apvts, schema::global::charModel, "Character Model");
    characterModel.setTooltip ("The colour stage after the bands: Clean leaves the signal alone, Warm FET adds gentle harmonics");
    content.addAndMakeVisible (characterModel);
    characterDrive.setParameter (apvts, schema::global::charDrive, "Drive", "Character Drive", theme::accent);
    content.addAndMakeVisible (characterDrive);
    colourOsLegend = &makeLegend ("Colour OS");
    oversampling.setParameter (apvts, schema::global::oversampling, "Oversampling");
    oversampling.setTooltip ("Oversampling of the colour stage. Adds latency (47 to 55 samples) while it is on");
    content.addAndMakeVisible (oversampling);

    clipLegend = &makeLegend ("Clip");
    clipMode.setParameter (apvts, schema::global::clipMode, "Output Clip Mode");
    clipMode.setTooltip ("The output clipper: Soft rounds the peaks off, Hard cuts them flat at the ceiling");
    content.addAndMakeVisible (clipMode);
    content.addAndMakeVisible (clipView);
    clipCeiling.setParameter (apvts, schema::global::clipCeiling, "Ceiling", "Clip Ceiling", theme::accent);
    content.addAndMakeVisible (clipCeiling);
    clipOsLegend = &makeLegend ("Clip OS");
    clipOversampling.setParameter (apvts, schema::global::clipOversampling, "Clip Oversampling");
    clipOversampling.setTooltip ("Oversampling of the output clipper: less aliasing on hard overs. "
                                 "Adds latency (47 to 55 samples) while it is on, whether or not the clipper is");
    content.addAndMakeVisible (clipOversampling);

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

CaptionLabel& PluginEditor::makeLegend (const juce::String& text)
{
    auto* legend = legends.add (new CaptionLabel (10.0f, 0.28f));
    legend->setText (text, juce::dontSendNotification);
    legend->setJustificationType (juce::Justification::centredLeft);
    legend->setInterceptsMouseClicks (false, false);
    content.addAndMakeVisible (*legend);
    return *legend;
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
    presetNameEditor.setText (current.isEmpty() || current.startsWith (starterPrefix) ? juce::String ("My Preset") : current, false);
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
    needle.setSlot (slot);
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
        for (int i = 0; i < schema::kActiveSlots; ++i)
            placeholders[(size_t) i]->setVisible (i < schema::kActiveSlots - count);

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

    if (selectedSlot > 0)
        if (auto* dir = processor.getState().getRawParameterValue (schema::bandParamId (selectedSlot, schema::band::dynDir)))
            needle.setUpward (dir->load (std::memory_order_relaxed) >= 0.5f);
    needle.update (selectedSlot > 0 ? columns[(size_t) selectedSlot - 1]->getGrBar().getCurrentDb() : 0.0f, now);

    // Header: preset name, A/B state, status line.
    {
        auto& presets = processor.getPresets();
        const auto name = presets.getCurrentName();
        const auto text = name.isEmpty() ? juce::String ("No preset") : name;
        if (text != presetName.getButtonText())
        {
            presetName.setButtonText (text);
            presetName.repaint();
        }

        const int active = presets.getActiveSlot();
        abA.setToggleState (active == 0, juce::dontSendNotification);
        abB.setToggleState (active == 1, juce::dontSendNotification);

        // The status line carries short messages; otherwise it says what is being auditioned,
        // for as long as it is, so that a soloed or listened band is never a mystery.
        if (juce::Time::getMillisecondCounterHiRes() > statusUntilMs)
        {
            juce::String status;
            const int slot = processor.getAuditionSlot();
            if (processor.getAuditionMode() == dsp::AuditionMode::Solo)
                status = "SOLO: Band " + juce::String (slot);
            else if (processor.getAuditionMode() == dsp::AuditionMode::Listen)
            {
                const bool external = processor.getState().getRawParameterValue (schema::bandParamId (slot, schema::band::scSource))->load() >= 0.5f;
                status = "LISTENING: Band " + juce::String (slot) + " detector (" + (external ? "External" : "Main") + ")";
            }

            if (status != statusLabel.getText())
                statusLabel.setText (status, juce::dontSendNotification);
        }
    }

    // Toolbar: the latency reported to the host.
    {
        const double fs = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        const int samples = processor.getLatencySamples();
        const auto text = "LAT " + juce::String (samples == 0 ? 0.0 : 1000.0 * samples / fs, 1) + " ms";
        if (text != latencyLabel.getText())
            latencyLabel.setText (text, juce::dontSendNotification);
    }

    // Master strip: the ceiling only matters while the clipper is on, the drive while a colour
    // model is; show the automatic gain.
    clipCeiling.setEnabled (juce::roundToInt (clipModeValue->load (std::memory_order_relaxed)) != 0);
    characterDrive.setEnabled (juce::roundToInt (characterModelValue->load (std::memory_order_relaxed)) != 0);
    const bool autoOn = autoGainOn->load (std::memory_order_relaxed) >= 0.5f;
    const float correction = processor.getEngine().getAutoGainDb();
    autoGainText = ! autoOn ? juce::String()
                 : (correction >= 0.05f ? "+" : "") + juce::String (std::abs (correction) < 0.05f ? 0.0f : correction, 1) + " dB";
    const auto shown = autoOn ? autoGainText.replace ("-", juce::String::fromUTF8 ("\xe2\x88\x92")) : juce::String ("off");
    if (shown != autoGainValue.getText())
    {
        autoGainValue.setText (shown, juce::dontSendNotification);
        autoGainValue.setColour (juce::Label::textColourId, autoOn ? theme::textHi : theme::textLow);
    }
    const auto toggleText = autoOn ? juce::String ("On") : juce::String ("Off");
    if (toggleText != autoGain.getButtonText())
        autoGain.setButtonText (toggleText);

    inspector.refresh();
    spectrum.refresh (running);
    meterRail.refresh (now, running);
    clipView.refresh (now, running);
}

//==============================================================================
void PluginEditor::PresetBox::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (theme::background);
    g.fillRect (r);
    g.setColour (hasKeyboardFocus (false) ? theme::accent : (highlighted ? theme::graphite : theme::border));
    g.drawRect (r, 1.0f);

    // Only the preset's own name: no "Starter" or "User" label in front of it.
    auto inner = r.reduced (12.0f, 0.0f);
    auto name = getButtonText();
    if (name.startsWith (starterPrefix)) name = name.substring (starterPrefix.length());

    g.setColour (theme::textHi);
    g.setFont (theme::label (13.0f, 0.06f));
    g.drawText (name, inner, juce::Justification::centredLeft, true);
}

void PluginEditor::ArrowButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (hasKeyboardFocus (false) ? theme::accent : (highlighted ? theme::graphite : theme::border));
    g.drawRect (r, 1.0f);

    // A chevron in a 16-unit box drawn 12 px wide.
    const auto box = juce::Rectangle<float> (12.0f, 12.0f).withCentre (r.getCentre());
    const float u = box.getWidth() / 16.0f;
    const float tip = left ? 5.0f : 11.0f, back = left ? 10.0f : 6.0f;
    juce::Path p;
    p.startNewSubPath (box.getX() + back * u, box.getY() + 3.0f * u);
    p.lineTo (box.getX() + tip * u, box.getY() + 8.0f * u);
    p.lineTo (box.getX() + back * u, box.getY() + 13.0f * u);
    g.setColour ((highlighted || down ? theme::textHi : theme::textMid).withMultipliedAlpha (isEnabled() ? 1.0f : 0.4f));
    g.strokePath (p, juce::PathStrokeType (1.4f));
}

//==============================================================================
void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);      // under the content while it is being rescaled
}

void PluginEditor::paintContent (juce::Graphics& g)
{
    const auto all = content.getLocalBounds();
    theme::drawBackdrop (g, all.toFloat());

    // Rack ears with two screws each.
    for (auto ear : { all.withWidth (theme::earWidth), all.withLeft (all.getRight() - theme::earWidth) })
    {
        const auto e = ear.toFloat();
        juce::ColourGradient grad (theme::earBase, e.getX(), 0.0f, theme::earBase, e.getRight(), 0.0f, false);
        grad.addColour (0.5, theme::earLight);
        g.setGradientFill (grad);
        g.fillRect (e);

        for (float cy : { e.getY() + 22.0f + 4.5f, e.getBottom() - 22.0f - 4.5f })
        {
            const auto screw = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ e.getCentreX(), cy });
            g.setGradientFill (juce::ColourGradient (theme::graphite, screw.getX() + 3.0f, screw.getY() + 3.0f,
                                                     theme::earBase, screw.getRight(), screw.getBottom(), true));
            g.fillEllipse (screw);
            g.setColour (theme::room);
            g.drawEllipse (screw, 1.0f);
            const auto c = screw.getCentre();
            const float a = juce::degreesToRadians (-30.0f), len = 3.5f;
            g.drawLine (c.x - len * std::cos (a), c.y - len * std::sin (a), c.x + len * std::cos (a), c.y + len * std::sin (a), 1.0f);
        }
    }

    const auto face = all.reduced (theme::earWidth, 0).toFloat();

    // Header and toolbar, each closed by a lattice line.
    g.setColour (theme::lattice);
    g.fillRect (face.getX(), (float) theme::headerHeight - 1.0f, face.getWidth(), 1.0f);
    {
        const auto tool = juce::Rectangle<float> (face.getX(), (float) theme::headerHeight, face.getWidth(), (float) theme::toolbarHeight - 1.0f);
        g.setGradientFill (juce::ColourGradient (theme::toolbarTop.withAlpha (0.85f), 0.0f, tool.getY(), theme::toolbarBot.withAlpha (0.85f), 0.0f, tool.getBottom(), false));
        g.fillRect (tool);
        g.setColour (theme::lattice);
        g.fillRect (face.getX(), tool.getBottom(), face.getWidth(), 1.0f);
    }

    // Brand: the Facet emblem, then the lockup, a rule and MULTIBAND on one baseline.
    if (! brandBounds.isEmpty())
    {
        const Lockup lockup;
        const auto b = brandBounds.toFloat();
        const float cy = b.getCentreY();
        theme::drawEmblem (g, juce::Rectangle<float> (30.0f, 30.0f).withPosition (b.getX(), cy - 15.0f), false);

        const float baseline = std::round (cy + lockup.cap * 0.5f);
        float x = b.getX() + 30.0f + 14.0f;
        g.setColour (theme::textHi);
        drawAtBaseline (g, "T", x, baseline, lockup.font);
        x += lockup.tWidth + lockup.tracking;
        const float unit = lockup.emblem / 64.0f;
        theme::drawEmblem (g, juce::Rectangle<float> (lockup.emblem, lockup.emblem)
                                  .withPosition (x, baseline - lockup.cap - emblemOuterTopUnits * unit), true);
        x += lockup.emblem + lockup.tracking;
        g.setColour (theme::textHi);
        drawAtBaseline (g, "NE LEGION", x, baseline, lockup.font);
        x += lockup.tailWidth + 14.0f;

        g.setColour (theme::border);
        g.fillRect (x, cy - 13.0f, 1.0f, 26.0f);
        x += 1.0f + 14.0f;
        g.setColour (theme::textHi);
        drawAtBaseline (g, "MULTIBAND", x, baseline, lockup.font);
    }

    // The master strip and the lines between its sections.
    if (! masterBounds.isEmpty())
    {
        theme::drawModule (g, masterBounds.toFloat());
        g.setColour (theme::lattice);
        for (int y : masterRules)
            g.fillRect ((float) masterBounds.getX() + 10.0f, (float) y, (float) masterBounds.getWidth() - 20.0f, 1.0f);
    }
}

void PluginEditor::layoutColumns()
{
    if (stripBounds.isEmpty()) return;

    const int gap = theme::stripGap;
    const int available = stripBounds.getWidth() - gap * (schema::kActiveSlots - 1);
    const int width = juce::jlimit (theme::columnMinWidth, theme::columnMaxWidth, available / schema::kActiveSlots);

    int x = stripBounds.getX();
    for (int i = 0; i < schema::kActiveSlots; ++i)
    {
        const juce::Rectangle<int> place (x, stripBounds.getY(), width, stripBounds.getHeight());
        if (i < shownCount) columns[(size_t) shownOrder[(size_t) i] - 1]->setBounds (place);
        else                placeholders[(size_t) (i - shownCount)]->setBounds (place);
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
    auto face = content.getLocalBounds().reduced (theme::earWidth, 0);

    // ---- Header: brand | presets | A/B | undo / redo | scale | bypass ------------------------
    {
        auto header = face.removeFromTop (theme::headerHeight).reduced (16, 0);
        const int y = (theme::headerHeight - headerButtonHeight) / 2;
        const auto place = [y] (juce::Component& c, int x, int w) { c.setBounds (x, y, w, headerButtonHeight); };

        const Lockup lockup;
        const int brandWidth = (int) std::ceil (lockup.totalWidth());
        const int bypassW = buttonWidth ("Bypass"), scaleW = 68;
        const int copyW = buttonWidth ("Copy"), undoW = buttonWidth ("Undo"), redoW = buttonWidth ("Redo"), saveW = buttonWidth ("Save");
        const int presetFixed = 32 + 4 + 4 + 32 + 4 + saveW;
        const int fixedWidth = brandWidth + presetFixed + (32 + 4 + 32 + 4 + copyW) + (undoW + 4 + redoW) + scaleW + bypassW;
        const int presetMin = 96, presetMax = 280;
        // 14 px between the groups when there is room, down to 8.
        const int gap = juce::jlimit (8, 14, (header.getWidth() - fixedWidth - presetMin) / 5);

        brandBounds = header.removeFromLeft (brandWidth).withY (0).withHeight (theme::headerHeight);
        header.removeFromLeft (gap);

        const auto fromRight = [&] (juce::Component& c, int w, int after)
        {
            place (c, header.getRight() - w, w);
            header.removeFromRight (w + after);
        };
        fromRight (bypass, bypassW, gap);
        fromRight (scaleSelect, scaleW, gap);
        fromRight (redoButton, redoW, 4);
        fromRight (undoButton, undoW, gap);
        fromRight (abCopy, copyW, 4);
        fromRight (abB, 32, 4);
        fromRight (abA, 32, gap);

        // The preset group sits in the middle of what is left.
        const int presetW = juce::jlimit (0, presetMax, header.getWidth() - presetFixed);
        int x = header.getX() + juce::jmax (0, (header.getWidth() - presetFixed - presetW) / 2);
        place (presetPrevious, x, 32);           x += 32 + 4;
        place (presetName, x, presetW);
        presetNameEditor.setBounds (x, y, presetW, headerButtonHeight);
        x += presetW + 4;
        place (presetNext, x, 32);               x += 32 + 4;
        place (presetSave, x, saveW);
    }

    // ---- Toolbar: status | latency, fixed | band count | add -------------------------
    {
        auto tool = face.removeFromTop (theme::toolbarHeight).reduced (18, 0);
        const int y = tool.getY() + (theme::toolbarHeight - toolHeight) / 2;
        const auto place = [y] (juce::Component& c, int x, int w, int h = toolHeight) { c.setBounds (x, y + (toolHeight - h) / 2, w, h); };

        const int x = tool.getX();

        int right = tool.getRight();
        const int addW = buttonWidth ("+ Band");
        place (addButton, right - addW, addW);              right -= addW + 14;
        const int countW = Vfd::widthFor ("8 / 8");
        place (bandCounter, right - countW, countW);        right -= countW + 14;
        const int fixedW = buttonWidth ("Fixed");
        place (fixedLatency, right - fixedW, fixedW);       right -= fixedW + 8;
        const int latW = Vfd::widthFor ("LAT 10.0 ms");
        place (latencyLabel, right - latW, latW);           right -= latW + 14;
        place (statusLabel, x, juce::jmax (0, right - x));
    }

    // ---- Body: left column and master strip ----------------------------------------------------
    auto body = face;
    body.removeFromTop (theme::bodyPadTop);
    body.removeFromBottom (theme::bodyPadBottom);
    body = body.reduced (theme::bodyPadSide, 0);

    masterBounds = body.removeFromRight (theme::masterWidth);
    body.removeFromRight (theme::bodyGap);

    {
        auto left = body;
        auto bottom = left.removeFromBottom (bottomRowHeight);
        needle.setBounds (bottom.removeFromLeft (theme::needleWidth));
        bottom.removeFromLeft (theme::rowGap);
        inspector.setBounds (bottom);
        left.removeFromBottom (theme::rowGap);
        stripBounds = left.removeFromBottom (stripRowHeight);
        left.removeFromBottom (theme::rowGap);
        spectrum.setBounds (left);
    }

    {
        masterRules.clear();
        auto m = masterBounds.reduced (1);
        const int railH = juce::jlimit (MeterRail::minHeight, MeterRail::preferredHeight, m.getHeight() - masterSectionsHeight);
        meterRail.setBounds (m.removeFromTop (railH));

        const auto row = [] (juce::Rectangle<int>& area, int h) { auto r = area.removeFromTop (h); area.removeFromTop (masterGap); return r; };
        const auto legendAndControl = [] (juce::Rectangle<int> r, juce::Component& legend, juce::Component& control, int controlWidth)
        {
            controlWidth = juce::jmin (controlWidth, r.getWidth() - 70);
            control.setBounds (r.removeFromRight (controlWidth));
            legend.setBounds (r);
        };
        const auto knobAndSwitch = [] (juce::Rectangle<int> r, ParamKnob& knob, CaptionLabel& legend, Segment& seg)
        {
            knob.setBounds (r.removeFromLeft (76));
            const int segW = juce::jmin (seg.getPreferredWidth(), r.getWidth());
            auto ctl = r.removeFromRight (segW).withSizeKeepingCentre (segW, 14 + 4 + 28);
            legend.setBounds (ctl.removeFromTop (14));
            ctl.removeFromTop (4);
            seg.setBounds (ctl);
        };

        // Input, mix, output (no line above: they belong with the meters).
        {
            auto s = m.removeFromTop (masterTrioHeight).reduced (sectionPadX, masterPad);
            const int w = s.getWidth() / 3;
            inputGain.setBounds (s.removeFromLeft (w));
            outputGain.setBounds (s.removeFromRight (w));
            globalMix.setBounds (s);
            masterRules.push_back (m.getY());
        }
        // Overlap and automatic gain.
        {
            auto s = m.removeFromTop (masterOverlapHeight).reduced (sectionPadX, masterPad);
            legendAndControl (row (s, masterRow), *overlapLegend, overlapMode, overlapMode.getPreferredWidth());
            auto r = row (s, masterRow);
            const int toggleW = buttonWidth ("Off");
            autoGain.setBounds (r.removeFromRight (toggleW).withSizeKeepingCentre (toggleW, 24));
            r.removeFromRight (8);
            autoGainValue.setBounds (r.removeFromRight (70));
            autoGainLegend->setBounds (r);
            masterRules.push_back (m.getY());
        }
        // Colour.
        {
            auto s = m.removeFromTop (masterColourHeight).reduced (sectionPadX, masterPad);
            legendAndControl (row (s, masterRow), *colourLegend, characterModel, characterModel.getPreferredWidth());
            knobAndSwitch (s.removeFromTop (ParamKnob::heightFor (40)), characterDrive, *colourOsLegend, oversampling);
            masterRules.push_back (m.getY());
        }
        // Clipper: mode, the clip meter (with the shave read-out), ceiling and oversampling.
        {
            auto s = m.reduced (sectionPadX, masterPad);
            legendAndControl (row (s, masterRow), *clipLegend, clipMode, clipMode.getPreferredWidth());
            clipView.setBounds (row (s, ClipView::preferredHeight));
            knobAndSwitch (s.removeFromTop (ParamKnob::heightFor (40)), clipCeiling, *clipOsLegend, clipOversampling);
        }
    }

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
