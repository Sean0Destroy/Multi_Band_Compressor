#include "BandInspector.h"

#include "ValueFormat.h"

namespace tl::ui
{
namespace
{
constexpr int kKnobDiameter = 40;
constexpr int kComboHeight = 44;     // legend, gap and the drop-down
}

BandInspector::BandInspector (PluginProcessor& p) : processor (p)
{
    lowLatencyOn = processor.getState().getRawParameterValue (schema::global::lowLatency);

    hint.setFont (theme::label (13.0f));
    hint.setColour (juce::Label::textColourId, theme::textLow);
    hint.setJustificationType (juce::Justification::centred);
    hint.setText ("Select a band to edit its frequency range, gain, stereo mode, detector and timing.",
                  juce::dontSendNotification);
    hint.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (hint);

    // Amber is reserved for warnings: an external source with nothing connected does nothing.
    warning.setFont (theme::label (11.0f));
    warning.setColour (juce::Label::textColourId, theme::warning);
    warning.setJustificationType (juce::Justification::centredRight);
    warning.setText ("Sidechain input not connected: this band is not compressing", juce::dontSendNotification);
    warning.setInterceptsMouseClicks (false, false);
    addChildComponent (warning);

    timingNote.setFont (theme::label (10.0f));
    timingNote.setColour (juce::Label::textColourId, theme::textLow);
    timingNote.setJustificationType (juce::Justification::centredLeft);
    timingNote.setText ("Attack and release live on the band strip.", juce::dontSendNotification);
    timingNote.setInterceptsMouseClicks (false, false);
    addChildComponent (timingNote);

    // Fixed Latency is global (one setting for the whole plugin), so it stays bound whatever band is shown.
    fixedLatency.setParameter (processor.getState(), schema::global::constantLatency, "Fixed Latency", "Fixed Latency");
    fixedLatency.setTooltip ("Fixed Latency (all bands): the plugin always delays by the full 10 ms lookahead, so its "
                             "latency never changes when lookahead or bands change. Use it on hosts that glitch when "
                             "latency changes. Greyed while Low Latency is on.");
    addChildComponent (fixedLatency);

    tabs.setItems ({ "Band", "Detector", "Timing" });
    tabs.setTitle ("Inspector tab");
    const char* tabIds[] { "action_inspectorTab_band", "action_inspectorTab_detector", "action_inspectorTab_timing" };
    for (int i = 0; i < 3; ++i)
    {
        tabs.getCell (i)->setComponentID (tabIds[i]);
        tabs.getCell (i)->setTitle (tabs.getItemText (i) + " tab");
    }
    tabs.setSelectedItemIndex (0, juce::dontSendNotification);
    tabs.onChange = [this] (int index) { setTab ((Tab) index); };
    addChildComponent (tabs);

    // Audition of the selected band: never saved, one at a time (see dsp::AuditionMode).
    const auto setupAudition = [this] (juce::TextButton& b, const char* text, const char* componentId, const char* tip, dsp::AuditionMode mode)
    {
        b.setButtonText (text);
        b.setComponentID (componentId);
        b.setTooltip (tip);
        b.setClickingTogglesState (false);
        b.onClick = [this, mode]
        {
            if (slot == 0) return;
            const bool active = processor.getAuditionMode() == mode && processor.getAuditionSlot() == slot;
            processor.setAudition (active ? dsp::AuditionMode::None : mode, slot);
            refresh();
        };
        addChildComponent (b);
    };
    setupAudition (soloButton, "Solo", "action_soloBand", "Hear only what this band lets through", dsp::AuditionMode::Solo);
    setupAudition (listenButton, "Listen", "action_listenBand", "Hear what this band's detector hears", dsp::AuditionMode::Listen);
    listenButton.setColour (juce::TextButton::buttonOnColourId, theme::warning);   // amber: listen states and warnings

    for (auto* c : std::initializer_list<juce::Component*> { &lowFreq, &highFreq, &width, &knee, &range, &makeup, &mix, &character, &stereo, &direction, &link,
                                                              &detector, &detBlend, &rmsTime, &source, &rangeMode, &followBand,
                                                              &scLow, &scHigh, &channels,
                                                              &lookahead, &adaptiveRelease, &freqRelease, &transient })
        addChildComponent (*c);

    setTitle ("Band inspector");
    setSlot (0);
}

void BandInspector::setTab (Tab t)
{
    tab = t;
    tabs.setSelectedItemIndex ((int) t, juce::dontSendNotification);
    updateVisibility();
    refresh();
}

void BandInspector::updateVisibility()
{
    const bool bound = slot > 0;
    const bool onBand = bound && tab == Tab::Band;
    const bool onDetector = bound && tab == Tab::Detector;
    const bool onTiming = bound && tab == Tab::Timing;

    for (auto* c : std::initializer_list<juce::Component*> { &lowFreq, &highFreq, &width, &knee, &range, &makeup, &mix, &character, &stereo, &direction, &link })
        c->setVisible (onBand);
    for (auto* c : std::initializer_list<juce::Component*> { &detector, &detBlend, &rmsTime, &source, &rangeMode, &followBand,
                                                              &scLow, &scHigh, &channels })
        c->setVisible (onDetector);
    for (auto* c : std::initializer_list<juce::Component*> { &lookahead, &adaptiveRelease, &freqRelease, &transient, &timingNote, &fixedLatency })
        c->setVisible (onTiming);

    hint.setVisible (! bound);
    tabs.setVisible (bound);
    for (int i = 0; i < tabs.getNumItems(); ++i)
        tabs.getCell (i)->setVisible (bound);
    soloButton.setVisible (bound);
    listenButton.setVisible (bound);
    if (! bound) warning.setVisible (false);
    repaint();
}

void BandInspector::setSlot (int newSlot)
{
    slot = juce::jlimit (0, schema::kActiveSlots, newSlot);
    const bool bound = slot > 0;

    for (auto* k : { &lowFreq, &highFreq, &width, &knee, &range, &makeup, &mix, &character, &link, &detBlend, &rmsTime, &scLow, &scHigh,
                      &lookahead, &adaptiveRelease, &freqRelease, &transient })
        k->clearParameter();
    for (auto* c : { &stereo, &direction, &detector, &source, &rangeMode, &channels })
        c->clearParameter();
    followBand.clearParameter();
    detMode = scSource = scMode = stereoMode = scChan = nullptr;

    if (bound)
    {
        auto& apvts = processor.getState();
        const auto id = [this] (const char* suffix) { return juce::String (schema::bandParamId (slot, suffix)); };
        const auto name = "Band " + juce::String (slot);
        const auto colour = theme::slotColour (slot);

        detMode = apvts.getRawParameterValue (id (schema::band::detMode));
        scSource = apvts.getRawParameterValue (id (schema::band::scSource));
        scMode = apvts.getRawParameterValue (id (schema::band::scMode));
        stereoMode = apvts.getRawParameterValue (id (schema::band::stereoMode));
        scChan = apvts.getRawParameterValue (id (schema::band::scChan));

        lowFreq.setParameter  (apvts, id (schema::band::fLo),        "Low Freq",   name + " Low Frequency",  colour);
        highFreq.setParameter (apvts, id (schema::band::fHi),        "High Freq",  name + " High Frequency", colour);
        width.setParameter    (apvts, id (schema::band::width),      "Width",      name + " Width",          colour);
        width.getSlider().setTooltip ("How far the band reaches either side of its centre. 100 % follows the Low and "
                                      "High Freq edges; higher widens the shoulders, lower narrows them. The detector "
                                      "still listens to the edges as set");
        knee.setParameter     (apvts, id (schema::band::knee),       "Knee",       name + " Knee",           colour);
        range.setParameter    (apvts, id (schema::band::range),      "Range",      name + " Range",          colour);
        makeup.setParameter   (apvts, id (schema::band::makeup),     "Makeup",     name + " Makeup",         colour);
        mix.setParameter      (apvts, id (schema::band::mix),        "Mix",        name + " Mix",            colour);
        character.setParameter (apvts, id (schema::band::character), "Character",  name + " Character",      colour);
        lowFreq.setFallbackCaption ("Lo Freq");
        highFreq.setFallbackCaption ("Hi Freq");
        character.setFallbackCaption ("Char");
        character.getSlider().setTooltip ("Warm saturation of this band alone, after its compression: adds harmonics of "
                                          "what is in the band. 0 % is off");
        stereo.setParameter   (apvts, id (schema::band::stereoMode), "Stereo Mode", name + " Stereo Mode");
        direction.setParameter (apvts, id (schema::band::dynDir),    "Direction",  name + " Dynamics Direction");
        direction.getCombo().setTooltip ("Down: compress above the threshold, by up to Range. Up: expand upward above the "
                                         "threshold, by up to Range. Dragging the band up or down on the spectrum sets both");
        link.setParameter     (apvts, id (schema::band::link),       "Link",       name + " Stereo Link",    colour);

        detector.setParameter   (apvts, id (schema::band::detMode),  "Detector",   name + " Detector");
        detBlend.setParameter   (apvts, id (schema::band::detBlend), "RMS Blend",  name + " Detector RMS Blend", colour);
        rmsTime.setParameter    (apvts, id (schema::band::rmsTime),  "RMS Time",   name + " RMS Time",       colour);
        source.setParameter     (apvts, id (schema::band::scSource), "Source",     name + " Detector Source");
        source.getCombo().setTooltip ("Main: the band's own input. External: the sidechain input the host connects to the plugin");
        rangeMode.setParameter  (apvts, id (schema::band::scMode),   "Listens To", name + " Detector Range");
        followBand.setParameter (apvts, id (schema::band::scRef),    "Follow",     name + " Detector Follows Band");
        scLow.setParameter      (apvts, id (schema::band::scLo),     "Det Low",    name + " Detector Low Frequency", colour);
        scHigh.setParameter     (apvts, id (schema::band::scHi),     "Det High",   name + " Detector High Frequency", colour);
        channels.setParameter   (apvts, id (schema::band::scChan),   "Channels",   name + " Detector Channels");
        lookahead.setParameter       (apvts, id (schema::band::lookahead), "Lookahead",    name + " Lookahead", colour);
        adaptiveRelease.setParameter (apvts, id (schema::band::adaptRel),  "Adaptive Rel", name + " Adaptive Release", colour);
        freqRelease.setParameter     (apvts, id (schema::band::freqRel),   "Freq Release", name + " Frequency-Dependent Release", colour);
        transient.setParameter       (apvts, id (schema::band::transient), "Transient",    name + " Transient Preservation", colour);
        adaptiveRelease.setFallbackCaption ("Adapt Rel");
        freqRelease.setFallbackCaption ("Freq Rel");

        soloButton.setTitle ("Solo " + name);
        listenButton.setTitle ("Listen to the detector of " + name);
    }

    updateVisibility();
    refresh();
    repaint();
}

void BandInspector::refresh()
{
    if (slot == 0 || detMode == nullptr) return;

    const bool auditioned = processor.getAuditionSlot() == slot;
    soloButton.setToggleState (auditioned && processor.getAuditionMode() == dsp::AuditionMode::Solo, juce::dontSendNotification);
    listenButton.setToggleState (auditioned && processor.getAuditionMode() == dsp::AuditionMode::Listen, juce::dontSendNotification);

    // The blend amount only acts in Blend mode; the RMS window only in RMS and Blend modes.
    const int mode = choice (detMode);
    detBlend.setEnabled (mode == (int) dsp::DetectorMode::Blend);
    rmsTime.setEnabled (mode != (int) dsp::DetectorMode::Peak);

    // Detector range: the band to follow or the custom edges, depending on the mode.
    const int rangeChoice = choice (scMode);
    followBand.setEnabled (rangeChoice == schema::scModeBand);
    scLow.setEnabled (rangeChoice == schema::scModeCustom);
    scHigh.setEnabled (rangeChoice == schema::scModeCustom);

    std::vector<int> others;
    for (int s : processor.getBandSlots().usedSlots())
        if (s != slot) others.push_back (s);
    followBand.setSlots (others, slot);

    // Stereo controls need a stereo signal. The link amount only acts in Stereo mode with
    // per-channel (L/R) detection: the other detector channel choices already give both lanes
    // the same signal (Mid, Side, Sum) or the deeper reduction (Max).
    const bool stereoSignal = processor.getMainBusNumInputChannels() > 1;
    stereo.setEnabled (stereoSignal);
    link.setEnabled (stereoSignal && choice (stereoMode) == (int) dsp::StereoMode::Stereo
                     && choice (scChan) == (int) dsp::DetectorChannels::PerChannel);

    // Low Latency overrides the lookahead and Fixed Latency (the stored values come back when it is off).
    const bool lowLatency = lowLatencyOn != nullptr && lowLatencyOn->load (std::memory_order_relaxed) >= 0.5f;
    lookahead.setEnabled (! lowLatency);
    fixedLatency.setEnabled (! lowLatency);

    const bool external = choice (scSource) == (int) dsp::DetectorSource::External;
    warning.setVisible (external && processor.getSidechainChannelCount() == 0);
}

void BandInspector::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    theme::drawModule (g, r);
    g.setColour (theme::lattice);
    g.fillRect (1.0f, (float) headHeight - 1.0f, r.getWidth() - 2.0f, 1.0f);

