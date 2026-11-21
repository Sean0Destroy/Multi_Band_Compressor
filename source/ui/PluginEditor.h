#pragma once

#include <array>
#include <memory>

#include "BandColumn.h"
#include "BandInspector.h"
#include "Controls.h"
#include "PluginProcessor.h"
#include "MeterRail.h"
#include "SpectrumView.h"

namespace tl::ui
{
/** The plugin editor. Layout top to bottom: header (name, engine tag, band counter, add, undo,
    redo, bypass), spectrum display (takes any extra height), band column strip (used slots only,
    ordered by frequency), the inspector of the selected band and the global bar (input gain,
    mix, output gain, automatic gain, output clipper). Runs entirely on the message thread; it only reads lock-free values the
    audio thread publishes (used mask, parameter atomics, GR meter). */
class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Synchronises everything with the processor: visible columns and their order, selection,
        band counter, undo/redo state, meters. Called by the 30 Hz timer and after user actions;
        public so tests can drive it without a running message loop. */
    void refreshFromProcessor();

    /** 0 = no band selected. Selecting an unused slot selects nothing. */
    void selectSlot (int slot);
    int getSelectedSlot() const noexcept { return selectedSlot; }

    /** Slots of the visible columns, left to right. */
    std::vector<int> getColumnOrder() const;

    BandColumn& getColumn (int slot) { return *columns[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1]; }
    BandInspector& getInspector() noexcept { return inspector; }
    SpectrumView& getSpectrum() noexcept { return spectrum; }
    MeterRail& getMeterRail() noexcept { return meterRail; }
    /** Text next to the Auto Gain button: the correction currently applied, empty when off. */
    juce::String getAutoGainText() const { return autoGainValue.getText(); }

    static constexpr int fixedHeight = theme::headerHeight + theme::columnStripHeight + theme::inspectorHeight
                                       + theme::globalBarHeight + 6 * theme::pad;   // everything but the display
    static constexpr int defaultWidth = 1180, defaultHeight = fixedHeight + theme::displayHeight;
    // 8 columns of the minimum width must always fit (column scrolling comes with the layout tiers).
    static constexpr int minWidth = 2 * (theme::pad + 4) + schema::kActiveSlots * theme::columnMinWidth
                                    + (schema::kActiveSlots - 1) * theme::pad,
                         minHeight = fixedHeight + theme::displayMinHeight;
    static constexpr int maxWidth = 1920, maxHeight = 1200;
    static constexpr int timerHz = 30;
    static constexpr int idleTicksBeforeMetersClear = 8;   // about 270 ms without a processed block

    /** True while the periodic refresh is running (it is from construction to destruction). */
    bool isRefreshing() const noexcept { return isTimerRunning(); }
    /** False once the host has stopped calling the processor for a moment. */
    bool isAudioRunning() const noexcept { return idleTicks < idleTicksBeforeMetersClear; }

private:
    void timerCallback() override { refreshFromProcessor(); }
    void addBand();
    void removeBand (int slot);
    void layoutColumns();

    PluginProcessor& processor;
    TlLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Label productName, engineTag, bandCounter, emptyHint;
    juce::TextButton addButton, undoButton, redoButton;
    ParamToggle bypass;

    std::array<std::unique_ptr<BandColumn>, schema::kActiveSlots> columns;
    SpectrumView spectrum;
    MeterRail meterRail;
    BandInspector inspector;
    ParamKnob inputGain { ParamKnob::Layout::Bar }, globalMix { ParamKnob::Layout::Bar }, outputGain { ParamKnob::Layout::Bar };
    ParamToggle autoGain;
    juce::Label autoGainValue;
    ParamCombo clipMode { ParamCombo::Layout::Bar };
    ParamKnob clipCeiling { ParamKnob::Layout::Bar };
    std::atomic<float>* clipModeValue = nullptr;
    std::atomic<float>* autoGainOn = nullptr;
    juce::Rectangle<int> globalBarBounds;

    std::uint32_t shownMask = 0xffffffffu;   // forces the first refresh
    std::array<int, schema::kActiveSlots> shownOrder {};
    int shownCount = 0;
    int selectedSlot = 0;
    std::uint32_t lastBlockCount = 0;
    int idleTicks = idleTicksBeforeMetersClear;   // "not running" until a block has been seen
    juce::Rectangle<int> stripBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
} // namespace tl::ui
