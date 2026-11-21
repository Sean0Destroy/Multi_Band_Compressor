#include "A3RegionEngine.h"

#include <cmath>

namespace tl::dsp
{
void A3RegionEngine::prepareRegions (double sampleRate) noexcept
{
    fs = sampleRate;
    for (auto& r : regions)
        r.lo = r.hi = -1.0f;   // the edge -> coefficient mapping depends on the sample rate
}

void A3RegionEngine::resetRegion (int band) noexcept
{
    for (auto& s : regions[band].state)
        s.reset();
}

bool A3RegionEngine::regionIsFinite (int band) const noexcept
{
    for (auto& s : regions[band].state)
        if (! s.isFinite())
            return false;
    return true;
}

void A3RegionEngine::applyChunk (float* const* channels, int nCh, int offset, int n, std::uint64_t pos) noexcept
{
    const int numSegments = core.numSegments();

    // Band regions in slot order (cascade), per channel.
    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        if (! core.isActive (bi))
            continue;

        auto& r = regions[bi];

        for (int k = 0; k < numSegments; ++k)
        {
            float lo = 0.0f, hi = 0.0f;
            core.segmentEdges (bi, k, lo, hi);

            // Identical edges give identical coefficients, so reusing them is exact.
            if (! sameBits (lo, r.lo) || ! sameBits (hi, r.hi))
            {
                r.lo = lo;
                r.hi = hi;
                const double wl = std::tan (kPi * std::exp2 ((double) lo) / fs);
                const double wh = std::tan (kPi * std::exp2 ((double) hi) / fs);
                const double g = std::sqrt (wl * wh);
                const double R = (wh - wl) / (2.0 * g);
                r.coeffs = SvfCoeffs::make (g, R);
            }
            segmentCoeffs[k] = r.coeffs;
        }

        if (nCh == 2 && core.isMidSide (bi))
        {
            // Mid/side lanes without a global encode: the region filter is the same linear system
            // on both channels, so its band-pass output can be split into M and S and the two
            // gain changes added back per channel.
            float* xl = channels[0] + offset;
            float* xr = channels[1] + offset;
            const float* gm = core.gains (bi, 0);
            const float* gs = core.gains (bi, 1);
            SvfState sl = r.state[0], sr = r.state[1];
            const bool solo = bi == soloBand;

            int k = 0;
            for (int i = 0; i < n;)
            {
                const int end = segmentEnd (pos, i, n);
                const SvfCoeffs cf = segmentCoeffs[k++];

                for (int j = i; j < end; ++j)
                {
                    const float bl = svfTick (cf, sl, xl[j]).bp;
                    const float br = svfTick (cf, sr, xr[j]).bp;
                    const float cm = (gm[j] - 1.0f) * cf.twoR;
                    const float cs = (gs[j] - 1.0f) * cf.twoR;

                    if (solo)
                    {
                        // The band's own component after its gains: mid and side, as left / right.
                        const float sm = gm[j] * cf.twoR * (0.5f * (bl + br));
                        const float ss = gs[j] * cf.twoR * (0.5f * (bl - br));
                        soloBuffer[0][j] = sm + ss;
                        soloBuffer[1][j] = sm - ss;
                    }
                    if (isExactlyZero (cm) && isExactlyZero (cs))
                        continue;                       // neutral: output stays bit-identical

                    const float dm = cm * (0.5f * (bl + br));
                    const float ds = cs * (0.5f * (bl - br));
                    xl[j] = xl[j] + (dm + ds);
                    xr[j] = xr[j] + (dm - ds);
                }
                i = end;
            }
            r.state[0] = sl;
            r.state[1] = sr;
            continue;
        }

        for (int c = 0; c < nCh; ++c)
        {
            float* x = channels[c] + offset;
            const float* g = core.gains (bi, c);
            SvfState st = r.state[c];
            float* solo = bi == soloBand ? soloBuffer[c] : nullptr;

            int k = 0;
            for (int i = 0; i < n;)
            {
                const int end = segmentEnd (pos, i, n);
                const SvfCoeffs cf = segmentCoeffs[k++];

                for (int j = i; j < end; ++j)
                {
                    const float bp = svfTick (cf, st, x[j]).bp;
                    const float cg = (g[j] - 1.0f) * cf.twoR;
                    if (solo != nullptr) solo[j] = g[j] * cf.twoR * bp;   // the band's component after its gain
                    if (! isExactlyZero (cg))           // gEff == 1: output stays bit-identical
                        x[j] = x[j] + cg * bp;
                }
                i = end;
            }
            r.state[c] = st;
        }
    }
}
} // namespace tl::dsp
