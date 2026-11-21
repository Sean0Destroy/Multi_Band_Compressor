#include "DynamicsView.h"

#include <cmath>

namespace tl::ui
{
DynamicsView::DynamicsView (PluginProcessor& p) : processor (p)
{
    setTitle ("Dynamics display");
    setDescription ("Transfer curve and gain-reduction history of the selected band.");
    setInterceptsMouseClicks (false, false);
    setOpaque (true);
}

void DynamicsView::setSelectedSlot (int slot)
{
    selectedSlot = juce::jlimit (0, schema::kActiveSlots, slot);
    repaint();
}

void DynamicsView::pushGainReduction (int slot, float grDb)
{
    if (slot >= 1 && slot <= schema::kActiveSlots)
        pending[(size_t) slot - 1] = juce::jmin (pending[(size_t) slot - 1], juce::jmin (0.0f, grDb));
}

void DynamicsView::endTick()
{
    for (size_t s = 0; s < history.size(); ++s)
    {
        history[s][(size_t) writeIndex] = pending[s];
        pending[s] = 0.0f;
    }
    writeIndex = (writeIndex + 1) % historyLength;
    filled = juce::jmin (historyLength, filled + 1);

    if (isVisible())
        repaint();
}

int DynamicsView::copyHistory (int slot, float* dest, int maxCount) const
{
    if (slot < 1 || slot > schema::kActiveSlots) return 0;
    const int count = juce::jmin (filled, maxCount);
    for (int i = 0; i < count; ++i)
        dest[i] = history[(size_t) slot - 1][(size_t) ((writeIndex - count + i + 2 * historyLength) % historyLength)];
    return count;
}

dsp::GainComputer DynamicsView::computerFor (int slot, double& makeupDb) const
{
    dsp::GainComputer gc;
    makeupDb = 0.0;
    auto& apvts = processor.getState();
    const auto value = [&apvts, slot] (const char* suffix)
    {
        return apvts.getRawParameterValue (schema::bandParamId (slot, suffix))->load (std::memory_order_relaxed);
    };
    gc.set (value (schema::band::thresh), value (schema::band::ratio), value (schema::band::knee), value (schema::band::range));
    makeupDb = (double) value (schema::band::makeup);
    return gc;
}

double DynamicsView::outputLevelDb (double inputDb) const
{
    if (selectedSlot == 0) return inputDb;
    double makeup = 0.0;
    const auto gc = computerFor (selectedSlot, makeup);
    return inputDb + (double) gc.gainReductionDb ((float) inputDb) + makeup;
}

double DynamicsView::inputLevelForReduction (double grDb) const
{
    if (selectedSlot == 0 || grDb >= 0.0) return minLevelDb;
    double makeup = 0.0;
    const auto gc = computerFor (selectedSlot, makeup);

    // The curve is monotonic (more level, more reduction) until the range limit: bisect.
    double lo = (double) gc.kneeStartDb(), hi = maxLevelDb + 60.0;
    if ((double) gc.gainReductionDb ((float) hi) > grDb) return hi;       // not reachable (ratio 1): far right
    for (int i = 0; i < 48; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        if ((double) gc.gainReductionDb ((float) mid) > grDb) lo = mid; else hi = mid;
    }
    return hi;
}

juce::Rectangle<float> DynamicsView::curveBounds() const noexcept
{
    auto r = getLocalBounds().toFloat().reduced (12.0f, 10.0f);
    r.removeFromLeft (30.0f);
    r.removeFromBottom (16.0f);
    const float side = juce::jmin (r.getHeight(), r.getWidth() * 0.4f);
    return r.removeFromLeft (side).withHeight (side);
}

juce::Rectangle<float> DynamicsView::historyBounds() const noexcept
{
    auto r = getLocalBounds().toFloat().reduced (12.0f, 10.0f);
    r.removeFromBottom (16.0f);
    r.removeFromLeft (30.0f + curveBounds().getWidth() + 56.0f);
    r.removeFromRight (4.0f);
    return r;
}

void DynamicsView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    if (selectedSlot == 0)
    {
        g.setColour (theme::textLow);
        g.setFont (theme::font (13.0f));
        g.drawText ("Select a band to see its transfer curve and gain-reduction history", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    const auto colour = theme::slotColour (selectedSlot);
    const auto curve = curveBounds();
    const auto xFor = [&curve] (double db) { return curve.getX() + (float) ((db - minLevelDb) / (maxLevelDb - minLevelDb)) * curve.getWidth(); };
    const auto yFor = [&curve] (double db) { return curve.getBottom() - (float) ((juce::jlimit (minLevelDb, maxLevelDb, db) - minLevelDb) / (maxLevelDb - minLevelDb)) * curve.getHeight(); };

    // ---- Transfer curve ------------------------------------------------------------------------
    g.setColour (theme::background);
    g.fillRect (curve);
    g.setFont (theme::font (10.0f));
    for (double db : { -60.0, -48.0, -36.0, -24.0, -12.0, 0.0 })
    {
        g.setColour (theme::border.withAlpha (0.6f));
        g.drawVerticalLine (juce::roundToInt (xFor (db)), curve.getY(), curve.getBottom());
        g.drawHorizontalLine (juce::roundToInt (yFor (db)), curve.getX(), curve.getRight());
        g.setColour (theme::textLow);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (xFor (db) - 16.0f, curve.getBottom() + 2.0f, 32.0f, 12.0f), juce::Justification::centred, false);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (curve.getX() - 30.0f, yFor (db) - 6.0f, 26.0f, 12.0f), juce::Justification::centredRight, false);
    }

