#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "A3Model.h"
#include "DspConstants.h"
#include "RealFft.h"

// Linear-phase mode of the band section.
//
// The band section's response at any moment is the product of the band models (A3Model.h) at the
// gains the dynamics core computes. In linear phase that product is taken as a pure magnitude and
// applied by a symmetric FIR: same curve as the zero-latency engine, no phase shift, a constant
// delay of half the kernel. The kernel is redesigned every hop (about 5.3 ms) from the gains of the
// audio at the kernel centre, and the outputs of the old and the new kernel are crossfaded across
// the hop, so gain changes are smooth and arrive with the audio they belong to.
//
//   design   per bin, a 2x2 real matrix per band in slot order (L/R bands: diagonal; mid/side
//            bands: [[s, d], [d, s]], s = (kM + kS) / 2, d = (kM - kS) / 2); the product over the
//            bands (which is diagonal on left / right, or on mid / side, unless the two kinds of
//            band are mixed, so that most settings need two kernels, not four); IFFT of size L; 2c + 1 centred taps (c = L/2 - B) under a Kaiser window
//            (beta 8); split into partitions of B taps; FFT of size 2B per partition.
//   apply    uniformly partitioned overlap-save convolution with block B, one frequency-domain
//            delay line per input lane. Latency = c + B = L / 2 exactly.
//   neutral  when every band is at unity the kernel is the identity: the output is the input
//            delayed by L / 2, exactly.
// Allocation happens only in the constructor (for the largest layout); everything else works in
// that fixed storage.
namespace tl::dsp
{
/** The three kernel lengths (parameter "lpLength": Short, Medium, Long). */
inline constexpr int kNumLinearPhaseLengths = 3;
inline constexpr double kLinearPhaseLengthSeconds[kNumLinearPhaseLengths] { 0.0853, 0.1707, 0.3413 };
inline constexpr double kLinearPhaseHopSeconds = 0.00533;
inline constexpr int kLinearPhaseMaxTaps = 32768;     // L at most
inline constexpr int kLinearPhaseMaxPartitions = 128; // L / B at most
inline constexpr int kLinearPhaseMinHop = 64, kLinearPhaseMaxHop = 1024;
inline constexpr double kLinearPhaseKaiserBeta = 8.0;

struct LinearPhaseLayout
{
    int taps = 0;   // L: FFT size of the design (power of two)
    int hop = 0;    // B: block and partition size
    int latency() const noexcept { return taps / 2; }
    int centre() const noexcept  { return taps / 2 - hop; }
};

inline int roundedPowerOfTwo (double x) noexcept
{
    return 1 << std::max (0, (int) std::lround (std::log2 (std::max (1.0, x))));
}

/** The layout for a sample rate and a length choice (0 Short, 1 Medium, 2 Long). At 48 kHz:
    L = 4096 / 8192 / 16384, B = 256 (latency 42.7 / 85.3 / 170.7 ms). */
inline LinearPhaseLayout linearPhaseLayout (double sampleRate, int lengthChoice) noexcept
{
    const int choice = std::clamp (lengthChoice, 0, kNumLinearPhaseLengths - 1);
    LinearPhaseLayout l;
    l.taps = std::min (kLinearPhaseMaxTaps, roundedPowerOfTwo (sampleRate * kLinearPhaseLengthSeconds[choice]));
    l.hop = std::clamp (roundedPowerOfTwo (sampleRate * kLinearPhaseHopSeconds), kLinearPhaseMinHop, kLinearPhaseMaxHop);
    l.hop = std::max (l.hop, l.taps / kLinearPhaseMaxPartitions);
    l.hop = std::min (l.hop, l.taps / 4);   // c = L/2 - B >= B
    return l;
}

/** The gains and edges of one hop: what the kernel is designed from. */
struct LinearPhaseSnapshot
{
    struct Band
    {
        bool active = false, midSide = false;
        float lo = 0.0f, hi = 0.0f;   // log2 Hz, as the engine uses them
        float gain[2] { 1.0f, 1.0f }; // linear, per lane (L / R, or M / S)
    };
    Band bands[kMaxBands];
    int numChannels = 1;

    bool isIdentity() const noexcept
    {
        for (const auto& b : bands)
            if (b.active && (! isExactlyOne (b.gain[0]) || (numChannels > 1 && ! isExactlyOne (b.gain[1]))))
                return false;
        return true;
    }
    bool sameAs (const LinearPhaseSnapshot& o) const noexcept;
};

/** Fixed-capacity delay of up to kLinearPhaseMaxTaps / 2 samples per lane. */
class LinearPhaseDelay
{
public:
    LinearPhaseDelay();
    void reset() noexcept;
    /** In place: x = x delayed by `delay` samples. */
    void process (int lane, float* x, int n, int delay) noexcept;
    void advance (int n) noexcept { write = (write + n) & mask; }
    void copyFrom (const LinearPhaseDelay& o) noexcept;

private:
    static constexpr int kSize = 32768;   // >= max delay + kChunk, power of two
    static constexpr int mask = kSize - 1;
    std::vector<float> ring[kMaxChannels];
    int write = 0;
};

class LinearPhaseStage
{
public:
    LinearPhaseStage();

