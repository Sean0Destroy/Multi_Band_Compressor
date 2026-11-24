#include "ConsoleMeters.h"

namespace tl::ui
{
namespace
{
const juce::String minusSign = juce::String::fromUTF8 ("\xe2\x88\x92");

juce::String withMinus (const juce::String& s) { return s.replace ("-", minusSign); }

// The needle's drawing, in the 200 x 118 units of the design.
constexpr float kCx = 100.0f, kCy = 112.0f, kRadius = 86.0f;
}

// =============================================================================================
NeedleMeter::NeedleMeter()
{
    setInterceptsMouseClicks (false, false);
    setTitle ("Gain reduction needle");
}

float NeedleMeter::angleFor (float reductionDb) noexcept
{
    return 50.0f - 100.0f * std::sqrt (juce::jlimit (0.0f, 20.0f, reductionDb) / 20.0f);
}

juce::String NeedleMeter::getTitleText() const
{
    return slot > 0 ? juce::String::fromUTF8 ("GAIN REDUCTION \xc2\xb7 BAND ") + juce::String (slot)
                    : juce::String ("GAIN REDUCTION");
}

void NeedleMeter::setSlot (int newSlot)
{
    if (newSlot != slot)
    {
        slot = newSlot;
        repaint();
    }
}

void NeedleMeter::update (float grDb, double nowMs)
{
    const float target = angleFor (slot > 0 ? -juce::jmin (0.0f, grDb) : 0.0f);
    const double dt = lastMs < 0.0 ? 1.0 : juce::jlimit (0.0, 0.2, (nowMs - lastMs) / 1000.0);
    lastMs = nowMs;
    angle += (target - angle) * (1.0f - std::exp (-(float) dt / timeConstantSeconds));

    if (std::abs (angle - paintedAngle) > 0.05f)
        repaint();
}

void NeedleMeter::paint (juce::Graphics& g)
{
    paintedAngle = angle;
    const auto bounds = getLocalBounds().toFloat();
    theme::drawWell (g, bounds);

    auto r = bounds.reduced (12.0f, 10.0f);
    g.setColour (theme::textMid);
    theme::drawTracked (g, getTitleText(), r.removeFromTop (14.0f), juce::Justification::centredLeft, theme::label (10.0f, 0.28f));
    r.removeFromTop (4.0f);

    // Fit the 200 x 118 drawing to the width (or the height).
    const float s = juce::jmin (r.getWidth() / 200.0f, r.getHeight() / 118.0f);
    const float ox = r.getCentreX() - 100.0f * s, oy = r.getY();
    const auto at = [&] (float degrees, float radius)
    {
        const float a = juce::degreesToRadians (degrees);
        return juce::Point<float> (ox + (kCx + radius * std::sin (a)) * s, oy + (kCy - radius * std::cos (a)) * s);
    };

    // Brass scale.
    juce::Path arc;
    arc.addCentredArc (ox + kCx * s, oy + kCy * s, kRadius * s, kRadius * s, 0.0f,
                       juce::degreesToRadians (-50.0f), juce::degreesToRadians (50.0f), true);
    g.setColour (theme::brass);
    g.strokePath (arc, juce::PathStrokeType (1.2f * s));

    g.setFont (theme::mono (9.0f * s));
    for (int db : { 0, 1, 2, 3, 4, 6, 8, 10, 14, 20 })
    {
        const float a = angleFor ((float) db);
        const bool major = db % 2 == 0 || db > 8;
        g.drawLine ({ at (a, kRadius), at (a, kRadius - (major ? 9.0f : 5.0f)) }, 1.2f * s);
        if (db == 0 || db == 2 || db == 6 || db == 10 || db == 20)
        {
            const auto p = at (a, kRadius + 10.0f);
            const auto text = db == 0 ? juce::String ("0") : minusSign + juce::String (db);
            g.drawText (text, juce::Rectangle<float> (40.0f * s, 12.0f * s).withCentre (p), juce::Justification::centred, false);
        }
    }

    g.setColour (theme::textLow);
    // Low on the left of the hub, where the needle never passes.
    theme::drawTracked (g, "dB GR", juce::Rectangle<float> (ox + 18.0f * s, oy + (kCy - 12.0f) * s, 62.0f * s, 12.0f * s),
                        juce::Justification::centred, theme::label (8.0f * s, 0.3f));

    // Needle and hub.
    g.setColour (theme::textHi);
    g.drawLine ({ at (angle, 0.0f), at (angle, kRadius - 4.0f) }, 1.6f * s);
    const auto hub = juce::Rectangle<float> (10.0f * s, 10.0f * s).withCentre ({ ox + kCx * s, oy + kCy * s });
    g.setColour (theme::elevated);
    g.fillEllipse (hub);
    g.setColour (theme::capEdge);
    g.drawEllipse (hub, 1.0f);
}

// =============================================================================================
ClipView::ClipView (PluginProcessor& p) : processor (p)
{
    setInterceptsMouseClicks (false, false);
    setTitle ("Clip meter");
    setDescription ("The signal going into the output clipper, the ceiling and what is cut off, and how far the peaks are shaved.");
    modeValue = processor.getState().getRawParameterValue (schema::global::clipMode);
    ceilingValue = processor.getState().getRawParameterValue (schema::global::clipCeiling);
    jassert (modeValue != nullptr && ceilingValue != nullptr);
}

juce::String ClipView::getShavedText() const
{
    if (juce::roundToInt (modeValue->load (std::memory_order_relaxed)) == 0) return "off";
    return minusSign + juce::String (shavedHeld, 1) + " dB";
}

juce::String ClipView::getTagText() const
{
    if (juce::roundToInt (modeValue->load (std::memory_order_relaxed)) == 0) return "Clipper off";
    return withMinus ("Ceiling " + juce::String (ceilingValue->load (std::memory_order_relaxed), 1));
}

void ClipView::refresh (double nowMs, bool audioRunning)
{
    const auto& meter = processor.getClipMeter();
    if (&meter != source)
    {
        // Another engine (or the first refresh): start from what it writes next.
        source = &meter;
        cursor = meter.getColumnCount();
    }

    float deepest = 0.0f;
    if (audioRunning)
    {
        const int n = meter.readColumns (cursor, scratch.data(), (int) scratch.size());
        for (int i = 0; i < n; ++i)
        {
            const auto& c = scratch[(size_t) i];
            columns[(size_t) head] = c;
            head = (head + 1) % kColumns;
            deepest = juce::jmax (deepest, c.shavedDb());
        }
    }
    else
    {
        cursor = meter.getColumnCount();         // nothing stale when the audio comes back
        columns.fill ({});
    }

    shavedNow = deepest;
    const double dt = lastMs < 0.0 ? 0.0 : juce::jlimit (0.0, 0.2, (nowMs - lastMs) / 1000.0);
    lastMs = nowMs;
    if (shavedNow >= shavedHeld)
    {
        shavedHeld = shavedNow;
        heldAtMs = nowMs;
    }
    else if (nowMs - heldAtMs > holdMs)
    {
        shavedHeld = juce::jmax (shavedNow, shavedHeld - 3.0f * (float) dt);      // falls back at 3 dB/s
    }

    repaint();
}

void ClipView::paint (juce::Graphics& g)
{
    const int mode = juce::roundToInt (modeValue->load (std::memory_order_relaxed));
    const bool on = mode != 0;
    const float ceilingDb = ceilingValue->load (std::memory_order_relaxed);

    auto r = getLocalBounds().toFloat();
    const auto well = r.removeFromTop ((float) scopeHeight);
    theme::drawWell (g, well);

    // Scope: 0 dBFS at 1/1.3 of the half height, so overs up to +2.3 dB stay visible.
    {
        const auto inner = well.reduced (1.0f);
        const float mid = inner.getCentreY();
        const float full = (inner.getHeight() * 0.5f - 4.0f) / 1.3f;
        const juce::Colour gridLine (0xff1f242c);
        g.setColour (gridLine);
        for (float y : { mid, mid - full, mid + full })
            g.fillRect (inner.getX(), std::round (y), inner.getWidth(), 1.0f);
        g.setColour (theme::capEdge);
        g.setFont (theme::mono (8.0f));
        g.drawText ("0", juce::Rectangle<float> (inner.getX() + 3.0f, mid - full - 11.0f, 12.0f, 10.0f), juce::Justification::centredLeft, false);

        const float bw = inner.getWidth() / (float) kColumns;
        const auto cut = mode == 1 ? theme::brass.withAlpha (0.75f) : theme::clip.withAlpha (0.85f);
        for (int i = 0; i < kColumns; ++i)
        {
            const auto& c = getColumn (i);
            const float x = inner.getX() + (float) i * bw, w = juce::jmax (1.0f, bw - 0.6f);
            const float ha = juce::jmin (c.inPeak, 1.3f) * full;
            const float hp = juce::jmin (c.outPeak, 1.3f) * full;
            if (c.clipperOn && ha > hp)
            {
                g.setColour (cut);
                g.fillRect (x, mid - ha, w, ha - hp);
                g.fillRect (x, mid + hp, w, ha - hp);
            }
            g.setColour (c.clipperOn ? theme::accent.withAlpha (0.75f) : theme::textMid.withAlpha (0.55f));
            g.fillRect (x, mid - hp, w, 2.0f * hp);
        }

        const float cy = juce::Decibels::decibelsToGain (ceilingDb) * full;
        if (on)
        {
            g.setColour (theme::accent);
            g.fillRect (inner.getX(), mid - cy - 0.6f, inner.getWidth(), 1.2f);
            g.fillRect (inner.getX(), mid + cy - 0.6f, inner.getWidth(), 1.2f);
        }
        else
        {
            g.setColour (theme::graphite);
            for (float x = inner.getX(); x < inner.getRight(); x += 6.0f)
            {
                g.fillRect (x, mid - cy - 0.6f, juce::jmin (3.0f, inner.getRight() - x), 1.2f);
                g.fillRect (x, mid + cy - 0.6f, juce::jmin (3.0f, inner.getRight() - x), 1.2f);
            }
        }

        g.setColour (on ? theme::accent : theme::textLow);
        theme::drawTracked (g, getTagText().toUpperCase(), juce::Rectangle<float> (inner.getX(), inner.getY() + 3.0f, inner.getWidth() - 6.0f, 11.0f),
                            juce::Justification::centredRight, theme::label (8.0f, 0.18f));
    }

    // Shaved: legend, bar (now) with the held value as a marker, read-out.
    r.removeFromTop (8.0f);
    auto row = r.removeFromTop (16.0f);
    g.setColour (theme::textMid);
    theme::drawTracked (g, "SHAVED", row.removeFromLeft (62.0f), juce::Justification::centredLeft, theme::label (10.0f, 0.28f));

    const auto valueArea = row.removeFromRight (60.0f);
    g.setColour (! on ? theme::textLow : (shavedHeld > 0.05f ? theme::clip : theme::textHi));
    g.setFont (theme::mono (12.0f));
    g.drawText (getShavedText(), valueArea, juce::Justification::centredRight, false);

    row.removeFromRight (8.0f);
    const auto bar = row.withSizeKeepingCentre (row.getWidth(), 6.0f);
    g.setColour (juce::Colour (0xff191d23));
    g.fillRect (bar);
    const auto toX = [] (float db) { return juce::jlimit (0.0f, 1.0f, db / fullScaleShaveDb); };
    if (on && shavedNow > 0.0f)
    {
        g.setGradientFill (juce::ColourGradient (theme::brass, bar.getX(), 0.0f, theme::clip, bar.getRight(), 0.0f, false));
        g.fillRect (bar.withWidth (bar.getWidth() * toX (shavedNow)));
    }
    if (on)
    {
        g.setColour (theme::textHi);
        g.fillRect (juce::jmax (bar.getX(), bar.getX() + bar.getWidth() * toX (shavedHeld) - 2.0f), bar.getY(), 2.0f, bar.getHeight());
    }
}
} // namespace tl::ui
