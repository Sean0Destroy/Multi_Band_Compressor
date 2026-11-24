#pragma once

#include <algorithm>
#include <cmath>

#include "DspConstants.h"

// Linear-phase 2x / 4x / 8x oversampling for the nonlinear stages (Kaiser-windowed half-band
// FIR stages; stop band about 84 dB for the first and 88 dB for the other two). Fixed storage, no
// allocation; coefficients are designed in prepare() (fixed loops, no allocation: it normally
// runs on the message thread, and on the audio thread when the band engine is switched).
//
// Round-trip latency in samples at the base rate (independent of the sample rate):
//   2x: 47     4x: 47 + 6 = 53     8x: 47 + 6 + 2 = 55
namespace tl::dsp
{
inline constexpr int kMaxOversamplingStages = 3;

/** Round-trip latency (base-rate samples) for a number of 2x stages (0..3). */
inline constexpr int oversamplingLatency (int stages) noexcept
{
    return stages <= 0 ? 0 : stages == 1 ? 47 : stages == 2 ? 53 : 55;
}

/** One 2x stage: interpolating and decimating FIR with the same odd-length low-pass
    (cut-off at a quarter of the high rate).

    Cost: a half-band filter is zero at every even distance from its centre except the centre
    itself, and it is symmetric. So of the high-rate outputs of the interpolator, one phase is the
    centre tap alone (a delayed input sample times a constant) and the other is a sum over the
    nonzero taps with mirrored pairs added before the multiply; the decimator likewise takes the
    centre tap from one polyphase branch and the pairs from the other. That is (numTaps + 1) / 4
    multiplies per output instead of numTaps. The histories are stored twice in a row, so every
    window is contiguous (no wrap-around in the inner loops). */
class HalfbandStage
{
public:
    static constexpr int maxTaps = 95;

    /** numTaps odd. The pair up + down delays by (numTaps - 1) samples at the high rate. */
    void design (int taps, double attenuationDb = 90.0) noexcept
    {
        numTaps = std::clamp (taps | 1, 3, maxTaps);
        const int centre = (numTaps - 1) / 2;
        const double beta = attenuationDb > 50.0 ? 0.1102 * (attenuationDb - 8.7)
                                                 : 0.5842 * std::pow (std::max (0.0, attenuationDb - 21.0), 0.4) + 0.07886 * (attenuationDb - 21.0);
        const auto bessel = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
            return sum;
        };

        // Computed for m >= 0 and mirrored, so the symmetry is exact.
        double total = 0.0;
        double h[maxTaps] {};
        for (int m = 0; m <= centre; ++m)
        {
            // Ideal half-band low-pass: 0.5 sinc (m / 2); exactly zero at even m != 0.
            const double ideal = m == 0 ? 0.5 : (m % 2 == 0 ? 0.0 : std::sin (kPi * m / 2.0) / (kPi * m));
            const double r = (double) m / (double) centre;
            const double v = ideal * bessel (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel (beta);
            h[centre + m] = h[centre - m] = v;
            total += m == 0 ? v : 2.0 * v;
        }
        for (int n = 0; n < numTaps; ++n)
            coeffs[n] = (float) (h[n] / total);

        // Polyphase layout. The nonzero side taps sit at odd distances from the centre: k = q + 2j
        // with q the parity opposite to the centre's, paired with its mirror 2 centre - k.
        centreTap = centre;
        centreParity = centre % 2;
        sideParity = 1 - centreParity;
        numPairs = (centre - sideParity + 1) / 2;
        for (int j = 0; j < numPairs; ++j)
            pairCoeffs[j] = coeffs[sideParity + 2 * j];
        centreCoeff = coeffs[centre];

        reset();
    }

    void reset() noexcept
    {
        std::fill (std::begin (upHistory), std::end (upHistory), 0.0f);
        std::fill (std::begin (evenHistory), std::end (evenHistory), 0.0f);
        std::fill (std::begin (oddHistory), std::end (oddHistory), 0.0f);
        upPos = downPos = 0;
    }

    int taps() const noexcept { return numTaps; }
    float coefficient (int i) const noexcept { return coeffs[i]; }

    /** n input samples -> 2n output samples (gain 1 in the pass band). */
    void upsample (const float* in, float* out, int n) noexcept
    {
        // Zero-stuffing by 2 followed by the FIR (and the gain of 2): output phase p at input i
        // sums the taps k = p (mod 2) over x[i - (k - p) / 2].
        const int ring = centreTap + 1;                   // input delays 0 .. centreTap are read
        const int centreDelay = (centreTap - centreParity) / 2;
        const float centreGain = 2.0f * centreCoeff;
        for (int i = 0; i < n; ++i)
        {
            upPos = upPos + 1 == ring ? 0 : upPos + 1;
            upHistory[upPos] = upHistory[upPos + ring] = in[i];
            const float* x = upHistory + upPos + ring;    // x[-d] is the input d samples ago

            // Side phase: taps k = q + 2j (delay j) and their mirrors (delay centre - q - j).
            const float acc = pairSum (x, -centreTap + sideParity, 0);

            out[2 * i + sideParity] = 2.0f * acc;
            out[2 * i + centreParity] = centreGain * x[-centreDelay];
        }
    }

