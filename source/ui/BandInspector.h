#pragma once

#include "Controls.h"
#include "PluginProcessor.h"

namespace tl::ui
{
/** Detail controls of the selected band on three tabs.
      Band:     frequency range, width, knee, range, makeup, mix, character, link, stereo mode, direction
      Detector: detector type, RMS blend / time, source (main or the host's sidechain input),
                channels, detector range (own, another band, custom) with its band or edges
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

    /** Message thread, from the editor's refresh: state-dependent enablement, the list of bands a
        detector can follow and the sidechain warning. */
    void refresh();

    /** True while the selected band listens to an external sidechain that is not connected. */
    bool isSidechainWarningShown() const noexcept { return warning.isVisible(); }

    /** The panel title as drawn: "BAND" next to the slot chip, "NO BAND" when nothing is selected. */
    juce::String getTitleText() const { return slot > 0 ? "BAND" : "NO BAND"; }

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int headHeight = 43;

private:
    void updateVisibility();
    int choice (std::atomic<float>* v) const noexcept { return v != nullptr ? juce::roundToInt (v->load (std::memory_order_relaxed)) : 0; }

    /** One place in the row: a knob, or one or two drop-downs stacked. */
    struct Cell
    {
        std::vector<juce::Component*> parts;
        bool knob;
    };
    void layoutRow (juce::Rectangle<int> area, const std::vector<Cell>&);

    PluginProcessor& processor;
    int slot = 0;
    Tab tab = Tab::Band;
    std::atomic<float>* detMode = nullptr;
    std::atomic<float>* scSource = nullptr;
    std::atomic<float>* scMode = nullptr;
    std::atomic<float>* stereoMode = nullptr;
    std::atomic<float>* scChan = nullptr;

    juce::Label hint, warning, timingNote;
    Segment tabs;
    juce::TextButton soloButton, listenButton;

    // Band tab
    ParamKnob lowFreq, highFreq, width, knee, range, makeup, mix, character, link;
    ParamCombo stereo, direction;

    // Detector tab
    ParamCombo detector, source, rangeMode, channels;
    ParamSlotCombo followBand;
    ParamKnob detBlend, rmsTime, scLow, scHigh;

    // Timing tab
    ParamKnob lookahead, adaptiveRelease, freqRelease, transient;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandInspector)
};
} // namespace tl::ui
