#include "Controls.h"

#include "ParameterSchema.h"
#include "ValueFormat.h"

namespace tl::ui
{
namespace
{
constexpr float kRotaryStart = juce::MathConstants<float>::pi * 1.25f;   // -135 degrees from 12 o'clock
constexpr float kRotaryEnd   = juce::MathConstants<float>::pi * 2.75f;   // +135 degrees
const juce::Identifier valuePxId { "tlValuePx" };

juce::Rectangle<int> knobArea (juce::Rectangle<int> bounds, int& knobBottom)
{
    const int hk = juce::jmax (0, bounds.getHeight() - theme::knobCaptionGap - theme::knobValueHeight);
    const int d = juce::jmin (bounds.getWidth(), hk);
    knobBottom = bounds.getY() + hk / 2 + (d + 1) / 2;
    return bounds.withHeight (hk);
}

bool isSegmentCell (const juce::Component& c) { return (bool) c.getProperties().getWithDefault (style::segment, false); }
}

// =============================================================================================
TlLookAndFeel::TlLookAndFeel()
    : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::getDarkColourScheme())
{
    setDefaultSansSerifTypeface (theme::typeface (theme::Face::LabelLight));

    setColour (juce::ResizableWindow::backgroundColourId, theme::panel);

    setColour (juce::Label::textColourId, theme::textMid);
    setColour (juce::Label::backgroundWhenEditingColourId, theme::background);
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

    setColour (juce::TextEditor::backgroundColourId, theme::background);
    setColour (juce::TextEditor::textColourId, theme::textHi);
    setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.4f));
    setColour (juce::TextEditor::highlightedTextColourId, theme::textHi);
    setColour (juce::TextEditor::outlineColourId, theme::border);
    setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    setColour (juce::CaretComponent::caretColourId, theme::textHi);

    setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    setColour (juce::TextButton::buttonOnColourId, theme::accent);
    setColour (juce::TextButton::textColourOffId, theme::textMid);
    setColour (juce::TextButton::textColourOnId, theme::accent);

    setColour (juce::ComboBox::backgroundColourId, theme::background);
    setColour (juce::ComboBox::outlineColourId, theme::border);
    setColour (juce::ComboBox::textColourId, theme::textHi);
    setColour (juce::ComboBox::arrowColourId, theme::textMid);
    setColour (juce::ComboBox::focusedOutlineColourId, theme::accent);

    setColour (juce::PopupMenu::backgroundColourId, theme::surface);
    setColour (juce::PopupMenu::textColourId, theme::textHi);
    setColour (juce::PopupMenu::headerTextColourId, theme::textMid);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent.withAlpha (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, theme::textHi);

    setColour (juce::TooltipWindow::backgroundColourId, theme::surface);
    setColour (juce::TooltipWindow::textColourId, theme::textHi);
    setColour (juce::TooltipWindow::outlineColourId, theme::border);
}

juce::Colour TlLookAndFeel::onColour (const juce::Button& b)
{
    return b.findColour (juce::TextButton::buttonOnColourId);
}

void TlLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                      float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float d = juce::jmin (area.getWidth(), area.getHeight());
    if (d < 8.0f) return;

    // Drawn in the 100-unit box of the design: ring radius 44, cap a hexagon of radius 32.
    const float s = d / 100.0f;
    const auto centre = area.getCentre();
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const float alpha = slider.isEnabled() ? 1.0f : 0.35f;
    const float ringR = 44.0f * s;

    const auto arc = [&] (float from, float to, float strokeWidth, juce::Colour colour)
    {
        if (to - from < 0.009f) to = from + 0.009f;      // a dot at the start
        juce::Path p;
        p.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f, from, to, true);
        g.setColour (colour.withMultipliedAlpha (alpha));
        g.strokePath (p, juce::PathStrokeType (strokeWidth * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };

    const bool hover = slider.isMouseOverOrDragging() && slider.isEnabled();
    arc (rotaryStartAngle, rotaryEndAngle, 4.0f, hover ? theme::graphite : theme::border);

    // The value ring: from the start, or from zero for a range around zero.
    const auto fill = slider.findColour (juce::Slider::rotarySliderFillColourId);
    const double lo = slider.getMinimum(), hi = slider.getMaximum();
    if (lo < 0.0 && hi > 0.0)
    {
        const float zero = rotaryStartAngle + (float) slider.valueToProportionOfLength (0.0) * (rotaryEndAngle - rotaryStartAngle);
        arc (juce::jmin (zero, angle), juce::jmax (zero, angle), 5.0f, fill);
    }
    else
    {
        arc (rotaryStartAngle, angle, 5.0f, fill);
    }

    // Matte circular face: lit from above, a soft shadow below, a fine pointer turned with the value.
    const float faceR = 33.0f * s;
    const auto face = juce::Rectangle<float> (faceR * 2.0f, faceR * 2.0f).withCentre (centre);
    g.setColour (juce::Colours::black.withAlpha (0.28f * alpha));
    g.fillEllipse (face.translated (0.0f, 2.0f * s).expanded (1.0f * s));
    {
        juce::ColourGradient grad (theme::elevated.brighter (0.10f).withMultipliedAlpha (alpha), centre.x, face.getY(),
                                   theme::elevated.darker (0.25f).withMultipliedAlpha (alpha), centre.x, face.getBottom(), false);
        g.setGradientFill (grad);
        g.fillEllipse (face);
    }
    g.setColour (theme::capEdge.withMultipliedAlpha (alpha));
    g.drawEllipse (face, juce::jmax (0.8f, 1.2f * s));
    {
        juce::Path rim;   // the top highlight
        rim.addCentredArc (centre.x, centre.y, faceR - 1.2f * s, faceR - 1.2f * s, 0.0f,
                           -juce::MathConstants<float>::pi * 0.38f, juce::MathConstants<float>::pi * 0.38f, true);
        g.setColour (theme::highlight.withMultipliedAlpha (alpha * 1.6f));
        g.strokePath (rim, juce::PathStrokeType (juce::jmax (0.8f, 1.2f * s)));
    }

    const auto turn = juce::AffineTransform::rotation (angle - juce::MathConstants<float>::twoPi)
                          .scaled (s).translated (centre.x, centre.y);
    juce::Path pointer;
    pointer.startNewSubPath (0.0f, -29.0f);
    pointer.lineTo (0.0f, -12.0f);
    g.setColour (theme::textHi.withMultipliedAlpha (alpha));
    g.strokePath (pointer, juce::PathStrokeType (juce::jmax (1.2f, 3.0f * s), juce::PathStrokeType::curved, juce::PathStrokeType::rounded), turn);

    if (slider.hasKeyboardFocus (true))
    {
        g.setColour (theme::accent);
        g.drawEllipse (juce::Rectangle<float> (d + 2.0f, d + 2.0f).withCentre (centre), 1.0f);
    }
}

juce::Slider::SliderLayout TlLookAndFeel::getSliderLayout (juce::Slider& slider)
{
    if (! (bool) slider.getProperties().getWithDefault (style::knob, false))
        return juce::LookAndFeel_V4::getSliderLayout (slider);

    // Knob at the top, the legend (drawn by the owner) under it, then the value.
    juce::Slider::SliderLayout layout;
    int knobBottom = 0;
    layout.sliderBounds = knobArea (slider.getLocalBounds(), knobBottom);
    layout.textBoxBounds = { 0, knobBottom + theme::knobCaptionGap, slider.getWidth(), theme::knobValueHeight };
    return layout;
}

juce::Label* TlLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox (slider);
    const float px = (float) (double) slider.getProperties().getWithDefault (valuePxId, (double) theme::valueFontSize);
    l->setFont (theme::valueFont (px));
    l->setJustificationType (juce::Justification::centred);
    l->setColour (juce::Label::textColourId, theme::textHi);
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setMinimumHorizontalScale (0.8f);
    l->setBorderSize ({ 1, 1, 1, 1 });          // narrow knobs need the room ("10.0 ms")
    return l;
}

