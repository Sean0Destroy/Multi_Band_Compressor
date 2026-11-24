#pragma once

#include <juce_core/juce_core.h>

#include "ParameterSchema.h"

// Display text <-> plain value conversion for the editor's value fields. Independent of the
// host-facing parameter text (which stays unit-less and fixed-decimal).
namespace tl::ui::fmt
{
enum class Unit { Hz, Ms, Db, Ratio, Percent, Fraction, None };

/** Unit of a parameter, derived from its schema label and range. */
Unit unitFor (const schema::ParamSpec&) noexcept;

/** e.g. 432 -> "432 Hz", 1250 -> "1.25 kHz", 0.05 -> "0.05 ms", 1500 -> "1.50 s",
    -18 -> "-18.0 dB", 100 (ratio) -> "inf:1", 0.5 (fraction) -> "50 %". */
juce::String toText (Unit, double value);

/** Parses what toText() produces plus common hand-typed forms ("1.2k", "2 s", "4:1", "inf").
    Returns `fallback` if no number can be read. The caller clamps to the parameter range. */
double fromText (Unit, const juce::String& text, double fallback);
} // namespace tl::ui::fmt
