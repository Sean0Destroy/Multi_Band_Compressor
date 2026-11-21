#include "Controls.h"

#include "ParameterSchema.h"
#include "ValueFormat.h"

namespace tl::ui
{
// =============================================================================================
TlLookAndFeel::TlLookAndFeel()
    : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::getDarkColourScheme())
{
    setColour (juce::ResizableWindow::backgroundColourId, theme::background);

    setColour (juce::Label::textColourId, theme::textMid);
    setColour (juce::Label::backgroundWhenEditingColourId, theme::elevated);
    setColour (juce::Label::textWhenEditingColourId, theme::textHi);
    setColour (juce::Label::outlineWhenEditingColourId, theme::accent);

    setColour (juce::Slider::textBoxTextColourId, theme::textHi);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, theme::accent.withAlpha (0.4f));
    setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, theme::border);
    setColour (juce::Slider::backgroundColourId, theme::border);
    setColour (juce::Slider::trackColourId, theme::accent);
    setColour (juce::Slider::thumbColourId, theme::textHi);

    setColour (juce::TextEditor::backgroundColourId, theme::elevated);
    setColour (juce::TextEditor::textColourId, theme::textHi);
    setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.4f));
    setColour (juce::TextEditor::highlightedTextColourId, theme::textHi);
    setColour (juce::TextEditor::outlineColourId, theme::border);
    setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    setColour (juce::CaretComponent::caretColourId, theme::textHi);

    setColour (juce::TextButton::buttonColourId, theme::elevated);
    setColour (juce::TextButton::buttonOnColourId, theme::textMid);
    setColour (juce::TextButton::textColourOffId, theme::textHi);
    setColour (juce::TextButton::textColourOnId, theme::background);
    setColour (juce::ComboBox::outlineColourId, theme::border);

    setColour (juce::ComboBox::backgroundColourId, theme::elevated);
    setColour (juce::ComboBox::textColourId, theme::textHi);
    setColour (juce::ComboBox::arrowColourId, theme::textMid);
    setColour (juce::ComboBox::focusedOutlineColourId, theme::accent);

    setColour (juce::PopupMenu::backgroundColourId, theme::surface);
    setColour (juce::PopupMenu::textColourId, theme::textHi);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent.withAlpha (0.35f));
    setColour (juce::PopupMenu::highlightedTextColourId, theme::textHi);
}

void TlLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                      float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    if (radius < 4.0f) return;

    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const float arcWidth = juce::jmax (2.0f, radius * 0.14f);
    const float arcRadius = radius - arcWidth * 0.5f;
    const float alpha = slider.isEnabled() ? 1.0f : 0.35f;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId).withMultipliedAlpha (alpha));
    g.strokePath (track, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId).withMultipliedAlpha (alpha));
    g.strokePath (value, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Matte body with a thin bezel.
    const float bodyRadius = radius - arcWidth - 2.0f;
    if (bodyRadius > 2.0f)
    {
        const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);
        g.setColour (theme::elevated.withMultipliedAlpha (alpha));
        g.fillEllipse (body);
        g.setColour (theme::border.brighter (0.25f).withMultipliedAlpha (alpha));
        g.drawEllipse (body, 1.0f);

        juce::Path pointer;
        pointer.addRoundedRectangle (-1.0f, -bodyRadius + 2.0f, 2.0f, bodyRadius * 0.55f, 1.0f);
        g.setColour (theme::textHi.withMultipliedAlpha (alpha));
        g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
    }

    if (slider.hasKeyboardFocus (true))
    {
        g.setColour (theme::accent);
        g.drawEllipse (bounds.withSizeKeepingCentre (radius * 2.0f, radius * 2.0f), 1.0f);
    }
}

juce::Label* TlLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox (slider);
    l->setFont (theme::font (theme::valueFontSize + 1.0f));
    l->setJustificationType (juce::Justification::centred);
    return l;
}

juce::Font TlLookAndFeel::getLabelFont (juce::Label& l)                { return l.getFont(); }
juce::Font TlLookAndFeel::getTextButtonFont (juce::TextButton&, int h) { return theme::font (juce::jmin (13.0f, (float) h * 0.6f)); }
juce::Font TlLookAndFeel::getComboBoxFont (juce::ComboBox&)            { return theme::font (theme::valueFontSize); }

