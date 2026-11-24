#pragma once

#include <array>
#include <functional>
#include <vector>

#include "PluginProcessor.h"
#include "Theme.h"
#include "analysis/SpectrumAnalyzer.h"

namespace tl::ui
{
/** Top display: input / output spectrum, every used band and the response the band engine
    currently applies (per band and combined).

    Each band is drawn as one object on its own curve: the static curve (its Makeup), a shaded
    dynamic area from there to the Range limit along the same shape (below for Down, above for
    Up), and the live curve moving inside it. One dot sits on the static curve at the band's
    centre, and a small arrow marker sits at the Range limit.

    Mouse: drag the dot to move the band (sideways) and set its Makeup (up/down); drag the arrow
    marker up or down to set the dynamics (below the static curve compresses, Down; above it
    expands upward, Up); drag an edge (left/right arrows) to stretch or shrink the band; drag
    the rest of the band sideways to move it; the mouse wheel over a band sets its Width; drag
    across empty space to create a band.

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
    enum class Part { None, Body, LowEdge, HighEdge, Dot, Arrow };
    struct Hit { int slot = 0; Part part = Part::None; };
    Hit hitTestAt (juce::Point<float>) const;
    void beginDrag (juce::Point<float>);
    void continueDrag (juce::Point<float>);
    void endDrag();
    bool isDragging() const noexcept { return dragActive; }

    /** Where a band's dot (static level at its centre) and arrow marker (Range limit) are drawn. */
    juce::Point<float> dotPosition (int slot) const noexcept;
    juce::Point<float> arrowPosition (int slot) const noexcept;
    /** The band's centre frequency as the dot shows it (geometric middle of its edges). */
    double centreHz (int slot) const noexcept;
    /** Sets a band's Makeup (as a vertical dot drag does), rounded to 0.1 dB. */
    void setMakeup (int slot, double db);
    /** Steps a band's Width (as the mouse wheel does): one step is a twelfth of an octave of
        the 25 .. 400 % log scale; positive widens. */
    void stepWidth (int slot, float steps);

    /** A band's dynamics as the arrow marker shows them: -Range for Down, +Range for Up (dB). */
    double dynamicsDb (int slot) const noexcept { return bandsShown[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1].dynamicsDb; }
    /** Sets a band's direction and range from a signed amount (as a vertical drag does). */
    void setDynamics (int slot, double signedDb);
    static constexpr double maxDynamicsDb = 48.0;   // the Range parameter's maximum
    static constexpr float dotRadius = 7.0f, dotGrabPixels = 10.0f, arrowGrabPixels = 9.0f;
    static constexpr float arrowMinOffset = 16.0f;  // the marker stays this far from the dot at Range 0

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
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

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
        double filterLo = 100.0, filterHi = 1000.0;   // the edges the filter uses (after Width)
        double dynamicsDb = -24.0;        // -Range (Down) or +Range (Up)
        bool up = false;
        double makeupDb = 0.0, mix = 1.0;
        double gain = 1.0;                // linear gain the response curve uses (live)
        double staticGain = 1.0;          // the static curve: Makeup through the band mix
        double limitGain = 1.0;           // the Range limit: Makeup -/+ Range through the band mix
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
        std::atomic<float>* width = nullptr;
        juce::RangedAudioParameter* makeupParam = nullptr;
        juce::RangedAudioParameter* widthParam = nullptr;
        juce::RangedAudioParameter* range = nullptr;
        juce::RangedAudioParameter* dynDir = nullptr;
        std::atomic<float>* rangeValue = nullptr;
        std::atomic<float>* dynDirValue = nullptr;
    };

    void setEdges (int slot, double lo, double hi);
    void beginGesture (int slot);
    void beginDynamicsGesture (int slot);
    void beginDotGesture (int slot);
    void openGesture (int slot, std::vector<juce::RangedAudioParameter*>);
    double responseDbOf (const BandShown&, double hz, double gain) const;
    void drawReadout (juce::Graphics&, juce::Rectangle<float> plot) const;
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
    double dragStartLo = 0.0, dragStartHi = 0.0, dragStartDynamics = 0.0, dragStartMakeup = 0.0;
    std::vector<juce::RangedAudioParameter*> gestureParams;   // the parameters with an open gesture
    int gestureSlot = 0;
    Hit hover;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
};
} // namespace tl::ui
