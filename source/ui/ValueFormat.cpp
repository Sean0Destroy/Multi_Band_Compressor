#include "ValueFormat.h"

#include <cmath>

#include "dsp/DspConstants.h"

namespace tl::ui::fmt
{
Unit unitFor (const schema::ParamSpec& s) noexcept
{
    if (s.kind != schema::Kind::Float) return Unit::None;
    if (s.label == "Hz")  return Unit::Hz;
    if (s.label == "ms")  return Unit::Ms;
    if (s.label == "dB" || s.label == "dBFS") return Unit::Db;
    if (s.label == ":1")  return Unit::Ratio;
    if (s.label == "%")   return Unit::Percent;
    if (s.label.empty() && s.minValue >= 0.0f && s.maxValue <= 1.0f) return Unit::Fraction;
    return Unit::None;
}

juce::String toText (Unit unit, double v)
{
    switch (unit)
    {
        case Unit::Hz:
            if (v >= 9999.5) return juce::String (v / 1000.0, 1) + " kHz";
            if (v >= 999.5)  return juce::String (v / 1000.0, 2) + " kHz";
            if (v >= 99.95)  return juce::String (v, 0) + " Hz";
            return juce::String (v, 1) + " Hz";

        case Unit::Ms:
            if (v >= 999.5)  return juce::String (v / 1000.0, 2) + " s";
            if (v >= 99.95)  return juce::String (v, 0) + " ms";
            if (v >= 9.995)  return juce::String (v, 1) + " ms";
            return juce::String (v, 2) + " ms";

        case Unit::Db:
            return juce::String (std::abs (v) < 0.05 ? 0.0 : v, 1) + " dB";

        case Unit::Ratio:
            if (v >= (double) dsp::kInfiniteRatio - 0.5) return "inf:1";
            return juce::String (v, v >= 9.995 ? 1 : 2) + ":1";

        case Unit::Percent:
            return juce::String (v, 0) + " %";

        case Unit::Fraction:
            return juce::String (v * 100.0, 0) + " %";

        case Unit::None:
            break;
    }
    return juce::String (v, 2);
}

double fromText (Unit unit, const juce::String& text, double fallback)
{
    const auto t = text.trim().toLowerCase().replaceCharacter (',', '.');

    if (unit == Unit::Ratio && (t.startsWith ("inf") || t.startsWith (juce::String::fromUTF8 ("\xe2\x88\x9e"))))
        return (double) dsp::kInfiniteRatio;

    // Leading number: optional sign, digits, one decimal point.
    int end = 0;
    bool seenDigit = false, seenPoint = false;
    for (; end < t.length(); ++end)
    {
        const auto c = t[end];
        if ((c == '-' || c == '+') && end == 0) continue;
        if (c >= '0' && c <= '9') { seenDigit = true; continue; }
        if (c == '.' && ! seenPoint) { seenPoint = true; continue; }
        break;
    }

    if (! seenDigit) return fallback;

    const double number = t.substring (0, end).getDoubleValue();
    const auto suffix = t.substring (end).trim();

    switch (unit)
    {
        case Unit::Hz:       return suffix.startsWithChar ('k') ? number * 1000.0 : number;
        case Unit::Ms:       return (suffix.startsWithChar ('s')) ? number * 1000.0 : number;   // "ms" starts with 'm'
        case Unit::Fraction: return number / 100.0;
        case Unit::Db:
        case Unit::Ratio:
        case Unit::Percent:
        case Unit::None:     break;
    }
    return number;
}
} // namespace tl::ui::fmt
