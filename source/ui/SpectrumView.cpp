#include "SpectrumView.h"

#include <cmath>

#include "ValueFormat.h"
#include "dsp/A3Model.h"

namespace tl::ui
{
namespace
{
constexpr float kLeftInset = 36.0f, kRightInset = 36.0f, kTopInset = 28.0f, kBottomInset = 18.0f;
constexpr float kCurveStepPixels = 2.0f;   // the shared curve and every band curve use the same columns

// Linear gain of a band at `db` through its mix (what the engine applies with that much gain).
double blendGain (double db, double mix) noexcept
{
    return 1.0 + juce::jlimit (0.0, 1.0, mix) * (std::pow (10.0, db / 20.0) - 1.0);
}

juce::MouseCursor cursorFor (SpectrumView::Part part)
{
    using P = SpectrumView::Part;
    switch (part)
    {
        case P::Dot:      return juce::MouseCursor::UpDownLeftRightResizeCursor;
        case P::Arrow:    return juce::MouseCursor::UpDownResizeCursor;
        case P::LowEdge:
        case P::HighEdge:
        case P::Body:     return juce::MouseCursor::LeftRightResizeCursor;
        case P::None:     break;
    }
    return juce::MouseCursor::CrosshairCursor;
}
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
        bp.makeupParam = apvts.getParameter (id (schema::band::makeup));
        bp.widthParam = apvts.getParameter (id (schema::band::width));
        bp.range = apvts.getParameter (id (schema::band::range));
        bp.dynDir = apvts.getParameter (id (schema::band::dynDir));
        bp.rangeValue = apvts.getRawParameterValue (id (schema::band::range));
        bp.dynDirValue = apvts.getRawParameterValue (id (schema::band::dynDir));
        jassert (bp.lo != nullptr && bp.hi != nullptr && bp.bypass != nullptr && bp.makeup != nullptr && bp.mix != nullptr && bp.width != nullptr
                 && bp.range != nullptr && bp.dynDir != nullptr
                 && bp.makeupParam != nullptr && bp.widthParam != nullptr);
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
        if (! (dragActive && dragHit.part == Part::Arrow && dragHit.slot == slot))   // the drag sets these itself
        {
            shown.up = bp.dynDirValue->load (std::memory_order_relaxed) >= 0.5f;
            const double r = (double) bp.rangeValue->load (std::memory_order_relaxed);
            shown.dynamicsDb = shown.up ? r : -r;
        }

        shown.makeupDb = (double) bp.makeup->load (std::memory_order_relaxed);
        shown.mix = juce::jlimit (0.0, 1.0, (double) bp.mix->load (std::memory_order_relaxed) * 0.01);
        shown.staticGain = blendGain (shown.makeupDb, shown.mix);
        shown.limitGain = blendGain (shown.makeupDb + shown.dynamicsDb, shown.mix);

        // Gain the engine applies right now: compression (the deeper of the two meter channels,
        // last block), makeup and band mix. A bypassed or unused band is neutral.
        double gain = 1.0;
        shown.sounding = shown.used && ! shown.bypassed && ! allBypassed;
        if (shown.sounding)
        {
            const double grDb = audioRunning ? (double) juce::jmin (meter.getGainReductionDb (slot, 0), meter.getGainReductionDb (slot, 1)) : 0.0;
            // The meter shows the amount as a reduction in both directions; Up applies its mirror.
            const double actionDb = shown.up ? -grDb : grDb;
            gain = blendGain (actionDb + shown.makeupDb, shown.mix);
        }
        shown.gain = gain;
    }

    repaint();
}

std::complex<double> SpectrumView::bandResponse (const BandShown& b, double hz, double gain) const
{
    const double fs = analyzerIn.sampleRate();
    if (hz >= 0.5 * fs) return { 1.0, 0.0 };
    double lo = b.filterLo, hi = b.filterHi;
    dsp::a3ClampEdges (fs, lo, hi);
    return dsp::regionResponse (processor.getEngineKind(), hz, fs, lo, hi, gain);
}

double SpectrumView::responseDbOf (const BandShown& b, double hz, double gain) const
{
    return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (bandResponse (b, hz, gain))));
}

double SpectrumView::withOthersDb (int slot, double hz, double gain) const
{
    std::complex<double> h (1.0, 0.0);
    for (int s = 1; s <= schema::kActiveSlots; ++s)
    {
        const auto& o = bandsShown[(size_t) s - 1];
        if (s == slot)              h *= bandResponse (o, hz, gain);
        else if (o.used && o.sounding) h *= bandResponse (o, hz, o.restGain());
    }
    return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (h)));
}

