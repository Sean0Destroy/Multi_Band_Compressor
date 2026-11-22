// Repair phase R1-R3: host notifications and the state mutex, engine changeover, host blocks
// larger than announced. The numbers these scenarios produced before and after the repairs are
// printed by RepairMeasurements.cpp.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dsp/A3RegionEngine.h"
#include "dsp/APrimeRegionEngine.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;
using Kind = dsp::EngineKind;

namespace
{
constexpr double kFs = 48000.0;

juce::MemoryBlock stateOf (PluginProcessor& p)
{
    juce::MemoryBlock b;
    p.getStateInformation (b);
    return b;
}

void load (PluginProcessor& p, const juce::MemoryBlock& b) { p.setStateInformation (b.getData(), (int) b.getSize()); }
bool same (const juce::MemoryBlock& a, const juce::MemoryBlock& b) { return a == b; }

/** A state that is a fixed point of load -> save (see PresetTests: one load settles it). */
juce::MemoryBlock settledStateOf (PluginProcessor& p)
{
    load (p, stateOf (p));
    return stateOf (p);
}

// ---- A host that records what it is told, and checks the rule on every call --------------------
struct RecordingHost final : juce::AudioProcessorListener
{
    enum class What { Value, GestureBegin, GestureEnd, Changed };
    struct Event { What what; int index; float value; };

    explicit RecordingHost (PluginProcessor& processor) : p (processor) { p.addListener (this); }
    ~RecordingHost() override { p.removeListener (this); }

    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override   { record ({ What::Value, index, value }); }
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override     { record ({ What::GestureBegin, index, 0.0f }); }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override       { record ({ What::GestureEnd, index, 0.0f }); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override              { record ({ What::Changed, -1, 0.0f }); }

    void record (Event e)
    {
        if (p.getStateGuard().isHeldByCurrentThread())
            ++callsUnderTheMutex;                    // the rule: never
        if (onEvent) onEvent (e);
        events.push_back (e);
    }

    int count (What what) const
    {
        int n = 0;
        for (auto& e : events) n += e.what == what ? 1 : 0;
        return n;
    }

    PluginProcessor& p;
    std::vector<Event> events;
    int callsUnderTheMutex = 0;
    std::function<void (const Event&)> onEvent;
};

int indexOf (PluginProcessor& p, const std::string& id) { return p.getState().getParameter (id)->getParameterIndex(); }
float normalised (PluginProcessor& p, const std::string& id) { return p.getState().getParameter (id)->getValue(); }

// ---- Threads that may deadlock -------------------------------------------------------------------
/** Runs the workers for `seconds`. If none of them makes progress for one second the process is
    ended (deadlocked threads cannot be joined, and the objects they use cannot be destroyed). */
void runOrDie (const char* what, double seconds, std::vector<std::function<void (std::atomic<bool>&, std::atomic<long>&)>> workers)
{
    auto stop = std::make_shared<std::atomic<bool>> (false);
    auto progress = std::make_shared<std::vector<std::atomic<long>>> (workers.size());
    auto finished = std::make_shared<std::atomic<int>> (0);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < workers.size(); ++i)
        threads.emplace_back ([stop, progress, finished, i, worker = workers[i]]
        {
            worker (*stop, (*progress)[i]);
            finished->fetch_add (1);
        });

    const auto die = [what] (const char* when)
    {
        std::printf ("\nDEADLOCK: %s - %s\n", what, when);
        std::fflush (stdout);
        std::_Exit (3);
    };

    const auto start = std::chrono::steady_clock::now();
    long last = -1;
    int stalled = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::duration<double> (seconds))
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        long sum = 0;
        for (auto& c : *progress) sum += c.load();
        stalled = sum == last ? stalled + 1 : 0;
        last = sum;
        if (stalled >= 20)
            die ("no thread made progress for one second");
    }

    // A deadlock in the last moments would otherwise hang the join below for ever.
    stop->store (true);
    const auto stopped = std::chrono::steady_clock::now();
    while (finished->load() < (int) threads.size())
    {
        if (std::chrono::steady_clock::now() - stopped > std::chrono::seconds (5))
            die ("the threads did not finish within five seconds of being told to stop");
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    for (auto& t : threads) t.join();
    for (auto& c : *progress) REQUIRE (c.load() > 0);          // every worker did something
}

// ---- Audio helpers ---------------------------------------------------------------------------------
juce::AudioBuffer<float> tone (double f, int n, double amplitude, int channels = 1)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = sine (f, kFs, n, amplitude);
    for (int c = 0; c < channels; ++c) b.copyFrom (c, 0, s.data(), n);
    return b;
}

double levelDb (const juce::AudioBuffer<float>& b, int start, int length)
{
    double acc = 0.0;
    for (int i = start; i < start + length; ++i) acc += (double) b.getSample (0, i) * b.getSample (0, i);
    return 10.0 * std::log10 (acc / (double) length + 1.0e-30);
}

