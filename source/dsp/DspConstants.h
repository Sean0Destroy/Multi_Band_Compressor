#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

#include "ParameterSchema.h"

// Shared constants of the experimental band engine. Nothing in source/dsp may allocate,
// lock, log or perform I/O inside a function that the audio thread calls.
namespace tl::dsp
{
inline constexpr int kMaxBands = schema::kActiveSlots;   // one band section per parameter slot
inline constexpr int kMaxChannels = 2;                   // main bus is mono or stereo
inline constexpr int kCoeffGrid = 16;                    // coefficient updates on an ABSOLUTE 16-sample grid
inline constexpr int kChunk = 256;                       // internal processing chunk (fixed, no allocation)
inline constexpr int kMaxSegments = kChunk / kCoeffGrid + 1;

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kEdgeTauSeconds = 0.020;         // edge smoothing time constant (log2 domain)
inline constexpr double kActivityRampSeconds = 0.010;    // used/unused and bypass ramp
inline constexpr double kMinEdgeHz = 10.0;
inline constexpr double kMaxEdgeFraction = 0.45;         // fHi <= 0.45 fs
inline constexpr double kMinEdgeRatioOct = 1.0 / 24.0;   // fHi >= fLo * 2^(1/24)
inline constexpr float  kInfiniteRatio = 100.0f;         // ratio >= 100 means 1/R = 0

/** Exact comparisons without -Wfloat-equal noise (+0 and -0 both count as zero). */
inline bool isExactlyZero (float x) noexcept { return (std::bit_cast<std::uint32_t> (x) & 0x7fffffffu) == 0u; }
inline bool isExactlyOne (float x) noexcept  { return std::bit_cast<std::uint32_t> (x) == 0x3f800000u; }
inline bool isFiniteFloat (float x) noexcept { return (std::bit_cast<std::uint32_t> (x) & 0x7f800000u) != 0x7f800000u; }

/** One-pole coefficient a = exp(-1 / (tau fs)); tau <= 0 gives 0 (instantaneous). */
inline double onePoleCoefficient (double tauSeconds, double sampleRate) noexcept
{
    const double n = tauSeconds * sampleRate;
    return n > 0.0 ? std::exp (-1.0 / n) : 0.0;
}
} // namespace tl::dsp
