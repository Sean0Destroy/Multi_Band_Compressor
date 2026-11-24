#pragma once

#include "PluginProcessor.h"
#include "Controls.h"

#include <array>

namespace tl::ui
{
/** Top of the master strip: the MASTER title with the sidechain state and the loudness reset,
    input, output and sidechain level bars (peak with a 2 s hold line) and the loudness cells
    (momentary, short-term, integrated, true peak). Message thread only; reads the lock-free
    meters the audio thread publishes. */
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

    /** Height of a level bar for `db` as a fraction of the bar (0 at -48 dB and below, 1 at 0 dB). */
    static float barFraction (float db) noexcept;

    static constexpr double holdMs = 2000.0;
    static constexpr float silentBelowDb = -90.0f;
    /** The meter well is 128 high at the preferred height and gives up to 48 of it when the
        window is short. */
    static constexpr int headHeight = 42, wellHeight = 128, preferredHeight = 232;
    static constexpr int minWellHeight = 80, minHeight = preferredHeight - wellHeight + minWellHeight;

private:
    void update (Reading&, const rt::LevelMeter&, double nowMs, bool audioRunning);
    void drawGroup (juce::Graphics&, juce::Rectangle<float> area, const Reading&, const juce::String& title, bool dimmed) const;

    PluginProcessor& processor;
    Reading in, out, sc;
    std::array<juce::String, 4> loudnessText;
    juce::TextButton resetButton;
    SidechainState sidechainState = SidechainState::Off;
    double silentSinceMs = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterRail)
};
} // namespace tl::ui