float largestStep (const juce::AudioBuffer<float>& b, int start, int end)
{
    float m = 0.0f;
    for (int i = std::max (1, start); i < end; ++i)
        m = std::max (m, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
    return m;
}

/** Drives the processor block by block with any block size (also larger than announced). */
juce::AudioBuffer<float> renderBlocks (PluginProcessor& p, const juce::AudioBuffer<float>& in, const std::vector<int>& blocks,
                                       const std::function<void (int, PluginProcessor&)>& hook = {})
{
    juce::AudioBuffer<float> out (in.getNumChannels(), in.getNumSamples());
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> chunk (in.getNumChannels(), 8192);
    for (int pos = 0, b = 0; pos < in.getNumSamples(); ++b)
    {
        const int n = std::min (blocks[(size_t) b % blocks.size()], in.getNumSamples() - pos);
        if (hook) hook (b, p);
        for (int c = 0; c < in.getNumChannels(); ++c) chunk.copyFrom (c, 0, in, c, pos, n);
        {
            juce::AudioBuffer<float> view (chunk.getArrayOfWritePointers(), chunk.getNumChannels(), n);
            AudioCallbackScope scope;                                   // allocations in here are counted
            p.processBlock (view, midi);
        }
        for (int c = 0; c < in.getNumChannels(); ++c) out.copyFrom (c, pos, chunk, c, 0, n);
        pos += n;
    }
    return out;
}
}

// ======================================================================================================
// R1: the host is told after the state mutex is released
// ======================================================================================================

TEST_CASE ("R1: the host is never called while the state mutex is held", "[state][host][repair]")
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("tl_repair_test_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    {
        PluginProcessor p;
        PresetManager presets (p, dir);
        setPlain (p, "b2_thresh", -40.0f);                      // so that creating band 2 has parameters to reset
        setPlain (p, "b2_lookahead", 5.0f);
        const auto empty = settledStateOf (p);

        RecordingHost host (p);
        auto& slots = p.getBandSlots();

        REQUIRE (slots.createBand() == 1);
        REQUIRE (slots.createBand() == 2);
        setBand (p, 1, "scMode", (float) schema::scModeBand);
        setBand (p, 1, "scRef", 2.0f);
        REQUIRE (slots.deleteBand (2));                         // converts the follower: several parameters
        REQUIRE (slots.undoLast());
        REQUIRE (slots.redoLast());
        REQUIRE (p.getUndoManager().undo());                    // the undo manager used directly
        const auto twoBands = stateOf (p);

        load (p, empty);                                        // many parameters change
        load (p, empty);                                        // nothing changes
        load (p, twoBands);
        REQUIRE (presets.loadByName ("Starter - Drum Bus 4 Band").wasOk());
        REQUIRE (presets.saveUser ("Mine").wasOk());
        presets.switchTo (1);
        presets.copyActiveToOther();
        presets.switchTo (0);
        presets.loadEmpty();
        p.setEngineKind (Kind::APrime);
        p.setEngineKind (Kind::A3);

        CHECK (host.count (RecordingHost::What::Value) > 50);
        CHECK (host.count (RecordingHost::What::GestureBegin) > 10);
        CHECK (host.count (RecordingHost::What::GestureBegin) == host.count (RecordingHost::What::GestureEnd));
        CHECK (host.count (RecordingHost::What::Changed) > 8);
        CHECK (host.callsUnderTheMutex == 0);
        CHECK_FALSE (p.getStateGuard().hasPending());           // everything was delivered
    }
    dir.deleteRecursively();
}

TEST_CASE ("R1: what the host is told about a band edit and a state load", "[state][host][repair]")
{
    PluginProcessor p;
    auto& slots = p.getBandSlots();
    const int thresh = indexOf (p, "b1_thresh"), ratio = indexOf (p, "b1_ratio");

    SECTION ("a band edit: each parameter as begin / value / end, and only once the whole edit is in the state")
    {
        setPlain (p, "b1_thresh", -40.0f);
        setPlain (p, "b1_ratio", 8.0f);
        RecordingHost host (p);
        int partialViews = 0;
        host.onEvent = [&] (const RecordingHost::Event&)
        {
            // Whenever the host hears anything, the band is already used and both parameters
            // are back at their defaults: it can never save half an edit.
            const bool whole = slots.isUsed (1)
                               && std::abs (p.getState().getRawParameterValue ("b1_thresh")->load() + 18.0f) < 1.0e-3f
                               && std::abs (p.getState().getRawParameterValue ("b1_ratio")->load() - 2.0f) < 1.0e-3f;
            partialViews += whole ? 0 : 1;
        };

        REQUIRE (slots.createBand() == 1);
        CHECK (partialViews == 0);
        CHECK (host.callsUnderTheMutex == 0);

        // The parameters that changed (these two, and any that sat an ulp away from their
        // default), each as a gesture carrying the value it has.
        for (const int index : { thresh, ratio })
        {
            int at = -1;
            for (size_t i = 0; i < host.events.size(); ++i)
                if (host.events[i].index == index && host.events[i].what == RecordingHost::What::GestureBegin) at = (int) i;
            REQUIRE (at >= 0);
            REQUIRE ((size_t) at + 2 < host.events.size());
            CHECK (host.events[(size_t) at + 1].what == RecordingHost::What::Value);
            CHECK (host.events[(size_t) at + 1].index == index);
            CHECK (juce::exactlyEqual (host.events[(size_t) at + 1].value, p.getParameters()[index]->getValue()));
            CHECK (host.events[(size_t) at + 2].what == RecordingHost::What::GestureEnd);
            CHECK (host.events[(size_t) at + 2].index == index);
        }
        CHECK (host.count (RecordingHost::What::Value) >= 2);
        CHECK (host.count (RecordingHost::What::Value) == host.count (RecordingHost::What::GestureBegin));
        CHECK (host.count (RecordingHost::What::Value) == host.count (RecordingHost::What::GestureEnd));
        for (size_t i = 0; i < host.events.size(); ++i)
            if (host.events[i].what == RecordingHost::What::Value)
            {
                REQUIRE (i > 0);
                REQUIRE (i + 1 < host.events.size());
                CHECK (host.events[i - 1].what == RecordingHost::What::GestureBegin);
                CHECK (host.events[i + 1].what == RecordingHost::What::GestureEnd);
                CHECK (juce::exactlyEqual (host.events[i].value, p.getParameters()[host.events[i].index]->getValue()));
            }
        CHECK (host.count (RecordingHost::What::Changed) >= 1);        // the band set is not a parameter
    }

    SECTION ("a state load: one value notification per parameter that really changed, none for a reload")
    {
        slots.createBand();
        const auto saved = settledStateOf (p);
        setPlain (p, "b1_thresh", -33.0f);
        setPlain (p, "b1_ratio", 5.0f);
        setPlain (p, "outGain", 3.0f);

        RecordingHost host (p);
        load (p, saved);
        CHECK (host.count (RecordingHost::What::Value) == 3);
        CHECK (host.count (RecordingHost::What::GestureBegin) == 0);
        for (auto& e : host.events)
            if (e.what == RecordingHost::What::Value)
                CHECK (juce::exactlyEqual (e.value, p.getParameters()[e.index]->getValue()));
        CHECK (juce::exactlyEqual (normalised (p, "b1_thresh"), p.getState().getParameter ("b1_thresh")->convertTo0to1 (-18.0f)));
        CHECK (same (stateOf (p), saved) == true);

        host.events.clear();
        load (p, saved);                                               // the state it already holds
        CHECK (host.count (RecordingHost::What::Value) == 0);
        CHECK (host.callsUnderTheMutex == 0);
    }

    SECTION ("latency follows a band edit, without a host call under the mutex")
    {
        slots.createBand();
        setBand (p, 1, "lookahead", 5.0f);
        OfflineRenderer renderer (p);
        REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));
        REQUIRE (p.getLatencySamples() == 240);

        RecordingHost host (p);
        REQUIRE (slots.deleteBand (1));
        CHECK (p.getLatencySamples() == 0);                            // told when deleteBand() returns
        REQUIRE (slots.undoLast());
        CHECK (p.getLatencySamples() == 240);
        CHECK (host.callsUnderTheMutex == 0);
    }
}

