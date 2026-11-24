#pragma once

#include <array>
#include <memory>

#include "BandColumn.h"
#include "BandInspector.h"
#include "ConsoleMeters.h"
#include "Controls.h"
#include "PluginProcessor.h"
#include "MeterRail.h"
#include "SpectrumView.h"

namespace tl::ui
{
/** The plugin editor, laid out as a console faceplate between two rack ears.
      Header:  Tone Legion lockup and MULTIBAND, presets, A/B, undo / redo, scale, bypass
      Toolbar: status line, latency, fixed latency, band count, add
      Body:    left, the spectrum display (takes any extra height), the row of band strips (used
               slots ordered by frequency, then empty places) and, under them, the gain-reduction
               needle and the inspector of the selected band; right, the master strip (meters and
               loudness, input / mix / output, overlap and automatic gain, colour, output clipper
               with its clip meter).
    Runs entirely on the message thread; it only reads lock-free values the audio thread
    publishes (used mask, parameter atomics, GR meter, level, loudness and clip meters). */
class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // ---- Window scale ------------------------------------------------------------------------
    /** The scales the editor offers, in percent. */
    static constexpr std::array<int, 6> uiScaleSteps { 75, 90, 100, 110, 125, 150 };
    /** Message thread: draws the same layout at `percent` (one of uiScaleSteps; anything else is
        100). The window size follows, so the layout itself does not change. Saved with the
        project, not with presets. */
    void setUiScalePercent (int percent);
    int getUiScalePercent() const noexcept { return uiScalePercent; }
    float getUiScale() const noexcept { return (float) uiScalePercent / 100.0f; }
    /** Holds every control, laid out at 100 % and drawn with the scale as its transform. */
    juce::Component& getContent() noexcept { return content; }
    /** A size at 100 %, as a window size at `percent`. */
    static int scaled (int size100, int percent) noexcept { return (int) (((juce::int64) size100 * percent + 50) / 100); }

    /** Synchronises everything with the processor: visible strips and their order, selection,
        band counter, undo/redo state, meters. Called by the 30 Hz timer and after user actions;
        public so tests can drive it without a running message loop. */
    void refreshFromProcessor();

    /** 0 = no band selected. Selecting an unused slot selects nothing. */
    void selectSlot (int slot);
    int getSelectedSlot() const noexcept { return selectedSlot; }

    /** Slots of the visible strips, left to right. */
    std::vector<int> getColumnOrder() const;

    BandColumn& getColumn (int slot) { return *columns[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1]; }
    /** The empty places after the used strips (index 0 is the leftmost). */
    EmptyStrip& getEmptyStrip (int index) { return *placeholders[(size_t) juce::jlimit (0, schema::kActiveSlots - 1, index)]; }
    BandInspector& getInspector() noexcept { return inspector; }
    SpectrumView& getSpectrum() noexcept { return spectrum; }
    MeterRail& getMeterRail() noexcept { return meterRail; }
    NeedleMeter& getNeedle() noexcept { return needle; }
    ClipView& getClipView() noexcept { return clipView; }

    /** Read-out next to the Auto Gain button: the correction currently applied, empty when off. */
    juce::String getAutoGainText() const { return autoGainText; }
    // ---- Presets and A/B (the buttons call these; public for tests) ----------------------------
    /** Saves the current state as a user preset; a failure is shown in the status line. */
    void savePresetAs (const juce::String& name);
    /** Preset menu. Ids: 1..N = entry N of PresetManager::list(), then the ids below. */
    juce::PopupMenu buildPresetMenu() const;
    void handlePresetMenu (int id);
    enum PresetMenuIds { menuNewEmpty = 1000, menuSaveAs, menuDeleteCurrent, menuOpenFolder };
    juce::String getPresetText() const { return presetName.getButtonText(); }
    juce::String getStatusText() const { return statusLabel.getText(); }
    bool isNamingPreset() const { return presetNameEditor.isVisible(); }

    /** Toolbar read-out of the latency the plugin reports to the host ("LAT 1.1 ms"). */
    juce::String getLatencyText() const { return latencyLabel.getText(); }
    /** Toolbar read-out of the number of bands ("3 / 8"). */
    juce::String getBandCountText() const { return bandCounter.getText(); }

