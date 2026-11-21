#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

#include "Theme.h"

// Reusable editor widgets. Every interactive widget carries a componentID that is either a
// registered parameter ID or "action_<name>" (the binding-audit test enforces this).
namespace tl::ui
{
class TlLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    TlLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
};

/** Rotary knob with a caption above and an editable value field below, bound to one parameter.
    The parameter can be swapped (inspector) with setParameter(). */
class ParamKnob final : public juce::Component
{
public:
    /** Knob: caption above, rotary, value below. Bar: caption left, horizontal slider, value right. */
    enum class Layout { Knob, Bar };

    explicit ParamKnob (Layout = Layout::Knob);
    ~ParamKnob() override;

    /** Binds to `paramId`. `accessibleTitle` is what screen readers announce (e.g. "Band 3 Threshold"). */
    void setParameter (juce::AudioProcessorValueTreeState&, const juce::String& paramId,
                       const juce::String& captionText, const juce::String& accessibleTitle,
                       juce::Colour accentColour);
    void clearParameter();

    void resized() override;
    juce::Slider& getSlider() noexcept { return slider; }

private:
    const Layout layout;
    juce::Slider slider;
    juce::Label caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamKnob)
};

/** Choice parameter as a drop-down with a caption above. */
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
    juce::Label caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamCombo)
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

/** Horizontal gain-reduction bar with a numeric read-out that holds the deepest value for 1 s. */
class GrBar final : public juce::Component
{
public:
    explicit GrBar (juce::Colour colour);

    /** currentDb <= 0. `nowMs` is a monotonic clock in milliseconds. */
    void update (float currentDb, double nowMs);

    float getCurrentDb() const noexcept { return current; }
    float getHeldDb() const noexcept    { return held; }

    void paint (juce::Graphics&) override;

    static constexpr float fullScaleDb = 24.0f;   // bar is full at -24 dB
    static constexpr double holdMs = 1000.0;

private:
    juce::Colour fill;
    float current = 0.0f, held = 0.0f;
    double heldAtMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrBar)
};
} // namespace tl::ui