TEST_CASE ("R1: a host may call back into the plugin from inside a notification", "[state][host][repair]")
{
    PluginProcessor p;
    p.getBandSlots().createBand();
    setBand (p, 1, "thresh", -30.0f);
    const auto one = settledStateOf (p);
    p.getBandSlots().createBand();
    setBand (p, 2, "ratio", 6.0f);
    const auto two = settledStateOf (p);
    load (p, one);

    RecordingHost host (p);
    int savesInside = 0;
    bool loaded = false, created = false;
    juce::MemoryBlock savedInside;
    host.onEvent = [&] (const RecordingHost::Event& e)
    {
        // Saves from inside a notification: allowed, and it sees a whole state.
        savedInside = stateOf (p);
        ++savesInside;

        // Loads and edits from inside a notification, once each.
        if (e.what == RecordingHost::What::Value && ! loaded)   { loaded = true; load (p, one); }
        if (e.what == RecordingHost::What::Changed && ! created) { created = true; p.getBandSlots().createBand(); }
    };

    load (p, two);                                                     // the notification storm starts here

    CHECK (savesInside > 0);
    CHECK (loaded);
    CHECK (host.callsUnderTheMutex == 0);
    CHECK_FALSE (p.getStateGuard().hasPending());

    // Whatever happened inside, the host's picture is right at the end: every parameter it was
    // told about has the last value it was told.
    std::vector<float> last ((size_t) p.getParameters().size(), -1.0f);
    for (auto& e : host.events)
        if (e.what == RecordingHost::What::Value) last[(size_t) e.index] = e.value;
    for (int i = 0; i < p.getParameters().size(); ++i)
        if (last[(size_t) i] >= 0.0f)
            CHECK (juce::exactlyEqual (last[(size_t) i], p.getParameters()[i]->getValue()));

    // The state is one that can be saved and loaded back unchanged.
    const auto end = stateOf (p);
    PluginProcessor q;
    load (q, end);
    CHECK (same (stateOf (q), end));
}

// ---- Threads ---------------------------------------------------------------------------------------

namespace
{
/** A host with a lock of its own, taken inside every notification it receives. */
struct LockingHost final : juce::AudioProcessorListener
{
    std::mutex lock;
    std::atomic<int> notifications { 0 };
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override     { enter(); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override    { enter(); }
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { enter(); }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override   { enter(); }
    void enter() { const std::lock_guard<std::mutex> hold (lock); notifications.fetch_add (1); }
};

/** Everything the threads touch, kept alive by them (a deadlocked thread must not see freed memory). */
struct Shared
{
    PluginProcessor p;
    LockingHost host;
    juce::MemoryBlock a, b;