// =============================================================================================
ParamKnob::ParamKnob (Layout l) : layout (l)
{
    const bool bar = layout == Layout::Bar;

    caption.setFont (theme::font (theme::captionFontSize + (bar ? 1.0f : 0.0f)));
    caption.setJustificationType (bar ? juce::Justification::centredRight : juce::Justification::centred);
    caption.setInterceptsMouseClicks (false, false);
    caption.setColour (juce::Label::textColourId, theme::textMid);
    addAndMakeVisible (caption);

    slider.setSliderStyle (bar ? juce::Slider::LinearHorizontal : juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (bar ? juce::Slider::TextBoxRight : juce::Slider::TextBoxBelow, false, bar ? 64 : 72, 16);
    slider.setWantsKeyboardFocus (true);
    addAndMakeVisible (slider);
}

ParamKnob::~ParamKnob() { clearParameter(); }

void ParamKnob::clearParameter()
{
    attachment.reset();
    slider.setComponentID ({});
}

void ParamKnob::setParameter (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId,
                              const juce::String& captionText, const juce::String& accessibleTitle,
                              juce::Colour accentColour)
{
    clearParameter();

    caption.setText (captionText, juce::dontSendNotification);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accentColour);
    slider.setColour (juce::Slider::trackColourId, accentColour);
    slider.setTitle (accessibleTitle);
    slider.setComponentID (paramId);

    if (apvts.getParameter (paramId) == nullptr)
    {
        jassertfalse;   // unknown parameter: the knob stays unbound
        return;
    }

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramId, slider);

    // Display text with units (the attachment installed the host-facing, unit-less text).
    if (const auto* spec = schema::findSpec (paramId.toStdString()))
    {
        const auto unit = fmt::unitFor (*spec);
        slider.textFromValueFunction = [unit] (double v) { return fmt::toText (unit, v); };
        slider.valueFromTextFunction = [unit, this] (const juce::String& t) { return fmt::fromText (unit, t, slider.getValue()); };
        slider.updateText();
    }
}

void ParamKnob::resized()
{
    auto r = getLocalBounds();

    if (layout == Layout::Bar)
    {
        caption.setBounds (r.removeFromLeft (56));
        r.removeFromLeft (4);
        slider.setBounds (r);
        return;
    }

    caption.setBounds (r.removeFromTop (14));
    slider.setBounds (r);
}

// =============================================================================================
ParamCombo::ParamCombo (Layout l) : layout (l)
{
    const bool bar = layout == Layout::Bar;
    caption.setFont (theme::font (theme::captionFontSize + (bar ? 1.0f : 0.0f)));
    caption.setJustificationType (bar ? juce::Justification::centredRight : juce::Justification::centred);
    caption.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (caption);
    addAndMakeVisible (combo);
}

ParamCombo::~ParamCombo() { clearParameter(); }

void ParamCombo::clearParameter()
{
    attachment.reset();
    combo.setComponentID ({});
}

void ParamCombo::setParameter (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId,
                               const juce::String& captionText, const juce::String& accessibleTitle)
{
    clearParameter();

    caption.setText (captionText, juce::dontSendNotification);
    combo.setTitle (accessibleTitle);
    combo.setComponentID (paramId);
    combo.clear (juce::dontSendNotification);

    auto* choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (paramId));
    if (choice == nullptr)
    {
        jassertfalse;   // not a choice parameter: the combo stays unbound
        return;
    }

    combo.addItemList (choice->choices, 1);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, paramId, combo);
}

void ParamCombo::resized()
{
    auto r = getLocalBounds();

    if (layout == Layout::Bar)
    {
        caption.setBounds (r.removeFromLeft (40));
        r.removeFromLeft (4);
        combo.setBounds (r.withSizeKeepingCentre (r.getWidth(), juce::jmin (24, r.getHeight())));
        return;
    }

    caption.setBounds (r.removeFromTop (14));
    combo.setBounds (r.removeFromTop (juce::jmin (24, r.getHeight())).reduced (2, 0));
}

// =============================================================================================
ParamSlotCombo::ParamSlotCombo()
{
    caption.setFont (theme::font (theme::captionFontSize));
    caption.setJustificationType (juce::Justification::centred);
    caption.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (caption);
    addAndMakeVisible (combo);

    combo.onChange = [this]
    {
        const int id = combo.getSelectedId();
        if (attachment != nullptr && id > 0 && id != currentValue)
            attachment->setValueAsCompleteGesture ((float) id);
    };
}

