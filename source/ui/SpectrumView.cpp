#include "SpectrumView.h"

#include <cmath>

#include "ValueFormat.h"
#include "dsp/A3Model.h"

namespace tl::ui
{
namespace
{
constexpr float kLeftInset = 36.0f, kRightInset = 36.0f, kTopInset = 28.0f, kBottomInset = 18.0f;
constexpr int kCurvePoints = 240;
}

SpectrumView::SpectrumView (PluginProcessor& p) : processor (p)
{
    auto& apvts = processor.getState();
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        auto& bp = params[(size_t) slot - 1];
        const auto id = [slot] (const char* suffix) { return juce::String (schema::bandParamId (slot, suffix)); };
        bp.lo = apvts.getParameter (id (schema::band::fLo));
        bp.hi = apvts.getParameter (id (schema::band::fHi));
        bp.loValue = apvts.getRawParameterValue (id (schema::band::fLo));
        bp.hiValue = apvts.getRawParameterValue (id (schema::band::fHi));
        bp.bypass = apvts.getRawParameterValue (id (schema::band::bypass));
        bp.makeup = apvts.getRawParameterValue (id (schema::band::makeup));
        bp.mix = apvts.getRawParameterValue (id (schema::band::mix));
        bp.width = apvts.getRawParameterValue (id (schema::band::width));
        bp.range = apvts.getParameter (id (schema::band::range));
        bp.dynDir = apvts.getParameter (id (schema::band::dynDir));
        bp.rangeValue = apvts.getRawParameterValue (id (schema::band::range));
        bp.dynDirValue = apvts.getRawParameterValue (id (schema::band::dynDir));
        jassert (bp.lo != nullptr && bp.hi != nullptr && bp.bypass != nullptr && bp.makeup != nullptr && bp.mix != nullptr && bp.width != nullptr
                 && bp.range != nullptr && bp.dynDir != nullptr);
    }
    globalBypass = apvts.getRawParameterValue (schema::global::bypass);

    setTitle ("Spectrum display");
    setDescription ("Shows the input and output spectrum and the frequency range of every band. "
                    "Band ranges can also be edited with the Low Freq and High Freq controls.");
    setComponentID ("action_spectrum");
    setOpaque (true);

    processor.getInputAnalyzerFifo().clear();
    processor.getOutputAnalyzerFifo().clear();
    processor.addAnalyzerClient();
}

SpectrumView::~SpectrumView()
{
    endDrag();
    processor.removeAnalyzerClient();
}

void SpectrumView::setSelectedSlot (int slot)
{
    if (slot != selectedSlot)
    {
        selectedSlot = slot;
        repaint();
    }
}

// ---- Coordinates ------------------------------------------------------------------------------
juce::Rectangle<float> SpectrumView::plotBounds() const noexcept
{
    auto r = getLocalBounds().toFloat();
    r.removeFromLeft (kLeftInset);
    r.removeFromRight (kRightInset);
    r.removeFromTop (kTopInset);
    r.removeFromBottom (kBottomInset);
    return r;
}

float SpectrumView::xForFrequency (double hz) const noexcept
{
    const auto plot = plotBounds();
    const double t = std::log (juce::jmax (1.0e-3, hz) / minHz) / std::log (maxHz / minHz);
    return plot.getX() + (float) t * plot.getWidth();
}

double SpectrumView::frequencyForX (float x) const noexcept
{
    const auto plot = plotBounds();
    const double t = plot.getWidth() > 0.0f ? (double) (x - plot.getX()) / (double) plot.getWidth() : 0.0;
    return minHz * std::exp (t * std::log (maxHz / minHz));
}

float SpectrumView::yForResponseDb (double db) const noexcept
{
    const auto plot = plotBounds();
    return plot.getY() + (float) ((topDb - db) / (topDb - bottomDb)) * plot.getHeight();
}

float SpectrumView::yForLevelDb (double dbfs) const noexcept
{
    const auto plot = plotBounds();
    return plot.getY() + (float) ((levelTopDb - dbfs) / (levelTopDb - levelBottomDb)) * plot.getHeight();
}

