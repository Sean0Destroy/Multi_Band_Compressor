#include "BandInspector.h"

#include "ValueFormat.h"

namespace tl::ui
{
BandInspector::BandInspector (PluginProcessor& p) : processor (p)
{
    title.setFont (theme::font (13.0f, true));
    title.setColour (juce::Label::textColourId, theme::textHi);
    title.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (title);

    hint.setFont (theme::font (13.0f));
    hint.setColour (juce::Label::textColourId, theme::textLow);
    hint.setJustificationType (juce::Justification::centred);
    hint.setText ("Select a band to edit its frequency range, gain, stereo mode and detector.",
                  juce::dontSendNotification);
    hint.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (hint);

    // Amber is reserved for warnings: an external source with nothing connected does nothing.
    warning.setFont (theme::font (12.0f, true));
    warning.setColour (juce::Label::textColourId, theme::warning);
    warning.setJustificationType (juce::Justification::centredRight);
    warning.setText ("Sidechain input not connected: this band is not compressing", juce::dontSendNotification);
    warning.setInterceptsMouseClicks (false, false);
    addChildComponent (warning);

    const auto setupTab = [this] (juce::TextButton& b, const char* text, const char* componentId, Tab t)
    {
        b.setButtonText (text);
        b.setComponentID (componentId);
        b.setTitle (juce::String (text) + " tab");
        b.setClickingTogglesState (false);
        b.onClick = [this, t] { setTab (t); };
        addChildComponent (b);
    };
    setupTab (bandTab, "Band", "action_inspectorTab_band", Tab::Band);
    setupTab (detectorTab, "Detector", "action_inspectorTab_detector", Tab::Detector);

    for (auto* c : std::initializer_list<juce::Component*> { &lowFreq, &highFreq, &knee, &range, &makeup, &mix, &stereo, &link,
                                                              &detector, &detBlend, &rmsTime, &source, &rangeMode, &followBand,
                                                              &scLow, &scHigh, &channels })
        addChildComponent (*c);

    setTitle ("Band inspector");
    setSlot (0);
}

void BandInspector::setTab (Tab t)
{
    tab = t;
    updateVisibility();
    refresh();
}

void BandInspector::updateVisibility()
{
    const bool bound = slot > 0;
    const bool onBand = bound && tab == Tab::Band;
    const bool onDetector = bound && tab == Tab::Detector;

    for (auto* c : std::initializer_list<juce::Component*> { &lowFreq, &highFreq, &knee, &range, &makeup, &mix, &stereo, &link })
        c->setVisible (onBand);
    for (auto* c : std::initializer_list<juce::Component*> { &detector, &detBlend, &rmsTime, &source, &rangeMode, &followBand,
                                                              &scLow, &scHigh, &channels })
        c->setVisible (onDetector);

    hint.setVisible (! bound);
    title.setVisible (bound);
    bandTab.setVisible (bound);
    detectorTab.setVisible (bound);
    bandTab.setToggleState (tab == Tab::Band, juce::dontSendNotification);
    detectorTab.setToggleState (tab == Tab::Detector, juce::dontSendNotification);
    if (! bound) warning.setVisible (false);
}

void BandInspector::setSlot (int newSlot)
{
    slot = juce::jlimit (0, schema::kActiveSlots, newSlot);
    const bool bound = slot > 0;

    for (auto* k : { &lowFreq, &highFreq, &knee, &range, &makeup, &mix, &link, &detBlend, &rmsTime, &scLow, &scHigh })
        k->clearParameter();
    for (auto* c : { &stereo, &detector, &source, &rangeMode, &channels })
        c->clearParameter();
    followBand.clearParameter();
    fLo = fHi = detMode = scSource = scMode = stereoMode = nullptr;

    if (bound)
    {
        auto& apvts = processor.getState();
        const auto id = [this] (const char* suffix) { return juce::String (schema::bandParamId (slot, suffix)); };
        const auto name = "Band " + juce::String (slot);
        const auto colour = theme::slotColour (slot);

        fLo = apvts.getRawParameterValue (id (schema::band::fLo));
        fHi = apvts.getRawParameterValue (id (schema::band::fHi));
        detMode = apvts.getRawParameterValue (id (schema::band::detMode));
        scSource = apvts.getRawParameterValue (id (schema::band::scSource));
        scMode = apvts.getRawParameterValue (id (schema::band::scMode));
        stereoMode = apvts.getRawParameterValue (id (schema::band::stereoMode));

        lowFreq.setParameter  (apvts, id (schema::band::fLo),        "Low Freq",   name + " Low Frequency",  colour);
        highFreq.setParameter (apvts, id (schema::band::fHi),        "High Freq",  name + " High Frequency", colour);
        knee.setParameter     (apvts, id (schema::band::knee),       "Knee",       name + " Knee",           colour);
        range.setParameter    (apvts, id (schema::band::range),      "Range",      name + " Range",          colour);
        makeup.setParameter   (apvts, id (schema::band::makeup),     "Makeup",     name + " Makeup",         colour);
        mix.setParameter      (apvts, id (schema::band::mix),        "Mix",        name + " Mix",            colour);
        stereo.setParameter   (apvts, id (schema::band::stereoMode), "Stereo Mode", name + " Stereo Mode");
        link.setParameter     (apvts, id (schema::band::link),       "Link",       name + " Stereo Link",    colour);

        detector.setParameter   (apvts, id (schema::band::detMode),  "Detector",   name + " Detector");
        detBlend.setParameter   (apvts, id (schema::band::detBlend), "RMS Blend",  name + " Detector RMS Blend", colour);
        rmsTime.setParameter    (apvts, id (schema::band::rmsTime),  "RMS Time",   name + " RMS Time",       colour);
        source.setParameter     (apvts, id (schema::band::scSource), "Source",     name + " Detector Source");
        rangeMode.setParameter  (apvts, id (schema::band::scMode),   "Listens To", name + " Detector Range");
        followBand.setParameter (apvts, id (schema::band::scRef),    "Follow",     name + " Detector Follows Band");
        scLow.setParameter      (apvts, id (schema::band::scLo),     "Det Low",    name + " Detector Low Frequency", colour);
        scHigh.setParameter     (apvts, id (schema::band::scHi),     "Det High",   name + " Detector High Frequency", colour);
        channels.setParameter   (apvts, id (schema::band::scChan),   "Channels",   name + " Detector Channels");
    }

    shownTitle.clear();
    updateVisibility();
    refresh();
    repaint();
}

void BandInspector::refresh()
{
    if (slot == 0 || fLo == nullptr) return;

    // Shown in ascending order, as the engine uses the two edges (the parameters may be crossed).
    const double a = (double) fLo->load (std::memory_order_relaxed), b = (double) fHi->load (std::memory_order_relaxed);
    const auto text = "BAND " + juce::String (slot) + "   "
                      + fmt::toText (fmt::Unit::Hz, juce::jmin (a, b)) + " - " + fmt::toText (fmt::Unit::Hz, juce::jmax (a, b));
    if (text != shownTitle)
    {
        shownTitle = text;
        title.setText (text, juce::dontSendNotification);
    }

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

    // Stereo controls need a stereo signal; the link amount only acts in Stereo mode.
    const bool stereoSignal = processor.getMainBusNumInputChannels() > 1;
    stereo.setEnabled (stereoSignal);
    link.setEnabled (stereoSignal && choice (stereoMode) == (int) dsp::StereoMode::Stereo);

    const bool external = choice (scSource) == (int) dsp::DetectorSource::External;
    warning.setVisible (external && processor.getSidechainChannelCount() == 0);
}

void BandInspector::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    if (slot > 0)
    {
        g.setColour (theme::slotColour (slot));
        g.fillRoundedRectangle (juce::Rectangle<float> (8.0f, 11.0f, 4.0f, 18.0f), 2.0f);
    }
}

