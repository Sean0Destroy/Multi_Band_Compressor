#pragma once

#include <algorithm>
#include <cmath>

#include "DspConstants.h"

// Global coloration stage ("character"). One original model so far:
//
//   Warm FET: an asymmetric tanh curve with unity small-signal gain,
//       y = (tanh (k (x + b)) - tanh (k b)) / (k (1 - tanh^2 (k b))),   b = 0.15,  k = 2.5 drive
//   The bias b makes the curve asymmetric (even harmonics, mostly the 2nd), tanh rounds the
//   peaks (odd harmonics, gentle level compression when driven). As drive -> 0 the curve
//   becomes the identity. The DC the asymmetry produces is removed by a 5 Hz high-pass.
//
// This is an original curve, not a model of any particular hardware.
namespace tl::dsp
{
enum class CharacterModel { Clean = 0, WarmFet = 1 };

inline constexpr float kCharacterBias = 0.15f;   // small enough that the 3rd harmonic keeps growing with drive
inline constexpr float kCharacterMaxK = 2.5f;
inline constexpr double kCharacterDcHz = 5.0;

struct WarmFetCurve
{
    float k = 0.0f, offset = 0.0f, scale = 1.0f;

    /** drive 0..1. Returns false if the curve is the identity (drive too small to matter). */
    bool set (float drive) noexcept
    {
        k = kCharacterMaxK * std::clamp (drive, 0.0f, 1.0f);
        if (k < 1.0e-4f)
            return false;

        const float t = std::tanh (k * kCharacterBias);
        offset = t;
        scale = 1.0f / (k * (1.0f - t * t));
        return true;
    }

    float operator() (float x) const noexcept { return (std::tanh (k * (x + kCharacterBias)) - offset) * scale; }
};

/** First-order DC blocker, y[n] = x[n] - x[n-1] + R y[n-1]. */
struct DcBlocker
{
    double x1 = 0.0, y1 = 0.0, r = 0.999;

    void prepare (double sampleRate) noexcept { r = 1.0 - 2.0 * kPi * kCharacterDcHz / sampleRate; reset(); }
    void reset() noexcept { x1 = y1 = 0.0; }

    float tick (float x) noexcept
    {
        const double y = (double) x - x1 + r * y1;
        x1 = (double) x;
        y1 = y;
        return (float) y;
    }
    bool isFinite() const noexcept { return std::isfinite (x1) && std::isfinite (y1); }
};
} // namespace tl::dsp
