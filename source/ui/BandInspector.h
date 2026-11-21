#pragma once

#include "Controls.h"
#include "PluginProcessor.h"

namespace tl::ui
{
/** Detail controls of the selected band: frequency range, knee, range, detector and link.
    Only parameters the engine actually implements are shown (no false controls). */
class BandInspector final : public juce::Component
{
public:
    explicit BandInspector (PluginProcessor&);

    /** 1..8 binds the controls to that slot; 0 shows the "nothing selected" hint. */
    void setSlot (int slot);
    int getSlot() const noexcept { return slot; }

    /** Message thread, from the editor's refresh: title text and detector-dependent enablement. */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    PluginProcessor& processor;
    int slot = 0;
    std::atomic<float>* fLo = nullptr;
    std::atomic<float>* fHi = nullptr;
    std::atomic<float>* detMode = nullptr;
    juce::String shownTitle;

    juce::Label title, hint;
    ParamKnob lowFreq, highFreq, knee, range, detBlend, rmsTime, link;
    ParamCombo detector;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandInspector)
};
} // namespace tl::ui