    // Title: the slot chip and "BAND" (the range is on the strip).
    auto title = juce::Rectangle<float> (12.0f, 0.0f, 120.0f, (float) headHeight - 1.0f);
    if (slot > 0)
    {
        theme::drawSlotChip (g, title.removeFromLeft (20.0f).withSizeKeepingCentre (20.0f, 20.0f), slot, 11.0f);
        title.removeFromLeft (8.0f);
    }
    g.setColour (theme::textHi);
    theme::drawTracked (g, getTitleText(), title, juce::Justification::centredLeft, theme::display (13.0f, 0.24f));
}

void BandInspector::layoutRow (juce::Rectangle<int> area, const std::vector<Cell>& cells)
{
    if (cells.empty()) return;

    // Preferred widths; squeezed (down to a floor) when the row is narrow, gaps between 4 and 18.
    const auto preferred = [] (const Cell& c) { return c.knob ? 60 : 100; };
    const auto floorWidth = [] (const Cell& c) { return c.knob ? 46 : 80; };
    int want = 0, minimum = 0;
    for (auto& c : cells) { want += preferred (c); minimum += floorWidth (c); }
    const int n = (int) cells.size();
    const int gapsAtPreferred = (area.getWidth() - want) / juce::jmax (1, n - 1);

    float squeeze = 1.0f;
    int gap = juce::jlimit (4, 18, gapsAtPreferred);
    if (want + gap * (n - 1) > area.getWidth())
        squeeze = juce::jlimit (0.0f, 1.0f, (float) (area.getWidth() - gap * (n - 1) - minimum) / (float) juce::jmax (1, want - minimum));

    int x = area.getX();
    for (auto& c : cells)
    {
        const int w = floorWidth (c) + juce::roundToInt (squeeze * (float) (preferred (c) - floorWidth (c)));
        if (c.knob)
        {
            const int h = ParamKnob::heightFor (kKnobDiameter);
            c.parts[0]->setBounds (juce::Rectangle<int> (x, area.getY(), w, area.getHeight()).withSizeKeepingCentre (w, h));
        }
        else
        {
            const int stack = (int) c.parts.size();
            const int h = stack * kComboHeight + (stack - 1) * 8;
            auto column = juce::Rectangle<int> (x, area.getY(), w, area.getHeight()).withSizeKeepingCentre (w, h);
            for (auto* part : c.parts)
            {
                part->setBounds (column.removeFromTop (kComboHeight));
                column.removeFromTop (8);
            }
        }
        x += w + gap;
    }
}

