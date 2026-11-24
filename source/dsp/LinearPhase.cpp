#include "LinearPhase.h"

#include <cstring>

namespace tl::dsp
{
namespace
{
constexpr int kMaxHopBins = kLinearPhaseMaxHop + 1;
constexpr int kMaxKernelBins = kLinearPhaseMaxTaps + kLinearPhaseMaxPartitions + kMaxHopBins;   // partitions * (B + 1)
constexpr int kHistorySize = 32768;   // >= c + B for every layout, power of two
constexpr int kHistoryMask = kHistorySize - 1;

double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k)
    {
        term *= q / ((double) k * (double) k);
        sum += term;
        if (term < 1.0e-17 * sum)
            break;
    }
    return sum;
}

/** The settled magnitude of one band at gain G, from v = (tan (pi f / fs) / g)^2. */
inline double bandMagnitudeSquared (EngineKind kind, double v, double R, double G) noexcept
{
    const double a = (1.0 - v) * (1.0 - v), b = 4.0 * R * R * v;
    if (kind == EngineKind::APrime)
    {
        const double gg = std::max (G, 1.0e-9);
        return (a + b * gg) / (a + b / gg);
    }
    const double den = a + b;
    const double p = den > 0.0 ? b / den : 1.0;
    return 1.0 + (G * G - 1.0) * p;
}

struct BandGeometry
{
    double g = 1.0, R = 1.0;
};

inline BandGeometry geometry (float lo, float hi, double fs) noexcept
{
    const double wl = std::tan (kPi * std::exp2 ((double) lo) / fs);
    const double wh = std::tan (kPi * std::exp2 ((double) hi) / fs);
    BandGeometry b;
    b.g = std::sqrt (wl * wh);
    b.R = (wh - wl) / (2.0 * b.g);
    return b;
}
} // namespace

bool LinearPhaseSnapshot::sameAs (const LinearPhaseSnapshot& o) const noexcept
{
    if (numChannels != o.numChannels)
        return false;
    for (int i = 0; i < kMaxBands; ++i)
    {
        const auto& a = bands[i];
        const auto& b = o.bands[i];
        if (a.active != b.active)
            return false;
        if (! a.active)
            continue;
        if (a.midSide != b.midSide || ! sameBits (a.lo, b.lo) || ! sameBits (a.hi, b.hi)
            || ! sameBits (a.gain[0], b.gain[0]) || ! sameBits (a.gain[1], b.gain[1]))
            return false;
    }
    return true;
}

// ---- Delay --------------------------------------------------------------------------------------
LinearPhaseDelay::LinearPhaseDelay()
{
    for (auto& r : ring)
        r.assign ((size_t) kSize, 0.0f);
}

void LinearPhaseDelay::reset() noexcept
{
    for (auto& r : ring)
        std::fill (r.begin(), r.end(), 0.0f);
    write = 0;
}

void LinearPhaseDelay::process (int lane, float* x, int n, int delay) noexcept
{
    float* r = ring[(size_t) lane].data();
    for (int j = 0; j < n; ++j)
    {
        const int w = (write + j) & mask;
        r[w] = x[j];
        x[j] = r[(w - delay) & mask];
    }
}

void LinearPhaseDelay::copyFrom (const LinearPhaseDelay& o) noexcept
{
    for (int c = 0; c < kMaxChannels; ++c)
        std::copy (o.ring[c].begin(), o.ring[c].end(), ring[c].begin());
    write = o.write;
}

// ---- Stage --------------------------------------------------------------------------------------
LinearPhaseStage::LinearPhaseStage()
{
    for (int c = 0; c < kMaxChannels; ++c)
    {
        inBlock[c].assign (kLinearPhaseMaxHop, 0.0f);
        outBlock[c].assign (kLinearPhaseMaxHop, 0.0f);
        identityBlock[c].assign (kLinearPhaseMaxHop, 0.0f);
        history[c].assign (kHistorySize, 0.0f);
        overlap[c].assign (2 * kLinearPhaseMaxHop, 0.0f);
        fdl[c].assign (kMaxKernelBins, Complex {});
        blockA[c].assign (2 * kLinearPhaseMaxHop + 2, 0.0f);
        blockB[c].assign (2 * kLinearPhaseMaxHop + 2, 0.0f);
    }
    for (auto& k : kernels)
        for (auto& e : k.k)
            e.assign (kMaxKernelBins, Complex {});
    binTan.assign (kLinearPhaseMaxTaps / 2 + 1, 0.0);
    window.assign (kLinearPhaseMaxTaps + 1, 0.0f);
    for (auto& m : mag)
        m.assign (kLinearPhaseMaxTaps / 2 + 1, 0.0f);
    scratch.assign (kLinearPhaseMaxTaps + 2, 0.0f);
    frame.assign (2 * kLinearPhaseMaxHop + 2, 0.0f);
    for (auto& a : accum)
        a.assign (kMaxHopBins, Complex {});
    for (auto& d : work)
        d.assign (kLinearPhaseMaxTaps / 2 + 1, 0.0);
    for (auto& d : scratchMag)
        d.assign (kLinearPhaseMaxTaps / 2 + 1, 0.0);
    prepare (48000.0, 1, EngineKind::A3);
}

