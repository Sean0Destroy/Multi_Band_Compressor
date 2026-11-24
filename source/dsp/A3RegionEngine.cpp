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

bool A3RegionEngine::copyRegionsFrom (const BandEngineBase& other) noexcept
{
    if (other.asBandEngineBase() == nullptr || otherTypeTag (other) != &typeTag)
        return false;

    const auto& o = static_cast<const A3RegionEngine&> (other);
    fs = o.fs;
    for (int i = 0; i < kMaxBands; ++i)
        regions[i] = o.regions[i];
    return true;
}

// This engine's filter is fed with x, so its state is the shared one divided by 2R.
bool A3RegionEngine::exportRegionState (int band, float state[kMaxChannels][2]) const noexcept
{
    const auto& r = regions[band];
    if (r.lo < 0.0f)
        return false;                               // no coefficients yet: nothing was filtered

    for (int c = 0; c < kMaxChannels; ++c)
    {
        state[c][0] = r.coeffs.twoR * r.state[c].s1;
        state[c][1] = r.coeffs.twoR * r.state[c].s2;
    }
    return true;
}

void A3RegionEngine::importRegionState (int band, const float state[kMaxChannels][2]) noexcept
{
    float lo = 0.0f, hi = 0.0f;
    if (! lastBandEdges (band, lo, hi))
        return;

    // The coefficients the next chunk will use (same expressions as in applyChunk).
    auto& r = regions[band];
    r.lo = lo;
    r.hi = hi;
    const double wl = std::tan (kPi * std::exp2 ((double) lo) / fs);
    const double wh = std::tan (kPi * std::exp2 ((double) hi) / fs);
    const double g = std::sqrt (wl * wh);
    const double R = (wh - wl) / (2.0 * g);
    r.coeffs = SvfCoeffs::make (g, R);

    if (! (r.coeffs.twoR > 0.0f))
        return;
    for (int c = 0; c < kMaxChannels; ++c)
    {
        r.state[c].s1 = state[c][0] / r.coeffs.twoR;
        r.state[c].s2 = state[c][1] / r.coeffs.twoR;
    }
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
            const bool colour = colourActive (bi);
            const bool colourMid = colour && colourLane (bi, 0), colourSide = colour && colourLane (bi, 1);

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

                    // Character: the residue of the band's own mid and side components after their gains.
                    float rm = 0.0f, rs = 0.0f;
                    if (colour)
                    {
                        if (colourMid)  rm = colourTick (bi, 0, j, gm[j] * cf.twoR * (0.5f * (bl + br)));
                        if (colourSide) rs = colourTick (bi, 1, j, gs[j] * cf.twoR * (0.5f * (bl - br)));
                    }

                    if (solo)
                    {
                        // The band's own component after its gains: mid and side, as left / right.
                        const float sm = gm[j] * cf.twoR * (0.5f * (bl + br)) + rm;
                        const float ss = gs[j] * cf.twoR * (0.5f * (bl - br)) + rs;
                        soloBuffer[0][j] = sm + ss;
                        soloBuffer[1][j] = sm - ss;
                    }
                    if (colour)
                    {
                        xl[j] = xl[j] + (rm + rs);
                        xr[j] = xr[j] + (rm - rs);
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
            const bool colour = colourActive (bi);

            int k = 0;
            for (int i = 0; i < n;)
            {
                const int end = segmentEnd (pos, i, n);
                const SvfCoeffs cf = segmentCoeffs[k++];

                for (int j = i; j < end; ++j)
                {
                    const float bp = svfTick (cf, st, x[j]).bp;
                    const float cg = (g[j] - 1.0f) * cf.twoR;
                    const float residue = colour ? colourTick (bi, c, j, g[j] * cf.twoR * bp) : 0.0f;
                    if (solo != nullptr) solo[j] = g[j] * cf.twoR * bp + residue;   // the band's component after its gain
                    if (! isExactlyZero (cg))           // gEff == 1: output stays bit-identical
                        x[j] = x[j] + cg * bp;
                    if (colour)
                        x[j] = x[j] + residue;
                }
                i = end;
            }
            r.state[c] = st;
        }
    }
}
} // namespace tl::dsp
