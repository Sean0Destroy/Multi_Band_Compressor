#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

// Real-input FFT for the linear-phase stage. JUCE's portable FFT allocates (or uses large alloca
// scratch) for big sizes, which is not allowed on the audio thread, so this one keeps every table
// it needs in storage reserved at construction, and transforms in place in the caller's buffer.
//   forward: N real samples in data[0 .. N) -> bins 0 .. N/2 as interleaved re/im in data[0 .. N + 2)
//   inverse: the same layout back to N real samples, scaled by 1 / N (inverse (forward (x)) == x)
// setSize() picks N (a power of two from 4 up to the maximum given at construction) and builds the
// tables for it: bounded work, no allocation. Radix-2, iterative, plain float arithmetic (no
// std::complex multiplies, which compile to library calls for their NaN handling and do not
// vectorise).
namespace tl::dsp
{
class RealFft
{
public:
    explicit RealFft (int maxRealSize)
        : maxSize (maxRealSize)
    {
        assert (maxRealSize >= 4 && (maxRealSize & (maxRealSize - 1)) == 0);
        const auto m = (size_t) maxRealSize / 2;
        stageTwiddle.assign (2 * m, 0.0f);
        splitTwiddle.assign (m + 2, 0.0f);
        bitReverse.assign (m, 0);
        setSize (maxRealSize);
    }

    int maximumSize() const noexcept { return maxSize; }
    int size() const noexcept        { return n; }

    /** Message or audio thread: bounded work (sin / cos per table entry), no allocation. */
    void setSize (int realSize) noexcept
    {
        assert (realSize >= 4 && realSize <= maxSize && (realSize & (realSize - 1)) == 0);
        if (realSize == n)
            return;
        n = realSize;
        const int m = n / 2;
        constexpr double twoPi = 2.0 * 3.14159265358979323846;

        // Stage with half-length h uses stageTwiddle[2 (h - 1) .. 2 (2h - 1)): exp(-2 pi i k / 2h).
        for (int h = 1; h < m; h <<= 1)
            for (int k = 0; k < h; ++k)
            {
                const double a = -twoPi * (double) k / (double) (2 * h);
                stageTwiddle[(size_t) (2 * (h - 1 + k))] = (float) std::cos (a);
                stageTwiddle[(size_t) (2 * (h - 1 + k) + 1)] = (float) std::sin (a);
            }
        // Split step: exp(-2 pi i k / n), k = 0 .. m/2.
        for (int k = 0; k <= m / 2; ++k)
        {
            const double a = -twoPi * (double) k / (double) n;
            splitTwiddle[(size_t) (2 * k)] = (float) std::cos (a);
            splitTwiddle[(size_t) (2 * k + 1)] = (float) std::sin (a);
        }
        int bits = 0;
        while ((1 << bits) < m) ++bits;
        for (int i = 0; i < m; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            bitReverse[(size_t) i] = r;
        }
    }

    void copyFrom (const RealFft& o) noexcept
    {
        assert (o.maxSize == maxSize);
        n = o.n;
        std::copy (o.stageTwiddle.begin(), o.stageTwiddle.end(), stageTwiddle.begin());
        std::copy (o.splitTwiddle.begin(), o.splitTwiddle.end(), splitTwiddle.begin());
        std::copy (o.bitReverse.begin(), o.bitReverse.end(), bitReverse.begin());
    }

