#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "dsp/KWeighting.h"
#include "dsp/Oversampler.h"

// Programme loudness and true peak of the plugin output, after ITU-R BS.1770 / EBU R 128
// [the documents could not be fetched while writing this; the implementation follows the
// commonly published algorithm and is checked against the EBU Tech 3341 style cases in the
// tests]:
//   K-weighting -> mean square per channel, summed -> 100 ms sub-blocks
//   momentary  = last 400 ms,  short-term = last 3 s,  L = -0.691 + 10 log10 (sum)
//   integrated = mean of all 400 ms blocks (75 % overlap) above -70 LUFS and above the
//                relative gate 10 LU below the mean of the blocks above -70 LUFS
//   true peak  = highest absolute sample value of the 4x oversampled signal (max since reset)
// Written on the audio thread (fixed storage, no allocation), read lock-free by the editor.
namespace tl::rt
{
class LoudnessMeter
{
public:
    static constexpr int maxChannels = 2;
    static constexpr float silenceLufs = -120.0f;

    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        shelf = dsp::kWeightingShelf (fs);
        highPass = dsp::kWeightingHighPass (fs);
        hop = std::max (1, (int) std::lround (0.1 * fs));
        for (auto& s : upA) s.design (95);
        for (auto& s : upB) s.design (25);
        for (int i = 0; i < numBins; ++i)
            binEnergy[i] = std::pow (10.0, (binLoudness (i) + 0.691) / 10.0);
        clearAll();
    }

    /** Any thread: start measuring afresh (integrated loudness, true-peak maximum and the
        momentary / short-term windows; nothing from before the reset is counted again). */
    void requestReset() noexcept { resetRequested.store (true, std::memory_order_relaxed); }

    /** Audio thread: measuring continues after a pause (no audio was seen in between). The
        momentary and short-term windows and the filters start afresh; the integrated loudness
        and the true-peak maximum are kept. */
    void restartWindows() noexcept { clearWindows(); }

    /** Audio thread: one block of the output (right may be nullptr for mono). */
    void process (const float* left, const float* right, int numSamples) noexcept
    {
        if (resetRequested.exchange (false, std::memory_order_relaxed))
            clearIntegrated();

        const float* in[maxChannels] { left, right };
        const int channels = right != nullptr ? 2 : 1;

        for (int offset = 0; offset < numSamples; offset += chunk)
        {
            const int n = std::min (chunk, numSamples - offset);

            // True peak: 4x interpolation, highest magnitude.
            for (int c = 0; c < channels; ++c)
            {
                upA[c].upsample (in[c] + offset, scratch2, n);
                upB[c].upsample (scratch2, scratch4, 2 * n);
                float m = truePeak;
                for (int i = 0; i < 4 * n; ++i) m = std::max (m, std::abs (scratch4[i]));
                truePeak = m;
            }

            // Loudness.
            for (int i = 0; i < n; ++i)
            {
                double sum = 0.0;
                for (int c = 0; c < channels; ++c)
                {
                    const double k = filter[c].tick (shelf, highPass, (double) in[c][offset + i]);
                    sum += k * k;
                }
                subEnergy += sum;
                if (++subCount >= hop)
                    finishSubBlock();
            }
        }

        truePeakDb.store (truePeak > 1.0e-6f ? 20.0f * std::log10 (truePeak) : silenceLufs, std::memory_order_relaxed);
    }

    // ---- Reader --------------------------------------------------------------------------------
    float getMomentaryLufs() const noexcept  { return momentary.load (std::memory_order_relaxed); }
    float getShortTermLufs() const noexcept  { return shortTerm.load (std::memory_order_relaxed); }
    float getIntegratedLufs() const noexcept { return integrated.load (std::memory_order_relaxed); }
    /** Highest true peak since the last reset, in dBTP. */
    float getTruePeakDb() const noexcept     { return truePeakDb.load (std::memory_order_relaxed); }

private:
    static constexpr int chunk = 256;
    static constexpr int subBlocks = 30;          // 3 s of 100 ms sub-blocks
    static constexpr int numBins = 800;           // -70 .. +10 LUFS in 0.1 LU steps
    static double binLoudness (int bin) noexcept { return -70.0 + 0.1 * (double) bin + 0.05; }

