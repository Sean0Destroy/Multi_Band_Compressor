// Level meters: the lock-free meter itself, the processor feed and the editor's meter rail.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

#include "rt/LevelMeter.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"
#include "ui/PluginEditor.h"

using namespace tl;
using Catch::Matchers::WithinAbs;

namespace
{
struct EditorCloser
{
    void operator() (juce::AudioProcessorEditor* ed) const
    {
        if (ed == nullptr) return;
        ed->processor.editorBeingDeleted (ed);
        delete ed;
    }
};

juce::AudioBuffer<float> sineBuffer (double f, double fs, int n, double amplitude, int channels)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = test::sine (f, fs, n, amplitude);
    for (int c = 0; c < channels; ++c) b.copyFrom (c, 0, s.data(), n);
    return b;
}
}

TEST_CASE ("Level meter: peak hold until read, 300 ms RMS, channel count", "[meter]")
{
    rt::LevelMeter m;
    m.prepare (48000.0);
    CHECK (m.getChannelCount() == 0);
    CHECK (m.readAndResetPeakDb (0) == rt::LevelMeter::silenceDb);
    CHECK (m.getRmsDb (0) == rt::LevelMeter::silenceDb);
    CHECK (m.readAndResetPeakDb (2) == rt::LevelMeter::silenceDb);      // out of range
    CHECK (m.getRmsDb (-1) == rt::LevelMeter::silenceDb);

    const auto s = test::sine (1000.0, 48000.0, 48000, 0.5);
    m.setChannelCount (2);
    for (int pos = 0; pos < 48000; pos += 480)
        m.process (0, s.data() + pos, 480);

    CHECK (m.getChannelCount() == 2);
    CHECK_THAT ((double) m.readAndResetPeakDb (0), WithinAbs (-6.0206, 0.01));
    // After 1 s (3.33 time constants) the mean square has reached 1 - e^-3.33 of its final value.
    CHECK_THAT ((double) m.getRmsDb (0), WithinAbs (-9.0309 + 10.0 * std::log10 (1.0 - std::exp (-1.0 / 0.3)), 0.01));
    CHECK (m.readAndResetPeakDb (1) == rt::LevelMeter::silenceDb);        // other channel untouched

    // Max-hold: a loud block followed by quiet ones is not missed by a slow reader ...
    std::vector<float> loud (64, 0.9f), quiet (64, 0.01f);
    m.process (0, loud.data(), 64);
    for (int i = 0; i < 10; ++i) m.process (0, quiet.data(), 64);
    CHECK_THAT ((double) m.readAndResetPeakDb (0), WithinAbs (20.0 * std::log10 (0.9), 0.01));
    // ... and with nothing new since the read, the last block's peak is reported (not silence).
    CHECK_THAT ((double) m.readAndResetPeakDb (0), WithinAbs (-40.0, 0.01));

    // Peaks above full scale are reported as such.
    std::vector<float> over (16, -2.0f);
    m.process (1, over.data(), 16);
    CHECK_THAT ((double) m.readAndResetPeakDb (1), WithinAbs (6.0206, 0.01));

    // RMS time constant: after 300 ms of a step the mean square has covered 63 % of the way.
    m.prepare (48000.0);
    std::vector<float> dc (480, 1.0f);
    for (int i = 0; i < 30; ++i) m.process (0, dc.data(), 480);
    CHECK_THAT ((double) m.getRmsDb (0), WithinAbs (10.0 * std::log10 (1.0 - std::exp (-1.0)), 0.01));

    // The block size does not change the result.
    rt::LevelMeter a, b;
    a.prepare (44100.0); b.prepare (44100.0);
    const auto t = test::sine (300.0, 44100.0, 22050, 0.3);
    for (int pos = 0; pos < 22050; pos += 441) a.process (0, t.data() + pos, 441);
    for (int pos = 0; pos < 22050; pos += 63)  b.process (0, t.data() + pos, 63);
    CHECK_THAT ((double) a.getRmsDb (0), WithinAbs ((double) b.getRmsDb (0), 0.02));
}

