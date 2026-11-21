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

    // The slot chip is a real button so that a band can be selected from the keyboard.
    select.setButtonText (juce::String (slot));
    select.setTitle ("Select " + name);
    select.setTooltip ("Select this band");
    select.setComponentID ("action_selectBand_" + juce::String (slot));
    select.setColour (juce::TextButton::buttonColourId, colour);
    select.setColour (juce::TextButton::buttonOnColourId, colour);
    select.setColour (juce::TextButton::textColourOffId, theme::background);
    select.setColour (juce::TextButton::textColourOnId, theme::background);
    select.onClick = [this] { if (onSelect) onSelect (slot); };
    addAndMakeVisible (select);

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

void BandColumn::resetMeter()
{
    const auto& meter = processor.getGainReductionMeter();
    meter.readAndResetHeldGainReductionDb (slot, 0);
    meter.readAndResetHeldGainReductionDb (slot, 1);
    grBar.reset();
}

void BandColumn::refresh (double nowMs, bool audioRunning)
{
    // Always consume the hold (so nothing stale is kept); the deeper of the two channels is shown.
    const auto& meter = processor.getGainReductionMeter();
    const float gr = juce::jmin (meter.readAndResetHeldGainReductionDb (slot, 0),
                                 meter.readAndResetHeldGainReductionDb (slot, 1));
    grBar.update (audioRunning ? gr : 0.0f, nowMs);

    const bool nowBypassed = bypassValue->load (std::memory_order_relaxed) >= 0.5f;
    if (nowBypassed != bypassedLook)
    {
        bypassedLook = nowBypassed;
        const float alpha = bypassedLook ? 0.45f : 1.0f;
        for (auto* c : std::initializer_list<juce::Component*> { &grBar, &threshold, &ratio, &attack, &release })
            c->setAlpha (alpha);
    }
}

void BandColumn::mouseDown (const juce::MouseEvent& e)
{
    // A click anywhere in the column selects the band, except on the remove button: removing
    // another band must not move the selection.
    if (e.eventComponent == &remove || remove.isParentOf (e.eventComponent))
        return;

    if (onSelect) onSelect (slot);
}

void BandColumn::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (selected ? theme::surface.brighter (0.06f) : theme::surface);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (selected ? colour : theme::border);
    g.drawRoundedRectangle (r, 6.0f, selected ? 1.5f : 1.0f);
}

void BandColumn::resized()
{
    auto r = getLocalBounds().reduced (theme::pad);

    auto header = r.removeFromTop (24);
    select.setBounds (header.removeFromLeft (30));
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
