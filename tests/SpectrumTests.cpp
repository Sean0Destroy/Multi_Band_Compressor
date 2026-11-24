// Spectrum display: analyzer FIFO, FFT analyzer calibration, processor feed, coordinate mapping,
// band region interaction and the live response curve.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <thread>

#include "analysis/SpectrumAnalyzer.h"
#include "dsp/A3Model.h"
#include "rt/AnalyzerFifo.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"
#include "ui/PluginEditor.h"

using namespace tl;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Slots = std::vector<int>;

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

struct Rig
{
    PluginProcessor proc;
    std::unique_ptr<juce::AudioProcessorEditor, EditorCloser> holder { proc.createEditorAndMakeActive() };
    ui::PluginEditor* editor = dynamic_cast<ui::PluginEditor*> (holder.get());
    ui::SpectrumView& view() { return editor->getSpectrum(); }
};

float plain (PluginProcessor& p, const std::string& id) { return p.getState().getRawParameterValue (id)->load(); }

struct GestureCounter : juce::AudioProcessorParameter::Listener
{
    int begins = 0, ends = 0, changes = 0;
    void parameterValueChanged (int, float) override { ++changes; }
    void parameterGestureChanged (int, bool starting) override { starting ? ++begins : ++ends; }
};
}

TEST_CASE ("Analyzer FIFO: order, mono mix, drop on overflow, concurrent use", "[analysis][fifo]")
{
    auto fifo = std::make_unique<rt::AnalyzerFifo>();
    CHECK (fifo->available() == 0);

    float l[4] { 1.0f, 2.0f, 3.0f, 4.0f }, r[4] { 3.0f, 2.0f, 1.0f, 0.0f }, out[8] {};
    CHECK (fifo->push (l, r, 4));
    CHECK (fifo->push (l, nullptr, 2));
    CHECK (fifo->available() == 6);
    CHECK (fifo->pull (out, 8) == 6);
    CHECK (out[0] == 2.0f); CHECK (out[1] == 2.0f); CHECK (out[2] == 2.0f); CHECK (out[3] == 2.0f);   // (l + r) / 2
    CHECK (out[4] == 1.0f); CHECK (out[5] == 2.0f);                                                   // mono as is
    CHECK (fifo->pull (out, 8) == 0);

    // A block that does not fit is dropped whole; nothing already queued is overwritten.
    std::vector<float> big ((size_t) rt::AnalyzerFifo::capacity - 2, 0.5f);
    CHECK (fifo->push (big.data(), nullptr, (int) big.size()));
    CHECK_FALSE (fifo->push (l, nullptr, 4));
    CHECK (fifo->push (l, nullptr, 2));
    CHECK (fifo->available() == rt::AnalyzerFifo::capacity);
    CHECK_FALSE (fifo->push (l, nullptr, 1));
    fifo->clear();
    CHECK (fifo->available() == 0);
    CHECK_FALSE (fifo->push (l, nullptr, 0));

    // One writer, one reader: every value that gets through arrives once and in order.
    std::atomic<bool> done { false };
    long long written = 0;
    std::thread writer ([&]
    {
        float block[64];
        float next = 0.0f;
        for (int i = 0; i < 20000; ++i)
        {
            for (auto& v : block) { v = next; next += 1.0f; }
            if (fifo->push (block, nullptr, 64)) written += 64;
            else next -= 64.0f;                                   // dropped: resend the same values
        }
        done = true;
    });

    float expected = 0.0f;
    long long read = 0;
    bool ordered = true;
    float buffer[256];
    while (! done.load() || fifo->available() > 0)
    {
        const int n = fifo->pull (buffer, 256);
        for (int i = 0; i < n; ++i) { ordered = ordered && buffer[i] == expected; expected += 1.0f; }
        read += n;
    }
    writer.join();
    CHECK (ordered);
    CHECK (read == written);
    CHECK (written > 0);
}

