#include "BandColumn.h"

namespace tl::ui
{
juce::String BandColumn::nameFor (double lowHz, double highHz)
{
    const double lo = juce::jlimit (10.0, 30000.0, juce::jmin (lowHz, highHz));
    const double hi = juce::jlimit (10.0, 20000.0, juce::jmax (lowHz, highHz));
    const double centre = std::sqrt (lo * juce::jmax (lo, hi));
    if (centre < 90.0)    return "SUB";
    if (centre < 300.0)   return "LOW";
    if (centre < 1000.0)  return "LOW MID";
    if (centre < 3000.0)  return "MID";
    if (centre < 6000.0)  return "PRESENCE";
    if (centre < 10000.0) return "UPPER";
    return "AIR";
}

juce::String BandColumn::compactHz (double hz)
{
    if (hz >= 10000.0) return juce::String (juce::roundToInt (hz / 1000.0)) + "k";
    if (hz >= 1000.0)  return juce::String (hz / 1000.0, 1) + "k";
    return juce::String (juce::roundToInt (hz));
}

juce::String BandColumn::getRangeText() const
{
    return compactHz (getLowHz()) + juce::String::fromUTF8 (" \xe2\x80\x93 ") + compactHz (getHighHz()) + " Hz";
}

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

    select.setTitle ("Select " + name);
    select.setTooltip ("Select this band");
    select.setComponentID ("action_selectBand_" + juce::String (slot));
    select.setWantsKeyboardFocus (true);
    select.onClick = [this] { if (onSelect) onSelect (slot); };
    addAndMakeVisible (select);

    bypass.setParameter (apvts, id (schema::band::bypass), "Byp", name + " Bypass");
    bypass.setTooltip ("Bypass this band");
    bypass.getProperties().set (style::small, true);
    bypass.setColour (juce::TextButton::buttonOnColourId, theme::warning);    // amber: bypass and listen states
    addAndMakeVisible (bypass);

    remove.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
    remove.setTitle ("Remove " + name);
    remove.setTooltip ("Remove this band");
    remove.setComponentID ("action_removeBand_" + juce::String (slot));
    remove.getProperties().set (style::small, true);
    remove.onClick = [this] { if (onRemove) onRemove (slot); };
    addAndMakeVisible (remove);

    addAndMakeVisible (grBar);

    threshold.setParameter (apvts, id (schema::band::thresh), "Threshold", name + " Threshold", colour);
    ratio.setParameter (apvts, id (schema::band::ratio), "Ratio", name + " Ratio", colour);
    attack.setParameter (apvts, id (schema::band::attack), "Attack", name + " Attack", colour);
    release.setParameter (apvts, id (schema::band::release), "Release", name + " Release", colour);
    attack.setShortCaption ("Att");
    release.setShortCaption ("Rel");

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
        const float alpha = bypassedLook ? 0.4f : 1.0f;
        for (auto* c : std::initializer_list<juce::Component*> { &grBar, &threshold, &ratio, &attack, &release })
            c->setAlpha (alpha);
    }

    const auto range = getRangeText();
    const auto name = getNameText();
    if (range != shownRange || name != shownName)
    {
        shownRange = range;
        shownName = name;
        select.repaint();
    }
}

void BandColumn::mouseDown (const juce::MouseEvent& e)
{
    // A click anywhere in the strip selects the band, except on the remove button: removing
    // another band must not move the selection.
    if (e.eventComponent == &remove || remove.isParentOf (e.eventComponent))
        return;

    if (onSelect) onSelect (slot);
}

void BandColumn::Header::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    auto r = getLocalBounds().toFloat().reduced (8.0f, 0.0f);
    r.removeFromTop (8.0f);

    auto top = r.removeFromTop (18.0f);
    const auto chip = top.removeFromLeft (18.0f);
    g.setColour (owner.colour);
    g.fillRect (chip);
    g.setColour (theme::background);
    g.setFont (theme::mono (11.0f));
    g.drawText (juce::String (owner.slot), chip, juce::Justification::centred, false);

    top.removeFromLeft (6.0f);
    g.setColour (highlighted ? theme::textHi : theme::textHi.withAlpha (0.92f));
    const auto nameFont = theme::display (11.0f, 0.14f);
    auto name = owner.shownName.isNotEmpty() ? owner.shownName : owner.getNameText();
    if (theme::textWidth (nameFont, name) > top.getWidth()) name = name.substring (0, 3);
    theme::drawTracked (g, name, top, juce::Justification::centredLeft, nameFont);

    r.removeFromTop (3.0f);
    g.setColour (theme::textMid);
    g.setFont (theme::mono (10.0f));
    g.drawFittedText (owner.shownRange.isNotEmpty() ? owner.shownRange : owner.getRangeText(),
                      r.removeFromTop (14.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);

    if (hasKeyboardFocus (false))
    {
        g.setColour (theme::accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), theme::radiusModule - 2.0f, 1.0f);
    }
}