ParamSlotCombo::~ParamSlotCombo() { clearParameter(); }

void ParamSlotCombo::clearParameter()
{
    attachment.reset();
    combo.setComponentID ({});
}

void ParamSlotCombo::setParameter (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId,
                                   const juce::String& captionText, const juce::String& accessibleTitle)
{
    clearParameter();

    caption.setText (captionText, juce::dontSendNotification);
    combo.setTitle (accessibleTitle);
    combo.setComponentID (paramId);

    auto* prm = apvts.getParameter (paramId);
    if (prm == nullptr)
    {
        jassertfalse;
        return;
    }

    attachment = std::make_unique<juce::ParameterAttachment> (*prm, [this] (float v)
    {
        currentValue = juce::roundToInt (v);
        rebuild();
    });
    attachment->sendInitialUpdate();
}

void ParamSlotCombo::setSlots (const std::vector<int>& slots, int ownSlot)
{
    if (slots != listed || ownSlot != own)
    {
        listed = slots;
        own = ownSlot;
        rebuild();
    }
}

void ParamSlotCombo::rebuild()
{
    combo.clear (juce::dontSendNotification);

    bool currentListed = false;
    for (int slot : listed)
    {
        combo.addItem ("Band " + juce::String (slot), slot);
        currentListed = currentListed || slot == currentValue;
    }

    if (! currentListed && currentValue > 0)
        combo.addItem (currentValue == own ? juce::String ("This band")
                                           : "Band " + juce::String (currentValue) + " (unused)", currentValue);

    combo.setSelectedId (currentValue, juce::dontSendNotification);
}

void ParamSlotCombo::resized()
{
    auto r = getLocalBounds();
    caption.setBounds (r.removeFromTop (14));
    combo.setBounds (r.removeFromTop (juce::jmin (24, r.getHeight())).reduced (2, 0));
}

// =============================================================================================
ParamToggle::ParamToggle()
{
    setClickingTogglesState (true);
}

ParamToggle::~ParamToggle() { clearParameter(); }

void ParamToggle::clearParameter()
{
    attachment.reset();
    setComponentID ({});
}

void ParamToggle::setParameter (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId,
                                const juce::String& buttonText, const juce::String& accessibleTitle)
{
    clearParameter();

    setButtonText (buttonText);
    setTitle (accessibleTitle);
    setComponentID (paramId);

    if (apvts.getParameter (paramId) == nullptr)
    {
        jassertfalse;
        return;
    }

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, paramId, *this);
}

// =============================================================================================
GrBar::GrBar (juce::Colour colour) : fill (colour)
{
    setInterceptsMouseClicks (false, false);
}

void GrBar::update (float currentDb, double nowMs)
{
    const float c = juce::jmin (0.0f, currentDb);
    float h = held;
    double at = heldAtMs;

    if (c <= h || nowMs - at >= holdMs)
    {
        h = c;
        at = nowMs;
    }

    current = c;
    held = h;
    heldAtMs = at;

    // Compare with what is on screen (not with the previous call): a slow release moves less
    // than the threshold per tick but must still be redrawn as it adds up.
    if (std::abs (current - paintedCurrent) > 0.05f || std::abs (held - paintedHeld) > 0.05f)
        repaint();
}

void GrBar::reset()
{
    current = held = 0.0f;
    heldAtMs = 0.0;
    repaint();
}

void GrBar::paint (juce::Graphics& g)
{
    paintedCurrent = current;
    paintedHeld = held;

    auto r = getLocalBounds().toFloat();
    auto text = r.removeFromRight (46.0f);
    auto bar = r.withSizeKeepingCentre (r.getWidth(), 6.0f);

    g.setColour (theme::background);
    g.fillRoundedRectangle (bar, 3.0f);

    const float amount = juce::jlimit (0.0f, 1.0f, -current / fullScaleDb);
    if (amount > 0.0f)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * amount)), 3.0f);
    }

    g.setColour (theme::textHi);
    g.setFont (theme::font (theme::valueFontSize));
    g.drawText (juce::String (std::abs (held) < 0.05f ? 0.0f : held, 1), text, juce::Justification::centredRight, false);
}
} // namespace tl::ui