juce::Font TlLookAndFeel::getLabelFont (juce::Label& l) { return l.getFont(); }

juce::Font TlLookAndFeel::getTextButtonFont (juce::TextButton& b, int)
{
    if ((bool) b.getProperties().getWithDefault (style::mono, false)) return theme::mono (11.0f);
    if (isSegmentCell (b)) return theme::label (10.0f, 0.18f);
    if ((bool) b.getProperties().getWithDefault (style::small, false)) return theme::label (9.0f, 0.14f);
    return theme::label (11.0f, 0.2f);
}

void TlLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    const bool on = b.getToggleState();

    if (isSegmentCell (b))
    {
        // The container (Segment) is recessed; the chosen cell is a soft ice pill inside it.
        const auto pill = r.reduced (2.0f);
        if (on)
        {
            g.setColour (b.isEnabled() ? theme::accent : theme::accent.withAlpha (0.4f));
            g.fillRoundedRectangle (pill, theme::radiusControl - 2.0f);
        }
        else if (highlighted && b.isEnabled())
        {
            g.setColour (theme::highlight);
            g.fillRoundedRectangle (pill, theme::radiusControl - 2.0f);
        }
        if (b.hasKeyboardFocus (false))
        {
            g.setColour (on ? theme::background : theme::accent);
            g.drawRoundedRectangle (pill.reduced (0.5f), theme::radiusControl - 2.0f, 1.0f);
        }
        return;
    }

    const auto lit = onColour (b);
    const auto box = r.reduced (0.5f);
    const float rad = juce::jmin (theme::radiusControl, r.getHeight() * 0.5f);

    // Unlit: a recessed pill. Lit: tinted, with a soft inner glow and a coloured edge.
    g.setColour (theme::background.withAlpha (0.55f));
    g.fillRoundedRectangle (box, rad);
    if (on && b.isEnabled())
    {
        g.setColour (lit.withAlpha (0.12f));
        g.fillRoundedRectangle (box, rad);
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clipShape;
        clipShape.addRoundedRectangle (box, rad);
        g.reduceClipRegion (clipShape);
        juce::ColourGradient glow (lit.withAlpha (0.20f), r.getCentreX(), r.getY(), lit.withAlpha (0.0f), r.getCentreX(), r.getY() + r.getHeight() * 0.6f, false);
        g.setGradientFill (glow);
        g.fillRect (r);
    }
    else if (highlighted && b.isEnabled())
    {
        g.setColour (theme::highlight);
        g.fillRoundedRectangle (box, rad);
    }

    g.setColour (on && b.isEnabled() ? lit : (highlighted && b.isEnabled() ? theme::graphite : theme::border));
    g.drawRoundedRectangle (box, rad, 1.0f);

    if (b.hasKeyboardFocus (false))
    {
        g.setColour (theme::accent);
        g.drawRoundedRectangle (r.expanded (1.5f), rad + 1.5f, 1.0f);
    }
}

void TlLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool)
{
    const bool on = b.getToggleState();
    const bool segment = isSegmentCell (b);
    const bool plainText = (bool) b.getProperties().getWithDefault (style::plain, false)
                           || (bool) b.getProperties().getWithDefault (style::mono, false);

    juce::Colour colour = theme::textMid;
    if (! b.isEnabled())             colour = theme::graphite;
    else if (on)                     colour = segment ? theme::background : onColour (b);
    else if (highlighted)            colour = theme::textHi;

    g.setColour (colour);
    const auto text = plainText ? b.getButtonText() : b.getButtonText().toUpperCase();
    theme::drawTracked (g, text, b.getLocalBounds().toFloat().reduced (4.0f, 0.0f), juce::Justification::centred,
                        getTextButtonFont (b, b.getHeight()));
}

void TlLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    const float rad = juce::jmin (theme::radiusControl, r.getHeight() * 0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, rad);
    g.setColour (box.hasKeyboardFocus (true) ? theme::accent : (box.isMouseOver (true) ? theme::graphite : theme::border));
    g.drawRoundedRectangle (r, rad, 1.0f);

    // A small chevron.
    const float cx = r.getRight() - 10.0f, cy = r.getCentreY();
    juce::Path arrow;
    arrow.startNewSubPath (cx - 3.5f, cy - 1.5f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 3.5f, cy - 1.5f);
    g.setColour (theme::textMid.withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.strokePath (arrow, juce::PathStrokeType (1.2f));
}

juce::Font TlLookAndFeel::getComboBoxFont (juce::ComboBox&) { return theme::mono (12.0f); }

void TlLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (1, 1, juce::jmax (0, box.getWidth() - 18), box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setMinimumHorizontalScale (0.75f);
}

juce::Font TlLookAndFeel::getPopupMenuFont() { return theme::label (14.0f); }

// =============================================================================================
CaptionLabel::CaptionLabel (float px, float trackEm) : size (px), tracking (trackEm)
{
    setInterceptsMouseClicks (false, false);
    setJustificationType (juce::Justification::centred);
    setColour (juce::Label::textColourId, theme::textMid);
}

juce::Font CaptionLabel::fontFor (const juce::String& upper) const
{
    // Full spacing if it fits, then tighter spacing, then a slightly smaller size.
    const float w = (float) getWidth();
    auto f = theme::label (size, tracking);
    if (theme::textWidth (f, upper) <= w) return f;
    f = theme::label (size, juce::jmin (tracking, 0.08f));
    if (theme::textWidth (f, upper) <= w) return f;
    return theme::label (size * 0.88f, 0.04f);
}

juce::String CaptionLabel::getDrawnText() const
{
    const auto full = (shortText.isNotEmpty() ? shortText : getText()).toUpperCase();
    if (theme::textWidth (fontFor (full), full) <= (float) getWidth()) return full;
    if (fallbackText.isNotEmpty())
    {
        const auto fb = fallbackText.toUpperCase();
        if (theme::textWidth (fontFor (fb), fb) <= (float) getWidth()) return fb;
        return fb.length() > 3 ? fb.substring (0, 3) : fb;
    }
    return full.length() > 3 ? full.substring (0, 3) : full;
}

void CaptionLabel::paint (juce::Graphics& g)
{
    const auto text = getDrawnText();
    g.setColour (findColour (juce::Label::textColourId).withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));
    theme::drawTracked (g, text, getLocalBounds().toFloat(), getJustificationType(), fontFor (text));
}

// =============================================================================================
ParamKnob::ParamKnob (float valuePixels, float captionPx, float captionTrackEm)
    : caption (captionPx, captionTrackEm), valuePx (valuePixels)
{
    addAndMakeVisible (caption);

    slider.getProperties().set (style::knob, true);
    slider.getProperties().set (valuePxId, (double) valuePx);
    slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider.setRotaryParameters (kRotaryStart, kRotaryEnd, true);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, theme::knobValueHeight);
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
    const auto r = getLocalBounds();
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, r.getWidth(), theme::knobValueHeight);
    slider.setBounds (r);

    int knobBottom = 0;
    knobArea (r, knobBottom);
    caption.setBounds (r.getX(), knobBottom, r.getWidth(), theme::knobCaptionGap);   // inside the knob, or it is clipped
}

// =============================================================================================
ParamCombo::ParamCombo()
{
    caption.setJustificationType (juce::Justification::centredLeft);
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
    caption.setBounds (r.removeFromTop (14));
    r.removeFromTop (4);
    combo.setBounds (r.removeFromTop (juce::jmin (26, r.getHeight())));
}

// =============================================================================================
ParamSlotCombo::ParamSlotCombo()
{
    caption.setJustificationType (juce::Justification::centredLeft);
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
    r.removeFromTop (4);
    combo.setBounds (r.removeFromTop (juce::jmin (26, r.getHeight())));
}

// =============================================================================================
Segment::Segment (bool monoCells) : mono (monoCells)
{
    setWantsKeyboardFocus (false);
}

Segment::~Segment() = default;