TEST_CASE ("Processor meters: input, output and sidechain, only while an editor listens", "[meter][alloc]")
{
    PluginProcessor p;
    test::OfflineRenderer renderer (p);
    REQUIRE (renderer.prepare (48000.0, 512, { 2, 1 }));
    test::setPlain (p, "outGain", -6.0f);
    REQUIRE (renderer.prepare (48000.0, 512, { 2, 1 }));

    const auto in = sineBuffer (1000.0, 48000.0, 48000, 0.5, 2);
    const auto sc = sineBuffer (200.0, 48000.0, 48000, 0.25, 1);

    renderer.render (in, &sc, { 512 });
    CHECK (p.getInputMeter().getChannelCount() == 0);                    // nobody listening: nothing measured
    CHECK (p.getInputMeter().readAndResetPeakDb (0) == rt::LevelMeter::silenceDb);

    p.addAnalyzerClient();
    test::resetAllocationCount();
    renderer.render (in, &sc, { 512, 64, 1 });
    CHECK (test::allocationCount() == 0);

    CHECK (p.getInputMeter().getChannelCount() == 2);
    CHECK (p.getOutputMeter().getChannelCount() == 2);
    CHECK (p.getSidechainMeter().getChannelCount() == 1);
    for (int ch = 0; ch < 2; ++ch)
    {
        CHECK_THAT ((double) p.getInputMeter().readAndResetPeakDb (ch), WithinAbs (-6.02, 0.02));
        CHECK_THAT ((double) p.getOutputMeter().readAndResetPeakDb (ch), WithinAbs (-12.02, 0.02));   // after the output gain
        CHECK_THAT ((double) p.getInputMeter().getRmsDb (ch), WithinAbs (-9.03, 0.2));
        CHECK_THAT ((double) p.getOutputMeter().getRmsDb (ch), WithinAbs (-15.03, 0.2));
    }
    CHECK_THAT ((double) p.getSidechainMeter().readAndResetPeakDb (0), WithinAbs (-12.04, 0.02));

    // Without a sidechain bus the meter reports no channels.
    REQUIRE (renderer.prepare (48000.0, 512, { 1, 0 }));
    renderer.render (sineBuffer (1000.0, 48000.0, 4800, 0.5, 1), nullptr, { 512 });
    CHECK (p.getSidechainMeter().getChannelCount() == 0);
    CHECK (p.getInputMeter().getChannelCount() == 1);
    p.removeAnalyzerClient();
}

TEST_CASE ("Meter rail: readings, clip flag, hold, sidechain state, silence when audio stops", "[ui][meter]")
{
    PluginProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor, EditorCloser> holder (p.createEditorAndMakeActive());
    auto* ed = dynamic_cast<ui::PluginEditor*> (holder.get());
    REQUIRE (ed != nullptr);
    auto& rail = ed->getMeterRail();
    using State = ui::MeterRail::SidechainState;

    CHECK (rail.isVisible());
    CHECK (rail.getRight() <= ed->getWidth());
    CHECK_FALSE (rail.getBounds().intersects (ed->getSpectrum().getBounds()));
    CHECK (rail.getSidechainState() == State::Off);
    CHECK (ui::MeterRail::sidechainStateText (State::Off) == "SC off");

    test::OfflineRenderer renderer (p);
    REQUIRE (renderer.prepare (48000.0, 512, { 2, 2 }));
    const auto in = sineBuffer (1000.0, 48000.0, 24000, 0.5, 2);
    juce::AudioBuffer<float> silentSc (2, 24000);
    silentSc.clear();

    // Sidechain connected but silent.
    renderer.render (in, &silentSc, { 512 });
    rail.refresh (0.0, true);
    CHECK (rail.getSidechainState() == State::Silent);
    CHECK_THAT ((double) rail.input().peakDb[0], WithinAbs (-6.02, 0.02));
    CHECK_THAT ((double) rail.output().peakDb[1], WithinAbs (-6.02, 0.02));
    CHECK (rail.input().channels == 2);
    CHECK_FALSE (rail.output().clipped);

    // Sidechain receiving; a short dropout does not flip the state, 300 ms of silence does.
    const auto liveSc = sineBuffer (100.0, 48000.0, 24000, 0.2, 2);
    renderer.render (in, &liveSc, { 512 });
    rail.refresh (100.0, true);
    CHECK (rail.getSidechainState() == State::Receiving);
    CHECK_THAT ((double) rail.sidechain().peakDb[0], WithinAbs (-13.98, 0.02));
    renderer.render (in, &silentSc, { 512 });
    rail.refresh (200.0, true);
    CHECK (rail.getSidechainState() == State::Receiving);
    renderer.render (in, &silentSc, { 512 });
    rail.refresh (501.0, true);
    CHECK (rail.getSidechainState() == State::Silent);

    // Output above full scale: flagged, and the numeric peak is held for two seconds.
    test::setPlain (p, "outGain", 12.0f);
    renderer.render (in, &silentSc, { 512 });
    rail.refresh (1000.0, true);
    CHECK (rail.output().clipped);
    CHECK_THAT ((double) rail.output().heldDb, WithinAbs (5.98, 0.05));
    test::setPlain (p, "outGain", -24.0f);
    renderer.render (in, &silentSc, { 512 });               // contains the 20 ms ramp down from +12 dB
    rail.refresh (1900.0, true);
    renderer.render (in, &silentSc, { 512 });
    rail.refresh (2000.0, true);
    CHECK (rail.output().peakDb[0] < -20.0f);
    CHECK_THAT ((double) rail.output().heldDb, WithinAbs (5.98, 0.05));    // still held
    CHECK (rail.output().clipped);
    rail.refresh (3001.0, true);
    CHECK (rail.output().heldDb < -20.0f);
    CHECK_FALSE (rail.output().clipped);

    // The host stops processing: the meters fall to silence instead of freezing.
    rail.refresh (3100.0, false);
    CHECK (rail.input().peakDb[0] == rt::LevelMeter::silenceDb);
    CHECK (rail.input().rmsDb[0] == rt::LevelMeter::silenceDb);

    // The host removes the sidechain: off.
    REQUIRE (renderer.prepare (48000.0, 512, { 2, 0 }));
    renderer.render (in, nullptr, { 512 });
    ed->refreshFromProcessor();
    CHECK (rail.getSidechainState() == State::Off);
    CHECK (rail.sidechain().channels == 0);
}
