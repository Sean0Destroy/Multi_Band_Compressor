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
    float getLowHz() const noexcept  { return fLo->load (std::memory_order_relaxed); }
    float getHighHz() const noexcept { return fHi->load (std::memory_order_relaxed); }

    void setSelected (bool);
    bool isSelected() const noexcept { return selected; }

    /** Message thread, from the editor's refresh: consumes the meter hold and updates the looks. */
    void refresh (double nowMs);

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

    juce::Label title;
    ParamToggle bypass;
    juce::TextButton remove;
    GrBar grBar;
    ParamKnob threshold, ratio, attack, release;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandColumn)
};
} // namespace tl::ui