    Shared()
    {
        p.getBandSlots().createBand();
        setBand (p, 1, "thresh", -30.0f);
        a = settledStateOf (p);
        for (int i = 0; i < 3; ++i) p.getBandSlots().createBand();
        setBand (p, 3, "ratio", 6.0f);
        setPlain (p, "outGain", -4.0f);
        b = settledStateOf (p);
        p.addListener (&host);
    }
    ~Shared() { p.removeListener (&host); }
};
}

TEST_CASE ("R1: a host that saves under its own lock cannot deadlock with loads and band edits", "[state][host][repair][concurrency]")
{
    auto s = std::make_shared<Shared>();

    runOrDie ("save under the host's lock against loads and band edits", 2.0, {
        [s] (std::atomic<bool>& stop, std::atomic<long>& progress)
        {
            while (! stop.load())
            {
                {
                    const std::lock_guard<std::mutex> hold (s->host.lock);      // the host's own lock ...
                    juce::MemoryBlock block;
                    s->p.getStateInformation (block);                           // ... held while it saves
                }
                progress.fetch_add (1);
                std::this_thread::yield();
            }
        },
        [s] (std::atomic<bool>& stop, std::atomic<long>& progress)
        {
            for (int i = 0; ! stop.load(); ++i)
            {
                switch (i % 6)
                {
                    case 0:  load (s->p, s->b); break;
                    case 1:  s->p.getBandSlots().deleteBand (1); break;
                    case 2:  s->p.getBandSlots().undoLast(); break;
                    case 3:  load (s->p, s->a); break;
                    case 4:  s->p.getBandSlots().createBand(); break;
                    default: s->p.setEngineKind (i % 12 == 5 ? Kind::APrime : Kind::A3); break;
                }
                progress.fetch_add (1);
            }
        } });

    CHECK (s->host.notifications.load() > 0);
    CHECK_FALSE (s->p.getStateGuard().hasPending());
}

TEST_CASE ("R1: a state saved while another thread loads is always a whole state", "[state][host][repair][concurrency]")
{
    auto s = std::make_shared<Shared>();
    auto torn = std::make_shared<std::atomic<int>> (0);
    auto sawA = std::make_shared<std::atomic<int>> (0);
    auto sawB = std::make_shared<std::atomic<int>> (0);

    runOrDie ("save against load", 2.0, {
        [s, torn, sawA, sawB] (std::atomic<bool>& stop, std::atomic<long>& progress)
        {
            while (! stop.load())
            {
                juce::MemoryBlock block;
                s->p.getStateInformation (block);
                if (block == s->a)      sawA->fetch_add (1);
                else if (block == s->b) sawB->fetch_add (1);
                else                    torn->fetch_add (1);
                progress.fetch_add (1);
            }
        },
        [s] (std::atomic<bool>& stop, std::atomic<long>& progress)
        {
            for (int i = 0; ! stop.load(); ++i)
            {
                load (s->p, (i & 1) != 0 ? s->a : s->b);
                progress.fetch_add (1);
            }
        } });

    CHECK (torn->load() == 0);
    CHECK (sawA->load() + sawB->load() > 100);
}

TEST_CASE ("R1: a state saved while bands are created and removed is never half an edit", "[state][host][repair][concurrency]")
{
    // Creating band 1 resets its parameters and marks the slot used; undo puts both back. A
    // save must see one or the other, never the new band set with the old parameter values
    // (which is what it would get if a commit left the saved values to the later notification).
    struct Edits
    {
        PluginProcessor p;
        juce::MemoryBlock unused, used;
        Edits()
        {
            setPlain (p, "b1_thresh", -40.0f);
            setPlain (p, "b1_ratio", 8.0f);
            setPlain (p, "b1_makeup", 6.0f);
            load (p, stateOf (p));
            // One round first: afterwards create / undo alternate between two fixed states.
            p.getBandSlots().createBand();
            p.getBandSlots().undoLast();
            unused = stateOf (p);
            p.getBandSlots().createBand();
            used = stateOf (p);
            p.getBandSlots().undoLast();
        }
    };
    auto s = std::make_shared<Edits>();
    REQUIRE (stateOf (s->p) == s->unused);
    REQUIRE (s->used != s->unused);

    auto torn = std::make_shared<std::atomic<int>> (0);
    auto sawUsed = std::make_shared<std::atomic<int>> (0);
    auto sawUnused = std::make_shared<std::atomic<int>> (0);

    runOrDie ("save against band edits", 2.0, {
        [s, torn, sawUsed, sawUnused] (std::atomic<bool>& stop, std::atomic<long>& progress)
        {
            while (! stop.load())
            {
                juce::MemoryBlock block;
                s->p.getStateInformation (block);
                if (block == s->unused)    sawUnused->fetch_add (1);
                else if (block == s->used) sawUsed->fetch_add (1);
                else                       torn->fetch_add (1);
                progress.fetch_add (1);
            }
        },
        [s] (std::atomic<bool>& stop, std::atomic<long>& progress)
        {
            while (! stop.load())
            {
                s->p.getBandSlots().createBand();
                s->p.getBandSlots().undoLast();
                progress.fetch_add (1);
            }
        } });

    CHECK (torn->load() == 0);
    CHECK (sawUsed->load() > 0);
    CHECK (sawUnused->load() > 0);
}

TEST_CASE ("R1: after concurrent loads the host's last word on every parameter is its value", "[state][host][repair][concurrency]")
{
    struct LastValues final : juce::AudioProcessorListener
    {
        std::vector<std::atomic<float>> last;
        std::atomic<int> inside { 0 }, overlaps { 0 };
        explicit LastValues (int n) : last ((size_t) n) { for (auto& v : last) v.store (-1.0f); }
        void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override
        {
            // Notifications are delivered by one thread at a time.
            if (inside.fetch_add (1) != 0) overlaps.fetch_add (1);
            last[(size_t) index].store (value);
            inside.fetch_sub (1);
        }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    };

    auto s = std::make_shared<Shared>();
    auto host = std::make_shared<LastValues> (s->p.getParameters().size());
    s->p.addListener (host.get());

    const auto loader = [s] (const juce::MemoryBlock* state)
    {
        return [s, state] (std::atomic<bool>& stop, std::atomic<long>& progress)
        {
            while (! stop.load())
            {
                load (s->p, *state);
                progress.fetch_add (1);
            }
        };
    };
    runOrDie ("two threads loading different states", 2.0, { loader (&s->a), loader (&s->b) });

    CHECK (host->overlaps.load() == 0);
    CHECK_FALSE (s->p.getStateGuard().hasPending());
    int told = 0;
    for (int i = 0; i < s->p.getParameters().size(); ++i)
    {
        const float v = host->last[(size_t) i].load();
        if (v < 0.0f) continue;
        ++told;
        CHECK (juce::exactlyEqual (v, s->p.getParameters()[i]->getValue()));
    }
    CHECK (told > 0);

    // And the state is one of the two, whole.
    const auto end = stateOf (s->p);
    CHECK ((end == s->a || end == s->b));
    s->p.removeListener (host.get());
}

// ======================================================================================================
// R2: engine changeover
// ======================================================================================================

namespace
{
dsp::EngineSettings busySettings()
{
    dsp::EngineSettings s;
    s.usedMask = 0b111u;
    auto& a = s.bands[0];
    a.fLoHz = 500.0f; a.fHiHz = 2000.0f; a.thresholdDb = -34.0f; a.ratio = 8.0f;
    a.attackMs = 300.0f; a.releaseMs = 1500.0f; a.adaptiveReleasePercent = 60.0f;
    auto& b = s.bands[1];
    b.fLoHz = 60.0f; b.fHiHz = 220.0f; b.thresholdDb = -40.0f; b.ratio = 4.0f;
    b.detectorMode = dsp::DetectorMode::Rms; b.rmsTimeMs = 300.0f; b.lookaheadMs = 5.0f;
    b.stereoMode = dsp::StereoMode::MidSide; b.makeupDb = 6.0f; b.transientPercent = 50.0f;
    auto& c = s.bands[2];
    c.fLoHz = 3000.0f; c.fHiHz = 9000.0f; c.thresholdDb = -38.0f; c.ratio = 20.0f;
    c.detectorMode = dsp::DetectorMode::Blend; c.linkPercent = 0.0f; c.mixPercent = 70.0f;
    c.freqReleasePercent = 100.0f; c.lookaheadMs = 2.0f;
    s.inputGainDb = 3.0f; s.outputGainDb = -2.0f; s.mixPercent = 90.0f;
    s.autoGain = true; s.clipMode = 1; s.clipCeilingDb = -3.0f; s.constantLatency = true;
    s.characterModel = 1; s.characterDrivePercent = 50.0f; s.oversamplingStages = 2;
    return s;
}

struct Traced
{
    std::vector<std::vector<float>> lanes;      // 3 bands x 2 lanes
    dsp::GainTrace trace;
    explicit Traced (int n) : lanes (6, std::vector<float> ((size_t) n, 0.0f))
    {
        for (int b = 0; b < 3; ++b)
            for (int c = 0; c < 2; ++c)
                trace.data[b][c] = lanes[(size_t) (2 * b + c)].data();
        trace.capacity = n;
    }
};

template <typename Engine>
void run (Engine& engine, const dsp::EngineSettings& s, juce::AudioBuffer<float>& buffer, int start, int end, int block)
{
    for (int pos = start; pos < end; pos += block)
    {
        const int n = std::min (block, end - pos);
        engine.setSettings (s);
        float* channels[2] { buffer.getWritePointer (0) + pos, buffer.getWritePointer (1) + pos };
        engine.process (channels, 2, n);
    }
}
}

TEST_CASE ("R2: an engine that continues from a running one computes the same band gains, exactly", "[dsp][engine][repair]")
{
    const int n = 3 * 48000, handover = 72000, block = 128;
    const auto settings = busySettings();
    auto in = makeNoise (2, n, 91u, 0.4f);
    for (int i = 0; i < n; ++i)                                         // level changes, so that the envelopes move
        for (int c = 0; c < 2; ++c)
            in.setSample (c, i, in.getSample (c, i) * (0.35f + 0.65f * (float) ((i / 9000) % 2)));

    const auto check = [&] (dsp::IBandEngine& running, dsp::IBandEngine& incoming, bool sameKind)
    {
        // Reference: the running engine alone, all the way.
        auto reference = in;
        Traced referenceTrace (n);
        running.setSettings (settings);
        running.prepare (kFs, 512);
        running.setGainTrace (&referenceTrace.trace);
        run (running, settings, reference, 0, n, block);
        running.setGainTrace (nullptr);

        // The same again up to the handover; then the incoming engine continues from it.
        auto first = in;
        running.setSettings (settings);
        running.prepare (kFs, 512);
        run (running, settings, first, 0, handover, block);

        incoming.setSettings (settings);
        incoming.prepare (kFs, 512);
        test::resetAllocationCount();
        {
            AudioCallbackScope scope;
            REQUIRE (incoming.continueFrom (running));
        }
        CHECK (test::allocationCount() == 0);

        auto continued = in;
        Traced continuedTrace (n - handover);
        incoming.setGainTrace (&continuedTrace.trace);
        run (incoming, settings, continued, handover, n, block);
        incoming.setGainTrace (nullptr);

        // Band gains (what the filters apply, after the lookahead): bit-identical from the first sample.
        long differing = 0;
        for (int lane = 0; lane < 6; ++lane)
            for (int i = 0; i < n - handover; ++i)
                differing += juce::exactlyEqual (continuedTrace.lanes[(size_t) lane][(size_t) i],
                                                 referenceTrace.lanes[(size_t) lane][(size_t) (handover + i)]) ? 0 : 1;
        CHECK (differing == 0);

        // The bands really were working at the handover.
        float deepest = 1.0f;
        for (int lane = 0; lane < 6; ++lane)
            deepest = std::min (deepest, referenceTrace.lanes[(size_t) lane][(size_t) handover]);
        CHECK (deepest < 0.7f);

        if (sameKind)
        {
            // The same kind of engine takes the filters over as well: the audio is bit-identical.
            long audioDiffering = 0;
            for (int c = 0; c < 2; ++c)
                for (int i = handover; i < n; ++i)
                    audioDiffering += juce::exactlyEqual (continued.getSample (c, i), reference.getSample (c, i)) ? 0 : 1;
            CHECK (audioDiffering == 0);
        }
        else
        {
            for (int c = 0; c < 2; ++c)
                for (int i = handover; i < n; ++i)
                    REQUIRE (std::isfinite (continued.getSample (c, i)));
        }
    };

    SECTION ("A3 -> A-prime")   { dsp::A3RegionEngine a; dsp::APrimeRegionEngine b; check (a, b, false); }
    SECTION ("A-prime -> A3")   { dsp::APrimeRegionEngine a; dsp::A3RegionEngine b; check (a, b, false); }
    SECTION ("A3 -> A3")        { dsp::A3RegionEngine a, b; check (a, b, true); }
    SECTION ("A-prime -> A-prime") { dsp::APrimeRegionEngine a, b; check (a, b, true); }
}

TEST_CASE ("R2: the level does not move at an engine changeover, whatever the detector settings", "[dsp][engine][repair]")
{
    // A tone at the band centre is treated alike by both engines, so any level change at the
    // changeover is the dynamics state, not the filter. Before the repair: +5.0 dB with a 100 ms
    // attack, +8.5 dB with 300 ms, +13.9 dB with 500 ms, +4.2 dB with a 300 ms RMS window,
    // -6.8 dB with automatic gain (125 ms after the click). After: within 0.01 dB.
    struct Config { const char* name; float attack, release; int detMode; float rmsTime; bool autoGain; float adaptive, lookahead, transient; };
    const Config configs[] = {
        { "defaults", 10.0f, 150.0f, 0, 30.0f, false, 0.0f, 0.0f, 0.0f },
        { "fastest attack and release", 0.01f, 1.0f, 0, 30.0f, false, 0.0f, 0.0f, 0.0f },
        { "attack 100 ms", 100.0f, 150.0f, 0, 30.0f, false, 0.0f, 0.0f, 0.0f },
        { "attack 300 ms", 300.0f, 150.0f, 0, 30.0f, false, 0.0f, 0.0f, 0.0f },
        { "slowest attack and release", 500.0f, 5000.0f, 0, 30.0f, false, 0.0f, 0.0f, 0.0f },
        { "RMS 1 ms", 10.0f, 150.0f, 1, 1.0f, false, 0.0f, 0.0f, 0.0f },
        { "RMS 300 ms", 10.0f, 150.0f, 1, 300.0f, false, 0.0f, 0.0f, 0.0f },
        { "blend, RMS 100 ms, slow attack", 200.0f, 2000.0f, 2, 100.0f, false, 0.0f, 0.0f, 0.0f },
        { "automatic gain", 10.0f, 150.0f, 0, 30.0f, true, 0.0f, 0.0f, 0.0f },
        { "adaptive release and transient", 30.0f, 400.0f, 0, 30.0f, false, 100.0f, 0.0f, 100.0f },
        { "attack 300 ms, lookahead 10 ms, automatic gain", 300.0f, 150.0f, 0, 30.0f, true, 0.0f, 10.0f, 0.0f },
    };

    const int n = 7 * 48000, block = 128, switchAt = 5 * 48000;
    const auto in = tone (1000.0, n, 0.5);

    for (const auto& c : configs)
        for (const Kind to : { Kind::APrime, Kind::A3 })
        {
            INFO (c.name << (to == Kind::APrime ? ", A3 -> A-prime" : ", A-prime -> A3"));
            PluginProcessor p;
            OfflineRenderer renderer (p);
            p.getBandSlots().createBand();
            setBand (p, 1, "fLo", 500.0f); setBand (p, 1, "fHi", 2000.0f);
            setBand (p, 1, "thresh", -30.0f); setBand (p, 1, "ratio", 8.0f);
            setBand (p, 1, "attack", c.attack); setBand (p, 1, "release", c.release);
            setBand (p, 1, "detMode", (float) c.detMode); setBand (p, 1, "rmsTime", c.rmsTime);
            setBand (p, 1, "adaptRel", c.adaptive); setBand (p, 1, "lookahead", c.lookahead);
            setBand (p, 1, "transient", c.transient);
            setPlain (p, "autoGain", c.autoGain ? 1.0f : 0.0f);
            p.setEngineKind (to == Kind::APrime ? Kind::A3 : Kind::APrime);
            REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));

