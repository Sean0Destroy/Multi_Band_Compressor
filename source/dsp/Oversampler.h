#pragma once

#include <algorithm>
#include <cmath>

#include "DspConstants.h"

// Linear-phase 2x / 4x / 8x oversampling for the nonlinear stages (Kaiser-windowed half-band
// FIR stages, about 90 dB stop band). Fixed storage, no allocation; coefficients are designed
// in prepare() on the message thread.
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
    (cut-off at a quarter of the high rate). */
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

        double total = 0.0;
        double h[maxTaps] {};
        for (int n = 0; n < numTaps; ++n)
        {
            const int m = n - centre;
            // Ideal half-band low-pass: 0.5 sinc (m / 2); exactly zero at even m != 0.
            const double ideal = m == 0 ? 0.5 : (m % 2 == 0 ? 0.0 : std::sin (kPi * m / 2.0) / (kPi * m));
            const double r = (double) m / (double) centre;
            h[n] = ideal * bessel (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel (beta);
            total += h[n];
        }
        for (int n = 0; n < numTaps; ++n)
            coeffs[n] = (float) (h[n] / total);

        reset();
    }

    void reset() noexcept
    {
        std::fill (std::begin (upHistory), std::end (upHistory), 0.0f);
        std::fill (std::begin (downHistory), std::end (downHistory), 0.0f);
        upPos = downPos = 0;
    }

    int taps() const noexcept { return numTaps; }
    float coefficient (int i) const noexcept { return coeffs[i]; }

    /** n input samples -> 2n output samples (gain 1 in the pass band). */
    void upsample (const float* in, float* out, int n) noexcept
    {
        const int historyLength = (numTaps + 1) / 2;
        for (int i = 0; i < n; ++i)
        {
            upPos = (upPos + 1) % historyLength;
            upHistory[upPos] = in[i];

            // Polyphase form of zero-stuffing by 2 followed by the FIR (and the gain of 2).
            for (int phase = 0; phase < 2; ++phase)
            {
                float acc = 0.0f;
                int idx = upPos;
                for (int k = phase; k < numTaps; k += 2)
                {
                    acc += coeffs[k] * upHistory[idx];
                    idx = idx == 0 ? historyLength - 1 : idx - 1;
                }
                out[2 * i + phase] = 2.0f * acc;
            }
        }
    }

    /** 2n input samples -> n output samples. */
    void downsample (const float* in, float* out, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            // The output is taken at the EVEN high-rate sample (the one aligned with a base-rate
            // sample), so the round trip is a whole number of base-rate samples.
            downPos = (downPos + 1) % numTaps;
            downHistory[downPos] = in[2 * i];

            float acc = 0.0f;
            int idx = downPos;
            for (int k = 0; k < numTaps; ++k)
            {
                acc += coeffs[k] * downHistory[idx];
                idx = idx == 0 ? numTaps - 1 : idx - 1;
            }
            out[i] = acc;

            downPos = (downPos + 1) % numTaps;
            downHistory[downPos] = in[2 * i + 1];
        }
    }

private:
    float coeffs[maxTaps] {};
    int numTaps = 3;
    float upHistory[(maxTaps + 1) / 2] {};
    float downHistory[maxTaps] {};
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
