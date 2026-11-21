#pragma once

#include <array>
#include <functional>
#include <vector>

#include "PluginProcessor.h"
#include "Theme.h"
#include "analysis/SpectrumAnalyzer.h"

namespace tl::ui
{
/** Top display: input / output spectrum, the nominal region of every used band (drag an edge
    or the body; drag across empty space to create a band) and the live response the band
    engine currently applies (per band and combined).

    Message thread only. The audio thread only feeds two lock-free FIFOs while a view exists.
    Scales: frequency 20 Hz .. 20 kHz (log); response +18 .. -30 dB; spectrum 0 .. -96 dBFS. */
class SpectrumView final : public juce::Component
{
public:
    explicit SpectrumView (PluginProcessor&);
    ~SpectrumView() override;

    void setSelectedSlot (int slot);
    int getSelectedSlot() const noexcept { return selectedSlot; }

    std::function<void (int slot)> onSelect;        // a band was clicked or created

    /** From the editor's timer: analyzer update, band state snapshot, repaint. With
        `audioRunning` false the curve shows no gain reduction (the meter value would be stale). */
    void refresh (bool audioRunning = true);

    // ---- Coordinates (component space) ------------------------------------------------------
    juce::Rectangle<float> plotBounds() const noexcept;
    float xForFrequency (double hz) const noexcept;
    double frequencyForX (float x) const noexcept;
    float yForResponseDb (double db) const noexcept;
    float yForLevelDb (double dbfs) const noexcept;

    // ---- Interaction (mouse handlers call these; tests drive them directly) -------------------
    enum class Part { None, Body, LowEdge, HighEdge };
    struct Hit { int slot = 0; Part part = Part::None; };
    Hit hitTestAt (juce::Point<float>) const;
    void beginDrag (juce::Point<float>);
    void continueDrag (juce::Point<float>);
    void endDrag();
    bool isDragging() const noexcept { return dragActive; }

    // ---- What is drawn (also for tests) -------------------------------------------------------
    /** Combined response in dB at `hz` for the last refreshed band state. */
    double responseDbAt (double hz) const;
    /** Linear gain the curve assumes for a slot (1 = neutral or unused). */
    double bandGain (int slot) const noexcept { return bandsShown[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1].gain; }
    const analysis::SpectrumAnalyzer& inputAnalyzer() const noexcept  { return analyzerIn; }
    const analysis::SpectrumAnalyzer& outputAnalyzer() const noexcept { return analyzerOut; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr double minHz = 20.0, maxHz = 20000.0;
    static constexpr double topDb = 18.0, bottomDb = -30.0;
    static constexpr double levelTopDb = 0.0, levelBottomDb = -96.0;
    static constexpr float edgeGrabPixels = 6.0f;
    static constexpr float createDragPixels = 4.0f;
    static constexpr double minWidthOctaves = 1.0 / 12.0;   // narrowest band the view lets you drag

private:
    struct BandShown
    {
        bool used = false, bypassed = false;
        double lo = 100.0, hi = 1000.0;   // nominal edges (parameters)
        double gain = 1.0;                // linear gain the response curve uses
    };

    struct BandParams
    {
        juce::RangedAudioParameter* lo = nullptr;
        juce::RangedAudioParameter* hi = nullptr;
        std::atomic<float>* loValue = nullptr;
        std::atomic<float>* hiValue = nullptr;
        std::atomic<float>* bypass = nullptr;
        std::atomic<float>* makeup = nullptr;
        std::atomic<float>* mix = nullptr;
    };

    void setEdges (int slot, double lo, double hi);
    void beginGesture (int slot);
    void drawGrid (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawSpectrum (juce::Graphics&, juce::Rectangle<float> plot, const analysis::SpectrumAnalyzer&, juce::Colour, bool fill) const;
    void drawBands (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawResponse (juce::Graphics&, juce::Rectangle<float> plot) const;

    PluginProcessor& processor;
    std::array<BandParams, schema::kActiveSlots> params {};
    std::array<BandShown, schema::kActiveSlots> bandsShown {};
    std::atomic<float>* globalBypass = nullptr;
    analysis::SpectrumAnalyzer analyzerIn, analyzerOut;
    int idleFrames = 0;
    int selectedSlot = 0;

    // Drag state
    bool dragActive = false, gestureOpen = false;
    Hit dragHit;
    juce::Point<float> dragStart;
    double dragStartLo = 0.0, dragStartHi = 0.0;
    int gestureSlot = 0;
    Hit hover;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
};
} // namespace tl::ui
