#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "Theme.h"

// Reusable editor widgets. Every interactive widget carries a componentID that is either a
// registered parameter ID or "action_<name>" (the binding-audit test enforces this).
namespace tl::ui
{
/** Property names the look-and-feel reads from a component. */
namespace style
{
inline const juce::Identifier segment { "tlSegment" };   // a cell of a segmented switch
inline const juce::Identifier mono { "tlMono" };         // text in DM Mono, as given
inline const juce::Identifier plain { "tlPlain" };       // text as given (not upper case)
inline const juce::Identifier knob { "tlKnob" };         // rotary slider with a legend under the knob
inline const juce::Identifier small { "tlSmall" };       // small button text (strip footer, reset)
}

class TlLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    TlLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Font getLabelFont (juce::Label&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;

    /** Colour a lit button shows: ice, or amber for bypass and listen (buttonOnColourId). */
    static juce::Colour onColour (const juce::Button&);
};

/** A legend: upper case, tracked Jost. Its text keeps the case it was given (tests and screen
    readers see "Threshold"); a legend too wide for its box is shortened to three letters. */
class CaptionLabel final : public juce::Label
{
public:
    explicit CaptionLabel (float px = theme::captionFontSize, float trackEm = 0.22f);
    void paint (juce::Graphics&) override;
    void setTracking (float px, float trackEm) { size = px; tracking = trackEm; repaint(); }
    /** What is drawn instead of the text (e.g. "Att" for "Attack"); empty: the text. */
    void setShortText (const juce::String& t) { shortText = t; repaint(); }
    /** Drawn only when the text does not fit, even with tighter letter spacing (e.g. "Lo Freq"). */
    void setFallbackText (const juce::String& t) { fallbackText = t; repaint(); }
    /** What paint() draws at the current width (upper case). */
    juce::String getDrawnText() const;

private:
    juce::Font fontFor (const juce::String& upper) const;
    float size, tracking;
    juce::String shortText, fallbackText;
};

/** Rotary knob with a hexagonal cap, its legend and an editable value under it, bound to one
    parameter. The parameter can be swapped (inspector) with setParameter(). */
class ParamKnob final : public juce::Component
{
public:
    explicit ParamKnob (float valuePx = theme::valueFontSize, float captionPx = theme::captionFontSize,
                        float captionTrackEm = 0.22f);
    ~ParamKnob() override;

    /** Binds to `paramId`. `accessibleTitle` is what screen readers announce (e.g. "Band 3 Threshold"). */
    void setParameter (juce::AudioProcessorValueTreeState&, const juce::String& paramId,
                       const juce::String& captionText, const juce::String& accessibleTitle,
                       juce::Colour accentColour);
    void clearParameter();
    /** Legend drawn in place of the caption text (which screen readers and tests still see). */
    void setShortCaption (const juce::String& t) { caption.setShortText (t); }
    /** Legend drawn only when the caption does not fit the knob's width. */
    void setFallbackCaption (const juce::String& t) { caption.setFallbackText (t); }
    const CaptionLabel& getCaption() const noexcept { return caption; }

    /** Height of a knob of `diameter` with its legend and value. */
    static constexpr int heightFor (int diameter) noexcept { return diameter + theme::knobCaptionGap + theme::knobValueHeight; }

    void resized() override;
    juce::Slider& getSlider() noexcept { return slider; }

private:
    juce::Slider slider;
    CaptionLabel caption;
    const float valuePx;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamKnob)
};

/** Choice parameter as a drop-down with its legend above. */
class ParamCombo final : public juce::Component
{
public:
    ParamCombo();
    ~ParamCombo() override;

    void setParameter (juce::AudioProcessorValueTreeState&, const juce::String& paramId,
                       const juce::String& captionText, const juce::String& accessibleTitle);
    void clearParameter();

    void resized() override;
    juce::ComboBox& getCombo() noexcept { return combo; }

private:
    juce::ComboBox combo;
    CaptionLabel caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamCombo)
};

/** Integer "band slot" parameter (1..16) as a drop-down whose items the owner supplies
    (item id = slot number). Used for the band a detector follows. */
class ParamSlotCombo final : public juce::Component
{
public:
    ParamSlotCombo();
    ~ParamSlotCombo() override;

    void setParameter (juce::AudioProcessorValueTreeState&, const juce::String& paramId,
                       const juce::String& captionText, const juce::String& accessibleTitle);
    void clearParameter();