void LinearPhaseStage::prepare (double sampleRate, int lengthChoice, EngineKind model) noexcept
{
    kind = model;
    const LinearPhaseLayout next = linearPhaseLayout (sampleRate, lengthChoice);
    const bool tablesValid = next.taps == lay.taps && next.hop == lay.hop && std::abs (sampleRate - fs) < 1.0e-9 && tablesBuilt;
    lay = next;
    fs = sampleRate;

    const int L = lay.taps, B = lay.hop, c = lay.centre();
    partitions = (2 * c) / B + 1;
    ringBlocks = partitions;

    if (! tablesValid)
    {
        fftDesign.setSize (L);
        fftBlock.setSize (2 * B);
        for (int k = 0; k <= L / 2; ++k)
            binTan[(size_t) k] = k == L / 2 ? 1.0e12 : std::tan (kPi * (double) k / (double) L);

        const double i0b = besselI0 (kLinearPhaseKaiserBeta);
        for (int m = 0; m <= 2 * c; ++m)
        {
            const double r = (double) (m - c) / (double) c;
            window[(size_t) m] = (float) (besselI0 (kLinearPhaseKaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b);
        }
        tablesBuilt = true;
    }

    reset();
}

void LinearPhaseStage::reset() noexcept
{
    for (int c = 0; c < kMaxChannels; ++c)
    {
        std::fill (inBlock[c].begin(), inBlock[c].end(), 0.0f);
        std::fill (outBlock[c].begin(), outBlock[c].end(), 0.0f);
        std::fill (identityBlock[c].begin(), identityBlock[c].end(), 0.0f);
        std::fill (history[c].begin(), history[c].end(), 0.0f);
        std::fill (overlap[c].begin(), overlap[c].end(), 0.0f);
        std::fill (fdl[c].begin(), fdl[c].begin() + ringBlocks * (lay.hop + 1), Complex {});
    }
    historyWrite = 0;
    fill = 0;
    blockIndex = 0;
    fdlHead = 0;
    for (auto& k : kernels)
        k.mode = Mode::Identity;
    current = 0;
    for (auto& s : snapshots)
        s = LinearPhaseSnapshot {};
    designedFrom = LinearPhaseSnapshot {};
    designedValid = true;      // identity, from neutral snapshots
    designs = 0;
}

void LinearPhaseStage::process (const float* const* in, float* const* out, float* const* delayedIn, int nCh, int n) noexcept
{
    nCh = std::clamp (nCh, 1, kMaxChannels);
    for (int j = 0; j < n;)
    {
        const int take = std::min (n - j, lay.hop - fill);
        for (int c = 0; c < nCh; ++c)
        {
            std::copy (in[c] + j, in[c] + j + take, inBlock[c].data() + fill);
            std::copy (outBlock[c].data() + fill, outBlock[c].data() + fill + take, out[c] + j);
            if (delayedIn != nullptr && delayedIn[c] != nullptr)
                std::copy (identityBlock[c].data() + fill, identityBlock[c].data() + fill + take, delayedIn[c] + j);
        }
        fill += take;
        j += take;

        if (fill == lay.hop)
        {
            runBlock (nCh);
            fill = 0;
        }
    }
}

void LinearPhaseStage::runBlock (int nCh) noexcept
{
    const int B = lay.hop, c = lay.centre();
    const int bins = B + 1;

    // Input: history (identity path), overlap-save frame and its spectrum.
    fdlHead = (fdlHead + 1) % ringBlocks;
    for (int ch = 0; ch < nCh; ++ch)
    {
        float* h = history[ch].data();
        for (int i = 0; i < B; ++i)
            h[(historyWrite + i) & kHistoryMask] = inBlock[ch][(size_t) i];

        // Identity output for this block: x[t - c], t = the block's input times.
        for (int i = 0; i < B; ++i)
            identityBlock[ch][(size_t) i] = h[(historyWrite + i - c + kHistorySize) & kHistoryMask];

        float* ov = overlap[ch].data();
        std::memmove (ov, ov + B, sizeof (float) * (size_t) B);
        std::copy (inBlock[ch].begin(), inBlock[ch].begin() + B, ov + B);

        std::copy (ov, ov + 2 * B, frame.data());
        fftBlock.forward (frame.data());
        const auto* spec = reinterpret_cast<const Complex*> (frame.data());
        std::copy (spec, spec + bins, fdl[ch].data() + (size_t) fdlHead * (size_t) bins);
    }
    historyWrite = (historyWrite + B) & kHistoryMask;

    // The gains of the audio now at the kernel centre: Ds = c / B blocks ago.
    const int ds = c / B;
    const auto& snap = snapshots[(size_t) ((blockIndex + kSnapshotRing - (std::uint64_t) ds) % kSnapshotRing)];

    Kernel& cur = kernels[current];
    const bool unchanged = designedValid && snap.sameAs (designedFrom);

    if (unchanged)
    {
        if (cur.mode == Mode::Identity)
            for (int ch = 0; ch < nCh; ++ch)
                std::copy (identityBlock[ch].begin(), identityBlock[ch].begin() + B, outBlock[ch].begin());
        else
        {
            float* dst[kMaxChannels] { blockA[0].data(), blockA[1].data() };
            convolve (cur, nCh, dst);
            for (int ch = 0; ch < nCh; ++ch)
                std::copy (blockA[ch].begin(), blockA[ch].begin() + B, outBlock[ch].begin());
        }
    }
    else
    {
        Kernel& next = kernels[1 - current];
        if (snap.isIdentity())
            next.mode = Mode::Identity;
        else
            design (snap, next);

        float* oldOut[kMaxChannels] { blockA[0].data(), blockA[1].data() };
        float* newOut[kMaxChannels] { blockB[0].data(), blockB[1].data() };
        if (cur.mode == Mode::Identity)
            for (int ch = 0; ch < nCh; ++ch) std::copy (identityBlock[ch].begin(), identityBlock[ch].begin() + B, blockA[ch].begin());
        else
            convolve (cur, nCh, oldOut);
        if (next.mode == Mode::Identity)
            for (int ch = 0; ch < nCh; ++ch) std::copy (identityBlock[ch].begin(), identityBlock[ch].begin() + B, blockB[ch].begin());
        else
            convolve (next, nCh, newOut);

        const float step = 1.0f / (float) B;
        for (int ch = 0; ch < nCh; ++ch)
            for (int i = 0; i < B; ++i)
            {
                const float w = (float) (i + 1) * step;
                const float a = blockA[ch][(size_t) i], b = blockB[ch][(size_t) i];
                outBlock[ch][(size_t) i] = a + w * (b - a);
            }

        current = 1 - current;
        designedFrom = snap;
        designedValid = true;
    }

    // The next block's snapshot starts as this one's (in case the caller leaves it alone).
    const auto& mine = snapshots[(size_t) (blockIndex % kSnapshotRing)];
    ++blockIndex;
    snapshots[(size_t) (blockIndex % kSnapshotRing)] = mine;
}

double LinearPhaseStage::modelMatrixEntry (EngineKind kind, const LinearPhaseSnapshot& s, double f, double fs, int lane, int from) noexcept
{
    const double t = std::tan (kPi * std::min (f, 0.4999999 * fs) / fs);
    double m[2][2] { { 1.0, 0.0 }, { 0.0, 1.0 } };
    for (const auto& b : s.bands)
    {
        if (! b.active)
            continue;
        const auto geo = geometry (b.lo, b.hi, fs);
        const double u = t / geo.g, v = u * u;
        const double k0 = std::sqrt (std::max (0.0, bandMagnitudeSquared (kind, v, geo.R, (double) b.gain[0])));
        const double k1 = std::sqrt (std::max (0.0, bandMagnitudeSquared (kind, v, geo.R, (double) b.gain[s.numChannels > 1 ? 1 : 0])));
        if (b.midSide && s.numChannels > 1)
        {
            const double sv = 0.5 * (k0 + k1), dv = 0.5 * (k0 - k1);
            for (int col = 0; col < 2; ++col)
            {
                const double r0 = m[0][col], r1 = m[1][col];
                m[0][col] = sv * r0 + dv * r1;
                m[1][col] = dv * r0 + sv * r1;
            }
        }
        else
        {
            for (int col = 0; col < 2; ++col)
            {
                m[0][col] *= k0;
                m[1][col] *= s.numChannels > 1 ? k1 : k0;
            }
        }
    }
    return m[lane][from];
}

void LinearPhaseStage::accumulateSquared (const LinearPhaseSnapshot::Band& b, int lane, double* w) const noexcept
{
    const auto geo = geometry (b.lo, b.hi, fs);
    const double invG2 = 1.0 / (geo.g * geo.g), r4 = 4.0 * geo.R * geo.R;
    const double G = std::max ((double) b.gain[lane], 1.0e-9);
    const double* t = binTan.data();
    const int half = lay.taps / 2;

    // Branch-free so that the loops vectorise. a + b > 0 for every bin (b > 0 above DC, a = 1 at DC).
    if (kind == EngineKind::APrime)
    {
        const double invG = 1.0 / G;
        for (int k = 0; k <= half; ++k)
        {
            const double v = t[k] * t[k] * invG2;
            const double a = (1.0 - v) * (1.0 - v), bb = r4 * v;
            w[k] *= (a + bb * G) / (a + bb * invG);
        }
    }
    else
    {
        const double g2m1 = G * G - 1.0;
        for (int k = 0; k <= half; ++k)
        {
            const double v = t[k] * t[k] * invG2;
            const double a = (1.0 - v) * (1.0 - v), bb = r4 * v;
            w[k] *= 1.0 + g2m1 * bb / (a + bb);
        }
    }
}

void LinearPhaseStage::design (const LinearPhaseSnapshot& s, Kernel& dst) noexcept
{
    ++designs;
    const int half = lay.taps / 2;
    const int nCh = s.numChannels;

    // Which kernels are needed: left / right bands with different gains need L/R lanes, mid/side
    // bands with different gains need M/S lanes; both kinds at once need the full matrix. A band
    // with the same gain on both lanes is the same in either basis.
    bool lrDiff = false, msDiff = false;
    for (const auto& b : s.bands)
    {
        if (! b.active || nCh < 2) continue;
        if (sameBits (b.gain[0], b.gain[1])) continue;
        (b.midSide ? msDiff : lrDiff) = true;
    }

    if (! (lrDiff && msDiff))
    {
        // Diagonal in one basis: accumulate squared magnitudes per lane, one square root at the end.
        const int lanes = (lrDiff || msDiff) ? 2 : 1;
        for (int l = 0; l < lanes; ++l)
        {
            double* w = work[l].data();
            std::fill (w, w + half + 1, 1.0);
            for (const auto& b : s.bands)
                if (b.active && ! isExactlyOne (b.gain[l]))
                    accumulateSquared (b, l, w);
            for (int k = 0; k <= half; ++k)
                mag[l][(size_t) k] = (float) std::sqrt (std::max (0.0, w[k]));
        }

        designEntry (mag[0].data(), dst.k[0]);
        if (lanes == 1)
            dst.mode = Mode::DiagSame;
        else
        {
            designEntry (mag[1].data(), dst.k[3]);
            dst.mode = msDiff ? Mode::MidSide : Mode::Diag;
        }
        return;
    }

    // Full 2x2 per bin: M = K_last ... K_first.
    std::fill (work[0].begin(), work[0].begin() + half + 1, 1.0);   // m00
    std::fill (work[1].begin(), work[1].begin() + half + 1, 0.0);   // m01
    std::fill (work[2].begin(), work[2].begin() + half + 1, 0.0);   // m10
    std::fill (work[3].begin(), work[3].begin() + half + 1, 1.0);   // m11
    double* k0s = scratchMag[0].data();
    double* k1s = scratchMag[1].data();
    for (const auto& b : s.bands)
    {
        if (! b.active) continue;
        const bool one0 = isExactlyOne (b.gain[0]), one1 = isExactlyOne (b.gain[1]);
        if (one0 && one1) continue;

        std::fill (k0s, k0s + half + 1, 1.0);
        std::fill (k1s, k1s + half + 1, 1.0);
        if (! one0) accumulateSquared (b, 0, k0s);
        if (! one1) accumulateSquared (b, 1, k1s);

        double* m00 = work[0].data(); double* m01 = work[1].data();
        double* m10 = work[2].data(); double* m11 = work[3].data();
        for (int k = 0; k <= half; ++k)
        {
            const double k0 = std::sqrt (std::max (0.0, k0s[k])), k1 = std::sqrt (std::max (0.0, k1s[k]));
            if (b.midSide)
            {
                const double sv = 0.5 * (k0 + k1), dv = 0.5 * (k0 - k1);
                const double a0 = m00[k], a1 = m01[k], b0 = m10[k], b1 = m11[k];
                m00[k] = sv * a0 + dv * b0;  m01[k] = sv * a1 + dv * b1;
                m10[k] = dv * a0 + sv * b0;  m11[k] = dv * a1 + sv * b1;
            }
            else
            {
                m00[k] *= k0; m01[k] *= k0;
                m10[k] *= k1; m11[k] *= k1;
            }
        }
    }
    for (int e = 0; e < 4; ++e)
    {
        for (int k = 0; k <= half; ++k)
            mag[e][(size_t) k] = (float) work[e][(size_t) k];
        designEntry (mag[e].data(), dst.k[e]);
    }
    dst.mode = Mode::Full;
}

void LinearPhaseStage::designEntry (const float* magnitude, std::vector<Complex>& dst) noexcept
{
    const int L = lay.taps, B = lay.hop, c = lay.centre();
    const int half = L / 2, bins = B + 1;

    // Zero-phase impulse response of the magnitude (real, even), centred at 0.
    float* s = scratch.data();
    for (int k = 0; k <= half; ++k)
    {
        s[2 * k] = magnitude[k];
        s[2 * k + 1] = 0.0f;
    }
    fftDesign.inverse (s);

    // Taps m = 0 .. 2c: the response shifted by c, windowed; partitions of B taps.
    for (int p = 0; p < partitions; ++p)
    {
        float* f = frame.data();
        std::fill (f, f + 2 * B + 2, 0.0f);
        for (int i = 0; i < B; ++i)
        {
            const int m = p * B + i;
            if (m > 2 * c) break;
            f[i] = s[(m - c + L) & (L - 1)] * window[(size_t) m];
        }
        fftBlock.forward (f);
        const auto* spec = reinterpret_cast<const Complex*> (f);
        std::copy (spec, spec + bins, dst.data() + (size_t) p * (size_t) bins);
    }
}

void LinearPhaseStage::convolve (const Kernel& kern, int nCh, float* const* dst) noexcept
{
    const int B = lay.hop, bins = B + 1;

    const auto multiplyAdd = [&] (Complex* acc, const std::vector<Complex>& k, int from)
    {
        for (int p = 0; p < partitions; ++p)
        {
            const int slot = (fdlHead - p + ringBlocks) % ringBlocks;
            const float* x = reinterpret_cast<const float*> (fdl[from].data() + (size_t) slot * (size_t) bins);
            const float* h = reinterpret_cast<const float*> (k.data() + (size_t) p * (size_t) bins);
            float* a = reinterpret_cast<float*> (acc);
            for (int i = 0; i < 2 * bins; i += 2)
            {
                a[i]     += x[i] * h[i]     - x[i + 1] * h[i + 1];
                a[i + 1] += x[i] * h[i + 1] + x[i + 1] * h[i];
            }
        }
    };
    const auto inverseTo = [&] (const Complex* acc, float* out)
    {
        float* f = frame.data();
        std::copy (reinterpret_cast<const float*> (acc), reinterpret_cast<const float*> (acc) + 2 * bins, f);
        fftBlock.inverse (f);
        std::copy (f + B, f + 2 * B, out);
    };

    if (kern.mode == Mode::MidSide && nCh == 2)
    {
        // Mid and side of the input spectra (the transform is linear), one kernel each.
        Complex* am = accum[0].data();
        Complex* as = accum[1].data();
        std::fill (am, am + bins, Complex {});
        std::fill (as, as + bins, Complex {});
        for (int p = 0; p < partitions; ++p)
        {
            const int slot = (fdlHead - p + ringBlocks) % ringBlocks;
            const float* xl = reinterpret_cast<const float*> (fdl[0].data() + (size_t) slot * (size_t) bins);
            const float* xr = reinterpret_cast<const float*> (fdl[1].data() + (size_t) slot * (size_t) bins);
            const float* hm = reinterpret_cast<const float*> (kern.k[0].data() + (size_t) p * (size_t) bins);
            const float* hs = reinterpret_cast<const float*> (kern.k[3].data() + (size_t) p * (size_t) bins);
            float* fm = reinterpret_cast<float*> (am);
            float* fs2 = reinterpret_cast<float*> (as);
            for (int i = 0; i < 2 * bins; i += 2)
            {
                const float mr = xl[i] + xr[i], mi = xl[i + 1] + xr[i + 1];
                const float sr = xl[i] - xr[i], si = xl[i + 1] - xr[i + 1];
                fm[i]      += mr * hm[i] - mi * hm[i + 1];
                fm[i + 1]  += mr * hm[i + 1] + mi * hm[i];
                fs2[i]     += sr * hs[i] - si * hs[i + 1];
                fs2[i + 1] += sr * hs[i + 1] + si * hs[i];
            }
        }
        {
            float* fm = reinterpret_cast<float*> (am);
            float* fs2 = reinterpret_cast<float*> (as);
            for (int i = 0; i < 2 * bins; ++i)
            {
                const float m = 0.5f * fm[i], sd = 0.5f * fs2[i];
                fm[i] = m + sd;     // left
                fs2[i] = m - sd;    // right
            }
        }
        inverseTo (am, dst[0]);
        inverseTo (as, dst[1]);
        return;
    }

    for (int out = 0; out < nCh; ++out)
    {
        Complex* acc = accum[out].data();
        std::fill (acc, acc + bins, Complex {});
        switch (kern.mode)
        {
            case Mode::DiagSame: multiplyAdd (acc, kern.k[0], out); break;
            case Mode::Diag:     multiplyAdd (acc, kern.k[out == 0 ? 0 : 3], out); break;
            case Mode::MidSide:  multiplyAdd (acc, kern.k[0], out); break;   // (mono: mid only)
            case Mode::Full:
                for (int from = 0; from < nCh; ++from)
                    multiplyAdd (acc, kern.k[out * 2 + from], from);
                break;
            case Mode::Identity: break;
        }
        inverseTo (acc, dst[out]);
    }
}

void LinearPhaseStage::copyFrom (const LinearPhaseStage& o) noexcept
{
    // Same capacity by construction: plain copies, no allocation.
    lay = o.lay; fs = o.fs;   // the model kind stays this stage's own
    partitions = o.partitions; ringBlocks = o.ringBlocks;
    tablesBuilt = false;
    if (o.tablesBuilt)
    {
        std::copy (o.binTan.begin(), o.binTan.end(), binTan.begin());
        std::copy (o.window.begin(), o.window.end(), window.begin());
        fftDesign.copyFrom (o.fftDesign);
        fftBlock.copyFrom (o.fftBlock);
        tablesBuilt = true;
    }
    for (int c = 0; c < kMaxChannels; ++c)
    {
        std::copy (o.inBlock[c].begin(), o.inBlock[c].end(), inBlock[c].begin());
        std::copy (o.outBlock[c].begin(), o.outBlock[c].end(), outBlock[c].begin());
        std::copy (o.identityBlock[c].begin(), o.identityBlock[c].end(), identityBlock[c].begin());
        std::copy (o.history[c].begin(), o.history[c].end(), history[c].begin());
        std::copy (o.overlap[c].begin(), o.overlap[c].end(), overlap[c].begin());
        std::copy (o.fdl[c].begin(), o.fdl[c].end(), fdl[c].begin());
    }
    historyWrite = o.historyWrite; fill = o.fill; blockIndex = o.blockIndex; fdlHead = o.fdlHead;
    for (int i = 0; i < 2; ++i)
    {
        kernels[i].mode = o.kernels[i].mode;
        for (int e = 0; e < 4; ++e)
            std::copy (o.kernels[i].k[e].begin(), o.kernels[i].k[e].end(), kernels[i].k[e].begin());
    }
    current = o.current;
    for (int i = 0; i < kSnapshotRing; ++i)
        snapshots[i] = o.snapshots[i];
    designedFrom = o.designedFrom;
    designedValid = o.designedValid && kind == o.kind;   // another model: redesign at the next block
    designs = o.designs;
}
} // namespace tl::dsp
