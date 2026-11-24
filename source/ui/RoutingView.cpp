#include "RoutingView.h"

#include "ValueFormat.h"

#include <algorithm>

namespace tl::ui
{
namespace
{
    juce::String hz (float v) { return fmt::toText (fmt::Unit::Hz, (double) v); }
    juce::String rangeOf (float lo, float hi) { return hz (lo) + " - " + hz (hi); }

    /** The scChan choice names, from the schema (the same for every slot). */
    const std::vector<std::string>& channelNames()
    {
        static const std::vector<std::string> names = [] {
            const auto* spec = schema::findSpec (schema::bandParamId (1, schema::band::scChan));
            return spec != nullptr ? spec->choices : std::vector<std::string> {};
        }();
        return names;
    }
}

RoutingView::RoutingView (PluginProcessor& p) : processor (p)
{
    setTitle ("Detector routing");
    setTooltip ("What each band's detector listens to. Click a band to edit its routing in the Detector tab");
    updateDescription();
}

//==============================================================================
void RoutingView::refresh (const std::vector<int>& columnOrder)
{
    auto& apvts = processor.getState();
    const auto value = [&apvts] (int slot, const char* suffix)
    {
        return apvts.getRawParameterValue (schema::bandParamId (slot, suffix))->load (std::memory_order_relaxed);
    };
    const auto choice = [&value] (int slot, const char* suffix, int last)
    {
        return juce::jlimit (0, last, juce::roundToInt (value (slot, suffix)));
    };

    const int channels = processor.getSidechainChannelCount();
    const auto& slots = processor.getBandSlots();

    std::vector<Route> next;
    next.reserve (columnOrder.size());
    for (int slot : columnOrder)
    {
        if (slot < 1 || slot > schema::kActiveSlots)
            continue;

        Route r;
        r.slot = slot;
        r.bandLoHz = value (slot, schema::band::fLo);
        r.bandHiHz = value (slot, schema::band::fHi);
        r.external = choice (slot, schema::band::scSource, 1) == 1;
        r.channel = choice (slot, schema::band::scChan, juce::jmax (0, (int) channelNames().size() - 1));
        r.detLoHz = r.bandLoHz;
        r.detHiHz = r.bandHiHz;

        // Same resolution as DynamicsCore::applySettings: a reference to a slot that does not
        // exist as a parameter slot means the band's own range.
        r.rangeMode = choice (slot, schema::band::scMode, 2);
        if (r.rangeMode == schema::scModeBand)
        {
            r.refSlot = juce::roundToInt (value (slot, schema::band::scRef));
            if (r.refSlot >= 1 && r.refSlot <= schema::kActiveSlots)
            {
                r.refUsed = slots.isUsed (r.refSlot);
                r.detLoHz = value (r.refSlot, schema::band::fLo);
                r.detHiHz = value (r.refSlot, schema::band::fHi);
            }
            else
            {
                r.rangeMode = schema::scModeOwn;
                r.refSlot = 0;
            }
        }
        else if (r.rangeMode == schema::scModeCustom)
        {
            r.detLoHz = value (slot, schema::band::scLo);
            r.detHiHz = value (slot, schema::band::scHi);
        }

        r.sidechainMissing = r.external && channels == 0;
        next.push_back (r);
    }

    if (next != routes || channels != sidechainChannels)
    {
        routes = std::move (next);
        sidechainChannels = channels;
        if (selectedSlot != 0 && std::none_of (routes.begin(), routes.end(), [this] (const Route& r) { return r.slot == selectedSlot; }))
            selectedSlot = 0;
        updateDescription();
        repaint();
    }
}

juce::String RoutingView::rangeText (const Route& r)
{
    switch (r.rangeMode)
    {
        case schema::scModeBand:
            if (r.refSlot == r.slot)
                return "Own range";
            return "Band " + juce::String (r.refSlot) + " range (" + rangeOf (r.detLoHz, r.detHiHz) + ")"
                   + (r.refUsed ? juce::String() : juce::String (", not in use"));
        case schema::scModeCustom:
            return "Custom " + rangeOf (r.detLoHz, r.detHiHz);
        default:
            return "Own range";
    }
}

juce::String RoutingView::channelText (const Route& r)
{
    const auto& names = channelNames();
    if (names.empty())
        return {};
    return juce::String (names[(size_t) juce::jlimit (0, (int) names.size() - 1, r.channel)]);
}

juce::String RoutingView::sidechainStateText() const
{
    if (sidechainChannels == 0) return "Not connected";
    if (sidechainChannels == 1) return "Mono";
    return juce::String (sidechainChannels) + " channels";
}

void RoutingView::setSelectedSlot (int slot)
{
    if (slot == selectedSlot)
        return;
    selectedSlot = slot;
    repaint();
}

void RoutingView::updateDescription()
{
    if (routes.empty())
    {
        setDescription ("No bands.");
        return;
    }

    juce::StringArray lines;
    for (const auto& r : routes)
        lines.add ("Band " + juce::String (r.slot) + " detects " + (r.external ? "the sidechain" : "the main input")
                   + ", " + rangeText (r) + ", " + channelText (r)
                   + (r.sidechainMissing ? ". Sidechain not connected: this band is not compressing" : ""));
    setDescription (lines.joinIntoString (". ") + ".");
}

//==============================================================================
juce::Rectangle<float> RoutingView::inner() const noexcept
{
    return getLocalBounds().toFloat().reduced (14.0f, 10.0f);
}

float RoutingView::rowHeight() const noexcept
{
    const auto n = juce::jmax (1, (int) routes.size());
    return juce::jmin (maxRowHeight, inner().getHeight() / (float) n);
}

juce::Rectangle<float> RoutingView::mainNodeBounds() const noexcept
{
    const auto area = inner();
    const float h = juce::jmin (46.0f, area.getHeight() * 0.36f);
    return { area.getX(), area.getY() + area.getHeight() * 0.27f - h * 0.5f, sourceWidth, h };
}

juce::Rectangle<float> RoutingView::sidechainNodeBounds() const noexcept
{
    const auto area = inner();
    const float h = juce::jmin (46.0f, area.getHeight() * 0.36f);
    return { area.getX(), area.getY() + area.getHeight() * 0.73f - h * 0.5f, sourceWidth, h };
}

juce::Rectangle<float> RoutingView::rowBounds (int index) const noexcept
{
    const auto area = inner();
    const float h = rowHeight();
    const float top = area.getCentreY() - h * (float) routes.size() * 0.5f;
    return { area.getRight() - rowWidth, top + h * (float) index, rowWidth, h };
}

juce::Rectangle<float> RoutingView::labelBounds (int index) const noexcept
{
    const auto row = rowBounds (index);
    return { row.getX() - labelWidth, row.getY(), labelWidth, row.getHeight() };
}

int RoutingView::slotAt (juce::Point<float> p) const noexcept
{
    for (int i = 0; i < (int) routes.size(); ++i)
        if (rowBounds (i).getUnion (labelBounds (i)).contains (p))
            return routes[(size_t) i].slot;
    return 0;
}

void RoutingView::mouseDown (const juce::MouseEvent& e)
{
    if (const int slot = slotAt (e.position); slot != 0 && onSelect)
        onSelect (slot);
}

void RoutingView::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (slotAt (e.position) != 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

//==============================================================================
void RoutingView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    // ---- Sources ---------------------------------------------------------------------------
    const auto drawNode = [&g] (juce::Rectangle<float> r, const juce::String& title, const juce::String& sub, bool active)
    {
        g.setColour (theme::surface);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (active ? theme::border.brighter (0.25f) : theme::border);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        auto text = r.reduced (10.0f, 4.0f);
        const bool twoLines = text.getHeight() >= 28.0f;
        g.setColour (active ? theme::textHi : theme::textMid);
        g.setFont (theme::font (theme::valueFontSize, true));
        g.drawText (title, twoLines ? text.removeFromTop (text.getHeight() * 0.5f) : text,
                    twoLines ? juce::Justification::bottomLeft : juce::Justification::centredLeft, true);
        if (twoLines)
        {
            g.setColour (theme::textLow);
            g.setFont (theme::font (theme::captionFontSize));
            g.drawText (sub, text, juce::Justification::topLeft, true);
        }
    };

    const auto mainNode = mainNodeBounds(), scNode = sidechainNodeBounds();
    const bool anyMain = std::any_of (routes.begin(), routes.end(), [] (const Route& r) { return ! r.external; });
    const bool anyExternal = std::any_of (routes.begin(), routes.end(), [] (const Route& r) { return r.external; });
    drawNode (mainNode, "Main input", "After the input gain", anyMain);
    drawNode (scNode, "Sidechain", sidechainStateText(), sidechainChannels > 0 && anyExternal);

    if (routes.empty())
    {
        g.setColour (theme::textLow);
        g.setFont (theme::font (13.0f));
        g.drawText ("Add a band to see what its detector listens to",
                    inner().withTrimmedLeft (sourceWidth + 20.0f), juce::Justification::centred, false);
        return;
    }

    const float rowH = rowHeight();
    const float fontSize = rowH < 22.0f ? theme::captionFontSize : theme::valueFontSize;

    // ---- Wires (drawn first, so rows and labels sit on top) ------------------------------------
    for (int i = 0; i < (int) routes.size(); ++i)
    {
        const auto& r = routes[(size_t) i];
        const auto from = (r.external ? scNode : mainNode);
        const juce::Point<float> a { from.getRight(), from.getCentreY() };
        const auto label = labelBounds (i);
        const juce::Point<float> b { label.getX(), label.getCentreY() };
        const float dx = (b.x - a.x) * 0.5f;

        juce::Path wire;
        wire.startNewSubPath (a);
        wire.cubicTo (a.x + dx, a.y, b.x - dx, b.y, b.x, b.y);

        const bool selected = r.slot == selectedSlot;
        const float thickness = selected ? 2.2f : 1.4f;
        if (r.sidechainMissing)
        {
            const float dashes[] { 5.0f, 4.0f };
            juce::Path dashed;
            juce::PathStrokeType (thickness).createDashedStroke (dashed, wire, dashes, 2);
            g.setColour (theme::warning);
            g.fillPath (dashed);
        }
        else
        {
            g.setColour (theme::slotColour (r.slot).withAlpha (selected ? 1.0f : 0.7f));
            g.strokePath (wire, juce::PathStrokeType (thickness));
        }
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (b));
    }

