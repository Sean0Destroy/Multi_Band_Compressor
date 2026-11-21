#pragma once

#include <array>

#include "PluginProcessor.h"
#include "Theme.h"
#include "dsp/GainComputer.h"

namespace tl::ui
{
/** Alternative top display for the selected band: its static transfer curve (threshold, ratio,
    knee, range, makeup) with the current operating point, and a scrolling history of its gain
    reduction. The history of every band is recorded all the time, so switching the selection or
    the view shows the recent past. Message thread only. */
class DynamicsView final : public juce::Component
{
public:
    explicit DynamicsView (PluginProcessor&);

    void setSelectedSlot (int slot);
    int getSelectedSlot() const noexcept { return selectedSlot; }

    /** From the editor's timer, once per tick and band: the gain reduction to record (dB <= 0). */
    void pushGainReduction (int slot, float grDb);
    /** After the pushes of a tick: advance the history and repaint if visible. */
    void endTick();

    // ---- What is drawn (also for tests) -------------------------------------------------------
    /** Output level (dB) for an input level (dB) with the selected band's current settings,
        including makeup. Unity (output = input) when no band is selected. */
    double outputLevelDb (double inputDb) const;
    /** Input level (dB) at which the static curve gives this gain reduction (dB <= 0); for a
        reduction at the range limit, the lowest such level. Used to place the operating point. */
    double inputLevelForReduction (double grDb) const;
    /** History of a slot, oldest first; `count` values of up to historyLength. */
    int copyHistory (int slot, float* dest, int maxCount) const;

    juce::Rectangle<float> curveBounds() const noexcept;
    juce::Rectangle<float> historyBounds() const noexcept;

    void paint (juce::Graphics&) override;

    static constexpr int historyLength = 256;          // ticks (about 8.5 s at 30 Hz)
    static constexpr double minLevelDb = -72.0, maxLevelDb = 12.0;
    static constexpr double historyBottomDb = -24.0;

private:
    dsp::GainComputer computerFor (int slot, double& makeupDb) const;

    PluginProcessor& processor;
    int selectedSlot = 0;
    std::array<std::array<float, historyLength>, schema::kActiveSlots> history {};
    std::array<float, schema::kActiveSlots> pending {};
    int writeIndex = 0, filled = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicsView)
};
} // namespace tl::ui