void Segment::setItems (const juce::StringArray& displayTexts, const juce::StringArray& texts)
{
    buttons.clear();
    itemTexts = texts.size() == displayTexts.size() ? texts : displayTexts;
    selected = -1;

    for (int i = 0; i < displayTexts.size(); ++i)
    {
        auto* b = buttons.add (new juce::TextButton (displayTexts[i]));
        b->getProperties().set (style::segment, true);
        if (mono) b->getProperties().set (style::mono, true);
        b->setClickingTogglesState (false);
        b->setComponentID (getComponentID());
        b->setTitle (getTitle().isNotEmpty() ? getTitle() + ": " + itemTexts[i] : itemTexts[i]);
        b->onClick = [this, i] { setSelectedItemIndex (i, juce::sendNotificationSync); };
        addAndMakeVisible (b);
    }
    resized();
}

void Segment::setSwitchId (const juce::String& componentId, const juce::String& accessibleTitle)
{
    setComponentID (componentId);
    setTitle (accessibleTitle);
    for (int i = 0; i < buttons.size(); ++i)
    {
        buttons[i]->setComponentID (componentId);
        buttons[i]->setTitle (accessibleTitle + ": " + itemTexts[i]);
    }
}

void Segment::setSelectedItemIndex (int index, juce::NotificationType notification)
{
    if (! juce::isPositiveAndBelow (index, buttons.size())) return;
    if (notification != juce::dontSendNotification && ! buttons[index]->isEnabled()) return;

    const bool moved = selected != index;
    selected = index;
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setToggleState (i == index, juce::dontSendNotification);
    if (moved) repaint();      // the dividers next to the lit cell change too

    if (notification != juce::dontSendNotification && onChange)
        onChange (index);
}

void Segment::setItemEnabled (int index, bool enabled)
{
    if (auto* b = buttons[index]) b->setEnabled (enabled);
}

bool Segment::isItemEnabled (int index) const
{
    if (auto* b = buttons[index]) return b->isEnabled();
    return false;
}

void Segment::setTooltip (const juce::String& text)
{
    for (auto* b : buttons)
        if (b->getTooltip().isEmpty()) b->setTooltip (text);
}

void Segment::setItemTooltip (int index, const juce::String& tip)
{
    if (auto* b = buttons[index]) b->setTooltip (tip);
}

int Segment::getPreferredWidth() const
{
    const auto f = mono ? theme::mono (11.0f) : theme::label (10.0f, 0.18f);
    const float padX = mono ? 7.0f : 9.0f;
    float w = 0.0f;
    for (auto* b : buttons)
        w += theme::textWidth (f, mono ? b->getButtonText() : b->getButtonText().toUpperCase()) + 2.0f * padX + 1.0f;
    return (int) std::ceil (w) + 1;
}

void Segment::resized()
{
    if (buttons.isEmpty()) return;

    const auto f = mono ? theme::mono (11.0f) : theme::label (10.0f, 0.18f);
    std::vector<float> want;
    float total = 0.0f;
    for (auto* b : buttons)
    {
        want.push_back (theme::textWidth (f, mono ? b->getButtonText() : b->getButtonText().toUpperCase()) + (mono ? 14.0f : 18.0f));
        total += want.back();
    }

    // Each cell gets its text width; what is left (or missing) is shared out evenly.
    const auto r = getLocalBounds().reduced (1);
    const float extra = ((float) r.getWidth() - total) / (float) buttons.size();
    float x = (float) r.getX();
    for (int i = 0; i < buttons.size(); ++i)
    {
        const float w = juce::jmax (8.0f, want[(size_t) i] + extra);
        const int x0 = juce::roundToInt (x), x1 = i == buttons.size() - 1 ? r.getRight() : juce::roundToInt (x + w);
        buttons[i]->setBounds (x0, r.getY(), x1 - x0, r.getHeight());
        x += w;
    }
}

void Segment::paint (juce::Graphics& g)
{
    // A recessed track the cells sit in.
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::background.withAlpha (0.7f));
    g.fillRoundedRectangle (r, juce::jmin (theme::radiusControl, r.getHeight() * 0.5f));
}

