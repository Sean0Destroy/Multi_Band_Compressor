// Starter presets on a drum loop: every band's gain reduction pulses with the hits it is set to
// hear, and comes back up between them. The duck preset is played with a music bed on the main
// input and the kick alone on the sidechain, the way it is meant to be used.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "PluginProcessor.h"
#include "PresetManager.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kBeat = 0.5;             // 120 BPM
constexpr int kBeats = 8;
constexpr int kBlock = 128;
constexpr double kPi = 3.14159265358979323846;

struct Loop
{
    juce::AudioBuffer<float> drums, kick, bed;
};

/** A plain drum loop: kick on every beat, snare on 2 and 4, eighth-note hats, at mixing levels
    (kick peaks near -6 dBFS). The bed is a steady broadband pad for the ducking preset. */
Loop makeLoop()
{
    const int n = (int) (kFs * kBeat * kBeats);
    Loop l;
    for (auto* b : { &l.drums, &l.kick, &l.bed }) { b->setSize (2, n); b->clear(); }

    juce::Random rng (7);
    const auto add = [&] (juce::AudioBuffer<float>& b, int i, float v) { for (int c = 0; c < 2; ++c) b.addSample (c, i, v); };

    for (int beat = 0; beat < kBeats; ++beat)
    {
        const int start = (int) (beat * kBeat * kFs);
        // Kick: pitch sweep 110 -> 50 Hz, 180 ms decay, short click.
        double ph = 0.0;
        for (int i = 0; i < (int) (0.4 * kFs) && start + i < n; ++i)
        {
            const double t = i / kFs;
            const double f = 50.0 + 60.0 * std::exp (-t / 0.03);
            ph += 2.0 * kPi * f / kFs;
            const float v = (float) (0.5 * std::exp (-t / 0.18) * std::sin (ph)
                                     + 0.15 * std::exp (-t / 0.004) * (rng.nextFloat() * 2.0f - 1.0f));
            add (l.drums, start + i, v);
            add (l.kick, start + i, v);
        }
        // Snare on 2 and 4: 190 Hz body and a noise burst.
        if (beat % 2 == 1)
            for (int i = 0; i < (int) (0.25 * kFs) && start + i < n; ++i)
            {
                const double t = i / kFs;
                add (l.drums, start + i, (float) (0.2 * std::exp (-t / 0.06) * std::sin (2.0 * kPi * 190.0 * t)
                                                  + 0.25 * std::exp (-t / 0.05) * (rng.nextFloat() * 2.0f - 1.0f)));
            }
        // Hats on the eighths: high-passed noise, 30 ms.
        for (int h = 0; h < 2; ++h)
        {
            const int hs = start + (int) (h * 0.5 * kBeat * kFs);
            float prev = 0.0f;
            for (int i = 0; i < (int) (0.08 * kFs) && hs + i < n; ++i)
            {
                const float w = rng.nextFloat() * 2.0f - 1.0f;
                add (l.drums, hs + i, (float) (0.12 * std::exp (-(i / kFs) / 0.03)) * (w - prev));
                prev = w;
            }
        }
    }
    // Bed: steady noise about -20 dBFS RMS.
    for (int i = 0; i < n; ++i) add (l.bed, i, 0.17f * (rng.nextFloat() * 2.0f - 1.0f));
    return l;
}

struct Pulse
{
    std::vector<float> deepest, shallowest;    // per beat, dB (0 = no reduction)
};

/** Renders the preset, reading every band's held reduction after each block. */
std::vector<Pulse> run (const juce::String& preset, const Loop& loop, std::vector<int>& slotsOut)
{
    auto proc = std::make_unique<PluginProcessor>();
    PresetManager presets (*proc, juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tl_pulse_none"));
    REQUIRE (presets.loadByName (preset).wasOk());

    const bool duck = preset.contains ("Sidechain");
    OfflineRenderer renderer (*proc);
    REQUIRE (renderer.prepare (kFs, kBlock, { 2, duck ? 2 : 0 }));

    slotsOut = proc->getBandSlots().usedSlots();
    std::vector<Pulse> pulses (slotsOut.size());
    for (auto& p : pulses) { p.deepest.assign (kBeats, 0.0f); p.shallowest.assign (kBeats, -1000.0f); }

    auto& meter = proc->getGainReductionMeter();
    const int samplesPerBeat = (int) (kBeat * kFs);
    renderer.render (duck ? loop.bed : loop.drums, duck ? &loop.kick : nullptr, { kBlock }, false,
                     [&] (int blockIndex, PluginProcessor&)
                     {
                         if (blockIndex == 0) return;
                         const int beat = ((blockIndex - 1) * kBlock) / samplesPerBeat;
                         for (size_t k = 0; k < slotsOut.size(); ++k)
                         {
                             const float gr = std::min (meter.readAndResetHeldGainReductionDb (slotsOut[k], 0),
                                                        meter.readAndResetHeldGainReductionDb (slotsOut[k], 1));
                             if (beat < kBeats)
                             {
                                 pulses[k].deepest[(size_t) beat] = std::min (pulses[k].deepest[(size_t) beat], gr);
                                 pulses[k].shallowest[(size_t) beat] = std::max (pulses[k].shallowest[(size_t) beat], gr);
                             }
                         }
                     });
    return pulses;
}
} // namespace

TEST_CASE ("Starter presets: every band pulses with the loop", "[presets][pulse]")
{
    const auto loop = makeLoop();
    auto proc = std::make_unique<PluginProcessor>();
    PresetManager presets (*proc, juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tl_pulse_none"));

    for (auto& entry : presets.list())
    {
        if (! entry.builtIn) continue;
        std::vector<int> slots;
        const auto pulses = run (entry.name, loop, slots);
        for (size_t k = 0; k < slots.size(); ++k)
        {
            const auto& p = pulses[k];
            INFO (entry.name << ", band " << slots[k]);
            // Beat 0 starts from rest; the loop repeats every two beats from there on.
            float deepest = 0.0f, swing = 0.0f;
            for (int b = 1; b < kBeats; ++b)
            {
                deepest = std::min (deepest, p.deepest[(size_t) b]);
                swing = std::max (swing, p.shallowest[(size_t) b] - p.deepest[(size_t) b]);
                if (b >= 3)   // the same hits give the same pulse (the hats are fresh noise each time)
                    CHECK (std::abs (p.deepest[(size_t) b] - p.deepest[(size_t) b - 2]) < 1.5f);
            }
            CHECK (deepest < -0.4f);   // the band reacts to the loop
            CHECK (swing > 0.4f);      // and comes back up between hits: a pulse, not a steady offset
        }

        if (entry.name.contains ("Sidechain"))
        {
            // The kick on the sidechain ducks the bed by the band's full range on every beat.
            for (int b = 1; b < kBeats; ++b)
            {
                CHECK (pulses[0].deepest[(size_t) b] < -5.5f);
                CHECK (pulses[0].shallowest[(size_t) b] > -3.5f);
            }
        }
    }
}