            test::resetAllocationCount();
            const auto out = renderer.render (in, nullptr, { block }, false, [&] (int b, PluginProcessor& q)
            {
                if (b == switchAt / block) q.setEngineKind (to);
            });
            CHECK (test::allocationCount() == 0);

            const double before = levelDb (out, switchAt - 4800, 4800);
            if (! c.autoGain)
                REQUIRE (before < levelDb (in, switchAt - 4800, 4800) - 6.0);   // it is compressing
            REQUIRE (p.getGainReductionMeter().getGainReductionDb (1, 0) < -6.0f);
            double worst = 0.0;
            for (int i = 0; i + 480 <= 2 * 48000 - 480; i += 240)
                worst = std::max (worst, std::abs (levelDb (out, switchAt + i, 480) - before));
            CHECK (worst < 0.05);

            const float own = largestStep (out, switchAt - 4800, switchAt);
            CHECK (largestStep (out, switchAt, n) < 1.02f * own);
        }
}

TEST_CASE ("R2: where the engines differ, the changeover goes from one level to the other and no further", "[dsp][engine][repair]")
{
    // A tone at the band edge: about 2.7 dB of a 12 dB cut with A3, 6 dB with A-prime. Slow
    // detector and automatic gain on, i.e. the settings that used to misbehave.
    const int n = 9 * 48000, block = 128, there = 4 * 48000, back = 6 * 48000;
    const auto in = tone (500.0, n, 0.5);

    PluginProcessor p;
    OfflineRenderer renderer (p);
    p.getBandSlots().createBand();
    setBand (p, 1, "fLo", 500.0f); setBand (p, 1, "fHi", 2000.0f);
    setBand (p, 1, "thresh", -30.0f); setBand (p, 1, "ratio", 4.0f);
    setBand (p, 1, "attack", 300.0f); setBand (p, 1, "release", 2000.0f);
    REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));
    const auto out = renderer.render (in, nullptr, { block }, false, [&] (int b, PluginProcessor& q)
    {
        if (b == there / block) q.setEngineKind (Kind::APrime);
        if (b == back / block)  q.setEngineKind (Kind::A3);
    });

    const double a3 = levelDb (out, there - 9600, 9600), aPrime = levelDb (out, back - 9600, 9600);
    REQUIRE (std::abs (a3 - aPrime) > 0.5);                             // the engines do differ here
    const double low = std::min (a3, aPrime) - 0.1, high = std::max (a3, aPrime) + 0.1;
    for (int i = there; i + 480 <= n; i += 240)
    {
        const double level = levelDb (out, i, 480);
        REQUIRE (level > low);
        REQUIRE (level < high);
    }
    CHECK_THAT (levelDb (out, n - 9600, 9600), WithinAbs (a3, 0.05));     // and back where it started
    CHECK (largestStep (out, there, n) < 1.1f * largestStep (out, there - 9600, there));
}

