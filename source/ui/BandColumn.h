#pragma once

#include <functional>

#include "Controls.h"
#include "PluginProcessor.h"

namespace tl::ui
{
/** One console strip, permanently tied to a parameter slot (1..8). The editor only shows the
    strips of used slots. Header (slot chip, name by frequency, range; selects the band), gain
    reduction ladder, threshold, ratio, attack and release, then bypass and remove. */
class BandColumn final : public juce::Component
{
public:
    BandColumn (PluginProcessor&, int slot);

    int getSlot() const noexcept { return slot; }
    /** Lower / upper edge as the engine uses them (the two parameters may be crossed). */
    float getLowHz() const noexcept  { return juce::jmin (fLo->load (std::memory_order_relaxed), fHi->load (std::memory_order_relaxed)); }
    float getHighHz() const noexcept { return juce::jmax (fLo->load (std::memory_order_relaxed), fHi->load (std::memory_order_relaxed)); }

    /** Name of a band by its centre frequency (SUB, LOW, LOW MID, MID, PRESENCE, UPPER, AIR). */
    static juce::String nameFor (double lowHz, double highHz);
    /** Compact frequency: "120", "1.2k", "12k". */
    static juce::String compactHz (double hz);
    /** The header's range text, e.g. "120 – 1.2k Hz", in ascending order. */
    juce::String getRangeText() const;
    juce::String getNameText() const { return nameFor (getLowHz(), getHighHz()); }

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

    static constexpr int headerHeight = 50;

private:
    /** The strip header: a real button, so that a band can be selected from the keyboard. */
    struct Header final : public juce::Button
    {
        explicit Header (BandColumn& o) : juce::Button ({}), owner (o) {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        BandColumn& owner;
    };

    PluginProcessor& processor;
    const int slot;
    const juce::Colour colour;
    std::atomic<float>* fLo = nullptr;
    std::atomic<float>* fHi = nullptr;
    std::atomic<float>* bypassValue = nullptr;
    bool selected = false, bypassedLook = false;
    juce::String shownRange, shownName;

    Header select { *this };
    ParamToggle bypass;
    juce::TextButton remove;
    GrBar grBar;
    ParamKnob threshold { 10.0f, 8.0f, 0.18f }, ratio { 10.0f, 8.0f, 0.18f },
              attack { 10.0f, 8.0f, 0.18f }, release { 10.0f, 8.0f, 0.18f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandColumn)
};

/** What stands in an unused place of the strip row: hatched, dashed edge, "Empty" and "+ Add". */
class EmptyStrip final : public juce::Component
{
public:
    EmptyStrip();
    std::function<void()> onAdd;
    juce::TextButton& getAddButton() noexcept { return add; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::TextButton add;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EmptyStrip)
};
} // namespace tl::ui
