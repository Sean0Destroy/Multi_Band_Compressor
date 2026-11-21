#include "BandColumn.h"

namespace tl::ui
{
BandColumn::BandColumn (PluginProcessor& p, int slotNumber)
    : processor (p), slot (slotNumber), colour (theme::slotColour (slotNumber)), grBar (colour)
{
    auto& apvts = processor.getState();
    const auto id = [this] (const char* suffix) { return juce::String (schema::bandParamId (slot, suffix)); };
    const auto name = "Band " + juce::String (slot);

    fLo = apvts.getRawParameterValue (id (schema::band::fLo));
    fHi = apvts.getRawParameterValue (id (schema::band::fHi));
    bypassValue = apvts.getRawParameterValue (id (schema::band::bypass));
    jassert (fLo != nullptr && fHi != nullptr && bypassValue != nullptr);

    title.setText (juce::String (slot), juce::dontSendNotification);
    title.setFont (theme::font (14.0f, true));
    title.setJustificationType (juce::Justification::centred);
    title.setColour (juce::Label::textColourId, theme::background);
    title.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (title);

    bypass.setParameter (apvts, id (schema::band::bypass), "BYP", name + " Bypass");
    bypass.setTooltip ("Bypass this band");
    addAndMakeVisible (bypass);

    remove.setButtonText ("X");
    remove.setTitle ("Remove " + name);
    remove.setTooltip ("Remove this band");
    remove.setComponentID ("action_removeBand_" + juce::String (slot));
    remove.onClick = [this] { if (onRemove) onRemove (slot); };
    addAndMakeVisible (remove);

    addAndMakeVisible (grBar);

    threshold.setParameter (apvts, id (schema::band::thresh), "Threshold", name + " Threshold", colour);
    ratio.setParameter (apvts, id (schema::band::ratio), "Ratio", name + " Ratio", colour);
    attack.setParameter (apvts, id (schema::band::attack), "Attack", name + " Attack", colour);
    release.setParameter (apvts, id (schema::band::release), "Release", name + " Release", colour);

    for (auto* k : { &threshold, &ratio, &attack, &release })
        addAndMakeVisible (*k);

    setTitle (name);
    addMouseListener (this, true);   // a click on any child also selects the band
}

void BandColumn::setSelected (bool shouldBeSelected)
{
    if (selected != shouldBeSelected)
    {
        selected = shouldBeSelected;
        repaint();
    }
}

void BandColumn::refresh (double nowMs)
{
    const auto& meter = processor.getGainReductionMeter();
    const float gr = juce::jmin (meter.readAndResetHeldGainReductionDb (slot, 0),
                                 meter.readAndResetHeldGainReductionDb (slot, 1));
    grBar.update (gr, nowMs);

    const bool nowBypassed = bypassValue->load (std::memory_order_relaxed) >= 0.5f;
    if (nowBypassed != bypassedLook)
    {
        bypassedLook = nowBypassed;
        const float alpha = bypassedLook ? 0.45f : 1.0f;
        for (auto* c : std::initializer_list<juce::Component*> { &grBar, &threshold, &ratio, &attack, &release })
            c->setAlpha (alpha);
    }
}

void BandColumn::mouseDown (const juce::MouseEvent&)
{
    if (onSelect) onSelect (slot);
}

void BandColumn::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (selected ? theme::surface.brighter (0.06f) : theme::surface);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (selected ? colour : theme::border);
    g.drawRoundedRectangle (r, 6.0f, selected ? 1.5f : 1.0f);

    // Slot chip behind the number.
    g.setColour (colour);
    g.fillRoundedRectangle (title.getBounds().toFloat(), 4.0f);
}

void BandColumn::resized()
{
    auto r = getLocalBounds().reduced (theme::pad);

    auto header = r.removeFromTop (24);
    title.setBounds (header.removeFromLeft (28));
    remove.setBounds (header.removeFromRight (26));
    header.removeFromRight (4);
    bypass.setBounds (header.removeFromRight (40));

    r.removeFromTop (6);
    grBar.setBounds (r.removeFromTop (18));
    r.removeFromTop (4);

    auto top = r.removeFromTop (r.getHeight() / 2 + 6);
    threshold.setBounds (top.removeFromLeft (top.getWidth() / 2));
    ratio.setBounds (top);

    r = r.reduced (4, 0);
    attack.setBounds (r.removeFromLeft (r.getWidth() / 2));
    release.setBounds (r);
}
} // namespace tl::ui
