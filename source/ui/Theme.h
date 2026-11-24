#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>

#include "ParameterSchema.h"

// Design tokens of the editor: one place for every colour, metric and typeface.
// Tone Legion identity, Soft Glass: navy faceplate, ice-cyan for what is active, brass only on the analog scale.
// Band identity is the SLOT: colour and number never change with frequency order.
namespace tl::ui::theme
{
// ---- Palette ----------------------------------------------------------------------------------
// Soft Glass: smoky navy surfaces, restrained ice-cyan for what is active, pastel band colours.
inline const juce::Colour room       { 0xff0e1b2a };   // behind the faceplate, the deepest backdrop tone
inline const juce::Colour background { 0xff12233a };   // recessed wells, displays, read-outs
inline const juce::Colour panel      { 0xff172b41 };   // the faceplate
inline const juce::Colour surface    { 0xff20374d };   // raised cards (strips, inspector, master)
inline const juce::Colour elevated   { 0xff2b435b };   // knob faces, popup fields
inline const juce::Colour lattice    { 0xff2a4158 };   // construction lines between sections
inline const juce::Colour border     { 0xff3a5470 };   // edges
inline const juce::Colour capEdge    { 0xff4b6782 };   // knob face outline
inline const juce::Colour graphite   { 0xff5f7890 };   // inactive marks (decorative only, never text that must be read)
inline const juce::Colour textHi     { 0xffe4f0f8 };   // values, titles, emblem
inline const juce::Colour textMid    { 0xffa6bed1 };   // legends
inline const juce::Colour textLow    { 0xff8ba0b3 };   // hints and dimmed legends (still >= 4.5:1)
inline const juce::Colour accent     { 0xff9cecf4 };   // ice-cyan: active state, value rings, the response curve
inline const juce::Colour spectrum   { 0xff68d6e5 };   // the analyser trace and its fill
inline const juce::Colour brass      { 0xffc9a46a };   // the needle meter's scale, soft clipping
inline const juce::Colour warning    { 0xfff0b040 };   // amber: warnings, listen and bypass states ONLY
inline const juce::Colour clip       { 0xfff47d86 };   // what the clipper cuts, overs
inline const juce::Colour iceGlow    { 0x479cecf4 };   // accent at 28 %

// Supporting tones (formerly literals in the views).
inline const juce::Colour highlight  { 0x14ffffff };   // the lit top edge of a raised card
inline const juce::Colour gridLine   { 0xff1e344b };   // graph and scope grid
inline const juce::Colour meterTrack { 0xff152940 };   // an unlit meter bar
inline const juce::Colour meterLow   { 0xff5fb9cc };   // the bottom of a level bar
inline const juce::Colour meterHigh  { 0xffd2f4f8 };   // a level bar near 0 dBFS
inline const juce::Colour ladderOff  { 0xff253c54 };   // an unlit gain-reduction step
inline const juce::Colour emptyBase  { 0xff162a3f };   // an empty strip's place
inline const juce::Colour emptyHatch { 0xff1a2f45 };

/** Permanent slot colours (slot 1..8), pastel. Always shown together with the slot number. */
inline juce::Colour slotColour (int slot) noexcept
{
    static const std::array<juce::uint32, schema::kActiveSlots> c {
        0xfff28b9a, 0xfff6b577, 0xffead473, 0xff91d5a0,
        0xff7fd3c8, 0xff8cc4f0, 0xffb3a9f2, 0xffe39bd8 };
    return juce::Colour (c[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1]);
}

// ---- Shape --------------------------------------------------------------------------------------
inline constexpr float radiusModule  = 10.0f;  // cards
inline constexpr float radiusWell    = 8.0f;   // recessed wells and displays
inline constexpr float radiusControl = 6.0f;   // buttons, segments, fields, read-outs

// ---- Metrics (logical pixels at 100 % scale) --------------------------------------------------
inline constexpr int earWidth          = 18;    // side margin of the faceplate (once the rack ears)
inline constexpr int headerHeight      = 60;
inline constexpr int toolbarHeight     = 48;
inline constexpr int bodyPadTop        = 14;
inline constexpr int bodyPadBottom     = 16;
inline constexpr int bodyPadSide       = 16;
inline constexpr int bodyGap           = 14;    // between the left column and the master strip
inline constexpr int rowGap            = 12;    // display / strips / inspector
inline constexpr int displayHeight     = 262;   // default; the display takes any extra height
inline constexpr int displayMinHeight  = 200;
inline constexpr int columnStripHeight = 308;
inline constexpr int inspectorHeight   = 178;
inline constexpr int needleWidth       = 232;
inline constexpr int masterWidth       = 268;
inline constexpr int stripGap          = 6;
inline constexpr int pad               = 8;
inline constexpr int columnMinWidth    = 96;
inline constexpr int columnMaxWidth    = 220;
inline constexpr int knobPrimary       = 36;    // smallest knob of the strip's first row (ratio)
inline constexpr int knobSecondary     = 30;    // attack and release
inline constexpr int knobCaptionGap    = 13;    // the legend between a knob and its value
inline constexpr int knobValueHeight   = 15;

// ---- Type ---------------------------------------------------------------------------------------
// Marcellus for names, Jost for legends, DM Mono for values (all SIL OFL 1.1, embedded; see
// resources/fonts). Sizes are CSS pixel sizes (em), tracking is in em.
enum class Face { Display, Label, LabelLight, Mono, MonoMedium };

/** The embedded typeface (created once, on first use). */
juce::Typeface::Ptr typeface (Face);

/** A font of `face` at `pixelSize` (CSS px, the em size) with `trackingEm` letter spacing. */
juce::Font font (Face face, float pixelSize, float trackingEm = 0.0f);

inline juce::Font display (float px, float trackEm = 0.0f) { return font (Face::Display, px, trackEm); }
inline juce::Font label (float px, float trackEm = 0.0f)   { return font (Face::Label, px, trackEm); }
inline juce::Font mono (float px, bool medium = false)     { return font (medium ? Face::MonoMedium : Face::Mono, px); }

/** Jost at `px` (regular when `bold`, light otherwise). */
inline juce::Font font (float px, bool bold = false) { return font (bold ? Face::Label : Face::LabelLight, px); }

inline constexpr float valueFontSize   = 11.0f;   // DM Mono under a knob
inline constexpr float captionFontSize = 9.0f;    // Jost legend under a knob
inline juce::Font valueFont (float px = valueFontSize) { return mono (px); }

/** Width of `text` in `f`, tracking included except after the last letter. */
float textWidth (const juce::Font& f, const juce::String& text);

/** Draws `text` with the font's tracking in `area`; centring ignores the space after the last letter. */
void drawTracked (juce::Graphics&, const juce::String& text, juce::Rectangle<float> area,
                  juce::Justification, const juce::Font&);

// ---- Shared drawing ---------------------------------------------------------------------------
/** A recessed, rounded well with an inner shadow. */
void drawWell (juce::Graphics&, juce::Rectangle<float>);
/** A raised, rounded card with a lit top edge (strip, inspector panel, master strip). */
void drawModule (juce::Graphics&, juce::Rectangle<float>);
/** The faceplate behind everything: a navy gradient with two soft glows. */
void drawBackdrop (juce::Graphics&, juce::Rectangle<float>);
/** The Tone Legion Facet emblem, its 64-unit box mapped onto `box`. `wordWeight` uses the heavier
    strokes of the emblem that stands in for the O of the word mark. */
void drawEmblem (juce::Graphics&, juce::Rectangle<float> box, bool wordWeight);

/** WCAG 2.x relative luminance / contrast ratio (used by the token contrast test). */
inline double relativeLuminance (juce::Colour c) noexcept
{
    const auto lin = [] (double v) { return v <= 0.03928 ? v / 12.92 : std::pow ((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin (c.getFloatRed()) + 0.7152 * lin (c.getFloatGreen()) + 0.0722 * lin (c.getFloatBlue());
}

inline double contrastRatio (juce::Colour a, juce::Colour b) noexcept
{
    const double la = relativeLuminance (a), lb = relativeLuminance (b);
    return (juce::jmax (la, lb) + 0.05) / (juce::jmin (la, lb) + 0.05);
}
} // namespace tl::ui::theme
