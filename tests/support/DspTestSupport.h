#pragma once

#include <complex>
#include <string>
#include <vector>

#include "PluginProcessor.h"
#include "dsp/A3Model.h"
#include "dsp/A3RegionEngine.h"

// Measurement helpers for the DSP tests. Every measured quantity comes from the production
// float32 engine (dsp::A3RegionEngine / PluginProcessor); the float64 models below are only
// references to compare against.
namespace tl::test
{
using cd = std::complex<double>;

double dbOf (double linear);
double magDb (cd h);
double phaseDeg (cd h);
double dbToGain (double db);

// ---- Spectra ------------------------------------------------------------------------------
void fftInPlace (std::vector<cd>& a);   // radix-2, size must be a power of two

/** Frequency response sampled from a (decayed) impulse response: zero-padded FFT, linearly
    interpolated between bins, plus an exact DTFT for single frequencies. */
class MeasuredResponse
{
public:
    MeasuredResponse (const std::vector<float>& impulse, double sampleRate, int padFactor = 8);

    cd at (double f) const;     // interpolated FFT (fast)
    cd exact (double f) const;  // direct DTFT in double (slow, exact)
    double sampleRate() const noexcept { return fs; }

    /** Frequency (log-bisection on at()) between fA and fB where magDb crosses `levelDb`. */
    double crossing (double fA, double fB, double levelDb) const;

private:
    std::vector<float> h;
    std::vector<cd> bins;
    double fs = 48000.0;
};

// ---- Engine driver --------------------------------------------------------------------------
struct RegionSpec
{
    double fLo = 500.0, fHi = 2000.0, gainDb = 0.0;
    int slot = 0;                 // 1-based slot; 0 = position in the list + 1
};

/** Impulse response of the production engine with every listed band used and its effective gain
    held at gainDb via the ExternalGains hook. */
std::vector<float> engineImpulseResponse (double fs, const std::vector<RegionSpec>&, int length);

/** The same for a chosen engine; `settleSamples` of silence are processed first (with the gains
    already held), for engines whose filter depends on the gain history (A-prime). */
std::vector<float> engineImpulseResponse (dsp::EngineKind, double fs, const std::vector<RegionSpec>&, int length, int settleSamples);

/** float64 model: product over bands of 1 + (G - 1) 2R s'/(s'^2 + 2R s' + 1), s' = s/g,
    bilinear (s = j tan(pi f/fs), edges prewarped) or analog (s = j 2 pi f) prototype. */
cd a3Model (double f, double fs, const std::vector<RegionSpec>&, bool analog = false);

/** Digital centre of a band: tan(pi fc/fs) = sqrt(wl wh). */
double digitalCentre (double fLo, double fHi, double fs);

/** |H| of the detector filter (Butterworth HP at fLo then LP at fHi, bilinear). */
double detectorGain (double f, double fLo, double fHi, double fs);

/** Smallest power of two >= n. */
int nextPow2 (int n);

std::vector<double> logGrid (double fMin, double fMax, int points);

// ---- Engine rig (direct engine use with block-wise settings) ---------------------------------
struct EngineRig
{
    dsp::A3RegionEngine engine;
    dsp::EngineSettings settings;
    double fs = 48000.0;

    explicit EngineRig (double sampleRate);
    void use (int slot, double fLo, double fHi);
    dsp::BandSettings& band (int slot) { return settings.bands[(size_t) slot - 1]; }

    /** Processes `channels` in place in blocks of blockSize; settings are applied per block. */
    void run (std::vector<std::vector<float>>& channels, int blockSize = 128);
};

std::vector<float> sine (double f, double fs, int n, double amplitude, double phase = 0.0);

// ---- Processor helpers ------------------------------------------------------------------------
void setPlain (PluginProcessor&, const std::string& id, float plain);
void setBand (PluginProcessor&, int slot, const std::string& suffix, float plain);

// ---- Measurement export (TL_MEASURE_DIR) -------------------------------------------------------
namespace measure
{
    bool enabled();
    /** Writes <TL_MEASURE_DIR>/<name> (CSV with a header line). No-op when disabled. */
    void writeCsv (const std::string& name, const std::vector<std::string>& header,
                   const std::vector<std::vector<double>>& rows);
    void writeText (const std::string& name, const std::string& text);
}
} // namespace tl::test