    static float toLufs (double meanSquareSum) noexcept
    {
        return meanSquareSum > 1.0e-12 ? (float) (-0.691 + 10.0 * std::log10 (meanSquareSum)) : silenceLufs;
    }

    void finishSubBlock() noexcept
    {
        ring[ringPos] = subEnergy / (double) subCount;
        ringPos = (ringPos + 1) % subBlocks;
        ringFilled = std::min (subBlocks, ringFilled + 1);
        subEnergy = 0.0;
        subCount = 0;

        const auto meanOfLast = [this] (int count)
        {
            count = std::min (count, ringFilled);
            double sum = 0.0;
            for (int i = 1; i <= count; ++i) sum += ring[(ringPos - i + 2 * subBlocks) % subBlocks];
            return count > 0 ? sum / (double) count : 0.0;
        };

        momentary.store (toLufs (meanOfLast (4)), std::memory_order_relaxed);
        shortTerm.store (toLufs (meanOfLast (subBlocks)), std::memory_order_relaxed);

        // Gating block: the last 400 ms, once per 100 ms (75 % overlap), above the absolute gate.
        if (ringFilled >= 4)
        {
            const double loudness = (double) toLufs (meanOfLast (4));
            if (loudness >= -70.0)
            {
                const int bin = std::clamp ((int) ((loudness + 70.0) * 10.0), 0, numBins - 1);
                ++histogram[bin];

                double total = 0.0;
                std::uint64_t count = 0;
                for (int i = 0; i < numBins; ++i) { total += binEnergy[i] * (double) histogram[i]; count += histogram[i]; }

                const double relativeGate = (double) toLufs (total / (double) count) - 10.0;
                double gated = 0.0;
                std::uint64_t gatedCount = 0;
                for (int i = 0; i < numBins; ++i)
                    if (binLoudness (i) + 0.05 >= relativeGate) { gated += binEnergy[i] * (double) histogram[i]; gatedCount += histogram[i]; }

                integrated.store (gatedCount > 0 ? toLufs (gated / (double) gatedCount) : silenceLufs, std::memory_order_relaxed);
            }
        }
    }

    void clearWindows() noexcept
    {
        for (auto& f : filter) f.reset();
        for (auto& s : upA) s.reset();
        for (auto& s : upB) s.reset();
        std::fill (std::begin (ring), std::end (ring), 0.0);
        ringPos = ringFilled = subCount = 0;
        subEnergy = 0.0;
        momentary.store (silenceLufs, std::memory_order_relaxed);
        shortTerm.store (silenceLufs, std::memory_order_relaxed);
    }

    void clearIntegrated() noexcept
    {
        clearWindows();
        std::fill (std::begin (histogram), std::end (histogram), 0u);
        integrated.store (silenceLufs, std::memory_order_relaxed);
        truePeak = 0.0f;
        truePeakDb.store (silenceLufs, std::memory_order_relaxed);
    }

    void clearAll() noexcept
    {
        clearIntegrated();
    }

    double fs = 48000.0;
    dsp::BiquadCoeffs shelf, highPass;
    dsp::KWeightingFilter filter[maxChannels];
    dsp::HalfbandStage upA[maxChannels], upB[maxChannels];
    float scratch2[2 * chunk] {}, scratch4[4 * chunk] {};

    int hop = 4800, subCount = 0, ringPos = 0, ringFilled = 0;
    double subEnergy = 0.0;
    double ring[subBlocks] {};
    std::uint32_t histogram[numBins] {};
    double binEnergy[numBins] {};
    float truePeak = 0.0f;

    std::atomic<float> momentary { silenceLufs }, shortTerm { silenceLufs }, integrated { silenceLufs }, truePeakDb { silenceLufs };
    std::atomic<bool> resetRequested { false };
};
} // namespace tl::rt