double SpectrumView::centreHz (int slot) const noexcept
{
    const auto& b = bandsShown[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1];
    return std::sqrt (juce::jmax (1.0e-3, b.lo) * juce::jmax (1.0e-3, b.hi));
}

juce::Point<float> SpectrumView::dotPosition (int slot) const noexcept
{
    const auto plot = plotBounds();
    const auto& b = bandsShown[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1];
    const double c = centreHz (slot);
    const float y = yForResponseDb (withOthersDb (slot, c, b.staticGain));
    return { xForFrequency (c), juce::jlimit (plot.getY() + dotRadius, plot.getBottom() - dotRadius, y) };
}

juce::Point<float> SpectrumView::arrowPosition (int slot) const noexcept
{
    const auto plot = plotBounds();
    const auto& b = bandsShown[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1];
    const auto dot = dotPosition (slot);
    float y = yForResponseDb (withOthersDb (slot, centreHz (slot), b.limitGain));
    // Kept clear of the dot (on the side the band acts), so that it can always be grabbed.
    y = b.dynamicsDb > 0.0 ? juce::jmin (y, dot.y - arrowMinOffset) : juce::jmax (y, dot.y + arrowMinOffset);
    if (y < plot.getY() + 4.0f || y > plot.getBottom() - 4.0f)   // no room on that side: just inside the plot
        y = juce::jlimit (plot.getY() + 4.0f, plot.getBottom() - 4.0f, y);
    return { dot.x, y };
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

    Hit best;

    // The dot and the arrow marker first: the nearest one within reach (the selected band wins a tie).
    float bestHandle = 1.0e9f;
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        if (! bandsShown[(size_t) slot - 1].used) continue;
        const float bonus = slot == selectedSlot ? 0.5f : 0.0f;
        const float dDot = p.getDistanceFrom (dotPosition (slot)), dArrow = p.getDistanceFrom (arrowPosition (slot));
        if (dDot <= dotGrabPixels && dDot - bonus < bestHandle)       { bestHandle = dDot - bonus;   best = { slot, Part::Dot }; }
        if (dArrow <= arrowGrabPixels && dArrow - bonus < bestHandle) { bestHandle = dArrow - bonus; best = { slot, Part::Arrow }; }
    }
    if (best.slot != 0)
        return best;

    // Then the edges (the selected band wins a tie), then the narrowest band under the pointer.
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

void SpectrumView::openGesture (int slot, std::vector<juce::RangedAudioParameter*> prms)
{
    if (gestureOpen || slot < 1) return;
    gestureParams = std::move (prms);
    for (auto* prm : gestureParams) prm->beginChangeGesture();
    gestureOpen = true;
    gestureSlot = slot;
}

void SpectrumView::beginGesture (int slot)
{
    if (slot < 1) return;
    auto& bp = params[(size_t) slot - 1];
    openGesture (slot, { bp.lo, bp.hi });
}

void SpectrumView::beginDynamicsGesture (int slot)
{
    if (slot < 1) return;
    auto& bp = params[(size_t) slot - 1];
    openGesture (slot, { bp.range, bp.dynDir });
}

void SpectrumView::beginDotGesture (int slot)
{
    if (slot < 1) return;
    auto& bp = params[(size_t) slot - 1];
    openGesture (slot, { bp.lo, bp.hi, bp.makeupParam });
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
    shown.limitGain = blendGain (shown.makeupDb + s, shown.mix);
    repaint();
}

void SpectrumView::setMakeup (int slot, double db)
{
    if (slot < 1 || slot > schema::kActiveSlots) return;
    auto& bp = params[(size_t) slot - 1];
    const auto& r = bp.makeupParam->getNormalisableRange();
    const double v = juce::jlimit ((double) r.start, (double) r.end, std::round (db * 10.0) / 10.0);
    if (std::abs ((double) bp.makeup->load (std::memory_order_relaxed) - v) > 1.0e-4)
        bp.makeupParam->setValueNotifyingHost (bp.makeupParam->convertTo0to1 ((float) v));

    auto& shown = bandsShown[(size_t) slot - 1];
    shown.makeupDb = v;
    shown.staticGain = blendGain (v, shown.mix);
    shown.limitGain = blendGain (v + shown.dynamicsDb, shown.mix);
    repaint();
}