    /** data needs room for size() + 2 floats. */
    void forward (float* data) const noexcept
    {
        const int m = n / 2;
        complexFft (data, m, false);

        // Split the half-length complex transform Z into the real transform's bins:
        //   E = (Z[k] + conj Z[m-k]) / 2,  O = (Z[k] - conj Z[m-k]) / 2i,  X[k] = E + W^k O,
        //   X[m-k] = conj E + W^(m-k) conj O = conj E - conj (W^k) conj O.
        const float z0r = data[0], z0i = data[1];
        data[0] = z0r + z0i;  data[1] = 0.0f;
        data[2 * m] = z0r - z0i;  data[2 * m + 1] = 0.0f;
        const float* w = splitTwiddle.data();
        for (int k = 1; k <= m / 2; ++k)
        {
            float* a = data + 2 * k;
            float* b = data + 2 * (m - k);
            const float ar = a[0], ai = a[1], br = b[0], bi = -b[1];   // b = conj Z[m-k]
            const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
            // O = (a - b) / 2i = (ai - bi) / 2 - i (ar - br) / 2
            const float orr = 0.5f * (ai - bi), oi = -0.5f * (ar - br);
            const float wr = w[2 * k], wi = w[2 * k + 1];
            const float tr = wr * orr - wi * oi, ti = wr * oi + wi * orr;    // W^k O
            a[0] = er + tr;   a[1] = ei + ti;
            b[0] = er - tr;   b[1] = -(ei - ti);                              // conj (E - W^k O)
        }
    }

    /** data holds size() / 2 + 1 bins (size() + 2 floats); on return the first size() floats are
        the real signal (scaled by 1 / size()). */
    void inverse (float* data) const noexcept
    {
        const int m = n / 2;
        const float r0 = data[0], rm = data[2 * m];
        data[0] = 0.5f * (r0 + rm);
        data[1] = 0.5f * (r0 - rm);
        const float* w = splitTwiddle.data();
        for (int k = 1; k <= m / 2; ++k)
        {
            float* a = data + 2 * k;
            float* b = data + 2 * (m - k);
            const float xr = a[0], xi = a[1], yr = b[0], yi = -b[1];     // y = conj X[m-k]
            const float er = 0.5f * (xr + yr), ei = 0.5f * (xi + yi);
            const float dr = 0.5f * (xr - yr), di = 0.5f * (xi - yi);    // W^k O
            const float wr = w[2 * k], wi = -w[2 * k + 1];                // conj W^k
            const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;
            // Z[k] = E + i O;  Z[m-k] = conj E + i conj O
            a[0] = er - oi;   a[1] = ei + orr;
            b[0] = er + oi;   b[1] = -ei + orr;
        }

        complexFft (data, m, true);
        const float scale = 1.0f / (float) m;
        for (int i = 0; i < n; ++i)
            data[i] *= scale;
    }

private:
    /** In-place radix-2 FFT of m complex values, interleaved (inverse: conjugate twiddles, no
        scaling). */
    void complexFft (float* z, int m, bool inverse) const noexcept
    {
        const int* rev = bitReverse.data();
        for (int i = 0; i < m; ++i)
        {
            const int j = rev[i];
            if (i < j)
            {
                std::swap (z[2 * i], z[2 * j]);
                std::swap (z[2 * i + 1], z[2 * j + 1]);
            }
        }

        // First stage: twiddle 1.
        for (int i = 0; i < m; i += 2)
        {
            float* a = z + 2 * i;
            const float tr = a[2], ti = a[3];
            a[2] = a[0] - tr;  a[3] = a[1] - ti;
            a[0] += tr;        a[1] += ti;
        }

        const float sign = inverse ? -1.0f : 1.0f;
        for (int h = 2; h <= m / 2; h <<= 1)
        {
            const float* w = stageTwiddle.data() + 2 * (h - 1);
            for (int i = 0; i < m; i += 2 * h)
            {
                float* a = z + 2 * i;
                float* b = z + 2 * (i + h);
                for (int k = 0; k < h; ++k)
                {
                    const float wr = w[2 * k], wi = sign * w[2 * k + 1];
                    const float br = b[2 * k], bi = b[2 * k + 1];
                    const float tr = br * wr - bi * wi, ti = br * wi + bi * wr;
                    const float ar = a[2 * k], ai = a[2 * k + 1];
                    b[2 * k] = ar - tr;  b[2 * k + 1] = ai - ti;
                    a[2 * k] = ar + tr;  a[2 * k + 1] = ai + ti;
                }
            }
        }
    }

    int maxSize;
    int n = 0;
    std::vector<float> stageTwiddle, splitTwiddle;
    std::vector<int> bitReverse;
};
} // namespace tl::dsp