    // ---- Labels and band rows --------------------------------------------------------------
    for (int i = 0; i < (int) routes.size(); ++i)
    {
        const auto& r = routes[(size_t) i];
        const auto colour = theme::slotColour (r.slot);
        const bool selected = r.slot == selectedSlot;

        auto label = labelBounds (i).reduced (10.0f, 0.0f);
        g.setFont (theme::font (fontSize));
        if (r.sidechainMissing)
        {
            g.setColour (theme::warning);
            g.drawText ("No sidechain: not compressing", label, juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour (r.rangeMode == schema::scModeOwn ? theme::textMid : theme::textHi);
            g.drawFittedText (labelText (r), label.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
        }

        auto row = rowBounds (i).reduced (0.0f, rowH >= 22.0f ? 3.0f : 1.0f);
        g.setColour (selected ? theme::elevated : theme::surface);
        g.fillRoundedRectangle (row, 4.0f);
        g.setColour (selected ? colour : theme::border);
        g.drawRoundedRectangle (row, 4.0f, selected ? 1.5f : 1.0f);

        auto content = row.reduced (8.0f, 0.0f);
        const float dot = juce::jmin (10.0f, row.getHeight() - 6.0f);
        g.setColour (colour);
        g.fillEllipse (content.removeFromLeft (dot).withSizeKeepingCentre (dot, dot));
        content.removeFromLeft (6.0f);
        g.setColour (theme::textHi);
        g.setFont (theme::font (fontSize, true));
        g.drawText ("Band " + juce::String (r.slot), content.removeFromLeft (54.0f), juce::Justification::centredLeft, false);
        g.setColour (theme::textMid);
        g.setFont (theme::font (fontSize));
        g.drawText (rangeOf (r.bandLoHz, r.bandHiHz), content, juce::Justification::centredRight, true);
    }
}
} // namespace tl::ui