void Segment::paintOverChildren (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float rad = juce::jmin (theme::radiusControl, r.getHeight() * 0.5f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r, rad, 1.0f);

    // Faint dividers, only between two cells that are both unlit (a lit pill needs no edge).
    g.setColour (theme::border.withAlpha (0.6f));
    for (int i = 0; i + 1 < buttons.size(); ++i)
        if (! buttons[i]->getToggleState() && ! buttons[i + 1]->getToggleState())
            g.fillRect ((float) buttons[i]->getRight(), r.getY() + 4.0f, 1.0f, r.getHeight() - 8.0f);

    if (! isEnabled())
    {
        g.setColour (theme::panel.withAlpha (0.5f));
        g.fillRoundedRectangle (r, rad);
    }
}

// =============================================================================================
ParamSegment::ParamSegment (bool monoCells) : Segment (monoCells) {}

ParamSegment::~ParamSegment() { attachment.reset(); }

void ParamSegment::setParameter (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId,
                                 const juce::String& accessibleTitle, const juce::StringArray& displayTexts)
{
    attachment.reset();
    setComponentID (paramId);
    setTitle (accessibleTitle);

    auto* choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (paramId));
    if (choice == nullptr)
    {
        jassertfalse;   // not a choice parameter
        return;
    }

    setItems (displayTexts.size() == choice->choices.size() ? displayTexts : choice->choices, choice->choices);
    setSwitchId (paramId, accessibleTitle);

    attachment = std::make_unique<juce::ParameterAttachment> (*choice, [this] (float v)
    {
        setSelectedItemIndex (juce::roundToInt (v), juce::dontSendNotification);
    });
    onChange = [this] (int index)
    {
        if (attachment != nullptr) attachment->setValueAsCompleteGesture ((float) index);
    };
    attachment->sendInitialUpdate();
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

int GrBar::litSegments (float grDb) noexcept
{
    const float depth = -grDb;
    int n = 0;
    for (float step : ladderDb)
        if (depth >= step - 0.5f) ++n;
    return n;
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

    if (litSegments (current) != paintedLit)
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
    paintedLit = litSegments (current);

    const auto r = getLocalBounds().toFloat();
    const float gap = 2.0f;
    const int n = (int) ladderDb.size();
    const float w = (r.getWidth() - gap * (float) (n - 1)) / (float) n;
    for (int i = 0; i < n; ++i)
    {
        const auto cell = juce::Rectangle<float> (r.getX() + (float) i * (w + gap), r.getY(), w, r.getHeight());
        if (i < paintedLit)
        {
            g.setColour (fill.withAlpha (0.35f));
            g.fillRoundedRectangle (cell.expanded (1.0f), 2.0f);
            g.setColour (fill);
        }
        else
        {
            g.setColour (theme::ladderOff);
        }
        g.fillRoundedRectangle (cell, 1.5f);
    }
}
// =============================================================================================
Vfd::Vfd()
{
    setInterceptsMouseClicks (false, false);
    setJustificationType (juce::Justification::centred);
}

int Vfd::widthFor (const juce::String& text)
{
    return (int) std::ceil (theme::textWidth (theme::mono (12.0f), text)) + 22;
}

void Vfd::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const auto box = r.reduced (0.5f);
    const float rad = juce::jmin (theme::radiusControl, box.getHeight() * 0.5f);
    g.setColour (theme::background);
    g.fillRoundedRectangle (box, rad);
    g.setColour (theme::border.withAlpha (0.8f));
    g.drawRoundedRectangle (box, rad, 1.0f);

    // Ice text with a soft glow, like a vacuum-fluorescent display.
    g.setFont (theme::mono (12.0f));
    g.setColour (theme::iceGlow.withMultipliedAlpha (0.5f));
    for (auto d : { -0.6f, 0.6f })
        g.drawText (getText(), r.translated (d, 0.0f), juce::Justification::centred, false);
    g.setColour (theme::accent);
    g.drawText (getText(), r, juce::Justification::centred, false);
}
} // namespace tl::ui