void BandInspector::resized()
{
    auto r = getLocalBounds().reduced (theme::pad);
    hint.setBounds (r);

    auto head = r.removeFromTop (24);
    head.removeFromLeft (10);
    title.setBounds (head.removeFromLeft (230));
    bandTab.setBounds (head.removeFromLeft (72));
    head.removeFromLeft (4);
    detectorTab.setBounds (head.removeFromLeft (84));
    warning.setBounds (head);
    r.removeFromTop (2);

    const int cell = juce::jlimit (72, 124, (r.getWidth() - 2 * theme::pad) / 9);
    const auto knobCell = [cell] (juce::Rectangle<int>& row) { auto c = row.removeFromLeft (cell); return c.withSizeKeepingCentre (cell, juce::jmin (c.getHeight(), 92)); };
    const auto comboCell = [cell] (juce::Rectangle<int>& row) { auto c = row.removeFromLeft (cell); return c.withSizeKeepingCentre (cell, juce::jmin (c.getHeight(), 92)).withTrimmedTop (16); };

    {
        auto row = r;
        lowFreq.setBounds (knobCell (row));
        highFreq.setBounds (knobCell (row));
        knee.setBounds (knobCell (row));
        range.setBounds (knobCell (row));
        makeup.setBounds (knobCell (row));
        mix.setBounds (knobCell (row));
        row.removeFromLeft (theme::pad);
        stereo.setBounds (comboCell (row));
        link.setBounds (knobCell (row));
    }
    {
        auto row = r;
        detector.setBounds (comboCell (row));
        detBlend.setBounds (knobCell (row));
        rmsTime.setBounds (knobCell (row));
        row.removeFromLeft (theme::pad);
        source.setBounds (comboCell (row));
        channels.setBounds (comboCell (row));
        rangeMode.setBounds (comboCell (row));
        followBand.setBounds (comboCell (row));
        scLow.setBounds (knobCell (row));
        scHigh.setBounds (knobCell (row));
    }
}
} // namespace tl::ui
