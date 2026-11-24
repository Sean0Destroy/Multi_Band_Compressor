#include "APrimeRegionEngine.h"

#include <algorithm>
#include <cmath>

namespace tl::dsp
{
void APrimeRegionEngine::prepareRegions (double sampleRate) noexcept
{
    fs = sampleRate;
    for (auto& r : regions)
        r.lo = r.hi = -1.0f;   // the edge -> coefficient mapping depends on the sample rate
}

void APrimeRegionEngine::resetRegion (int band) noexcept
{
    auto& r = regions[band];
    for (auto& s : r.state) s.reset();
    r.widthDb = 0.0;
    r.snapWidth = false;
    r.lo = r.hi = -1.0f;       // coefficients are rebuilt for the unity-gain width
}

bool APrimeRegionEngine::copyRegionsFrom (const BandEngineBase& other) noexcept
{
    if (other.asBandEngineBase() == nullptr || otherTypeTag (other) != &typeTag)
        return false;

    const auto& o = static_cast<const APrimeRegionEngine&> (other);
    fs = o.fs;
    for (int i = 0; i < kMaxBands; ++i)
        regions[i] = o.regions[i];
    return true;
}

// This engine's filter is fed with 2R' x: at unity gain (R' = R) its state is the shared one.
bool APrimeRegionEngine::exportRegionState (int band, float state[kMaxChannels][2]) const noexcept
{
    const auto& r = regions[band];
    if (r.lo < 0.0f)
        return false;

    for (int c = 0; c < kMaxChannels; ++c)
    {
        state[c][0] = r.state[c].s1;
        state[c][1] = r.state[c].s2;
    }
    return true;
}

void APrimeRegionEngine::importRegionState (int band, const float state[kMaxChannels][2]) noexcept
{
    auto& r = regions[band];
    for (int c = 0; c < kMaxChannels; ++c)
    {
        r.state[c].s1 = state[c][0];
        r.state[c].s2 = state[c][1];
    }
}

void APrimeRegionEngine::primeRegions() noexcept
{
    // Taking over from an engine that is already applying gains: the width belongs where the
    // gain is now, not at unity with the lag still to run.
    for (auto& r : regions)
        r.snapWidth = true;
}

bool APrimeRegionEngine::regionIsFinite (int band) const noexcept
{
    const auto& r = regions[band];
    for (auto& s : r.state)
        if (! s.isFinite())
            return false;
    return std::isfinite (r.widthDb);
}

void APrimeRegionEngine::applyChunk (float* const* channels, int nCh, int offset, int n, std::uint64_t pos) noexcept
{
    constexpr double kDbPerLn = 8.6858896380650365530;   // 20 / ln 10

    // Band regions in slot order (cascade), per channel.
    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        if (! core.isActive (bi))
            continue;

        auto& r = regions[bi];
        const float* g0 = core.gains (bi, 0);
        const float* g1 = core.gains (bi, nCh > 1 ? 1 : 0);

        // Coefficients per segment: edges from the core, width from the lagged gain. Both only
        // change at grid points (a segment that starts off the grid continues the previous one).
        int k = 0;
        for (int i = 0; i < n;)
        {
            const int end = segmentEnd (pos, i, n);
            float lo = 0.0f, hi = 0.0f;
            core.segmentEdges (bi, k, lo, hi);

            bool rebuild = false;
            if (! sameBits (lo, r.lo) || ! sameBits (hi, r.hi))
            {
                r.lo = lo;
                r.hi = hi;
                const double wl = std::tan (kPi * std::exp2 ((double) lo) / fs);
                const double wh = std::tan (kPi * std::exp2 ((double) hi) / fs);
                r.g = std::sqrt (wl * wh);
                r.R = (wh - wl) / (2.0 * r.g);

                // Lag tied to the band filter's own time constant 1 / (R w0), w0 = 2 fs atan (g).
                const double w0 = 2.0 * fs * std::atan (r.g);
                const double lagSeconds = std::max (minLagSeconds, lagFactor / (r.R * w0));
                r.lagB = 1.0 - std::exp (-(double) kCoeffGrid / (lagSeconds * fs));
                rebuild = true;
            }

            if (onGrid (pos, i))
            {
                // Width follows the deeper (or more boosted) of the two lanes, lagged.
                const double a = (double) g0[i], b = (double) g1[i];
                const double da = kDbPerLn * std::log (std::max (a, 1.0e-6)), db = kDbPerLn * std::log (std::max (b, 1.0e-6));
                const double target = std::abs (da) >= std::abs (db) ? da : db;
                const double next = r.snapWidth ? target : r.widthDb + r.lagB * (target - r.widthDb);
                r.snapWidth = false;
                const double settled = std::abs (target - next) < 1.0e-9 ? target : next;
                if (settled != r.widthDb) { r.widthDb = settled; rebuild = true; }
            }

            if (rebuild)
            {
                // R' = R / sqrt (Gw) = R * 10^(-widthDb / 40)
                const double rPrime = r.R * std::exp (-r.widthDb / (2.0 * kDbPerLn));
                r.coeffs = SvfCoeffs::make (r.g, rPrime);
            }

            segmentCoeffs[k++] = r.coeffs;
            i = end;
        }

        if (nCh == 2 && core.isMidSide (bi))
        {
            // Mid/side lanes: the filter (one shared width) is the same linear system on both
            // channels, so its band-pass output is split into M and S afterwards.
            float* xl = channels[0] + offset;
            float* xr = channels[1] + offset;
            SvfState sl = r.state[0], sr = r.state[1];
            const bool solo = bi == soloBand;
            const bool colour = colourActive (bi);
            const bool colourMid = colour && colourLane (bi, 0), colourSide = colour && colourLane (bi, 1);

            k = 0;
            for (int i = 0; i < n;)
            {
                const int end = segmentEnd (pos, i, n);
                const SvfCoeffs cf = segmentCoeffs[k++];

                for (int j = i; j < end; ++j)
                {
                    const float bl = svfTick (cf, sl, cf.twoR * xl[j]).bp;
                    const float br = svfTick (cf, sr, cf.twoR * xr[j]).bp;
                    const float bm = 0.5f * (bl + br), bs = 0.5f * (bl - br);

                    // Character: the residue of the band's own mid and side components after their gains.
                    float rm = 0.0f, rs = 0.0f;
                    if (colour)
                    {
                        if (colourMid)  rm = colourTick (bi, 0, j, g0[j] * bm);
                        if (colourSide) rs = colourTick (bi, 1, j, g1[j] * bs);
                        xl[j] = xl[j] + (rm + rs);
                        xr[j] = xr[j] + (rm - rs);
                    }

                    if (solo)
                    {
                        soloBuffer[0][j] = g0[j] * bm + g1[j] * bs + (rm + rs);
                        soloBuffer[1][j] = g0[j] * bm - g1[j] * bs + (rm - rs);
                    }

                    const float cm = g0[j] - 1.0f, cs = g1[j] - 1.0f;
                    if (isExactlyZero (cm) && isExactlyZero (cs))
                        continue;                       // neutral: output stays bit-identical

                    const float dm = cm * bm, ds = cs * bs;
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

            k = 0;
            for (int i = 0; i < n;)
            {
                const int end = segmentEnd (pos, i, n);
                const SvfCoeffs cf = segmentCoeffs[k++];

                for (int j = i; j < end; ++j)
                {
                    const float bp = svfTick (cf, st, cf.twoR * x[j]).bp;   // unity-peak band-pass of the band
                    const float residue = colour ? colourTick (bi, c, j, g[j] * bp) : 0.0f;
                    if (solo != nullptr) solo[j] = g[j] * bp + residue;
                    const float cg = g[j] - 1.0f;
                    if (! isExactlyZero (cg))           // G == 1: output stays bit-identical
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
