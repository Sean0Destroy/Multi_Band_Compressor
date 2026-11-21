#pragma once

#include "Controls.h"
#include "PluginProcessor.h"

namespace tl::ui
{
/** Detail controls of the selected band on three tabs.
      Band:     frequency range, knee, range, makeup, mix, stereo mode, link
      Detector: detector type, RMS blend / time, source, detector range (own, another band,
                custom), detector channels
      Timing:   lookahead, adaptive release, frequency-dependent release, transient preservation
    Only parameters the engine actually implements are shown (no false controls). */
class BandInspector final : public juce::Component
{
public:
    enum class Tab { Band, Detector, Timing };

    explicit BandInspector (PluginProcessor&);

    /** 1..8 binds the controls to that slot; 0 shows the "nothing selected" hint. */
    void setSlot (int slot);
    int getSlot() const noexcept { return slot; }

    void setTab (Tab);
    Tab getTab() const noexcept { return tab; }

    /** Message thread, from the editor's refresh: title text, state-dependent enablement, the
        list of bands a detector can follow and the sidechain warning. */
    void refresh();

    /** True while the selected band listens to an external sidechain that is not connected. */
    bool isSidechainWarningShown() const noexcept { return warning.isVisible(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void updateVisibility();
    int choice (std::atomic<float>* v) const noexcept { return v != nullptr ? juce::roundToInt (v->load (std::memory_order_relaxed)) : 0; }

    PluginProcessor& processor;
    int slot = 0;
    Tab tab = Tab::Band;
    std::atomic<float>* fLo = nullptr;
    std::atomic<float>* fHi = nullptr;
    std::atomic<float>* detMode = nullptr;
    std::atomic<float>* scSource = nullptr;
    std::atomic<float>* scMode = nullptr;
    std::atomic<float>* stereoMode = nullptr;
    juce::String shownTitle;

    juce::Label title, hint, warning;
    juce::TextButton bandTab, detectorTab, timingTab, soloButton, listenButton;

    // Band tab
    ParamKnob lowFreq, highFreq, knee, range, makeup, mix, link;
    ParamCombo stereo;

    // Detector tab
    ParamCombo detector, source, rangeMode, channels;
    ParamSlotCombo followBand;
    ParamKnob detBlend, rmsTime, scLow, scHigh;

    // Timing tab
    ParamKnob lookahead, adaptiveRelease, freqRelease, transient;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandInspector)
};
} // namespace tl::ui