    /** Replaces the list: one "Band N" item per slot. The current value is always listed: as
        "This band" if it is `ownSlot`, otherwise marked "(unused)" if it is not among `slots`. */
    void setSlots (const std::vector<int>& slots, int ownSlot);
    int getSlotValue() const noexcept { return currentValue; }

    void resized() override;
    juce::ComboBox& getCombo() noexcept { return combo; }

private:
    void rebuild();

    juce::ComboBox combo;
    CaptionLabel caption;
    std::vector<int> listed;
    int currentValue = 1, own = 0;
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSlotCombo)
};

/** A segmented switch: one cell per choice, exactly one lit. Every cell carries the switch's
    componentID. Item indices start at 0. */
class Segment : public juce::Component
{
public:
    /** `monoCells`: short cells in DM Mono (2x, 4x, A3). */
    explicit Segment (bool monoCells = false);
    ~Segment() override;

    /** `displayTexts` are what the cells show; `itemTexts` (same count) what getItemText returns
        (default: the display texts). */
    void setItems (const juce::StringArray& displayTexts, const juce::StringArray& itemTexts = {});
    int getNumItems() const noexcept { return buttons.size(); }
    juce::String getItemText (int index) const { return itemTexts[index]; }
    int getSelectedItemIndex() const noexcept { return selected; }
    /** Lights `index`; with a notification it counts as the user's choice (onChange). */
    void setSelectedItemIndex (int index, juce::NotificationType = juce::sendNotificationSync);
    void setItemEnabled (int index, bool enabled);
    bool isItemEnabled (int index) const;
    void setItemTooltip (int index, const juce::String&);
    /** Tooltip of every cell that has none of its own. */
    void setTooltip (const juce::String&);
    /** componentID of the switch and of every cell, and the title screen readers announce. */
    void setSwitchId (const juce::String& componentId, const juce::String& accessibleTitle);
    juce::TextButton* getCell (int index) const { return buttons[index]; }

    /** Width the cells need for their text. */
    int getPreferredWidth() const;

    std::function<void (int index)> onChange;

    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void enablementChanged() override { repaint(); }

private:
    juce::OwnedArray<juce::TextButton> buttons;
    juce::StringArray itemTexts;
    int selected = -1;
    const bool mono;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Segment)
};

/** A choice parameter as a segmented switch. */
class ParamSegment final : public Segment
{
public:
    explicit ParamSegment (bool monoCells = false);
    ~ParamSegment() override;

    /** `displayTexts` replaces the cell texts (same count as the choices); empty: the choices. */
    void setParameter (juce::AudioProcessorValueTreeState&, const juce::String& paramId,
                       const juce::String& accessibleTitle, const juce::StringArray& displayTexts = {});

private:
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSegment)
};

/** Bool parameter as a latching text button (lit = parameter true). */
class ParamToggle final : public juce::TextButton
{
public:
    ParamToggle();
    ~ParamToggle() override;

    void setParameter (juce::AudioProcessorValueTreeState&, const juce::String& paramId,
                       const juce::String& buttonText, const juce::String& accessibleTitle);
    void clearParameter();

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamToggle)
};

/** A read-out in a recessed obsidian box: ice DM Mono (latency, band count). */
class Vfd final : public juce::Label
{
public:
    Vfd();
    void paint (juce::Graphics&) override;
    /** Width the box needs for `text`. */
    static int widthFor (const juce::String& text);
};

/** Gain-reduction meter of a band: a ladder of 12 segments (1 to 24 dB) that light in the band
    colour. Holds the deepest value for 1 s (getHeldDb). */
class GrBar final : public juce::Component
{
public:
    explicit GrBar (juce::Colour colour);

    /** currentDb <= 0. `nowMs` is a monotonic clock in milliseconds. */
    void update (float currentDb, double nowMs);

    /** Back to 0 dB with no hold (the band was hidden or the audio stopped). */
    void reset();

    float getCurrentDb() const noexcept { return current; }
    float getHeldDb() const noexcept    { return held; }

    static constexpr std::array<float, 12> ladderDb { 1, 2, 3, 4, 6, 8, 10, 12, 15, 18, 21, 24 };
    /** How many ladder segments are lit for `grDb` (<= 0). */
    static int litSegments (float grDb) noexcept;

    void paint (juce::Graphics&) override;

    static constexpr float fullScaleDb = 24.0f;   // the ladder is full at -24 dB
    static constexpr double holdMs = 1000.0;

private:
    juce::Colour fill;
    float current = 0.0f, held = 0.0f;
    int paintedLit = -1;   // what is on screen
    double heldAtMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrBar)
};
} // namespace tl::ui
