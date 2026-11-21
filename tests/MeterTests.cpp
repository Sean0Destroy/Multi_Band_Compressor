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

    // Loudness column: the output of a -6 dBFS stereo 1 kHz tone at -24 dB output gain.
    test::setPlain (p, "outGain", 0.0f);
    p.getLoudnessMeter().requestReset();
    for (int i = 0; i < 8; ++i) renderer.render (in, &silentSc, { 512 });      // 4 s
    rail.refresh (4000.0, true);
    CHECK_THAT ((double) rail.getLoudnessText (0).getFloatValue(), WithinAbs (-6.0, 0.2));    // momentary
    CHECK_THAT ((double) rail.getLoudnessText (1).getFloatValue(), WithinAbs (-6.0, 0.2));    // short-term
    CHECK_THAT ((double) rail.getLoudnessText (2).getFloatValue(), WithinAbs (-6.0, 0.2));    // integrated
    CHECK_THAT ((double) rail.getLoudnessText (3).getFloatValue(), WithinAbs (-6.0, 0.2));    // true peak
    rail.refresh (4100.0, false);                                                     // audio stopped
    CHECK (rail.getLoudnessText (0) == "-");
    CHECK (rail.getLoudnessText (2) != "-");                                          // integrated is kept

    // The reset button starts the integrated loudness again.
    std::vector<juce::Button*> buttons;
    for (auto* child : rail.getChildren()) if (auto* b = dynamic_cast<juce::Button*> (child)) buttons.push_back (b);
    REQUIRE (buttons.size() == 1u);
    CHECK (buttons[0]->getComponentID() == "action_resetLoudness");
    CHECK (buttons[0]->getWidth() >= 24);
    CHECK (buttons[0]->getHeight() >= 24);
    buttons[0]->onClick();
    renderer.render (silentSc, &silentSc, { 512 });
    rail.refresh (4200.0, true);
    CHECK (rail.getLoudnessText (2) == "-");
    CHECK (rail.getLoudnessText (3) == "-");

    // The host removes the sidechain: off.
    REQUIRE (renderer.prepare (48000.0, 512, { 2, 0 }));
    renderer.render (in, nullptr, { 512 });
    ed->refreshFromProcessor();
    CHECK (rail.getSidechainState() == State::Off);
    CHECK (rail.sidechain().channels == 0);
}

// ---- Loudness and true peak ----------------------------------------------------------------------
#include "rt/LoudnessMeter.h"

namespace
{
/** Feeds a stereo 997 Hz sine of the given peak level (dBFS) for `seconds`. */
void feedSine (rt::LoudnessMeter& m, double fs, double peakDb, double seconds, double frequency = 997.0, double phase = 0.0)
{
    const int total = (int) std::lround (seconds * fs);
    const double a = std::pow (10.0, peakDb / 20.0);
    std::vector<float> block (480);
    for (int pos = 0; pos < total; pos += 480)
    {
        const int n = std::min (480, total - pos);
        for (int i = 0; i < n; ++i)
            block[(size_t) i] = (float) (a * std::sin (2.0 * 3.14159265358979323846 * frequency * (double) (pos + i) / fs + phase));
        m.process (block.data(), block.data(), n);
    }
}
}

TEST_CASE ("Loudness meter: reference tones read their level in LUFS (EBU Tech 3341 style)", "[meter][loudness]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        INFO ("fs " << fs);
        auto m = std::make_unique<rt::LoudnessMeter>();
        m->prepare (fs);
        CHECK (m->getMomentaryLufs() == rt::LoudnessMeter::silenceLufs);
        CHECK (m->getIntegratedLufs() == rt::LoudnessMeter::silenceLufs);

        // Case 1: stereo 997 Hz sine at -23 dBFS reads -23.0 LUFS on all three meters.
        feedSine (*m, fs, -23.0, 5.0);
        CHECK_THAT ((double) m->getMomentaryLufs(), WithinAbs (-23.0, 0.1));
        CHECK_THAT ((double) m->getShortTermLufs(), WithinAbs (-23.0, 0.1));
        CHECK_THAT ((double) m->getIntegratedLufs(), WithinAbs (-23.0, 0.1));

        // Case 2: -33 dBFS reads -33.0 LUFS.
        m->prepare (fs);
        feedSine (*m, fs, -33.0, 5.0);
        CHECK_THAT ((double) m->getIntegratedLufs(), WithinAbs (-33.0, 0.1));
        CHECK_THAT ((double) m->getMomentaryLufs(), WithinAbs (-33.0, 0.1));
    }

    // Mono is 3 dB below the same signal in both channels.
    auto mono = std::make_unique<rt::LoudnessMeter>();
    mono->prepare (48000.0);
    std::vector<float> block (480);
    for (int pos = 0; pos < 48000 * 4; pos += 480)
    {
        for (int i = 0; i < 480; ++i)
            block[(size_t) i] = (float) (std::pow (10.0, -23.0 / 20.0) * std::sin (2.0 * 3.14159265358979323846 * 997.0 * (double) (pos + i) / 48000.0));
        mono->process (block.data(), nullptr, 480);
    }
    CHECK_THAT ((double) mono->getIntegratedLufs(), WithinAbs (-26.01, 0.1));
}