void SpectrumView::stepWidth (int slot, float steps)
{
    if (slot < 1 || slot > schema::kActiveSlots || ! std::isfinite (steps) || std::abs (steps) < 1.0e-6f) return;
    auto& bp = params[(size_t) slot - 1];
    const auto& r = bp.widthParam->getNormalisableRange();
    double w = (double) bp.width->load (std::memory_order_relaxed) * std::exp2 ((double) steps / 12.0);
    w = juce::jlimit ((double) r.start, (double) r.end, std::round (w * 10.0) / 10.0);
    if (std::abs (w - 100.0) < 0.75) w = 100.0;                       // 100 % (exactly as before) is easy to find again
    bp.widthParam->beginChangeGesture();
    bp.widthParam->setValueNotifyingHost (bp.widthParam->convertTo0to1 ((float) w));
    bp.widthParam->endChangeGesture();
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
        dragStartMakeup = b.makeupDb;
        setMouseCursor (cursorFor (dragHit.part));
        if (onSelect) onSelect (dragHit.slot);
    }
}

void SpectrumView::continueDrag (juce::Point<float> p)
{
    if (! dragActive) return;

    const double minRatio = std::exp2 (minWidthOctaves);
    const double f = juce::jlimit (minHz, maxHz, frequencyForX (p.x));
    const double dbPerPixel = (topDb - bottomDb) / juce::jmax (1.0, (double) plotBounds().getHeight());

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

    // Moves both edges by the same number of octaves, keeping the width, inside the view.
    const auto moveBand = [&]
    {
        const double startF = frequencyForX (dragStart.x);
        double shift = f / juce::jmax (1.0e-3, startF);
        shift = juce::jlimit (minHz / dragStartLo, maxHz / dragStartHi, shift);
        if (dragStartLo < minHz || dragStartHi > maxHz) shift = f / juce::jmax (1.0e-3, startF);   // band partly outside: free move
        setEdges (dragHit.slot, dragStartLo * shift, dragStartHi * shift);
    };

    switch (dragHit.part)
    {
        case Part::Dot:
            // The dot: sideways moves the band, up/down sets its Makeup (the static curve).
            beginDotGesture (dragHit.slot);
            moveBand();
            setMakeup (dragHit.slot, dragStartMakeup - (double) (p.y - dragStart.y) * dbPerPixel);
            break;

        case Part::Arrow:
            // The arrow marker: the Range limit, measured from the static curve. Below it
            // compresses (Down), above it expands upward (Up).
            beginDynamicsGesture (dragHit.slot);
            setDynamics (dragHit.slot, dragStartDynamics - (double) (p.y - dragStart.y) * dbPerPixel);
            break;

        case Part::LowEdge:
            beginGesture (dragHit.slot);
            setEdges (dragHit.slot, juce::jmin (f, dragStartHi / minRatio), juce::jmax (dragStartHi, minHz * minRatio));
            break;

        case Part::HighEdge:
            beginGesture (dragHit.slot);
            setEdges (dragHit.slot, juce::jmin (dragStartLo, maxHz / minRatio), juce::jmax (f, dragStartLo * minRatio));
            break;

        case Part::Body:
            beginGesture (dragHit.slot);
            moveBand();
            break;

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
    if (dragActive)
        setMouseCursor (cursorFor (hover.part));
    dragActive = false;
    dragHit = {};
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
        setMouseCursor (cursorFor (h.part));
        repaint();
    }
}

void SpectrumView::mouseExit (const juce::MouseEvent&)
{
    hover = {};
    repaint();
}

