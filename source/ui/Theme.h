#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>

#include "ParameterSchema.h"

// Design tokens of the editor: one place for every colour and metric.
// Band identity is the SLOT: colour and number never change with frequency order.
namespace tl::ui::theme
{
inline const juce::Colour background { 0xff0d0f14 };
inline const juce::Colour panel      { 0xff14171d };
inline const juce::Colour surface    { 0xff1c2028 };
inline const juce::Colour elevated   { 0xff262b34 };
inline const juce::Colour border     { 0xff343b46 };
inline const juce::Colour textHi     { 0xffe9ecf1 };   // values, titles
inline const juce::Colour textMid    { 0xffa9b1bd };   // captions
inline const juce::Colour textLow    { 0xff8a93a1 };   // hints, disabled captions
inline const juce::Colour accent     { 0xff9a8cf0 };   // primary accent (violet)
inline const juce::Colour warning    { 0xfff0b040 };   // amber: warnings and listen states ONLY

/** Permanent slot colours (slot 1..8). Always shown together with the slot number. */
inline juce::Colour slotColour (int slot) noexcept
{
    static const std::array<juce::uint32, schema::kActiveSlots> c {
        0xffe5727a, 0xffe8975e, 0xffd9bd55, 0xff83c872,
        0xff55c3b1, 0xff5fa9e8, 0xff9a90ea, 0xffd480ca };
    return juce::Colour (c[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1]);
}

// Metrics (logical pixels at 100 % scale).
inline constexpr int headerHeight    = 48;
inline constexpr int displayHeight   = 230;   // default; the display takes any extra height
inline constexpr int displayMinHeight = 160;
inline constexpr int columnStripHeight = 250;
inline constexpr int inspectorHeight = 150;
inline constexpr int globalBarHeight = 44;
inline constexpr int pad             = 8;
inline constexpr int columnMinWidth  = 132;
inline constexpr int columnMaxWidth  = 200;
inline constexpr int knobPrimary     = 44;
inline constexpr int knobSecondary   = 36;
inline constexpr float valueFontSize   = 12.0f;
inline constexpr float captionFontSize = 11.0f;

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

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