    /** Sets the layout and clears everything (bounded work, no allocation): history of silence,
        identity kernel. */
    void prepare (double sampleRate, int lengthChoice, EngineKind model) noexcept;
    void reset() noexcept;
    void setModel (EngineKind model) noexcept { kind = model; }

    const LinearPhaseLayout& layout() const noexcept { return lay; }
    int latency() const noexcept                     { return lay.latency(); }

    /** The snapshot for the block in progress is asked for once per block, at its middle sample:
        `sampleWanted` returns that sample's index within the next process() call (or -1 if it is
        not in it). The caller fills `pendingSnapshot()` from that sample's gains before calling
        process(). */
    int sampleWanted (int n) const noexcept
    {
        const int i = lay.hop / 2 - fill;
        return i >= 0 && i < n ? i : -1;
    }
    LinearPhaseSnapshot& pendingSnapshot() noexcept { return snapshots[(size_t) (blockIndex % kSnapshotRing)]; }

    /** in: the band section's input (nCh lanes, n samples); out: the linear-phase output;
        delayedIn: the input delayed by the same latency (the identity path). Any n. */
    void process (const float* const* in, float* const* out, float* const* delayedIn, int nCh, int n) noexcept;

    void copyFrom (const LinearPhaseStage& o) noexcept;

    /** Kernel redesigns since prepare (tests, benchmark). */
    std::uint64_t designCount() const noexcept { return designs; }

    /** Settled magnitude of the band section at frequency f for a snapshot (the curve this stage
        is designed to apply; tests use it as the reference). Lane `lane` out for an impulse on
        lane `from`. */
    static double modelMatrixEntry (EngineKind kind, const LinearPhaseSnapshot& s, double f, double fs, int lane, int from) noexcept;

private:
    using Complex = std::complex<float>;   // storage only: the hot loops use plain float arithmetic
    static constexpr int kSnapshotRing = 128;      // > the largest snapshot delay (L / B - 1)

    // DiagSame: one kernel for every lane. Diag: left (k[0]) and right (k[3]). MidSide: mid (k[0])
    // and side (k[3]) on the mid / side of the input. Full: the 2x2 matrix k[out * 2 + from].
    enum class Mode { Identity, DiagSame, Diag, MidSide, Full };
    struct Kernel
    {
        Mode mode = Mode::Identity;
        std::vector<Complex> k[4];   // LL, LR, RL, RR: partitions * (hop + 1) bins each
    };

    void runBlock (int nCh) noexcept;
    void design (const LinearPhaseSnapshot& s, Kernel& dst) noexcept;
    /** w[k] *= |H_band(bin k)|^2 on `lane`, for every bin 0 .. L/2. */
    void accumulateSquared (const LinearPhaseSnapshot::Band& b, int lane, double* w) const noexcept;
    void designEntry (const float* magnitude, std::vector<Complex>& dst) noexcept;
    void convolve (const Kernel& kern, int nCh, float* const* dst) noexcept;

    RealFft fftDesign { kLinearPhaseMaxTaps };        // size L
    RealFft fftBlock { 2 * kLinearPhaseMaxHop };      // size 2B
    LinearPhaseLayout lay;
    double fs = 48000.0;
    EngineKind kind = EngineKind::A3;
    int partitions = 0;     // partitions that hold taps (2c / B + 1)
    int ringBlocks = 0;     // frequency-domain delay line length in blocks (>= partitions)

    // Time domain: input block being filled, output block being emitted, input history (2B).
    std::vector<float> inBlock[kMaxChannels], outBlock[kMaxChannels], identityBlock[kMaxChannels];
    std::vector<float> history[kMaxChannels];      // ring of the input, for the identity path
    int historyWrite = 0;
    std::vector<float> overlap[kMaxChannels];      // last 2B input samples (overlap-save frame)
    int fill = 0;
    std::uint64_t blockIndex = 0;

    // Frequency-domain delay line: spectra of the last ringBlocks input frames.
    std::vector<Complex> fdl[kMaxChannels];
    int fdlHead = 0;

    Kernel kernels[2];
    int current = 0;
    LinearPhaseSnapshot snapshots[kSnapshotRing];
    LinearPhaseSnapshot designedFrom;
    bool designedValid = false;

    // Design scratch.
    std::vector<double> binTan;                    // tan (pi k / L), k = 0 .. L/2
    std::vector<float> window;                     // Kaiser, 2c + 1 taps (only the first 2c+1 used)
    std::vector<float> mag[4];                     // per bin magnitude matrix entries
    std::vector<float> scratch;                    // L + 2
    std::vector<float> frame;                      // 2B + 2
    std::vector<Complex> accum[2];                 // B + 1 per output lane
    std::vector<float> blockA[kMaxChannels], blockB[kMaxChannels];
    std::vector<double> work[4];                   // per bin accumulators (squared magnitudes / matrix)
    std::vector<double> scratchMag[2];             // one band's squared magnitude per lane (full matrix)
    bool tablesBuilt = false;

    std::uint64_t designs = 0;
};
} // namespace tl::dsp