TEST_CASE ("R2: the unheard run is as long as the narrowest band needs, within 10 and 250 ms", "[dsp][engine][repair]")
{
    // Four time constants of the band-pass, 4 / (pi * bandwidth), per used band that is not bypassed.
    dsp::A3RegionEngine engine;
    dsp::EngineSettings s;
    engine.setSettings (s);
    CHECK (engine.getFilterSettlingSeconds() == 0.0);                    // no bands

    s.usedMask = 0b11u;
    s.bands[0].fLoHz = 500.0f; s.bands[0].fHiHz = 2000.0f;
    s.bands[1].fLoHz = 60.0f;  s.bands[1].fHiHz = 70.0f;
    engine.setSettings (s);
    CHECK_THAT (engine.getFilterSettlingSeconds(), WithinAbs (4.0 / (dsp::kPi * 10.0), 1.0e-9));     // 127 ms
    s.bands[1].bypass = true;
    engine.setSettings (s);
    CHECK_THAT (engine.getFilterSettlingSeconds(), WithinAbs (4.0 / (dsp::kPi * 1500.0), 1.0e-9));   // 0.85 ms
    s.bands[0].makeupDb = 12.0f;                                         // a boost may narrow the band by sqrt (gain)
    engine.setSettings (s);
    CHECK_THAT (engine.getFilterSettlingSeconds(), WithinAbs (2.0 * 4.0 / (dsp::kPi * 1500.0), 1.0e-5));   // 12 dB = x 3.98
    s.bypassAll = true;
    engine.setSettings (s);
    CHECK (engine.getFilterSettlingSeconds() == 0.0);

    // In the plugin: when the change becomes audible after the click (band-edge tone, see above).
    struct Case { float lo, hi; double f; double expectedSeconds; };
    for (const Case c : { Case { 500.0f, 2000.0f, 500.0, 0.010 },        // wide band: the 10 ms minimum
                          Case { 60.0f, 70.0f, 60.0, 4.0 / (dsp::kPi * 10.0) },
                          Case { 30.0f, 31.0f, 30.0, 0.250 } })         // very narrow: the 250 ms maximum
    {
        INFO ("band " << c.lo << " - " << c.hi << " Hz");
        const int n = 6 * 48000, block = 64, switchAt = 4 * 48000;
        const auto in = tone (c.f, n, 0.5);
        const auto render = [&] (bool doSwitch)
        {
            PluginProcessor p;
            OfflineRenderer renderer (p);
            p.getBandSlots().createBand();
            setBand (p, 1, "fLo", c.lo); setBand (p, 1, "fHi", c.hi);
            setBand (p, 1, "thresh", 12.0f); setBand (p, 1, "makeup", -12.0f);
            REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));
            return renderer.render (in, nullptr, { block }, false, [&] (int b, PluginProcessor& q)
            {
                if (doSwitch && b == switchAt / block) q.setEngineKind (Kind::APrime);
            });
        };
        const auto plain = render (false), switched = render (true);
        int firstDifference = -1;
        for (int i = switchAt; i < n && firstDifference < 0; ++i)
            if (! juce::exactlyEqual (plain.getSample (0, i), switched.getSample (0, i))) firstDifference = i;
        REQUIRE (firstDifference > 0);
        const double unheard = (double) (firstDifference - switchAt) / kFs;
        CHECK (unheard >= c.expectedSeconds - 1.0e-4);
        CHECK (unheard <= c.expectedSeconds + (double) (2 * block) / kFs);
    }
}