TEST_CASE ("Loudness meter: gating, time windows, reset", "[meter][loudness]")
{
    const double fs = 48000.0;
    auto m = std::make_unique<rt::LoudnessMeter>();
    m->prepare (fs);

    // Case 3: quiet (-36), loud (-23), quiet (-36). The relative gate (10 LU below the ungated
    // mean) drops the quiet parts, so the integrated loudness is that of the loud part.
    feedSine (*m, fs, -36.0, 5.0);
    feedSine (*m, fs, -23.0, 30.0);
    feedSine (*m, fs, -36.0, 5.0);
    CHECK_THAT ((double) m->getIntegratedLufs(), WithinAbs (-23.0, 0.1));
    CHECK_THAT ((double) m->getMomentaryLufs(), WithinAbs (-36.0, 0.1));     // the last 400 ms
    CHECK_THAT ((double) m->getShortTermLufs(), WithinAbs (-36.0, 0.1));     // the last 3 s

    // Case 4 (absolute gate): long silence does not pull the integrated value down.
    std::vector<float> silence (480, 0.0f);
    for (int i = 0; i < 100 * 20; ++i) m->process (silence.data(), silence.data(), 480);
    CHECK_THAT ((double) m->getIntegratedLufs(), WithinAbs (-23.0, 0.1));
    CHECK (m->getMomentaryLufs() == rt::LoudnessMeter::silenceLufs);

    // Windows: 400 ms after a jump the momentary value has arrived, the short-term one has not.
    m->prepare (fs);
    feedSine (*m, fs, -40.0, 4.0);
    feedSine (*m, fs, -20.0, 0.4);
    CHECK_THAT ((double) m->getMomentaryLufs(), WithinAbs (-20.0, 0.1));
    CHECK (m->getShortTermLufs() < -27.0f);
    feedSine (*m, fs, -20.0, 2.6);
    CHECK_THAT ((double) m->getShortTermLufs(), WithinAbs (-20.0, 0.1));

    // Reset starts every reading afresh: nothing from before it is counted.
    m->requestReset();
    feedSine (*m, fs, -30.0, 0.1);
    CHECK (m->getTruePeakDb() < -29.0f);
    feedSine (*m, fs, -30.0, 3.0);
    CHECK_THAT ((double) m->getIntegratedLufs(), WithinAbs (-30.0, 0.1));
}

TEST_CASE ("True peak: inter-sample peaks are found", "[meter][loudness]")
{
    const double fs = 48000.0;
    auto m = std::make_unique<rt::LoudnessMeter>();
    m->prepare (fs);

    // fs/4 with a 45 degree phase: every sample is at 0.707 of the real peak.
    feedSine (*m, fs, 0.0, 1.0, 12000.0, 0.78539816339744831);
    CHECK_THAT ((double) m->getTruePeakDb(), WithinAbs (0.0, 0.3));         // sample peak would read -3.01

    // A low tone: true peak equals the sample peak.
    m->prepare (fs);
    feedSine (*m, fs, -6.0, 1.0, 100.0);
    CHECK_THAT ((double) m->getTruePeakDb(), WithinAbs (-6.0, 0.1));

    // It is a maximum since the last reset.
    feedSine (*m, fs, -30.0, 1.0, 100.0);
    CHECK_THAT ((double) m->getTruePeakDb(), WithinAbs (-6.0, 0.1));
}

TEST_CASE ("Processor feeds the loudness meter with its output while an editor listens", "[meter][loudness][alloc]")
{
    PluginProcessor p;
    test::OfflineRenderer renderer (p);
    test::setPlain (p, "outGain", -6.0f);
    REQUIRE (renderer.prepare (48000.0, 512, {}));
    const auto in = sineBuffer (997.0, 48000.0, 4 * 48000, std::pow (10.0, -17.0 / 20.0), 2);

    renderer.render (in, nullptr, { 512 });
    CHECK (p.getLoudnessMeter().getIntegratedLufs() == rt::LoudnessMeter::silenceLufs);   // nobody listening

    p.addAnalyzerClient();
    test::resetAllocationCount();
    renderer.render (in, nullptr, { 512, 100, 1 });
    CHECK (test::allocationCount() == 0);
    CHECK_THAT ((double) p.getLoudnessMeter().getIntegratedLufs(), WithinAbs (-23.0, 0.1));   // -17 dBFS in, -6 dB out gain
    CHECK_THAT ((double) p.getLoudnessMeter().getTruePeakDb(), WithinAbs (-23.0, 0.1));
    p.removeAnalyzerClient();
}
