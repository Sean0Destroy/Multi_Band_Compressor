#pragma once

#include <array>

#include "Controls.h"
#include "PluginProcessor.h"

namespace tl::ui
{
/** Analog-style needle for the gain reduction of the selected band: brass scale from 0 to 20 dB
    (square-root spacing, so the first few dB get the most room) and a needle with a short
    ballistic (time constant 0.12 s). Display only; message thread. */
class NeedleMeter final : public juce::Component
{
public:
    NeedleMeter();

    /** The band the needle shows (0: none, the needle rests at 0). */
    void setSlot (int slot);
    /** From the editor's timer: `grDb` <= 0 is the band's current gain reduction. */
    void update (float grDb, double nowMs);

    /** Needle angle for `grDb` of reduction (>= 0), in degrees from vertical: +50 at 0 dB, -50 at 20 dB. */
    static float angleFor (float reductionDb) noexcept;
    float getNeedleAngle() const noexcept { return angle; }
    juce::String getTitleText() const;

    void paint (juce::Graphics&) override;

    static constexpr float timeConstantSeconds = 0.12f;

private:
    int slot = 0;
    float angle = 50.0f, paintedAngle = 1000.0f;
    double lastMs = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeedleMeter)
};

/** The output clipper as you hear it: a scrolling view of the signal going into the clipper (one
    column per 1/120 s): ice for what passes, red (hard) or brass (soft) for what is cut, the
    ceiling as a line (dashed while the clipper is off). Under it, how far the peaks are shaved,
    held for a second. Reads PluginProcessor::getClipMeter (lock-free); message thread only. */
class ClipView final : public juce::Component
{
public:
    explicit ClipView (PluginProcessor&);

    /** From the editor's timer. `audioRunning` false clears the view. */
    void refresh (double nowMs, bool audioRunning);

    /** Deepest shave of the last refresh and the held value shown (dB, >= 0). */
    float getShavedNowDb() const noexcept  { return shavedNow; }
    float getShavedHeldDb() const noexcept { return shavedHeld; }
    /** "-1.2 dB" (with a minus sign), or "off" while the clipper is off. */
    juce::String getShavedText() const;
    /** "Ceiling -0.3" or "Clipper off". */
    juce::String getTagText() const;
    /** Columns shown, oldest first: input and output peaks (linear). */
    int getNumColumns() const noexcept { return kColumns; }
    const dsp::ClipMeter::Column& getColumn (int index) const noexcept { return columns[(size_t) ((head + index) % kColumns)]; }

    void paint (juce::Graphics&) override;

    static constexpr int kColumns = 110;           // about 0.9 s
    static constexpr int scopeHeight = 74;
    static constexpr int preferredHeight = scopeHeight + 8 + 16;
    static constexpr double holdMs = 1000.0;
    static constexpr float fullScaleShaveDb = 6.0f;

private:
    PluginProcessor& processor;
    std::atomic<float>* modeValue = nullptr;
    std::atomic<float>* ceilingValue = nullptr;
    const dsp::ClipMeter* source = nullptr;
    std::uint32_t cursor = 0;
    std::array<dsp::ClipMeter::Column, kColumns> columns {};
    std::array<dsp::ClipMeter::Column, dsp::ClipMeter::kRingSize> scratch {};
    int head = 0;
    float shavedNow = 0.0f, shavedHeld = 0.0f;
    double heldAtMs = 0.0, lastMs = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipView)
};
} // namespace tl::ui
