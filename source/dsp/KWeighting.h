#pragma once

#include <cmath>

#include "DspConstants.h"

// K-weighting pre-filter used for loudness measurement (two biquads: a high shelf modelling
// the head, then the RLB high-pass), as specified for 48 kHz in ITU-R BS.1770. For other sample
// rates the two sections are re-derived from the analog prototypes that reproduce the published
// 48 kHz coefficients (shelf f0 1681.97 Hz, +3.9998 dB, Q 0.70718; high-pass f0 38.135 Hz,
// Q 0.50033) with the bilinear transform. [The standard text itself could not be fetched while
// writing this; the 48 kHz coefficients are checked against the commonly published values in
// the tests.]
namespace tl::dsp
{
struct BiquadCoeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

inline BiquadCoeffs kWeightingShelf (double fs) noexcept
{
    const double f0 = 1681.974450955533, gainDb = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan (kPi * f0 / fs);
    const double Vh = std::pow (10.0, gainDb / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    return { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
             2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
}

inline BiquadCoeffs kWeightingHighPass (double fs) noexcept
{
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = std::tan (kPi * f0 / fs);
    const double a0 = 1.0 + K / Q + K * K;
    return { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
}

/** Transposed direct form II, double state (the high-pass has poles very close to z = 1). */
struct BiquadState
{
    double z1 = 0.0, z2 = 0.0;
    void reset() noexcept { z1 = z2 = 0.0; }

    double tick (const BiquadCoeffs& c, double x) noexcept
    {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

/** K-weighted sample of one channel. */
struct KWeightingFilter
{
    BiquadState shelf, highPass;
    void reset() noexcept { shelf.reset(); highPass.reset(); }

    double tick (const BiquadCoeffs& shelfC, const BiquadCoeffs& hpC, double x) noexcept
    {
        return highPass.tick (hpC, shelf.tick (shelfC, x));
    }
    bool isFinite() const noexcept
    {
        return std::isfinite (shelf.z1) && std::isfinite (shelf.z2) && std::isfinite (highPass.z1) && std::isfinite (highPass.z2);
    }
};
} // namespace tl::dsp