    // ---- Sizes at 100 % ----------------------------------------------------------------------
    static constexpr int stripRowHeight = theme::columnStripHeight;
    static constexpr int bottomRowHeight = theme::inspectorHeight;
    // Master strip sections below the meters (each with 10 px padding top and bottom).
    static constexpr int masterPad = 10, masterRow = 28, masterGap = 8;
    static constexpr int masterTrioHeight = 2 * masterPad + ParamKnob::heightFor (50);
    static constexpr int masterOverlapHeight = 2 * masterPad + 2 * masterRow + masterGap;
    static constexpr int masterColourHeight = 2 * masterPad + masterRow + masterGap + ParamKnob::heightFor (40);
    static constexpr int masterClipHeight = 2 * masterPad + masterRow + masterGap + ClipView::preferredHeight + masterGap + ParamKnob::heightFor (40);
    static constexpr int masterSectionsHeight = masterTrioHeight + masterOverlapHeight + masterColourHeight + masterClipHeight;
    static constexpr int masterMinHeight = MeterRail::minHeight + masterSectionsHeight + 2;
    static constexpr int chromeHeight = theme::headerHeight + theme::toolbarHeight + theme::bodyPadTop + theme::bodyPadBottom;
    static constexpr int defaultWidth = 1320,
                         defaultHeight = chromeHeight + theme::displayHeight + theme::rowGap * 2 + stripRowHeight + bottomRowHeight;
    // Eight strips of the minimum width and the master strip must always fit.
    static constexpr int minWidth = 2 * theme::earWidth + 2 * theme::bodyPadSide + schema::kActiveSlots * theme::columnMinWidth
                                    + (schema::kActiveSlots - 1) * theme::stripGap + theme::bodyGap + theme::masterWidth,
                         minHeight = chromeHeight + masterMinHeight;
    static constexpr int maxWidth = 1920, maxHeight = 1200;      // at 100 %; every limit scales with the window
    static constexpr int timerHz = 30;
    static constexpr int idleTicksBeforeMetersClear = 8;   // about 270 ms without a processed block

    /** True while the periodic refresh is running (it is from construction to destruction). */
    bool isRefreshing() const noexcept { return isTimerRunning(); }
    /** False once the host has stopped calling the processor for a moment. */
    bool isAudioRunning() const noexcept { return idleTicks < idleTicksBeforeMetersClear; }

private:
    void timerCallback() override { refreshFromProcessor(); }
    void paintContent (juce::Graphics&);
    void layoutContent();
    void applyResizeLimits();
    static int validScalePercent (int) noexcept;
    void undoLast();
    void redoLast();

    /** The component that carries the scale transform (JUCE keeps the editor's own transform for
        the host's display scaling). */
    struct Content final : public juce::Component
    {
        std::function<void (juce::Graphics&)> painter;
        std::function<void()> onResized;
        void paint (juce::Graphics& g) override { if (painter) painter (g); }
        void resized() override                 { if (onResized) onResized(); }
    };

    /** The preset field: the preset's name alone; opens the preset menu. */
    struct PresetBox final : public juce::TextButton
    {
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };

    /** A small arrow button (previous / next preset). */
    struct ArrowButton final : public juce::Button
    {
        explicit ArrowButton (bool pointsLeft) : juce::Button ({}), left (pointsLeft) {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        const bool left;
    };

    void addBand();
    void removeBand (int slot);
    void layoutColumns();
    void afterStateChange();
    void showStatus (const juce::String& text);
    void beginNamingPreset();
    CaptionLabel& makeLegend (const juce::String& text);

    PluginProcessor& processor;
    TlLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 600 };
    Content content;
    juce::ComboBox scaleSelect;
    int uiScalePercent = 100;

    // Header
    juce::Rectangle<int> brandBounds;
    ArrowButton presetPrevious { true }, presetNext { false };
    PresetBox presetName;
    juce::TextButton presetSave, abA, abB, abCopy, undoButton, redoButton;
    juce::TextEditor presetNameEditor;
    ParamToggle bypass;

    // Toolbar
    juce::Label statusLabel;
    Vfd latencyLabel, bandCounter;
    ParamToggle fixedLatency;
    juce::TextButton addButton;
    double statusUntilMs = 0.0;

    // Left column
    SpectrumView spectrum;
    std::array<std::unique_ptr<BandColumn>, schema::kActiveSlots> columns;
    std::array<std::unique_ptr<EmptyStrip>, schema::kActiveSlots> placeholders;
    NeedleMeter needle;
    BandInspector inspector;

    // Master strip
    juce::Rectangle<int> masterBounds;
    std::vector<int> masterRules;                 // y of the lines between its sections
    MeterRail meterRail;
    ParamKnob inputGain, globalMix, outputGain;
    juce::OwnedArray<CaptionLabel> legends;
    CaptionLabel* overlapLegend = nullptr;
    CaptionLabel* autoGainLegend = nullptr;
    CaptionLabel* colourLegend = nullptr;
    CaptionLabel* colourOsLegend = nullptr;
    CaptionLabel* clipLegend = nullptr;
    CaptionLabel* clipOsLegend = nullptr;
    ParamSegment overlapMode, characterModel, oversampling { true }, clipMode, clipOversampling { true };
    ParamToggle autoGain;
    juce::Label autoGainValue;
    juce::String autoGainText;
    ParamKnob characterDrive, clipCeiling;
    ClipView clipView;
    std::atomic<float>* characterModelValue = nullptr;
    std::atomic<float>* clipModeValue = nullptr;
    std::atomic<float>* autoGainOn = nullptr;

    std::uint32_t shownMask = 0xffffffffu;   // forces the first refresh
    std::array<int, schema::kActiveSlots> shownOrder {};
    int shownCount = 0;
    int selectedSlot = 0;
    bool constructed = false;            // window size changes are only remembered after construction
    std::uint32_t lastBlockCount = 0;
    int idleTicks = idleTicksBeforeMetersClear;   // "not running" until a block has been seen
    juce::Rectangle<int> stripBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
} // namespace tl::ui