// ---- Data ---------------------------------------------------------------------------------------
void SpectrumView::refresh (bool audioRunning)
{
    const double fs = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
    analyzerIn.setSampleRate (fs);
    analyzerOut.setSampleRate (fs);

    const int got = analyzerIn.pull (processor.getInputAnalyzerFifo()) + analyzerOut.pull (processor.getOutputAnalyzerFifo());
    idleFrames = got > 0 ? 0 : idleFrames + 1;

    if (idleFrames > 6)
    {
        // No audio is arriving (transport stopped, host idle): let the display fall to silence.
        static const float zeros[1024] {};
        analyzerIn.push (zeros, 1024);
        analyzerOut.push (zeros, 1024);
    }

    analyzerIn.compute();
    analyzerOut.compute();

    const auto mask = processor.getBandSlots().usedMaskSnapshot();
    const bool allBypassed = globalBypass != nullptr && globalBypass->load (std::memory_order_relaxed) >= 0.5f;
    const auto& meter = processor.getGainReductionMeter();

    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        const auto& bp = params[(size_t) slot - 1];
        auto& shown = bandsShown[(size_t) slot - 1];
        shown.used = (mask & (1u << (slot - 1))) != 0;
        shown.bypassed = bp.bypass->load (std::memory_order_relaxed) >= 0.5f;
        shown.lo = (double) bp.loValue->load (std::memory_order_relaxed);
        shown.hi = (double) bp.hiValue->load (std::memory_order_relaxed);
        const auto fe = dsp::widenedEdges ((float) shown.lo, (float) shown.hi, bp.width->load (std::memory_order_relaxed));
        shown.filterLo = (double) fe.loHz;
        shown.filterHi = (double) fe.hiHz;
        if (! (dragActive && bodyAxis == Axis::Vertical && dragHit.slot == slot))   // the drag sets these itself
        {
            shown.up = bp.dynDirValue->load (std::memory_order_relaxed) >= 0.5f;
            const double r = (double) bp.rangeValue->load (std::memory_order_relaxed);
            shown.dynamicsDb = shown.up ? r : -r;
        }

        // Gain the engine applies right now: compression (the deeper of the two meter channels,
        // last block), makeup and band mix. A bypassed or unused band is neutral.
        double gain = 1.0;
        if (shown.used && ! shown.bypassed && ! allBypassed)
        {
            const double grDb = audioRunning ? (double) juce::jmin (meter.getGainReductionDb (slot, 0), meter.getGainReductionDb (slot, 1)) : 0.0;
            // The meter shows the amount as a reduction in both directions; Up applies its mirror.
            const double actionDb = shown.up ? -grDb : grDb;
            const double wet = std::pow (10.0, (actionDb + (double) bp.makeup->load (std::memory_order_relaxed)) / 20.0);
            const double mix = juce::jlimit (0.0, 1.0, (double) bp.mix->load (std::memory_order_relaxed) * 0.01);
            gain = 1.0 + mix * (wet - 1.0);
        }
        shown.gain = gain;
    }

    repaint();
}

double SpectrumView::responseDbAt (double hz) const
{
    const double fs = analyzerIn.sampleRate();
    if (hz >= 0.5 * fs) return 0.0;

    std::complex<double> h (1.0, 0.0);
    for (const auto& b : bandsShown)
    {
        if (! b.used) continue;
        double lo = b.filterLo, hi = b.filterHi;
        dsp::a3ClampEdges (fs, lo, hi);
        h *= dsp::regionResponse (processor.getEngineKind(), hz, fs, lo, hi, b.gain);
    }
    return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (h)));
}

// ---- Interaction --------------------------------------------------------------------------------
SpectrumView::Hit SpectrumView::hitTestAt (juce::Point<float> p) const
{
    const auto plot = plotBounds();
    if (! plot.expanded (edgeGrabPixels, 0.0f).contains (p))
        return {};

    // Edges first (the selected band wins a tie), then the narrowest region under the pointer.
    Hit best;
    float bestDistance = edgeGrabPixels + 1.0f;
    const auto considerEdge = [&] (int slot, Part part, float x)
    {
        const float d = std::abs (p.x - x) - (slot == selectedSlot ? 0.5f : 0.0f);
        if (std::abs (p.x - x) <= edgeGrabPixels && d < bestDistance)
        {
            bestDistance = d;
            best = { slot, part };
        }
    };

    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        const auto& b = bandsShown[(size_t) slot - 1];
        if (! b.used) continue;
        considerEdge (slot, Part::LowEdge, xForFrequency (juce::jmin (b.lo, b.hi)));
        considerEdge (slot, Part::HighEdge, xForFrequency (juce::jmax (b.lo, b.hi)));
    }
    if (best.slot != 0)
        return best;

    float bestWidth = 1.0e9f;
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        const auto& b = bandsShown[(size_t) slot - 1];
        if (! b.used) continue;
        const float x0 = xForFrequency (juce::jmin (b.lo, b.hi)), x1 = xForFrequency (juce::jmax (b.lo, b.hi));
        if (p.x >= x0 && p.x <= x1)
        {
            const float width = (x1 - x0) - (slot == selectedSlot ? 0.5f : 0.0f);
            if (width < bestWidth)
            {
                bestWidth = width;
                best = { slot, Part::Body };
            }
        }
    }
    return best;
}