void SpectrumView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto h = hitTestAt (e.position);
    if (h.slot == 0 || dragActive)
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }
    const float delta = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
    stepWidth (h.slot, delta * 10.0f);          // about one twelfth of an octave per notch
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
    drawReadout (g, plot);

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
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        const auto& b = bandsShown[(size_t) slot - 1];
        if (! b.used) continue;

        const auto colour = theme::slotColour (slot);
        const bool selected = slot == selectedSlot;
        const bool active = (dragActive && dragHit.slot == slot) || hover.slot == slot;
        const float dim = b.bypassed ? 0.4f : 1.0f;
        const float x0 = xForFrequency (juce::jmin (b.lo, b.hi)), x1 = xForFrequency (juce::jmax (b.lo, b.hi));

        if (selected)
        {
            g.setColour (colour.withAlpha (0.05f * dim));
            g.fillRect (juce::Rectangle<float> (x0, plot.getY(), juce::jmax (1.0f, x1 - x0), plot.getHeight()));
        }

        // Edges: thin guides to grab (left/right arrows) to stretch or compress the band.
        for (auto part : { Part::LowEdge, Part::HighEdge })
        {
            const float x = part == Part::LowEdge ? x0 : x1;
            const bool hot = (hover.slot == slot && hover.part == part) || (dragActive && dragHit.slot == slot && dragHit.part == part);
            g.setColour (colour.withAlpha ((hot ? 0.9f : selected ? 0.4f : 0.16f) * dim));
            g.fillRect (juce::Rectangle<float> (x - (hot ? 1.0f : 0.5f), plot.getY(), hot ? 2.0f : 1.0f, plot.getHeight()));
        }

        // One band object, drawn on the shared curve: the static curve (this band at Makeup), the
        // Range limit along the same shape, and the area between them. The other bands sit at their
        // static level, so where this band stops acting its curves meet the shared curve exactly,
        // and the live curve (drawn after the bands) moves inside this band's area.
        // The curves cover the band's reach: wherever it moves the level by more than 0.05 dB.
        const int columns = juce::jmax (2, juce::roundToInt (plot.getWidth() / kCurveStepPixels));
        int first = -1, last = -1;
        for (int i = 0; i <= columns; ++i)
        {
            const double f = frequencyForX (plot.getX() + plot.getWidth() * (float) i / (float) columns);
            if (std::abs (responseDbOf (b, f, b.staticGain)) > 0.05 || std::abs (responseDbOf (b, f, b.limitGain)) > 0.05)
            {
                if (first < 0) first = i;
                last = i;
            }
        }
        if (first >= 0)
        {
            first = juce::jmax (0, first - 1);
            last = juce::jmin (columns, last + 1);

            juce::Path staticCurve, limitCurve, area;
            std::vector<juce::Point<float>> limitPoints;
            limitPoints.reserve ((size_t) (last - first + 1));
            for (int i = first; i <= last; ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / (float) columns;
                const double f = frequencyForX (x);
                const juce::Point<float> sp { x, yForResponseDb (withOthersDb (slot, f, b.staticGain)) };
                const juce::Point<float> lp { x, yForResponseDb (withOthersDb (slot, f, b.limitGain)) };
                if (i == first) { staticCurve.startNewSubPath (sp); limitCurve.startNewSubPath (lp); area.startNewSubPath (sp); }
                else            { staticCurve.lineTo (sp);          limitCurve.lineTo (lp);          area.lineTo (sp); }
                limitPoints.push_back (lp);
            }
            for (auto it = limitPoints.rbegin(); it != limitPoints.rend(); ++it)
                area.lineTo (*it);
            area.closeSubPath();

            // The dynamic area: shaded from the static curve to the Range limit, deeper towards the limit.
            const float y0 = yForResponseDb (withOthersDb (slot, centreHz (slot), b.staticGain));
            const float y1 = yForResponseDb (withOthersDb (slot, centreHz (slot), b.limitGain));
            const float areaAlpha = (active ? 0.34f : selected ? 0.26f : 0.11f) * dim;
            if (std::abs (y1 - y0) > 0.5f)
                g.setGradientFill (juce::ColourGradient (colour.withAlpha (areaAlpha * 0.35f), 0.0f, y0, colour.withAlpha (areaAlpha), 0.0f, y1, false));
            else
                g.setColour (colour.withAlpha (areaAlpha));
            g.fillPath (area);

            const juce::PathStrokeType thin (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            g.setColour (colour.withAlpha ((selected || active ? 0.55f : 0.3f) * dim));
            g.strokePath (limitCurve, thin);
            // At Makeup 0 the static curve is the shared curve itself: only the dot marks it then.
            if (std::abs (b.makeupDb) >= 0.05)
            {
                g.setColour (colour.withAlpha ((selected || active ? 1.0f : 0.7f) * dim));
                g.strokePath (staticCurve, juce::PathStrokeType (selected ? 1.6f : 1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }

        // Arrow marker at the Range limit: a short bar with the direction the band acts in.
        {
            const auto a = arrowPosition (slot);
            const bool hot = (hover.slot == slot && hover.part == Part::Arrow) || (dragActive && dragHit.slot == slot && dragHit.part == Part::Arrow);
            g.setColour (colour.withAlpha ((hot ? 1.0f : selected || active ? 0.85f : 0.5f) * dim));
            g.fillRect (juce::Rectangle<float> (14.0f, hot ? 2.5f : 2.0f).withCentre (a));
            // One triangle points the way the band pushes the level; while grabbed, both show
            // that it moves up and down.
            const float dir = b.dynamicsDb > 0.0 ? -1.0f : 1.0f;
            juce::Path tri;
            tri.addTriangle (a.x - 4.0f, a.y + dir * 3.0f, a.x + 4.0f, a.y + dir * 3.0f, a.x, a.y + dir * 8.0f);
            if (hot)
                tri.addTriangle (a.x - 4.0f, a.y - dir * 3.0f, a.x + 4.0f, a.y - dir * 3.0f, a.x, a.y - dir * 8.0f);
            g.fillPath (tri);
        }

        // The dot: on the static curve at the band's centre, carrying the slot number.
        {
            const auto d = dotPosition (slot);
            const bool hot = (hover.slot == slot && hover.part == Part::Dot) || (dragActive && dragHit.slot == slot && dragHit.part == Part::Dot);
            const float r = dotRadius + (hot ? 1.5f : 0.0f);
            if (hot || selected)
            {
                g.setColour (colour.withAlpha (0.25f * dim));
                g.fillEllipse (juce::Rectangle<float> (2.0f * r + 8.0f, 2.0f * r + 8.0f).withCentre (d));
            }
            g.setColour (colour.withAlpha (dim));
            g.fillEllipse (juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (d));
            g.setColour (selected || hot ? theme::textHi : theme::background);
            g.drawEllipse (juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (d), selected || hot ? 1.6f : 1.0f);
            g.setColour (theme::background);
            g.setFont (theme::mono (10.0f));
            g.drawText (juce::String (slot), juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (d), juce::Justification::centred, false);
        }
    }
}

void SpectrumView::drawReadout (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const Hit h = dragActive && dragHit.slot != 0 ? dragHit : hover;
    if (h.slot == 0) return;
    const auto& b = bandsShown[(size_t) h.slot - 1];
    if (! b.used) return;

    const auto signedDb = [] (double v) { return (v > 0.05 ? "+" : "") + juce::String (std::abs (v) < 0.05 ? 0.0 : v, 1) + " dB"; };
    const double octaves = std::log2 (juce::jmax (b.filterLo, b.filterHi) / juce::jmax (1.0e-3, juce::jmin (b.filterLo, b.filterHi)));
    const double widthPercent = (double) params[(size_t) h.slot - 1].width->load (std::memory_order_relaxed);

    const juce::StringArray lines {
        fmt::toText (fmt::Unit::Hz, centreHz (h.slot)),
        fmt::toText (fmt::Unit::Hz, b.lo) + juce::String::fromUTF8 (" \xe2\x80\x93 ") + fmt::toText (fmt::Unit::Hz, b.hi),
        "Width " + juce::String (octaves, 2) + " oct (" + juce::String (juce::roundToInt (widthPercent)) + " %)",
        "Makeup " + signedDb (b.makeupDb),
        std::abs (b.dynamicsDb) < 0.05 ? juce::String ("Range 0 dB")
            : (b.dynamicsDb > 0.0 ? juce::String ("Up ") : juce::String ("Down ")) + juce::String (std::abs (b.dynamicsDb), 1) + " dB"
    };

    // One line in the title strip above the plot, right-aligned, so it never covers the band
    // being edited or the curves around it.
    const auto font = theme::mono (11.0f);
    const auto colour = theme::slotColour (h.slot);
    const juce::String sep = juce::String::fromUTF8 ("  \xc2\xb7  ");
    struct Piece { juce::String text; juce::Colour colour; };
    std::vector<Piece> pieces;
    for (int i = 0; i < lines.size(); ++i)
    {
        if (i > 0) pieces.push_back ({ sep, theme::textLow });
        const bool hot = (i == 1 && (h.part == Part::Body || h.part == Part::LowEdge || h.part == Part::HighEdge))
                      || (i == 3 && h.part == Part::Dot) || (i == 4 && h.part == Part::Arrow);
        pieces.push_back ({ lines[i], i == 0 ? theme::textHi : hot ? colour : theme::textMid });
    }

    float total = 0.0f;
    for (auto& piece : pieces) total += theme::textWidth (font, piece.text);
    const float chip = 8.0f, gap = 8.0f, height = 16.0f;
    const float right = plot.getRight();
    const float left = juce::jmax (plot.getX() + 150.0f, right - total - chip - gap);
    const float top = plot.getY() - kTopInset + 6.0f;

    g.setColour (colour);
    g.fillRect (juce::Rectangle<float> (left, top + (height - chip) / 2.0f, chip, chip));

    g.setFont (font);
    float x = left + chip + gap;
    for (auto& piece : pieces)
    {
        const float pw = theme::textWidth (font, piece.text);
        if (x + pw > right + 0.5f) break;
        g.setColour (piece.colour);
        g.drawText (piece.text, juce::Rectangle<float> (x, top, pw + 1.0f, height), juce::Justification::centredLeft, false);
        x += pw;
    }
}

void SpectrumView::drawResponse (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    juce::Path curve;
    const int columns = juce::jmax (2, juce::roundToInt (plot.getWidth() / kCurveStepPixels));
    for (int i = 0; i <= columns; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / (float) columns;
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
