#pragma once

#include <algorithm>
#include <cmath>
#include <complex>

#include "DspConstants.h"

// Closed-form frequency response of one A3 region at a FIXED gain (the engine's filter with the
// gain frozen). Used by the editor's response curve and by the tests as the reference the
// production engine is measured against. Not used on the audio thread.
namespace tl::dsp
{
/** The edges the engine really uses for nominal edges fLo/fHi at sample rate fs (sorted, then
    fLo >= 10 Hz, fHi <= 0.45 fs, fHi >= fLo * 2^(1/24)). */
inline void a3ClampEdges (double fs, double& fLo, double& fHi) noexcept
{
    if (fHi < fLo) std::swap (fLo, fHi);
    const double minRatio = std::exp2 (kMinEdgeRatioOct);
    const double maxHz = kMaxEdgeFraction * fs;
    fLo = std::clamp (fLo, kMinEdgeHz, maxHz / minRatio);
    fHi = std::clamp (fHi, fLo * minRatio, maxHz);
}

/** H(f) = 1 + (G - 1) 2R s' / (s'^2 + 2R s' + 1), s' = j tan(pi f / fs) / g,
    g = sqrt(wl wh), R = (wh - wl) / (2g), w = tan(pi fEdge / fs). `gain` is linear. */
inline std::complex<double> a3RegionResponse (double f, double fs, double fLo, double fHi, double gain) noexcept
{
    const double wl = std::tan (kPi * fLo / fs), wh = std::tan (kPi * fHi / fs);
    const double g = std::sqrt (wl * wh), R = (wh - wl) / (2.0 * g);
    const std::complex<double> sp (0.0, std::tan (kPi * f / fs) / g);
    const auto D = sp * sp + 2.0 * R * sp + 1.0;
    return 1.0 + 2.0 * R * (gain - 1.0) * sp / D;
}
/** Settled response of one A-prime region (APrimeRegionEngine) at a fixed gain:
    H = (s'^2 + 2R sqrt(G) s' + 1) / (s'^2 + 2 (R / sqrt(G)) s' + 1). */
inline std::complex<double> aPrimeRegionResponse (double f, double fs, double fLo, double fHi, double gain) noexcept
{
    const double wl = std::tan (kPi * fLo / fs), wh = std::tan (kPi * fHi / fs);
    const double g = std::sqrt (wl * wh), R = (wh - wl) / (2.0 * g);
    const double root = std::sqrt (std::max (gain, 1.0e-9));
    const std::complex<double> sp (0.0, std::tan (kPi * f / fs) / g);
    return (sp * sp + 2.0 * R * root * sp + 1.0) / (sp * sp + 2.0 * (R / root) * sp + 1.0);
}

/** Which experimental band engine is running (not a parameter: a listening switch). */
enum class EngineKind { A3 = 0, APrime = 1 };
inline constexpr int kNumEngineKinds = 2;

inline std::complex<double> regionResponse (EngineKind kind, double f, double fs, double fLo, double fHi, double gain) noexcept
{
    return kind == EngineKind::APrime ? aPrimeRegionResponse (f, fs, fLo, fHi, gain) : a3RegionResponse (f, fs, fLo, fHi, gain);
}

inline const char* engineName (EngineKind kind) noexcept
{
    return kind == EngineKind::APrime ? "A-prime (constant width)" : "A3 (bell narrows with depth)";
}
} // namespace tl::dsp