    /** 2n input samples -> n output samples. */
    void downsample (const float* in, float* out, int n) noexcept
    {
        // The output is taken at the EVEN high-rate sample (the one aligned with a base-rate
        // sample), so the round trip is a whole number of base-rate samples. With t = 2i, tap k
        // reads in[2i - k]: an even k reads the even branch E[i - k / 2], an odd k the odd branch
        // O[i - (k + 1) / 2] (O[i] = in[2i + 1] has not arrived yet).
        const int ring = centreTap + 1;
        const int centreOffset = (centreTap + centreParity) / 2;
        for (int i = 0; i < n; ++i)
        {
            const int next = downPos + 1 == ring ? 0 : downPos + 1;
            evenHistory[next] = evenHistory[next + ring] = in[2 * i];

            const float* e = evenHistory + next + ring;    // e[-d] = E[i - d]
            const float* o = oddHistory + downPos + ring;  // o[-d] = O[i - 1 - d]

            // Side taps k = q + 2j and 2 centre - k read branch offsets j + q and centre - j.
            const float* side = sideParity == 0 ? e : o + 1;
            const float acc = pairSum (side, -centreTap, sideParity);

            const float* mid = centreParity == 0 ? e : o + 1;
            out[i] = acc + centreCoeff * mid[-centreOffset];

            downPos = next;
            oddHistory[downPos] = oddHistory[downPos + ring] = in[2 * i + 1];
        }
    }

private:
    /** sum_j pairCoeffs[j] (p[-(j + near)] + p[far + j]) over the pairs. The usual pair counts
        (24, 6 and 4 for the three oversampling stages) get loops of fixed length, which the
        compiler unrolls and vectorises. */
    float pairSum (const float* p, int far, int near) const noexcept
    {
        switch (numPairs)
        {
            case 24: return pairSumFixed<24> (p, far, near);
            case 12: return pairSumFixed<12> (p, far, near);
            case 6:  return pairSumFixed<6> (p, far, near);
            case 4:  return pairSumFixed<4> (p, far, near);
            default: break;
        }
        float acc = 0.0f;
        for (int j = 0; j < numPairs; ++j)
            acc += pairCoeffs[j] * (p[-(j + near)] + p[far + j]);
        return acc;
    }

    template <int N>
    float pairSumFixed (const float* p, int far, int near) const noexcept
    {
        float acc = 0.0f;
        for (int j = 0; j < N; ++j)
            acc += pairCoeffs[j] * (p[-(j + near)] + p[far + j]);
        return acc;
    }

    float coeffs[maxTaps] {};
    float pairCoeffs[(maxTaps + 1) / 4] {};
    float centreCoeff = 0.0f;
    int numTaps = 3, centreTap = 1, centreParity = 1, sideParity = 0, numPairs = 1;
    float upHistory[2 * maxTaps] {};
    float evenHistory[2 * maxTaps] {}, oddHistory[2 * maxTaps] {};
    int upPos = 0, downPos = 0;
};

/** Up to three cascaded stages for one channel. process() runs a caller-supplied function on
    the oversampled block. */
class Oversampler
{
public:
    Oversampler() noexcept { prepare(); }

    void prepare() noexcept
    {
        stages[0].design (95);   // base <-> 2x: pass band to about 0.44 fs, stop band from 0.56 fs
        stages[1].design (25);   // 2x <-> 4x: wide transition
        stages[2].design (17);   // 4x <-> 8x
    }

    void reset() noexcept { for (auto& s : stages) s.reset(); }

    /** In place on n (<= kChunk) base-rate samples with `numStages` (1..3) 2x stages.
        `shaper (float* block, int count)` works on the oversampled block. */
    template <typename Shaper>
    void process (float* x, int n, int numStages, Shaper&& shaper) noexcept
    {
        numStages = std::clamp (numStages, 1, kMaxOversamplingStages);

        const float* src = x;
        int count = n;
        for (int s = 0; s < numStages; ++s)
        {
            float* dst = (s % 2 == 0) ? bufferA : bufferB;
            stages[s].upsample (src, dst, count);
            src = dst;
            count *= 2;
        }

        float* work = (numStages % 2 == 1) ? bufferA : bufferB;
        shaper (work, count);

        const float* down = work;
        for (int s = numStages - 1; s >= 0; --s)
        {
            count /= 2;
            float* dst = s == 0 ? x : ((s % 2 == 1) ? bufferA : bufferB);
            stages[s].downsample (down, dst, count);
            down = dst;
        }
    }

    const HalfbandStage& stage (int i) const noexcept { return stages[i]; }

private:
    HalfbandStage stages[kMaxOversamplingStages];
    float bufferA[kChunk * 8] {}, bufferB[kChunk * 8] {};
};
} // namespace tl::dsp
