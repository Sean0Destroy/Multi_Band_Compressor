#pragma once

#include <array>
#include <memory>

#include "BandColumn.h"
#include "BandInspector.h"
#include "Controls.h"
#include "DynamicsView.h"
#include "PluginProcessor.h"
#include "MeterRail.h"
#include "RoutingView.h"
#include "SpectrumView.h"

namespace tl::ui
{
/** The plugin editor. Layout top to bottom: header (name, engine tag, band counter, add, undo,
    redo, bypass), the display (spectrum, dynamics or detector routing; takes any extra height), band column strip (used slots only,
    ordered by frequency), the inspector of the selected band and the global bar (input gain,
    mix, output gain, automatic gain, output clipper). Runs entirely on the message thread; it only reads lock-free values the
    audio thread publishes (used mask, parameter atomics, GR meter). */
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

    /** Synchronises everything with the processor: visible columns and their order, selection,
        band counter, undo/redo state, meters. Called by the 30 Hz timer and after user actions;
        public so tests can drive it without a running message loop. */
    void refreshFromProcessor();

    /** 0 = no band selected. Selecting an unused slot selects nothing. */
    void selectSlot (int slot);
    int getSelectedSlot() const noexcept { return selectedSlot; }

    /** Slots of the visible columns, left to right. */
    std::vector<int> getColumnOrder() const;

    BandColumn& getColumn (int slot) { return *columns[(size_t) juce::jlimit (1, schema::kActiveSlots, slot) - 1]; }
    BandInspector& getInspector() noexcept { return inspector; }
    SpectrumView& getSpectrum() noexcept { return spectrum; }
    DynamicsView& getDynamics() noexcept { return dynamics; }
    RoutingView& getRouting() noexcept { return routing; }

    /** Which display is shown above the band strip. */
    enum class View { Spectrum, Dynamics, Routing };
    void setView (View);
    View getView() const noexcept { return view; }
    MeterRail& getMeterRail() noexcept { return meterRail; }
    /** Text next to the Auto Gain button: the correction currently applied, empty when off. */
    juce::String getAutoGainText() const { return autoGainValue.getText(); }
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

    /** Header read-out of the latency the plugin reports to the host. */
    juce::String getLatencyText() const { return latencyLabel.getText(); }

    static constexpr int fixedHeight = theme::headerHeight + theme::toolbarHeight + theme::columnStripHeight + theme::inspectorHeight
                                       + theme::globalBarHeight + 6 * theme::pad;   // everything but the display
    static constexpr int defaultWidth = 1180, defaultHeight = fixedHeight + theme::displayHeight;
    // 8 columns of the minimum width must always fit (column scrolling comes with the layout tiers).
    static constexpr int minWidth = 2 * (theme::pad + 4) + schema::kActiveSlots * theme::columnMinWidth
                                    + (schema::kActiveSlots - 1) * theme::pad,
                         minHeight = fixedHeight + theme::displayMinHeight;
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
    void addBand();
    void removeBand (int slot);
    void layoutColumns();
    void afterStateChange();
    void showStatus (const juce::String& text);
    void beginNamingPreset();

    PluginProcessor& processor;
    TlLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 600 };
    Content content;
    juce::ComboBox scaleSelect;
    int uiScalePercent = 100;

    juce::Label productName, engineTag, bandCounter, emptyHint, latencyLabel;
    ParamToggle fixedLatency;
    juce::TextButton addButton, undoButton, redoButton;
    juce::TextButton presetPrevious, presetNext, presetName, presetSave, abA, abB, abCopy;
    juce::TextEditor presetNameEditor;
    juce::Label statusLabel;
    double statusUntilMs = 0.0;
    ParamToggle bypass;

    std::array<std::unique_ptr<BandColumn>, schema::kActiveSlots> columns;
    SpectrumView spectrum;
    DynamicsView dynamics;
    RoutingView routing;
    juce::TextButton spectrumTab, dynamicsTab, routingTab;
    juce::ComboBox engineSelect;
    View view = View::Spectrum;
    MeterRail meterRail;
    BandInspector inspector;
    ParamKnob inputGain { ParamKnob::Layout::Bar }, globalMix { ParamKnob::Layout::Bar }, outputGain { ParamKnob::Layout::Bar };
    ParamToggle autoGain;
    juce::Label autoGainValue;
    ParamCombo clipMode { ParamCombo::Layout::Bar }, characterModel { ParamCombo::Layout::Bar },
               oversampling { ParamCombo::Layout::Bar };
    ParamKnob characterDrive { ParamKnob::Layout::Bar };
    std::atomic<float>* characterModelValue = nullptr;
    ParamKnob clipCeiling { ParamKnob::Layout::Bar };
    std::atomic<float>* clipModeValue = nullptr;
    std::atomic<float>* autoGainOn = nullptr;
    juce::Rectangle<int> globalBarBounds;

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