void BandInspector::resized()
{
    auto r = getLocalBounds();
    auto head = r.removeFromTop (headHeight).reduced (12, 8);
    head.removeFromBottom (1);

    head.removeFromLeft (120);                                     // the painted title
    tabs.setBounds (head.removeFromLeft (tabs.getPreferredWidth()).withSizeKeepingCentre (tabs.getPreferredWidth(), 28));
    listenButton.setBounds (head.removeFromRight (74));
    head.removeFromRight (6);
    soloButton.setBounds (head.removeFromRight (62));

    auto body = r.reduced (14, 6);
    hint.setBounds (body);
    warning.setBounds (body.removeFromBottom (14));

    layoutRow (body, { { { &lowFreq }, true }, { { &highFreq }, true }, { { &width }, true }, { { &knee }, true }, { { &range }, true },
                       { { &makeup }, true }, { { &mix }, true }, { { &character }, true }, { { &link }, true },
                       { { &stereo, &direction }, false } });
    layoutRow (body, { { { &detector }, false }, { { &detBlend }, true }, { { &rmsTime }, true },
                       { { &source, &channels }, false }, { { &rangeMode, &followBand }, false },
                       { { &scLow }, true }, { { &scHigh }, true } });

    auto timingRow = body;
    auto side = timingRow.removeFromRight (juce::jmin (210, timingRow.getWidth() / 3)).withSizeKeepingCentre (
        juce::jmin (210, timingRow.getWidth() / 3), 28 + 8 + 16);
    fixedLatency.setBounds (side.removeFromTop (28).withWidth (juce::jmin (side.getWidth(), 140)));
    side.removeFromTop (8);
    timingNote.setBounds (side);
    layoutRow (timingRow, { { { &lookahead }, true }, { { &adaptiveRelease }, true }, { { &freqRelease }, true }, { { &transient }, true } });
}
} // namespace tl::ui