TEST_CASE ("R2: a narrow low band changes over without a bump", "[dsp][engine][repair]")
{
    // 60 - 70 Hz, the filter rings for tens of milliseconds: the unheard run (127 ms) lets it
    // settle before the fade. Tone at the band centre, compressing, slow release.
    const int n = 8 * 48000, block = 128, switchAt = 5 * 48000;
    const auto in = tone (64.8, n, 0.5);
    PluginProcessor p;
    OfflineRenderer renderer (p);
    p.getBandSlots().createBand();
    setBand (p, 1, "fLo", 60.0f); setBand (p, 1, "fHi", 70.0f);
    setBand (p, 1, "thresh", -30.0f); setBand (p, 1, "ratio", 8.0f);
    setBand (p, 1, "attack", 100.0f); setBand (p, 1, "release", 2000.0f);
    REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));
    const auto out = renderer.render (in, nullptr, { block }, false, [&] (int b, PluginProcessor& q)
    {
        if (b == switchAt / block) q.setEngineKind (Kind::APrime);
    });

    // Whole cycles of 64.8 Hz per window (about 100 ms), so that the level reading is steady.
    const int window = 4815;
    const double before = levelDb (out, switchAt - 2 * window, window);
    REQUIRE (before < levelDb (in, switchAt - 2 * window, window) - 3.0);
    double worst = 0.0;
    for (int i = 0; i + window <= 2 * 48000; i += window / 4)
        worst = std::max (worst, std::abs (levelDb (out, switchAt + i, window) - before));
    CHECK (worst < 0.3);
    CHECK (largestStep (out, switchAt, n) < 1.05f * largestStep (out, switchAt - 9600, switchAt));
}