void SpectrumView::beginGesture (int slot)
{
    if (gestureOpen || slot < 1) return;
    auto& bp = params[(size_t) slot - 1];
    gestureParams = { bp.lo, bp.hi };
    for (auto* prm : gestureParams) prm->beginChangeGesture();
    gestureOpen = true;
    gestureSlot = slot;
}

void SpectrumView::beginDynamicsGesture (int slot)
{
    if (gestureOpen || slot < 1) return;
    auto& bp = params[(size_t) slot - 1];
    gestureParams = { bp.range, bp.dynDir };
    for (auto* prm : gestureParams) prm->beginChangeGesture();
    gestureOpen = true;
    gestureSlot = slot;
}

void SpectrumView::setDynamics (int slot, double signedDb)
{
    if (slot < 1 || slot > schema::kActiveSlots) return;
    auto& bp = params[(size_t) slot - 1];
    const double s = juce::jlimit (-maxDynamicsDb, maxDynamicsDb, std::round (signedDb * 10.0) / 10.0);
    const bool up = s > 0.0;
    const float range = (float) std::abs (s);

    if (std::abs (bp.rangeValue->load (std::memory_order_relaxed) - range) > 1.0e-4f)
        bp.range->setValueNotifyingHost (bp.range->convertTo0to1 (range));
    const bool wasUp = bp.dynDirValue->load (std::memory_order_relaxed) >= 0.5f;
    if (up != wasUp && range > 0.0f)                                  // at 0 dB the direction is kept
        bp.dynDir->setValueNotifyingHost (up ? 1.0f : 0.0f);

    auto& shown = bandsShown[(size_t) slot - 1];
    shown.up = range > 0.0f ? up : wasUp;
    shown.dynamicsDb = s;
    repaint();
}

void SpectrumView::setEdges (int slot, double lo, double hi)
{
    auto& bp = params[(size_t) slot - 1];
    const auto set = [] (juce::RangedAudioParameter* prm, std::atomic<float>* current, double hz)
    {
        const float v = (float) hz;
        if (std::abs (current->load (std::memory_order_relaxed) - v) > 1.0e-4f * v)
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };
    set (bp.lo, bp.loValue, lo);
    set (bp.hi, bp.hiValue, hi);

    auto& shown = bandsShown[(size_t) slot - 1];
    shown.lo = lo;
    shown.hi = hi;
    repaint();
}

void SpectrumView::beginDrag (juce::Point<float> p)
{
    endDrag();
    dragHit = hitTestAt (p);
    dragStart = p;
    dragActive = true;

    if (dragHit.slot != 0)
    {
        const auto& b = bandsShown[(size_t) dragHit.slot - 1];
        dragStartLo = juce::jmin (b.lo, b.hi);
        dragStartHi = juce::jmax (b.lo, b.hi);
        dragStartDynamics = b.dynamicsDb;
        bodyAxis = Axis::Undecided;
        if (onSelect) onSelect (dragHit.slot);
    }
}

