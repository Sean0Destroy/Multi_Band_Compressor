#include "Theme.h"

#include "TLFontsData.h"

namespace tl::ui::theme
{
juce::Typeface::Ptr typeface (Face face)
{
    // Created once and kept for the life of the process (a plugin may open many editors).
    struct Faces
    {
        juce::Typeface::Ptr display, label, labelLight, mono, monoMedium;
        Faces()
        {
            const auto make = [] (const char* data, int size) { return juce::Typeface::createSystemTypefaceFor (data, (size_t) size); };
            display    = make (TLFontsData::MarcellusRegular_ttf, TLFontsData::MarcellusRegular_ttfSize);
            label      = make (TLFontsData::JostRegular_ttf, TLFontsData::JostRegular_ttfSize);
            labelLight = make (TLFontsData::JostLight_ttf, TLFontsData::JostLight_ttfSize);
            mono       = make (TLFontsData::DMMonoRegular_ttf, TLFontsData::DMMonoRegular_ttfSize);
            monoMedium = make (TLFontsData::DMMonoMedium_ttf, TLFontsData::DMMonoMedium_ttfSize);
        }
    };
    static const Faces faces;

    switch (face)
    {
        case Face::Display:    return faces.display;
        case Face::Label:      return faces.label;
        case Face::LabelLight: return faces.labelLight;
        case Face::Mono:       return faces.mono;
        case Face::MonoMedium: return faces.monoMedium;
    }
    return faces.label;
}

juce::Font font (Face face, float pixelSize, float trackingEm)
{
    juce::Font f { juce::FontOptions (typeface (face)).withPointHeight (juce::jmax (1.0f, pixelSize)) };
    if (std::abs (trackingEm) > 1.0e-6f)
    {
        // JUCE's kerning factor is a multiple of the font's (ascent + descent) height, CSS
        // letter-spacing a multiple of the em size.
        const float h = f.getHeight();
        if (h > 0.0f)
            f = f.withExtraKerningFactor (trackingEm * pixelSize / h);
    }
    return f;
}

float textWidth (const juce::Font& f, const juce::String& text)
{
    if (text.isEmpty()) return 0.0f;
    const float w = juce::GlyphArrangement::getStringWidth (f, text);
    return juce::jmax (0.0f, w - f.getExtraKerningFactor() * f.getHeight());   // no space after the last letter
}

void drawTracked (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                  juce::Justification just, const juce::Font& f)
{
    if (text.isEmpty()) return;
    const float w = textWidth (f, text);
    float x = area.getX();
    if (just.testFlags (juce::Justification::horizontallyCentred)) x = area.getCentreX() - w * 0.5f;
    else if (just.testFlags (juce::Justification::right))          x = area.getRight() - w;

    g.setFont (f);
    const auto vertical = just.getOnlyVerticalFlags() != 0 ? just.getOnlyVerticalFlags() : juce::Justification::verticallyCentred;
    g.drawText (text, juce::Rectangle<float> (x, area.getY(), w + f.getHeight(), area.getHeight()),
                juce::Justification (juce::Justification::left | vertical), false);
}

void drawWell (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (background);
    g.fillRect (r);

    // Inner shadow along the top edge (the well is recessed into the faceplate).
    juce::ColourGradient shade (juce::Colours::black.withAlpha (0.45f), r.getX(), r.getY(),
                                juce::Colours::transparentBlack, r.getX(), r.getY() + 10.0f, false);
    g.setGradientFill (shade);
    g.fillRect (r.withHeight (juce::jmin (10.0f, r.getHeight())));

    g.setColour (border);
    g.drawRect (r, 1.0f);
}

void drawModule (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setGradientFill (juce::ColourGradient (surface, r.getX(), r.getY(), juce::Colour (0xff1e222a), r.getX(), r.getBottom(), false));
    g.fillRect (r);
    g.setColour (border);
    g.drawRect (r, 1.0f);
}

void drawEmblem (juce::Graphics& g, juce::Rectangle<float> box, bool wordWeight)
{
    const auto t = juce::AffineTransform::scale (box.getWidth() / 64.0f, box.getHeight() / 64.0f).translated (box.getX(), box.getY());
    const float unit = box.getWidth() / 64.0f;

    juce::Path hex;
    hex.startNewSubPath (32.0f, 4.0f);
    hex.lineTo (56.25f, 18.0f);
    hex.lineTo (56.25f, 46.0f);
    hex.lineTo (32.0f, 60.0f);
    hex.lineTo (7.75f, 46.0f);
    hex.lineTo (7.75f, 18.0f);
    hex.closeSubPath();

    juce::Path tri;
    tri.addTriangle (32.0f, 20.0f, 42.4f, 38.0f, 21.6f, 38.0f);
    g.setColour (accent);
    g.fillPath (tri, t);

    juce::Path facets;
    const auto line = [&facets] (float x0, float y0, float x1, float y1) { facets.startNewSubPath (x0, y0); facets.lineTo (x1, y1); };
    line (32.0f, 20.0f, 7.75f, 18.0f);  line (32.0f, 20.0f, 32.0f, 4.0f);   line (32.0f, 20.0f, 56.25f, 18.0f);
    line (42.4f, 38.0f, 56.25f, 18.0f); line (42.4f, 38.0f, 56.25f, 46.0f); line (42.4f, 38.0f, 32.0f, 60.0f);
    line (21.6f, 38.0f, 32.0f, 60.0f);  line (21.6f, 38.0f, 7.75f, 46.0f);  line (21.6f, 38.0f, 7.75f, 18.0f);

    g.setColour (textHi);
    g.strokePath (hex, juce::PathStrokeType ((wordWeight ? 5.5f : 3.4f) * unit, juce::PathStrokeType::curved), t);
    g.strokePath (facets, juce::PathStrokeType ((wordWeight ? 2.4f : 1.8f) * unit, juce::PathStrokeType::curved), t);
}
} // namespace tl::ui::theme
