// Hidden measurements for the repair phase R1-R3 (not run by ctest; they print, they do not judge).
//   tl_tests "[.repair-measure]"
// The same file is run on the code before and after each repair; the numbers go into the
// repair report. The pass/fail versions of these scenarios are in RepairTests.cpp.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "dsp/A3RegionEngine.h"
#include "dsp/APrimeRegionEngine.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Kind = dsp::EngineKind;

namespace
{
constexpr double kFs = 48000.0;

/** Drives the processor block by block with any block size (also larger than announced). */
struct Driver
{
    PluginProcessor& p;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, const std::vector<int>& blocks,
                                     const std::function<void (int, PluginProcessor&)>& hook = {})
    {
        juce::AudioBuffer<float> out (in.getNumChannels(), in.getNumSamples());
        for (int pos = 0, b = 0; pos < in.getNumSamples(); ++b)
        {
            const int n = std::min (blocks[(size_t) b % blocks.size()], in.getNumSamples() - pos);
            if (hook) hook (b, p);
            juce::AudioBuffer<float> chunk (in.getNumChannels(), n);
            for (int c = 0; c < in.getNumChannels(); ++c) chunk.copyFrom (c, 0, in, c, pos, n);
            p.processBlock (chunk, midi);
            for (int c = 0; c < in.getNumChannels(); ++c) out.copyFrom (c, pos, chunk, c, 0, n);
            pos += n;
        }
        return out;
    }
};

juce::AudioBuffer<float> tone (double f, int n, double amplitude)
{
    juce::AudioBuffer<float> b (1, n);
    const auto s = sine (f, kFs, n, amplitude);
    b.copyFrom (0, 0, s.data(), n);
    return b;
}

double levelDb (const juce::AudioBuffer<float>& b, int start, int length)
{
    double acc = 0.0;
    for (int i = start; i < start + length; ++i) acc += (double) b.getSample (0, i) * b.getSample (0, i);
    return 10.0 * std::log10 (acc / (double) length + 1.0e-30);
}
}

TEST_CASE ("Repair measurement R2: level after an engine switch", "[.repair-measure]")
{
    struct Config { const char* name; float attack, release; int detMode; float rmsTime; bool autoGain; float adaptive; float lookahead; };
    const Config configs[] = {
        { "attack 10 ms (default)", 10.0f, 150.0f, 0, 30.0f, false, 0.0f, 0.0f },
        { "attack 30 ms", 30.0f, 150.0f, 0, 30.0f, false, 0.0f, 0.0f },
        { "attack 100 ms", 100.0f, 150.0f, 0, 30.0f, false, 0.0f, 0.0f },
        { "attack 300 ms", 300.0f, 150.0f, 0, 30.0f, false, 0.0f, 0.0f },
        { "attack 500 ms, release 5 s", 500.0f, 5000.0f, 0, 30.0f, false, 0.0f, 0.0f },
        { "RMS 100 ms", 10.0f, 150.0f, 1, 100.0f, false, 0.0f, 0.0f },
        { "RMS 300 ms", 10.0f, 150.0f, 1, 300.0f, false, 0.0f, 0.0f },
        { "auto gain on", 10.0f, 150.0f, 0, 30.0f, true, 0.0f, 0.0f },
        { "adaptive release 100 %", 10.0f, 150.0f, 0, 30.0f, false, 100.0f, 0.0f },
        { "attack 300 ms, lookahead 10 ms, auto gain", 300.0f, 150.0f, 0, 30.0f, true, 0.0f, 10.0f },
    };

    const int n = 8 * 48000, block = 128, switchAt = 5 * 48000;
    const auto in = tone (1000.0, n, 0.5);
    std::printf ("\nR2: band 500-2000 Hz, -30 dB, 8:1, 1 kHz tone 0.5; A3 -> A-prime at 5 s; level in 10 ms windows relative to the level before the click (dB)\n");
    std::printf ("%-44s %9s %9s %9s %9s %12s %10s\n", "setting", "+60 ms", "+125 ms", "+300 ms", "+1 s", "worst 0-2 s", "max step");

    for (const auto& c : configs)
    {
        PluginProcessor p;
        OfflineRenderer renderer (p);
        p.getBandSlots().createBand();
        setBand (p, 1, "fLo", 500.0f); setBand (p, 1, "fHi", 2000.0f);
        setBand (p, 1, "thresh", -30.0f); setBand (p, 1, "ratio", 8.0f);
        setBand (p, 1, "attack", c.attack); setBand (p, 1, "release", c.release);
        setBand (p, 1, "detMode", (float) c.detMode); setBand (p, 1, "rmsTime", c.rmsTime);
        setBand (p, 1, "adaptRel", c.adaptive); setBand (p, 1, "lookahead", c.lookahead);
        setPlain (p, "autoGain", c.autoGain ? 1.0f : 0.0f);
        REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));
        const auto out = renderer.render (in, nullptr, { block }, false, [&] (int b, PluginProcessor& q)
        {
            if (b == switchAt / block) q.setEngineKind (Kind::APrime);
        });

        const double before = levelDb (out, switchAt - 4800, 4800);
        const auto at = [&] (double seconds) { return levelDb (out, switchAt + (int) (seconds * kFs), 480) - before; };
        double worst = 0.0;
        for (int i = 0; i + 480 <= 2 * 48000; i += 240)
        {
            const double d = levelDb (out, switchAt + i, 480) - before;
            if (std::abs (d) > std::abs (worst)) worst = d;
        }
        float step = 0.0f;
        for (int i = switchAt; i < switchAt + 2 * 48000; ++i)
            step = std::max (step, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
        float ownStep = 0.0f;
        for (int i = switchAt - 4800; i < switchAt; ++i)
            ownStep = std::max (ownStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));

        std::printf ("%-44s %+9.2f %+9.2f %+9.2f %+9.2f %+12.2f %9.2fx\n", c.name, at (0.060), at (0.125), at (0.300), at (1.0), worst, (double) (step / ownStep));
    }
}

