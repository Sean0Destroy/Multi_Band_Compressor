#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>

#include "ParameterSchema.h"

// Design tokens of the editor: one place for every colour, metric and typeface.
// Tone Legion identity: slate faceplate, ice for what is active, brass only on the analog scale.
// Band identity is the SLOT: colour and number never change with frequency order.
namespace tl::ui::theme
{
// ---- Palette ----------------------------------------------------------------------------------
inline const juce::Colour room       { 0xff0b0d10 };   // behind the faceplate
inline const juce::Colour background { 0xff12151a };   // obsidian: recessed wells, displays, read-outs
inline const juce::Colour panel      { 0xff1c2027 };   // slate: the faceplate
inline const juce::Colour surface    { 0xff222731 };   // slate-hi: raised modules (strips, inspector, master)
inline const juce::Colour elevated   { 0xff262c35 };   // knob caps, popup fields
inline const juce::Colour lattice    { 0xff2a303a };   // construction lines between sections
inline const juce::Colour border     { 0xff2e343e };   // edges
inline const juce::Colour capEdge    { 0xff3a414c };   // knob cap outline
inline const juce::Colour graphite   { 0xff5a616c };   // inactive marks (decorative only, never text that must be read)
inline const juce::Colour textHi     { 0xffeef0f3 };   // frost: values, titles, emblem
inline const juce::Colour textMid    { 0xffa7adb6 };   // pewter: legends
inline const juce::Colour textLow    { 0xff8b929d };   // hints and dimmed legends (still >= 4.5:1)
inline const juce::Colour accent     { 0xff9fd4e6 };   // ice: active state, value rings, the response curve
inline const juce::Colour brass      { 0xffc9a46a };   // the needle meter's scale, soft clipping
inline const juce::Colour warning    { 0xfff0b040 };   // amber: warnings, listen and bypass states ONLY
inline const juce::Colour clip       { 0xffe5727a };   // what the clipper cuts, overs
inline const juce::Colour iceGlow    { 0x479fd4e6 };   // ice at 28 %

/** Permanent slot colours (slot 1..8). Always shown together with the slot number. */
inline juce::Colour slotColour (int slot) noexcept
{
    static const std::array<juce::uint32, schema::kActiveSlots> c {
        0xffe5727a, 0xffe8975e, 0xffd9bd55, 0xff83c872,
        0xff55c3b1, 0xff5fa9e8, 0xff9a90ea, 0xffd480ca };
    return juce::Colour (c[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1]);
}

// ---- Metrics (logical pixels at 100 % scale) --------------------------------------------------
inline constexpr int earWidth          = 18;    // rack ears left and right
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
/** A recessed obsidian well with an inner shadow. */
void drawWell (juce::Graphics&, juce::Rectangle<float>);
/** A raised slate module (strip, inspector panel, master strip). */
void drawModule (juce::Graphics&, juce::Rectangle<float>);
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