TEST_CASE ("Spectrum analyzer: a full-scale sine reads 0 dBFS at its frequency", "[analysis]")
{
    analysis::SpectrumAnalyzer a;
    a.setSampleRate (48000.0);
    a.compute (1000.0f);
    CHECK (a.levelDb (20.0, 20000.0) == analysis::SpectrumAnalyzer::floorDb);        // silence

    for (double amplitude : { 1.0, 0.5, 0.01 })
        for (double f : { 100.0, 997.0, 10000.0 })
        {
            INFO ("f " << f << " amplitude " << amplitude);
            a.reset();
            const auto s = test::sine (f, 48000.0, 8192, amplitude);
            a.push (s.data(), (int) s.size());
            a.compute (1000.0f);

            const double expected = 20.0 * std::log10 (amplitude);
            CHECK_THAT ((double) a.levelDb (f * 0.97, f * 1.03), WithinAbs (expected, 1.5));   // scalloping loss <= 1.42 dB (Hann)
            CHECK (a.levelDb (f * 3.0, f * 3.5) < expected - 60.0);                            // nothing elsewhere

            // The loudest bin is the one nearest the tone.
            int loudest = 0;
            for (int b = 1; b < a.numBins(); ++b)
                if (a.binLevelDb (b) > a.binLevelDb (loudest)) loudest = b;
            CHECK (std::abs (a.binFrequency (loudest) - f) <= 48000.0 / 4096.0);
        }

    // Ballistics: after the tone stops a bin falls by at most `fallDb` per update.
    const auto s = test::sine (1000.0, 48000.0, 8192, 1.0);
    a.reset();
    a.push (s.data(), (int) s.size());
    a.compute (1000.0f);
    const float before = a.levelDb (990.0, 1010.0);
    std::vector<float> zeros (8192, 0.0f);
    a.push (zeros.data(), (int) zeros.size());
    a.compute (3.0f);
    CHECK_THAT ((double) a.levelDb (990.0, 1010.0), WithinAbs ((double) before - 3.0, 1.0e-3));

    // Power average of white noise sits near the per-bin noise level, not at the peaks.
    a.reset();
    const auto noise = test::makeNoise (1, 8192, 9u, 0.5f);
    a.push (noise.getReadPointer (0), 8192);
    a.compute (1000.0f);
    CHECK (a.averageLevelDb (1000.0, 2000.0) < a.levelDb (1000.0, 2000.0));
    CHECK (a.averageLevelDb (1000.0, 2000.0) > a.levelDb (1000.0, 2000.0) - 15.0f);
}

TEST_CASE ("Processor feeds the analyzer only while an editor asks for it, without allocating", "[analysis][alloc]")
{
    PluginProcessor p;
    p.getBandSlots().createBand();
    test::setBand (p, 1, "thresh", -40.0f);
    test::OfflineRenderer renderer (p);
    REQUIRE (renderer.prepare (48000.0, 256, {}));
    const auto in = test::makeNoise (2, 4096, 3u, 0.5f);

    CHECK_FALSE (p.isAnalyzerActive());
    renderer.render (in, nullptr, { 256 });
    CHECK (p.getInputAnalyzerFifo().available() == 0);
    CHECK (p.getOutputAnalyzerFifo().available() == 0);

    p.addAnalyzerClient();
    CHECK (p.isAnalyzerActive());
    test::resetAllocationCount();
    const auto out = renderer.render (in, nullptr, { 256, 100, 1 });
    CHECK (test::allocationCount() == 0);                  // counted inside processBlock only
    CHECK (p.getInputAnalyzerFifo().available() == 4096);
    CHECK (p.getOutputAnalyzerFifo().available() == 4096);

    std::vector<float> preTap (4096), postTap (4096);
    REQUIRE (p.getInputAnalyzerFifo().pull (preTap.data(), 4096) == 4096);
    REQUIRE (p.getOutputAnalyzerFifo().pull (postTap.data(), 4096) == 4096);
    bool preOk = true, postOk = true;
    for (int i = 0; i < 4096; ++i)
    {
        preOk = preOk && preTap[(size_t) i] == 0.5f * (in.getSample (0, i) + in.getSample (1, i));
        postOk = postOk && postTap[(size_t) i] == 0.5f * (out.getSample (0, i) + out.getSample (1, i));
    }
    CHECK (preOk);
    CHECK (postOk);

    // A reader that never comes: the audio keeps running and blocks are dropped.
    for (int i = 0; i < 8; ++i) renderer.render (in, nullptr, { 256 });
    CHECK (p.getInputAnalyzerFifo().available() <= rt::AnalyzerFifo::capacity);

    p.removeAnalyzerClient();
    CHECK_FALSE (p.isAnalyzerActive());

    // The editor registers and unregisters itself.
    {
        std::unique_ptr<juce::AudioProcessorEditor, EditorCloser> ed (p.createEditorAndMakeActive());
        CHECK (p.isAnalyzerActive());
    }
    CHECK_FALSE (p.isAnalyzerActive());
}