void SpectrumView::continueDrag (juce::Point<float> p)
{
    if (! dragActive) return;

    const double minRatio = std::exp2 (minWidthOctaves);
    const double f = juce::jlimit (minHz, maxHz, frequencyForX (p.x));

    if (dragHit.slot == 0)
    {
        // Dragging across empty space creates a band over the dragged range.
        if (std::abs (p.x - dragStart.x) < createDragPixels || ! plotBounds().expanded (edgeGrabPixels, 0.0f).contains (dragStart))
            return;

        const int slot = processor.getBandSlots().createBand();
        if (slot == 0) { dragActive = false; return; }            // all eight slots are in use

        const double start = juce::jlimit (minHz, maxHz, frequencyForX (dragStart.x));
        auto& shown = bandsShown[(size_t) slot - 1];
        shown.used = true;
        shown.gain = 1.0;
        dragHit = { slot, f >= start ? Part::HighEdge : Part::LowEdge };
        dragStartLo = dragStartHi = start;
        beginGesture (slot);
        if (onSelect) onSelect (slot);
    }

    if (dragHit.part == Part::Body)
    {
        // A body drag picks its axis once it has moved a few pixels: up/down sets the band's
        // dynamics, sideways moves the band.
        if (bodyAxis == Axis::Undecided)
        {
            const float dx = std::abs (p.x - dragStart.x), dy = std::abs (p.y - dragStart.y);
            if (juce::jmax (dx, dy) < axisLockPixels) return;
            bodyAxis = dy >= dx ? Axis::Vertical : Axis::Horizontal;
            setMouseCursor (bodyAxis == Axis::Vertical ? juce::MouseCursor::UpDownResizeCursor
                                                       : juce::MouseCursor::LeftRightResizeCursor);
        }

        if (bodyAxis == Axis::Vertical)
        {
            // The handle follows the pointer on the response scale (dB per pixel of the plot).
            const double dbPerPixel = (topDb - bottomDb) / juce::jmax (1.0, (double) plotBounds().getHeight());
            beginDynamicsGesture (dragHit.slot);
            setDynamics (dragHit.slot, dragStartDynamics - (double) (p.y - dragStart.y) * dbPerPixel);
            return;
        }
    }

    beginGesture (dragHit.slot);

    switch (dragHit.part)
    {
        case Part::LowEdge:
            setEdges (dragHit.slot, juce::jmin (f, dragStartHi / minRatio), juce::jmax (dragStartHi, minHz * minRatio));
            break;

        case Part::HighEdge:
            setEdges (dragHit.slot, juce::jmin (dragStartLo, maxHz / minRatio), juce::jmax (f, dragStartLo * minRatio));
            break;

        case Part::Body:
        {
            // Move both edges by the same number of octaves, keeping the width, inside the view.
            const double startF = frequencyForX (dragStart.x);
            double shift = f / juce::jmax (1.0e-3, startF);
            shift = juce::jlimit (minHz / dragStartLo, maxHz / dragStartHi, shift);
            if (dragStartLo < minHz || dragStartHi > maxHz) shift = f / juce::jmax (1.0e-3, startF);   // band partly outside: free move
            setEdges (dragHit.slot, dragStartLo * shift, dragStartHi * shift);
            break;
        }

        case Part::None:
            break;
    }
}

void SpectrumView::endDrag()
{
    if (gestureOpen)
    {
        for (auto* prm : gestureParams) prm->endChangeGesture();
        gestureParams.clear();
        gestureOpen = false;
    }
    if (bodyAxis != Axis::Undecided)
        setMouseCursor (hover.part == Part::Body ? juce::MouseCursor::UpDownResizeCursor : getMouseCursor());
    dragActive = false;
    dragHit = {};
    bodyAxis = Axis::Undecided;
}

void SpectrumView::mouseDown (const juce::MouseEvent& e) { beginDrag (e.position); }
void SpectrumView::mouseDrag (const juce::MouseEvent& e) { continueDrag (e.position); }
void SpectrumView::mouseUp (const juce::MouseEvent&)     { endDrag(); }

void SpectrumView::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitTestAt (e.position);
    if (h.slot != hover.slot || h.part != hover.part)
    {
        hover = h;
        setMouseCursor (h.part == Part::LowEdge || h.part == Part::HighEdge ? juce::MouseCursor::LeftRightResizeCursor
                        : h.part == Part::Body ? juce::MouseCursor::UpDownResizeCursor
                                               : juce::MouseCursor::CrosshairCursor);
        repaint();
    }
}

void SpectrumView::mouseExit (const juce::MouseEvent&)
{
    hover = {};
    repaint();
}

// ---- Drawing ------------------------------------------------------------------------------------
void SpectrumView::paint (juce::Graphics& g)
{
    const auto plot = plotBounds();
    theme::drawWell (g, getLocalBounds().toFloat());

    g.setColour (theme::textMid);
    theme::drawTracked (g, juce::String::fromUTF8 ("SPECTRUM \xc2\xb7 BANDS"), { 14.0f, 6.0f, 300.0f, 16.0f },
                        juce::Justification::centredLeft, theme::label (10.0f, 0.28f));

    drawGrid (g, plot);

    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());
    drawSpectrum (g, plot, analyzerIn, theme::textLow.withAlpha (0.55f), false);
    drawSpectrum (g, plot, analyzerOut, theme::accent.withAlpha (0.75f), true);
    drawBands (g, plot);
    drawResponse (g, plot);
    g.restoreState();

    bool anyBand = false;
    for (const auto& b : bandsShown) anyBand = anyBand || b.used;
    if (! anyBand)
    {
        g.setColour (theme::textLow);
        g.setFont (theme::label (13.0f));
        g.drawText ("Drag across the spectrum to create a band", plot.toNearestInt(), juce::Justification::centred, false);
    }
}

