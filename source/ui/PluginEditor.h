#pragma once

#include <array>
#include <memory>

#include "BandColumn.h"
#include "BandInspector.h"
#include "Controls.h"
#include "PluginProcessor.h"

namespace tl::ui
{
/** The plugin editor. Layout top to bottom: header (name, engine tag, band counter, add, undo,
    redo, bypass), band column strip (used slots only, ordered by frequency), the inspector of
    the selected band and the global bar (input gain, mix, output gain). Runs entirely on the message thread; it only reads lock-free values the
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

    static constexpr int defaultWidth = 1180, defaultHeight = 480 + theme::globalBarHeight + theme::pad;
    // 8 columns of the minimum width must always fit (column scrolling comes with the layout tiers).
    static constexpr int minWidth = 2 * (theme::pad + 4) + schema::kActiveSlots * theme::columnMinWidth
                                    + (schema::kActiveSlots - 1) * theme::pad, minHeight = defaultHeight;
    static constexpr int maxWidth = 1920, maxHeight = defaultHeight;
    static constexpr int timerHz = 30;

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
    BandInspector inspector;
    ParamKnob inputGain { ParamKnob::Layout::Bar }, globalMix { ParamKnob::Layout::Bar }, outputGain { ParamKnob::Layout::Bar };
    juce::Rectangle<int> globalBarBounds;

    std::uint32_t shownMask = 0xffffffffu;   // forces the first refresh
    std::array<int, schema::kActiveSlots> shownOrder {};
    int shownCount = 0;
    int selectedSlot = 0;
    juce::Rectangle<int> stripBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
} // namespace tl::ui