TEST_CASE ("Spectrum view: frequency and level mapping", "[ui][spectrum]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& v = r.view();
    const auto plot = v.plotBounds();
    REQUIRE (plot.getWidth() > 600.0f);
    REQUIRE (plot.getHeight() > 100.0f);

    CHECK_THAT ((double) v.xForFrequency (20.0), WithinAbs ((double) plot.getX(), 0.01));
    CHECK_THAT ((double) v.xForFrequency (20000.0), WithinAbs ((double) plot.getRight(), 0.01));
    CHECK_THAT ((double) v.xForFrequency (632.455), WithinAbs ((double) plot.getCentreX(), 0.05));   // geometric middle
    for (double f : { 20.0, 55.0, 440.0, 1000.0, 7300.0, 20000.0 })
        CHECK_THAT (v.frequencyForX (v.xForFrequency (f)), WithinRel (f, 1.0e-4));

    CHECK_THAT ((double) v.yForResponseDb (ui::SpectrumView::topDb), WithinAbs ((double) plot.getY(), 0.01));
    CHECK_THAT ((double) v.yForResponseDb (ui::SpectrumView::bottomDb), WithinAbs ((double) plot.getBottom(), 0.01));
    CHECK (v.yForResponseDb (0.0) < v.yForResponseDb (-6.0));
    CHECK_THAT ((double) v.yForLevelDb (0.0), WithinAbs ((double) plot.getY(), 0.01));
    CHECK_THAT ((double) v.yForLevelDb (-96.0), WithinAbs ((double) plot.getBottom(), 0.01));

    // The display takes the extra height when the editor grows.
    const int before = v.getHeight();
    r.editor->setSize (r.editor->getWidth(), ui::PluginEditor::defaultHeight + 200);
    CHECK (v.getHeight() == before + 200);
    r.editor->setSize (r.editor->getWidth(), ui::PluginEditor::minHeight);
    CHECK (v.getHeight() >= ui::theme::displayMinHeight);
}