void SpectrumView::drawGrid (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    g.setFont (theme::mono (9.0f));
    const juce::Colour gridLine (0xff1f242c);

    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = xForFrequency (f);
        g.setColour (gridLine);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (theme::textLow);
        const auto label = f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String (f, 0);
        g.drawText (label, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centred, false);
    }

    for (double db : { 12.0, 0.0, -12.0, -24.0 })
    {
        const float y = yForResponseDb (db);
        if (std::abs (db) < 1.0e-9)
        {
            // The zero line of the response, dashed.
            g.setColour (theme::border);
            for (float x = plot.getX(); x < plot.getRight(); x += 7.0f)
                g.fillRect (x, (float) juce::roundToInt (y), juce::jmin (3.0f, plot.getRight() - x), 1.0f);
        }
        else
        {
            g.setColour (gridLine);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        }
        g.setColour (theme::textLow);
        g.drawText ((db > 0.0 ? "+" : "") + juce::String (db, 0), juce::Rectangle<float> (0.0f, y - 7.0f, kLeftInset - 5.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }

    // Spectrum level scale (dBFS) on the right.
    g.setColour (theme::textLow);
    for (double db : { -24.0, -48.0, -72.0 })
        g.drawText (juce::String (db, 0), juce::Rectangle<float> (plot.getRight() + 4.0f, yForLevelDb (db) - 7.0f, kRightInset - 6.0f, 14.0f),
                    juce::Justification::centredLeft, false);
}

void SpectrumView::drawSpectrum (juce::Graphics& g, juce::Rectangle<float> plot, const analysis::SpectrumAnalyzer& a,
                                 juce::Colour colour, bool fill) const
{
    juce::Path path;
    const float step = 2.0f;
    bool started = false;

    for (float x = plot.getX(); x <= plot.getRight() + 0.5f; x += step)
    {
        // 1/6-octave power average: readable on noise-like material, still shows tonal peaks.
        const double f = frequencyForX (x);
        const float y = juce::jlimit (plot.getY(), plot.getBottom(),
                                      yForLevelDb ((double) a.averageLevelDb (f * 0.9439, f * 1.0595)));
        if (! started) { path.startNewSubPath (x, y); started = true; }
        else           path.lineTo (x, y);
    }

    if (fill)
    {
        juce::Path area (path);
        area.lineTo (plot.getRight(), plot.getBottom());
        area.lineTo (plot.getX(), plot.getBottom());
        area.closeSubPath();
        g.setColour (colour.withMultipliedAlpha (0.12f));
        g.fillPath (area);
    }

    g.setColour (colour);
    g.strokePath (path, juce::PathStrokeType (1.2f));
}

void SpectrumView::drawBands (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const double fs = analyzerIn.sampleRate();

    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        const auto& b = bandsShown[(size_t) slot - 1];
        if (! b.used) continue;

        const auto colour = theme::slotColour (slot);
        const bool selected = slot == selectedSlot;
        const float dim = b.bypassed ? 0.4f : 1.0f;
        const float x0 = xForFrequency (juce::jmin (b.lo, b.hi)), x1 = xForFrequency (juce::jmax (b.lo, b.hi));
        const auto region = juce::Rectangle<float> (x0, plot.getY(), juce::jmax (1.0f, x1 - x0), plot.getHeight());

        // Nominal region (what the parameters say) ...
        g.setColour (colour.withAlpha ((selected ? 0.16f : 0.08f) * dim));
        g.fillRect (region);

        for (auto part : { Part::LowEdge, Part::HighEdge })
        {
            const float x = part == Part::LowEdge ? x0 : x1;
            const bool hot = hover.slot == slot && hover.part == part;
            g.setColour (colour.withAlpha ((hot ? 1.0f : selected ? 0.8f : 0.45f) * dim));
            g.fillRect (juce::Rectangle<float> (x - (hot ? 1.5f : 0.75f), plot.getY(), hot ? 3.0f : 1.5f, plot.getHeight()));
        }

        // ... and the response this band really applies at its current gain (soft skirts).
        if (std::abs (b.gain - 1.0) > 1.0e-4)
        {
            double lo = b.filterLo, hi = b.filterHi;
            dsp::a3ClampEdges (fs, lo, hi);
            juce::Path curve;
            const float zero = yForResponseDb (0.0);
            curve.startNewSubPath (plot.getX(), zero);
            for (int i = 0; i <= kCurvePoints; ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / (float) kCurvePoints;
                const double f = frequencyForX (x);
                const double db = f < 0.5 * fs ? 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (dsp::regionResponse (processor.getEngineKind(), f, fs, lo, hi, b.gain)))) : 0.0;
                curve.lineTo (x, yForResponseDb (db));
            }
            curve.lineTo (plot.getRight(), zero);
            curve.closeSubPath();
            g.setColour (colour.withAlpha (0.28f * dim));
            g.fillPath (curve);
        }

        // Dynamics handle: a short bar at the band's Range on the response scale, below the zero
        // line for Down (compression), above it for Up (upward expansion). Drag the body to move it.
        {
            const bool hot = hover.slot == slot && hover.part == Part::Body;
            const bool dragging = dragActive && bodyAxis == Axis::Vertical && dragHit.slot == slot;
            const float y = juce::jlimit (plot.getY() + 2.0f, plot.getBottom() - 2.0f, yForResponseDb (b.dynamicsDb));
            const float inset = juce::jmin (6.0f, 0.2f * region.getWidth());
            const float bx0 = region.getX() + inset, bx1 = region.getRight() - inset;
            if (bx1 > bx0 + 2.0f)
            {
                const float alpha = (dragging || hot ? 1.0f : selected ? 0.75f : 0.35f) * dim;
                g.setColour (colour.withAlpha (alpha));
                g.fillRect (juce::Rectangle<float> (bx0, y - (dragging ? 1.5f : 1.0f), bx1 - bx0, dragging ? 3.0f : 2.0f));

                // A small arrow on the bar shows the direction.
                const float cx = region.getCentreX(), dir = b.dynamicsDb > 0.0 ? -1.0f : 1.0f;
                if (std::abs (b.dynamicsDb) > 0.05 && region.getWidth() > 18.0f)
                {
                    juce::Path arrow;
                    arrow.addTriangle (cx - 4.0f, y + dir * 3.0f, cx + 4.0f, y + dir * 3.0f, cx, y + dir * 8.0f);
                    g.fillPath (arrow);
                }
            }

            if (dragging || hot)
            {
                const auto text = std::abs (b.dynamicsDb) < 0.05 ? juce::String ("0 dB")
                                : (b.dynamicsDb > 0.0 ? juce::String ("UP ") : juce::String ("DOWN "))
                                      + juce::String (std::abs (b.dynamicsDb), 1) + " dB";
                const auto font = theme::mono (11.0f);
                const float w = theme::textWidth (font, text) + 10.0f;
                auto label = juce::Rectangle<float> (w, 16.0f).withCentre ({ region.getCentreX(), y + (b.dynamicsDb > 0.0 ? -16.0f : 16.0f) });
                label = label.constrainedWithin (plot);
                g.setColour (theme::background.withAlpha (0.85f));
                g.fillRect (label);
                g.setColour (colour);
                g.setFont (font);
                g.drawText (text, label, juce::Justification::centred, false);
            }
        }

        // Slot chip: the number is always shown with the colour.
        const auto chip = juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ region.getCentreX(), plot.getY() + 12.0f });
        g.setColour (colour.withAlpha (dim));
        g.fillRect (chip);
        g.setColour (theme::background);
        g.setFont (theme::mono (11.0f));
        g.drawText (juce::String (slot), chip, juce::Justification::centred, false);
    }
}

void SpectrumView::drawResponse (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    juce::Path curve;
    for (int i = 0; i <= kCurvePoints; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / (float) kCurvePoints;
        const float y = yForResponseDb (responseDbAt (frequencyForX (x)));
        if (i == 0) curve.startNewSubPath (x, y);
        else        curve.lineTo (x, y);
    }
    // Ice with a soft glow: the curve is what the bands do together.
    g.setColour (theme::accent.withAlpha (0.18f));
    g.strokePath (curve, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (theme::accent);
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
} // namespace tl::ui
