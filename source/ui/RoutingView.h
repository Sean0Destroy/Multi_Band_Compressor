#pragma once

#include <functional>
#include <vector>

#include "PluginProcessor.h"
#include "Theme.h"

namespace tl::ui
{
/** Alternative top display: what each used band's detector listens to. Left, the two sources
    (main input after the input gain, and the external sidechain bus with its state); right, one
    row per band in column order; between them, a wire from the source to each band, labelled
    with the detector's frequency range and channel choice. A band set to the external sidechain
    while the host has not connected one is drawn with a dashed amber wire, because it does not
    compress. Clicking a band selects it (the editor then opens the inspector's Detector tab,
    where the routing is edited). Read-only otherwise: it reads parameter atomics and the
    sidechain bus layout on the message thread and never writes state. */
class RoutingView final : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    explicit RoutingView (PluginProcessor&);

    /** One band's detector routing, resolved the way the DSP resolves it. */
    struct Route
    {
        int slot = 0;
        float bandLoHz = 0.0f, bandHiHz = 0.0f;
        bool external = false;              // scSource
        int rangeMode = schema::scModeOwn;  // scMode as the DSP applies it (a reference to a slot that does not exist is Own)
        int refSlot = 0;                    // for rangeMode == Band
        bool refUsed = false;               // the referenced slot is a used band
        float detLoHz = 0.0f, detHiHz = 0.0f; // the range the detector listens to
        int channel = 0;                    // scChan index
        bool sidechainMissing = false;      // external, and the host has no sidechain bus connected

        bool operator== (const Route&) const = default;
    };

    /** Message thread: re-reads the routing of the bands in `columnOrder` (the editor's column
        order) and repaints when anything shown changed. */
    void refresh (const std::vector<int>& columnOrder);

    const std::vector<Route>& getRoutes() const noexcept { return routes; }
    int getSidechainChannels() const noexcept { return sidechainChannels; }

    /** Text of the wire label, e.g. "Own range  ·  L/R", "Band 2 range (400 Hz - 1.00 kHz)  ·  Mid". */
    static juce::String rangeText (const Route&);
    static juce::String channelText (const Route&);
    static juce::String labelText (const Route& r) { return rangeText (r) + juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  ")) + channelText (r); }
    /** Text under the sidechain node: "Not connected", "Mono" or "2 channels". */
    juce::String sidechainStateText() const;

    void setSelectedSlot (int slot);
    int getSelectedSlot() const noexcept { return selectedSlot; }
    std::function<void (int slot)> onSelect;

    // ---- Geometry (also for tests) ------------------------------------------------------------
    juce::Rectangle<float> mainNodeBounds() const noexcept;
    juce::Rectangle<float> sidechainNodeBounds() const noexcept;
    /** Row of the band at `index` in getRoutes(). */
    juce::Rectangle<float> rowBounds (int index) const noexcept;
    juce::Rectangle<float> labelBounds (int index) const noexcept;
    /** Slot of the band row under `p`, 0 if none. */
    int slotAt (juce::Point<float> p) const noexcept;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    static constexpr float sourceWidth = 150.0f, labelWidth = 250.0f, rowWidth = 190.0f;
    static constexpr float maxRowHeight = 34.0f;

private:
    juce::Rectangle<float> inner() const noexcept;
    float rowHeight() const noexcept;
    void updateDescription();

    PluginProcessor& processor;
    std::vector<Route> routes;
    int sidechainChannels = 0;
    int selectedSlot = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RoutingView)
};
} // namespace tl::ui