TEST_CASE ("Spectrum view: selecting and dragging band regions", "[ui][spectrum]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& v = r.view();
    auto& ed = *r.editor;
    for (int i = 0; i < 3; ++i) r.proc.getBandSlots().createBand();     // 20-120, 120-400, 400-1000 Hz
    ed.refreshFromProcessor();
    const float y = v.plotBounds().getCentreY();
    const auto at = [&] (double hz) { return juce::Point<float> (v.xForFrequency (hz), y); };

    SECTION ("hit testing")
    {
        CHECK (v.hitTestAt (at (60.0)).slot == 1);
        CHECK (v.hitTestAt (at (60.0)).part == ui::SpectrumView::Part::Body);
        CHECK (v.hitTestAt (at (700.0)).slot == 3);
        CHECK (v.hitTestAt (at (5000.0)).slot == 0);
        CHECK (v.hitTestAt (at (1000.0)).part == ui::SpectrumView::Part::HighEdge);
        CHECK (v.hitTestAt (at (1000.0)).slot == 3);
        CHECK (v.hitTestAt ({ v.xForFrequency (1000.0) + 4.0f, y }).part == ui::SpectrumView::Part::HighEdge);   // within the grab width
        CHECK (v.hitTestAt ({ 2.0f, y }).slot == 0);                    // outside the plot

        // A shared edge (120 Hz belongs to bands 1 and 2): the selected band gets it.
        ed.selectSlot (1);
        CHECK (v.hitTestAt (at (120.0)).slot == 1);
        CHECK (v.hitTestAt (at (120.0)).part == ui::SpectrumView::Part::HighEdge);
        ed.selectSlot (2);
        CHECK (v.hitTestAt (at (120.0)).slot == 2);
        CHECK (v.hitTestAt (at (120.0)).part == ui::SpectrumView::Part::LowEdge);
    }

    SECTION ("clicking a region selects the band everywhere")
    {
        v.beginDrag (at (700.0));
        v.endDrag();
        CHECK (ed.getSelectedSlot() == 3);
        CHECK (v.getSelectedSlot() == 3);
        CHECK (ed.getInspector().getSlot() == 3);
        CHECK (ed.getColumn (3).isSelected());
        CHECK_THAT ((double) plain (r.proc, "b3_fLo"), WithinRel (400.0, 1e-6));     // a click moves nothing
    }

    SECTION ("dragging an edge changes that edge inside one host gesture")
    {
        GestureCounter lo, hi;
        r.proc.getState().getParameter ("b3_fLo")->addListener (&lo);
        r.proc.getState().getParameter ("b3_fHi")->addListener (&hi);

        v.beginDrag (at (1000.0));
        v.continueDrag (at (1500.0));
        v.continueDrag (at (2000.0));
        CHECK (v.isDragging());
        v.endDrag();

        CHECK_THAT ((double) plain (r.proc, "b3_fHi"), WithinRel (2000.0, 2e-3));
        CHECK_THAT ((double) plain (r.proc, "b3_fLo"), WithinRel (400.0, 1e-6));
        CHECK (hi.begins == 1); CHECK (hi.ends == 1); CHECK (hi.changes == 2);
        CHECK (lo.begins == 1); CHECK (lo.ends == 1); CHECK (lo.changes == 0);

        // An edge cannot be dragged through the other one: a minimum width remains.
        v.beginDrag (at (2000.0));
        v.continueDrag (at (100.0));
        v.endDrag();
        CHECK_THAT ((double) plain (r.proc, "b3_fHi"), WithinRel (400.0 * std::exp2 (ui::SpectrumView::minWidthOctaves), 2e-3));
        CHECK (plain (r.proc, "b3_fHi") > plain (r.proc, "b3_fLo"));

        r.proc.getState().getParameter ("b3_fLo")->removeListener (&lo);
        r.proc.getState().getParameter ("b3_fHi")->removeListener (&hi);
    }

    SECTION ("dragging the body moves both edges and keeps the width; columns re-order")
    {
        v.beginDrag (at (60.0));                                        // band 1: 20 - 120 Hz
        v.continueDrag (at (6000.0));                                   // two decades up
        v.endDrag();
        const double lo = plain (r.proc, "b1_fLo"), hi = plain (r.proc, "b1_fHi");
        CHECK_THAT (lo, WithinRel (2000.0, 5e-3));
        CHECK_THAT (hi / lo, WithinRel (6.0, 1e-3));
        ed.refreshFromProcessor();
        CHECK (ed.getColumnOrder() == Slots { 2, 3, 1 });

        // The body stops at the ends of the display.
        v.beginDrag (at (5000.0));
        v.continueDrag (at (20000.0));
        v.continueDrag ({ v.plotBounds().getRight() + 500.0f, y });
        v.endDrag();
        CHECK_THAT ((double) plain (r.proc, "b1_fHi"), WithinRel (20000.0, 1e-3));
        CHECK_THAT ((double) plain (r.proc, "b1_fHi") / (double) plain (r.proc, "b1_fLo"), WithinRel (6.0, 1e-3));
    }

    SECTION ("one band object: the dot moves the band and sets Makeup, the arrow sets the dynamics")
    {
        using Part = ui::SpectrumView::Part;
        GestureCounter range, dir, makeup;
        r.proc.getState().getParameter ("b2_range")->addListener (&range);
        r.proc.getState().getParameter ("b2_dynDir")->addListener (&dir);
        r.proc.getState().getParameter ("b2_makeup")->addListener (&makeup);
        const auto dbToPixels = [&] (double db) { return v.yForResponseDb (db) - v.yForResponseDb (0.0); };

        // Band 2 (120 - 400 Hz, Makeup 0, Down 24 dB): the dot sits on the static curve at its
        // centre (0 dB), the arrow marker below it at the Range limit.
        CHECK_THAT (v.centreHz (2), WithinRel (std::sqrt (120.0 * 400.0), 1e-6));
        CHECK_THAT (v.dynamicsDb (2), WithinAbs (-24.0, 1e-6));
        const auto dot = v.dotPosition (2), arrow = v.arrowPosition (2);
        CHECK_THAT ((double) dot.y, WithinAbs ((double) v.yForResponseDb (0.0), 1.0));
        CHECK (arrow.y > dot.y + ui::SpectrumView::arrowMinOffset - 0.01f);
        CHECK (v.hitTestAt (dot).slot == 2);
        CHECK (v.hitTestAt (dot).part == Part::Dot);
        CHECK (v.hitTestAt (arrow).part == Part::Arrow);

        // The arrow: up by 18 dB leaves Down 6 dB; through the static curve it becomes Up.
        v.beginDrag (arrow);
        v.continueDrag (arrow.translated (0.0f, dbToPixels (18.0)));
        CHECK_THAT ((double) plain (r.proc, "b2_range"), WithinAbs (6.0, 0.06));
        CHECK (plain (r.proc, "b2_dynDir") == 0.0f);
        v.continueDrag (arrow.translated (0.0f, dbToPixels (33.0)));
        v.endDrag();
        CHECK_THAT ((double) plain (r.proc, "b2_range"), WithinAbs (9.0, 0.06));
        CHECK (plain (r.proc, "b2_dynDir") == 1.0f);
        CHECK (range.begins == 1); CHECK (range.ends == 1);
        CHECK (dir.begins == 1);   CHECK (dir.ends == 1);   CHECK (dir.changes == 1);
        CHECK (makeup.begins == 0);
        CHECK_THAT ((double) plain (r.proc, "b2_fLo"), WithinRel (120.0, 1e-6));     // nothing else moved
        CHECK (plain (r.proc, "b2_makeup") == 0.0f);

        // For an Up band the marker sits above the dot.
        ed.refreshFromProcessor();
        CHECK (v.arrowPosition (2).y < v.dotPosition (2).y);

        // The dot: an octave up and 6 dB up in one drag, inside one gesture.
        const auto d0 = v.dotPosition (2);
        v.beginDrag (d0);
        v.continueDrag ({ v.xForFrequency (2.0 * v.centreHz (2)), d0.y + dbToPixels (6.0) });
        v.endDrag();
        CHECK_THAT ((double) plain (r.proc, "b2_makeup"), WithinAbs (6.0, 0.06));
        CHECK_THAT ((double) plain (r.proc, "b2_fLo"), WithinRel (240.0, 5e-3));
        CHECK_THAT ((double) plain (r.proc, "b2_fHi") / (double) plain (r.proc, "b2_fLo"), WithinRel (400.0 / 120.0, 1e-3));
        CHECK (makeup.begins == 1); CHECK (makeup.ends == 1);
        CHECK_THAT ((double) plain (r.proc, "b2_range"), WithinAbs (9.0, 0.06));       // the dynamics stay

        // The static curve follows Makeup, and the dot stays on it.
        ed.refreshFromProcessor();
        CHECK_THAT ((double) v.dotPosition (2).y, WithinAbs ((double) v.yForResponseDb (6.0), 1.5));

        // The rest of the band only moves it sideways: up/down changes neither Makeup nor Range.
        const auto body = juce::Point<float> (v.xForFrequency (300.0), v.yForResponseDb (-20.0));
        REQUIRE (v.hitTestAt (body).part == Part::Body);
        v.beginDrag (body);
        v.continueDrag (body.translated (0.0f, -60.0f));
        v.endDrag();
        CHECK_THAT ((double) plain (r.proc, "b2_makeup"), WithinAbs (6.0, 0.06));
        CHECK_THAT ((double) plain (r.proc, "b2_range"), WithinAbs (9.0, 0.06));
        CHECK_THAT ((double) plain (r.proc, "b2_fLo"), WithinRel (240.0, 5e-3));

        // The wheel sets Width: twelve steps double it, twelve back land exactly on 100 %.
        v.stepWidth (2, 12.0f);
        CHECK_THAT ((double) plain (r.proc, "b2_width"), WithinAbs (200.0, 0.06));
        v.stepWidth (2, -12.0f);
        CHECK (plain (r.proc, "b2_width") == 100.0f);

        r.proc.getState().getParameter ("b2_range")->removeListener (&range);
        r.proc.getState().getParameter ("b2_dynDir")->removeListener (&dir);
        r.proc.getState().getParameter ("b2_makeup")->removeListener (&makeup);
    }

    SECTION ("dragging across empty space creates a band over that range (undoable)")
    {
        v.beginDrag (at (3000.0));
        v.continueDrag ({ v.xForFrequency (3000.0) + 2.0f, y });        // below the threshold: nothing yet
        CHECK (r.proc.getBandSlots().usedSlots().size() == 3u);
        v.continueDrag (at (5000.0));
        v.continueDrag (at (9000.0));
        v.endDrag();

        CHECK (r.proc.getBandSlots().usedSlots() == Slots { 1, 2, 3, 4 });
        CHECK_THAT ((double) plain (r.proc, "b4_fLo"), WithinRel (3000.0, 2e-3));
        CHECK_THAT ((double) plain (r.proc, "b4_fHi"), WithinRel (9000.0, 2e-3));
        CHECK (ed.getSelectedSlot() == 4);
        CHECK (ed.getColumn (4).isVisible());

        // Right to left works too.
        v.beginDrag (at (18000.0));
        v.continueDrag (at (12000.0));
        v.endDrag();
        CHECK_THAT ((double) plain (r.proc, "b5_fLo"), WithinRel (12000.0, 2e-3));
        CHECK_THAT ((double) plain (r.proc, "b5_fHi"), WithinRel (18000.0, 2e-3));

        REQUIRE (r.proc.getUndoManager().undo());
        CHECK (r.proc.getBandSlots().usedSlots() == Slots { 1, 2, 3, 4 });

        // With all eight slots in use a drag on empty space does nothing.
        while (r.proc.getBandSlots().createBand() != 0) {}
        ed.refreshFromProcessor();
        for (int s = 1; s <= 8; ++s) { test::setBand (r.proc, s, "fLo", 30.0f); test::setBand (r.proc, s, "fHi", 60.0f); }
        ed.refreshFromProcessor();
        v.beginDrag (at (3000.0));
        v.continueDrag (at (9000.0));
        v.endDrag();
        CHECK (r.proc.getBandSlots().usedSlots().size() == 8u);
        CHECK_THAT ((double) plain (r.proc, "b1_fLo"), WithinRel (30.0, 1e-5));
    }

    SECTION ("closing the editor in the middle of a drag ends the host gesture")
    {
        GestureCounter hi;
        r.proc.getState().getParameter ("b3_fHi")->addListener (&hi);
        v.beginDrag (at (1000.0));
        v.continueDrag (at (1800.0));
        r.holder.reset();
        CHECK (hi.begins == 1);
        CHECK (hi.ends == 1);
        r.proc.getState().getParameter ("b3_fHi")->removeListener (&hi);
    }
}

