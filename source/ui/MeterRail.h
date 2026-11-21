#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

#include <array>

namespace tl::ui
{
/** Level meters at the right of the display: input and output (peak bar, RMS marker, numeric
    peak with a 2 s hold) and, when the host has connected it, the external sidechain with its
    state. Message thread only; reads the lock-free meters the audio thread publishes. */
class MeterRail final : public juce::Component
{
public:
    explicit MeterRail (PluginProcessor&);

    /** From the editor's timer. `audioRunning` false shows silence. */
    void refresh (double nowMs, bool audioRunning);

    enum class SidechainState { Off, Silent, Receiving };
    SidechainState getSidechainState() const noexcept { return sidechainState; }
    static juce::String sidechainStateText (SidechainState);

    struct Reading
    {
        float peakDb[2] { rt::LevelMeter::silenceDb, rt::LevelMeter::silenceDb };
        float rmsDb[2] { rt::LevelMeter::silenceDb, rt::LevelMeter::silenceDb };
        float heldDb = rt::LevelMeter::silenceDb;   // highest peak of the last holdMs, both channels
        double heldAtMs = 0.0;
        int channels = 0;
        bool clipped = false;                       // a peak above 0 dBFS within the hold time
    };
    const Reading& input() const noexcept     { return in; }
    const Reading& output() const noexcept    { return out; }
    const Reading& sidechain() const noexcept { return sc; }

    /** Loudness column as shown: "M -14.2" style text for momentary, short-term, integrated
        loudness (LUFS) and the true-peak maximum (dBTP); "-" while there is nothing to show. */
    juce::String getLoudnessText (int row) const { return loudnessText[(size_t) juce::jlimit (0, 3, row)]; }

    void resized() override;

    void paint (juce::Graphics&) override;

    static constexpr float bottomDb = -60.0f, topDb = 6.0f;
    static constexpr double holdMs = 2000.0;
    static constexpr float silentBelowDb = -90.0f;
    static constexpr int preferredWidth = 214;
    static constexpr int loudnessColumnWidth = 82;

private:
    void update (Reading&, const rt::LevelMeter&, double nowMs, bool audioRunning);
    void drawMeter (juce::Graphics&, juce::Rectangle<float> area, const Reading&, const juce::String& title, bool dimmed) const;
    float yFor (juce::Rectangle<float> bar, float db) const noexcept;

    PluginProcessor& processor;
    Reading in, out, sc;
    std::array<juce::String, 4> loudnessText;
    juce::TextButton resetButton;
    SidechainState sidechainState = SidechainState::Off;
    double silentSinceMs = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterRail)
};
} // namespace tl::ui