// ======================================================================================================
// R3: host blocks larger than announced
// ======================================================================================================

TEST_CASE ("R3: a host block larger than announced is processed like any other", "[dsp][engine][repair]")
{
    SECTION ("static settings: the output does not depend on the block layout, announced or not")
    {
        const int n = 48000;
        const auto in = makeNoise (2, n, 7u, 0.4f);
        const auto render = [&] (const std::vector<int>& blocks)
        {
            PluginProcessor p;
            OfflineRenderer renderer (p);
            for (int i = 0; i < 3; ++i) p.getBandSlots().createBand();
            setBand (p, 1, "thresh", -40.0f); setBand (p, 2, "lookahead", 4.0f);
            setPlain (p, "oversampling", 2.0f); setPlain (p, "charModel", 1.0f); setPlain (p, "charDrive", 40.0f);
            REQUIRE (renderer.prepare (kFs, 64, { 2, 0 }));                // announced: 64
            test::resetAllocationCount();
            auto out = renderBlocks (p, in, blocks);
            CHECK (test::allocationCount() == 0);
            return out;
        };
        const auto reference = render ({ 64 });
        CHECK (bitIdentical (render ({ 512 }), reference));
        CHECK (bitIdentical (render ({ 4096, 1, 63, 257, 1000 }), reference));
    }

    SECTION ("during an engine changeover: the unheard run and the crossfade still happen")
    {
        // Before the repair the changeover was made at once: 480 samples of silence with a
        // 10 ms lookahead (deviation 0.5) and a step of 0.46 against the tone's own 0.065.
        for (const int k : { 0, 1, 4, 20 })
            for (const int oversize : { 512, 5000 })
            {
                INFO ("oversize block of " << oversize << ", " << k << " blocks after the request");
                PluginProcessor p;
                OfflineRenderer renderer (p);
                p.getBandSlots().createBand();
                setBand (p, 1, "thresh", 12.0f); setBand (p, 1, "lookahead", 10.0f);
                REQUIRE (renderer.prepare (kFs, 64, { 1, 0 }));
                const int latency = p.getLatencySamples();

                const int n = 96000, switchBlock = 600;
                const auto in = tone (1000.0, n, 0.5);
                std::vector<int> blocks ((size_t) (switchBlock + k), 64);
                blocks.push_back (oversize);
                for (int i = 0; i < 4000; ++i) blocks.push_back (64);

                test::resetAllocationCount();
                const auto out = renderBlocks (p, in, blocks, [&] (int b, PluginProcessor& q)
                {
                    if (b == switchBlock) q.setEngineKind (Kind::APrime);
                });
                CHECK (test::allocationCount() == 0);

                int mismatches = 0;
                for (int i = latency; i < n; ++i)
                    mismatches += juce::exactlyEqual (out.getSample (0, i), in.getSample (0, i - latency)) ? 0 : 1;
                CHECK (mismatches == 0);                                  // a neutral band: the delayed input, exactly
            }
    }

    SECTION ("during a changeover with a compressing band: no step")
    {
        for (const int k : { 0, 1, 4, 20 })
        {
            INFO (k << " blocks after the request");
            PluginProcessor p;
            OfflineRenderer renderer (p);
            p.getBandSlots().createBand();
            setBand (p, 1, "fLo", 200.0f); setBand (p, 1, "fHi", 2000.0f);
            setBand (p, 1, "thresh", -30.0f); setBand (p, 1, "ratio", 8.0f);
            REQUIRE (renderer.prepare (kFs, 64, { 1, 0 }));

            const int n = 96000, switchBlock = 600;
            const auto in = tone (300.0, n, 0.5);
            std::vector<int> blocks ((size_t) (switchBlock + k), 64);
            blocks.push_back (512);
            for (int i = 0; i < 4000; ++i) blocks.push_back (64);
            const auto out = renderBlocks (p, in, blocks, [&] (int b, PluginProcessor& q)
            {
                if (b == switchBlock) q.setEngineKind (Kind::APrime);
            });

            const int from = switchBlock * 64;
            const float own = largestStep (out, from - 9600, from);
            CHECK (largestStep (out, from, from + 24000) < 1.3f * own);      // was up to 26 times
        }
    }
}