    g.setColour (theme::border.brighter (0.2f));                          // unity line
    g.drawLine (xFor (minLevelDb), yFor (minLevelDb), xFor (maxLevelDb), yFor (maxLevelDb), 1.0f);

    juce::Path path;
    for (int i = 0; i <= 168; ++i)
    {
        const double in = minLevelDb + (maxLevelDb - minLevelDb) * i / 168.0;
        const float x = xFor (in), y = yFor (outputLevelDb (in));
        if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
    }
    g.saveState();
    g.reduceClipRegion (curve.toNearestInt());
    g.setColour (colour);
    g.strokePath (path, juce::PathStrokeType (2.0f));

    // Operating point: where on the curve the band is right now (from its gain reduction).
    float latest = 0.0f;
    if (filled > 0)
        latest = history[(size_t) selectedSlot - 1][(size_t) ((writeIndex - 1 + historyLength) % historyLength)];
    if (latest < -0.05f)
    {
        const double in = inputLevelForReduction ((double) latest);
        g.setColour (theme::textHi);
        g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ xFor (in), yFor (outputLevelDb (in)) }));
    }
    g.restoreState();

    g.setColour (theme::textMid);
    g.setFont (theme::font (10.5f, true));
    g.drawText ("BAND " + juce::String (selectedSlot) + "  IN / OUT (dB)", curve.withHeight (14.0f).translated (4.0f, 2.0f), juce::Justification::centredLeft, false);

    // ---- Gain-reduction history ------------------------------------------------------------------
    const auto hist = historyBounds();
    g.setColour (theme::background);
    g.fillRect (hist);
    const auto hy = [&hist] (double db) { return hist.getY() + (float) (juce::jlimit (historyBottomDb, 0.0, db) / historyBottomDb) * hist.getHeight(); };
    g.setFont (theme::font (10.0f));
    for (double db : { 0.0, -6.0, -12.0, -18.0, -24.0 })
    {
        g.setColour (theme::border.withAlpha (0.6f));
        g.drawHorizontalLine (juce::roundToInt (hy (db)), hist.getX(), hist.getRight());
        g.setColour (theme::textLow);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (hist.getX() - 30.0f, hy (db) - 6.0f, 26.0f, 12.0f), juce::Justification::centredRight, false);
    }

    std::array<float, historyLength> values {};
    const int count = copyHistory (selectedSlot, values.data(), historyLength);
    if (count > 1)
    {
        juce::Path trace;
        trace.startNewSubPath (hist.getRight() - hist.getWidth() * (float) (count - 1) / (float) (historyLength - 1), hy (0.0));
        for (int i = 0; i < count; ++i)
        {
            const float x = hist.getRight() - hist.getWidth() * (float) (count - 1 - i) / (float) (historyLength - 1);
            trace.lineTo (x, hy ((double) values[(size_t) i]));
        }
        trace.lineTo (hist.getRight(), hy (0.0));
        trace.closeSubPath();
        g.setColour (colour.withAlpha (0.35f));
        g.fillPath (trace);
        g.setColour (colour);
        g.strokePath (trace, juce::PathStrokeType (1.2f));
    }

    g.setColour (theme::textMid);
    g.setFont (theme::font (10.5f, true));
    g.drawText ("GAIN REDUCTION (dB), LAST 8 s", hist.withHeight (14.0f).translated (4.0f, 2.0f), juce::Justification::centredLeft, false);
}
} // namespace tl::ui