TEST_CASE ("Spectrum view: the response curve is what the engine does", "[ui][spectrum]")
{
    Rig r;
    REQUIRE (r.editor != nullptr);
    auto& v = r.view();
    r.proc.setEngineKind (dsp::EngineKind::A3);           // compared below with the A3 engine (A-prime: EditorTests)
    r.proc.getBandSlots().createBand();
    test::setBand (r.proc, 1, "fLo", 500.0f);
    test::setBand (r.proc, 1, "fHi", 2000.0f);
    test::setBand (r.proc, 1, "thresh", 12.0f);            // no compression: the curve shows makeup and mix only
    r.editor->refreshFromProcessor();

    CHECK (v.bandGain (1) == 1.0);
    for (double f : { 50.0, 1000.0, 9000.0 })
        CHECK (v.responseDbAt (f) == 0.0);

    // Makeup: the drawn curve equals the response measured from the production engine.
    test::setBand (r.proc, 1, "makeup", -9.0f);
    r.editor->refreshFromProcessor();
    CHECK_THAT (v.bandGain (1), WithinRel (std::pow (10.0, -9.0 / 20.0), 1e-6));

    const auto impulse = test::engineImpulseResponse (48000.0, { { 500.0, 2000.0, -9.0, 1 } }, 1 << 14);
    const test::MeasuredResponse measured (impulse, 48000.0);
    for (double f : { 100.0, 400.0, 700.0, 1000.0, 1500.0, 2500.0, 8000.0 })
    {
        INFO ("f " << f);
        CHECK_THAT (v.responseDbAt (f), WithinAbs (test::magDb (measured.exact (f)), 0.02));
    }

    // Band mix scales the change; bypass and the plugin bypass make the band neutral.
    test::setBand (r.proc, 1, "mix", 50.0f);
    r.editor->refreshFromProcessor();
    CHECK_THAT (v.bandGain (1), WithinRel (1.0 + 0.5 * (std::pow (10.0, -9.0 / 20.0) - 1.0), 1e-6));
    test::setBand (r.proc, 1, "bypass", 1.0f);
    r.editor->refreshFromProcessor();
    CHECK (v.bandGain (1) == 1.0);
    test::setBand (r.proc, 1, "bypass", 0.0f);
    test::setPlain (r.proc, "bypass", 1.0f);
    r.editor->refreshFromProcessor();
    CHECK (v.bandGain (1) == 1.0);
    test::setPlain (r.proc, "bypass", 0.0f);

    // Compression: the curve follows the gain-reduction meter.
    test::setBand (r.proc, 1, "makeup", 0.0f);
    test::setBand (r.proc, 1, "mix", 100.0f);
    test::setBand (r.proc, 1, "thresh", -40.0f);
    test::setBand (r.proc, 1, "ratio", 8.0f);
    test::OfflineRenderer renderer (r.proc);
    REQUIRE (renderer.prepare (48000.0, 512, {}));
    renderer.render (test::makeNoise (2, 24000, 5u, 0.6f), nullptr, { 512 });
    r.editor->refreshFromProcessor();
    const double gr = (double) juce::jmin (r.proc.getGainReductionMeter().getGainReductionDb (1, 0),
                                           r.proc.getGainReductionMeter().getGainReductionDb (1, 1));
    REQUIRE (gr < -3.0);
    CHECK_THAT (20.0 * std::log10 (v.bandGain (1)), WithinAbs (gr, 1e-4));
    CHECK_THAT (v.responseDbAt (test::digitalCentre (500.0, 2000.0, 48000.0)), WithinAbs (gr, 0.01));

    // The analyzers received the audio (input above output inside the compressed band).
    CHECK (v.inputAnalyzer().averageLevelDb (800.0, 1250.0) > -60.0f);
    CHECK (v.outputAnalyzer().averageLevelDb (800.0, 1250.0) < v.inputAnalyzer().averageLevelDb (800.0, 1250.0) - 2.0f);

    // With no audio arriving the display falls back to silence instead of freezing.
    for (int i = 0; i < 120; ++i) r.editor->refreshFromProcessor();
    CHECK (v.inputAnalyzer().averageLevelDb (800.0, 1250.0) < -100.0f);
}
