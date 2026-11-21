#pragma once

#include <functional>

#include "Controls.h"
#include "PluginProcessor.h"

namespace tl::ui
{
/** One column of the band strip, permanently tied to a parameter slot (1..8). The editor only
    shows the columns of used slots. Immediate controls: bypass, remove, GR read-out,
    threshold, ratio, attack, release. */
class BandColumn final : public juce::Component
{
public:
    BandColumn (PluginProcessor&, int slot);

    int getSlot() const noexcept { return slot; }
    /** Lower / upper edge as the engine uses them (the two parameters may be crossed). */
    float getLowHz() const noexcept  { return juce::jmin (fLo->load (std::memory_order_relaxed), fHi->load (std::memory_order_relaxed)); }
    float getHighHz() const noexcept { return juce::jmax (fLo->load (std::memory_order_relaxed), fHi->load (std::memory_order_relaxed)); }

    void setSelected (bool);
    bool isSelected() const noexcept { return selected; }

    /** Message thread, from the editor's refresh: consumes the meter hold and updates the looks.
        `audioRunning` false (the host stopped processing) shows no gain reduction. */
    void refresh (double nowMs, bool audioRunning);

    /** Discards whatever the meter collected while the column was hidden. */
    void resetMeter();

    GrBar& getGrBar() noexcept { return grBar; }

    std::function<void (int slot)> onSelect;
    std::function<void (int slot)> onRemove;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    PluginProcessor& processor;
    const int slot;
    const juce::Colour colour;
    std::atomic<float>* fLo = nullptr;
    std::atomic<float>* fHi = nullptr;
    std::atomic<float>* bypassValue = nullptr;
    bool selected = false, bypassedLook = false;

    juce::TextButton select;   // slot chip: selects the band (mouse and keyboard)
    ParamToggle bypass;
    juce::TextButton remove;
    GrBar grBar;
    ParamKnob threshold, ratio, attack, release;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandColumn)
};
} // namespace tl::ui