TEST_CASE ("Repair measurement R3: a host block larger than announced during an engine switch", "[.repair-measure]")
{
    std::printf ("\nR3: prepareToPlay (48000, 64); engine switched; one 512-sample block arrives k blocks after the request\n");
    std::printf ("%-52s %6s %14s %12s %12s\n", "scenario", "k", "max deviation", "max step", "own step");

    struct Scenario { const char* name; bool compress; double f; };
    for (const Scenario s : { Scenario { "neutral band, lookahead 10 ms, 1 kHz 0.5", false, 1000.0 },
                              Scenario { "band 200-2000 Hz 8:1, 300 Hz 0.5, no lookahead", true, 300.0 } })
        for (const int k : { 0, 1, 4, 20 })
        {
            PluginProcessor p;
            OfflineRenderer renderer (p);
            p.getBandSlots().createBand();
            if (s.compress)
            {
                setBand (p, 1, "fLo", 200.0f); setBand (p, 1, "fHi", 2000.0f);
                setBand (p, 1, "thresh", -30.0f); setBand (p, 1, "ratio", 8.0f);
            }
            else
            {
                setBand (p, 1, "thresh", 12.0f); setBand (p, 1, "lookahead", 10.0f);
            }
            REQUIRE (renderer.prepare (kFs, 64, { 1, 0 }));
            const int latency = p.getLatencySamples();

            const int n = 96000, switchBlock = 600;
            const auto in = tone (s.f, n, 0.5);
            std::vector<int> blocks ((size_t) (switchBlock + k), 64);
            blocks.push_back (512);
            for (int i = 0; i < 4000; ++i) blocks.push_back (64);

            Driver driver { p, {} };
            const auto out = driver.render (in, blocks, [&] (int b, PluginProcessor& q)
            {
                if (b == switchBlock) q.setEngineKind (Kind::APrime);
            });

            const int from = switchBlock * 64, to = from + 24000;
            float deviation = 0.0f, step = 0.0f, own = 0.0f;
            for (int i = from; i < to; ++i)
            {
                if (! s.compress) deviation = std::max (deviation, std::abs (out.getSample (0, i) - in.getSample (0, i - latency)));
                step = std::max (step, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
            }
            for (int i = from - 9600; i < from; ++i)
                own = std::max (own, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
            std::printf ("%-52s %6d %14.4f %12.4f %12.4f\n", s.name, k, (double) deviation, (double) step, (double) own);
        }
}

TEST_CASE ("Repair measurement R2: cost of handing the state over", "[.repair-measure]")
{
    dsp::A3RegionEngine a3;
    dsp::APrimeRegionEngine aPrime;
    dsp::EngineSettings s;
    s.usedMask = 0xffu;
    for (double fs : { 48000.0, 192000.0 })
    {
        a3.setSettings (s);     a3.prepare (fs, 512);
        aPrime.setSettings (s); aPrime.prepare (fs, 512);
        std::vector<float> l (512, 0.1f), r (512, 0.1f);
        float* channels[2] { l.data(), r.data() };
        a3.process (channels, 2, 512);

        double total = 0.0, worst = 0.0;
        const int runs = 200;
        for (int i = 0; i < runs; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = (i & 1) == 0 ? aPrime.continueFrom (a3) : a3.continueFrom (aPrime);
            const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
            REQUIRE (ok);
            total += us;
            worst = std::max (worst, us);
        }
        std::printf ("R2: continueFrom at %.0f Hz: mean %.1f us, worst %.1f us over %d runs; engine objects %zu and %zu bytes\n",
                     fs, total / runs, worst, runs, sizeof (dsp::A3RegionEngine), sizeof (dsp::APrimeRegionEngine));
    }
}

// A host that takes a lock of its own both while it saves the plugin state on one thread and
// inside the notifications it receives from the plugin.
namespace
{
struct LockingHost final : juce::AudioProcessorListener
{
    std::mutex hostLock;
    std::atomic<int> notifications { 0 };

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override   { enter(); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override  { enter(); }
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { enter(); }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override   { enter(); }

    void enter()
    {
        const std::lock_guard<std::mutex> lock (hostLock);
        notifications.fetch_add (1, std::memory_order_relaxed);
    }
};
}

TEST_CASE ("Repair measurement R1: a host that saves under its own lock while the plugin edits", "[.repair-measure]")
{
    // Leaked on purpose: if the two threads deadlock they can never be joined.
    auto* p = new PluginProcessor();
    auto* host = new LockingHost();
    p->addListener (host);
    juce::MemoryBlock stateA, stateB;
    p->getStateInformation (stateA);
    for (int i = 0; i < 4; ++i) p->getBandSlots().createBand();
    setBand (*p, 1, "thresh", -30.0f);
    p->getStateInformation (stateB);

    auto* saves = new std::atomic<int> (0);
    auto* edits = new std::atomic<int> (0);
    auto* stop = new std::atomic<bool> (false);

    std::thread saver ([=]
    {
        while (! stop->load())
        {
            const std::lock_guard<std::mutex> lock (host->hostLock);      // the host's own lock ...
            juce::MemoryBlock block;
            p->getStateInformation (block);                               // ... held while it saves
            saves->fetch_add (1);
        }
    });
    std::thread editor ([=]
    {
        for (int i = 0; ! stop->load(); ++i)
        {
            switch (i % 4)
            {
                case 0: p->setStateInformation (stateB.getData(), (int) stateB.getSize()); break;
                case 1: p->getBandSlots().deleteBand (1); break;
                case 2: p->getBandSlots().undoLast(); break;
                default: p->setStateInformation (stateA.getData(), (int) stateA.getSize()); break;
            }
            edits->fetch_add (1);
        }
    });

    const auto start = std::chrono::steady_clock::now();
    int lastSaves = -1, lastEdits = -1, stalled = 0;
    bool deadlock = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds (4))
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        const int s = saves->load(), e = edits->load();
        stalled = (s == lastSaves && e == lastEdits) ? stalled + 1 : 0;
        lastSaves = s; lastEdits = e;
        if (stalled >= 10) { deadlock = true; break; }
    }

    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
    std::printf ("\nR1: host saves under its own lock on one thread while another loads states and edits bands\n");
    std::printf ("    saves %d, edits %d, host notifications %d, %s after %.0f ms\n", saves->load(), edits->load(),
                 host->notifications.load(), deadlock ? "DEADLOCK (no progress for 1 s)" : "no deadlock", ms);
    std::fflush (stdout);

    if (deadlock)
        std::_Exit (0);            // the threads cannot be joined; this is a measurement, not a failure

    stop->store (true);
    saver.join();
    editor.join();
    p->removeListener (host);
    delete p; delete host; delete saves; delete edits; delete stop;
}
