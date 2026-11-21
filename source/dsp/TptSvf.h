#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

// Topology-preserving-transform state-variable filter (Zavalishin). Pure value types,
// real-time safe. Coefficients are computed in double and stored in float.
namespace tl::dsp
{
struct SvfCoeffs
{
    float g = 0.0f;          // tan(pi fc / fs)
    float twoR = 2.0f;       // 2R (damping)
    float twoRplusG = 2.0f;  // 2R + g
    float k = 1.0f;          // 1 / (1 + 2Rg + g^2)

    static SvfCoeffs make (double g, double R) noexcept
    {
        SvfCoeffs c;
        c.g = (float) g;
        c.twoR = (float) (2.0 * R);
        c.twoRplusG = (float) (2.0 * R + g);
        c.k = (float) (1.0 / (1.0 + 2.0 * R * g + g * g));
        return c;
    }

    bool operator== (const SvfCoeffs&) const = default;
};

struct SvfState
{
    float s1 = 0.0f, s2 = 0.0f;

    void reset() noexcept { s1 = s2 = 0.0f; }

    bool isFinite() const noexcept
    {
        constexpr std::uint32_t expMask = 0x7f800000u;
        return (std::bit_cast<std::uint32_t> (s1) & expMask) != expMask
            && (std::bit_cast<std::uint32_t> (s2) & expMask) != expMask;
    }
};

struct SvfOut { float hp, bp, lp; };

/** hp = (x - (2R+g) s1 - s2) / (1 + 2Rg + g^2); bp = g hp + s1; lp = g bp + s2;
    s1 = g hp + bp; s2 = g bp + lp. */
inline SvfOut svfTick (const SvfCoeffs& c, SvfState& s, float x) noexcept
{
    const float hp = (x - c.twoRplusG * s.s1 - s.s2) * c.k;
    const float bp = c.g * hp + s.s1;
    const float lp = c.g * bp + s.s2;
    s.s1 = c.g * hp + bp;
    s.s2 = c.g * bp + lp;
    return { hp, bp, lp };
}

inline constexpr double kButterworthR = 0.70710678118654752440;   // 1/sqrt(2)
} // namespace tl::dsp