void BandColumn::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    theme::drawModule (g, r);

    g.setColour (theme::lattice);
    g.fillRect (6.0f, (float) headerHeight - 1.0f, r.getWidth() - 12.0f, 1.0f);

    if (selected)
    {
        g.setColour (theme::accent.withAlpha (0.12f));
        g.drawRoundedRectangle (r.reduced (3.0f), theme::radiusModule - 3.0f, 2.0f);
        g.setColour (theme::accent);
        g.drawRoundedRectangle (r.reduced (1.0f), theme::radiusModule - 1.0f, 2.0f);
    }
    else
    {
        // The slot colour along the top edge, following the card's corners.
        juce::Graphics::ScopedSaveState save (g);
        juce::Path shape;
        shape.addRoundedRectangle (r, theme::radiusModule);
        g.reduceClipRegion (shape);
        g.setColour (colour);
        g.fillRect (r.withHeight (2.0f));
    }
}

void BandColumn::resized()
{
    auto r = getLocalBounds();
    select.setBounds (r.removeFromTop (headerHeight));

    r.removeFromTop (8);
    grBar.setBounds (r.removeFromTop (5).reduced (8, 0));

    auto foot = r.removeFromBottom (34).reduced (6, 0).withTrimmedTop (4).withTrimmedBottom (6);      // 24 px buttons
    const int half = (foot.getWidth() - 3) / 2;
    bypass.setBounds (foot.removeFromLeft (half));
    foot.removeFromLeft (3);
    remove.setBounds (foot);

    // Threshold, ratio, then attack and release side by side, spread over the height.
    auto main = r.reduced (4, 0).withTrimmedTop (6).withTrimmedBottom (2);
    const int hThresh = ParamKnob::heightFor (54), hRatio = ParamKnob::heightFor (36), hPair = ParamKnob::heightFor (30);
    const int spare = juce::jmax (0, main.getHeight() - hThresh - hRatio - hPair) / 2;
    const int knobWidth = juce::jmin (main.getWidth(), 76);

    threshold.setBounds (main.removeFromTop (hThresh).withSizeKeepingCentre (knobWidth, hThresh));
    main.removeFromTop (spare);
    ratio.setBounds (main.removeFromTop (hRatio).withSizeKeepingCentre (knobWidth, hRatio));
    main.removeFromTop (spare);
    auto pair = main.removeFromTop (hPair);
    const int pairWidth = juce::jmin (pair.getWidth(), 2 * 40 + 6);
    pair = pair.withSizeKeepingCentre (pairWidth, hPair);
    const int each = (pair.getWidth() - 6) / 2;
    attack.setBounds (pair.removeFromLeft (each));
    pair.removeFromLeft (6);
    release.setBounds (pair);
}

// =============================================================================================
EmptyStrip::EmptyStrip()
{
    setInterceptsMouseClicks (false, true);
    add.setButtonText ("+ Add");
    add.setComponentID ("action_addBand");
    add.setTitle ("Add band");
    add.getProperties().set (style::small, true);
    add.onClick = [this] { if (onAdd) onAdd(); };
    addAndMakeVisible (add);
}

void EmptyStrip::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();

    // Hatched at 135 degrees, 6 px bands.
    juce::Path shape;
    shape.addRoundedRectangle (r, theme::radiusModule);
    g.setColour (theme::emptyBase.withAlpha (0.6f));
    g.fillPath (shape);
    g.saveState();
    g.reduceClipRegion (shape);
    g.setColour (theme::emptyHatch);
    for (float x = -r.getHeight(); x < r.getWidth(); x += 12.0f)
    {
        juce::Path stripe;
        stripe.startNewSubPath (x, r.getBottom());
        stripe.lineTo (x + 6.0f, r.getBottom());
        stripe.lineTo (x + 6.0f + r.getHeight(), 0.0f);
        stripe.lineTo (x + r.getHeight(), 0.0f);
        stripe.closeSubPath();
        g.fillPath (stripe);
    }
    g.restoreState();

    // Dashed edge.
    g.setColour (theme::border);
    juce::Path edge;
    edge.addRoundedRectangle (r.reduced (0.5f), theme::radiusModule);
    juce::Path dashed;
    const float dashes[] { 4.0f, 3.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, edge, dashes, 2);
    g.fillPath (dashed);

    g.setColour (theme::textLow);
    theme::drawTracked (g, "EMPTY", r.withSizeKeepingCentre (r.getWidth(), 14.0f).translated (0.0f, -20.0f),
                        juce::Justification::centred, theme::label (10.0f, 0.24f));
}

void EmptyStrip::resized()
{
    add.setBounds (getLocalBounds().withSizeKeepingCentre (juce::jmin (getWidth() - 16, 70), 24).translated (0, 10));
}
} // namespace tl::ui
