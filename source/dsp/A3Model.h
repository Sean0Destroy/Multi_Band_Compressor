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
} // namespace tl::dsp
