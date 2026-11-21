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
    hint.setText ("Select a band to edit its frequency range, knee, range, makeup, mix, detector and stereo link.",
                  juce::dontSendNotification);
    hint.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (hint);

    for (auto* c : std::initializer_list<juce::Component*> { &lowFreq, &highFreq, &knee, &range, &makeup, &mix, &detector, &detBlend, &rmsTime, &link })
        addChildComponent (*c);

    setTitle ("Band inspector");
    setSlot (0);
}

void BandInspector::setSlot (int newSlot)
{
    slot = juce::jlimit (0, schema::kActiveSlots, newSlot);
    const bool bound = slot > 0;

    for (auto* k : { &lowFreq, &highFreq, &knee, &range, &makeup, &mix, &detBlend, &rmsTime, &link })
        k->clearParameter();
    detector.clearParameter();
    fLo = fHi = detMode = nullptr;

    if (bound)
    {
        auto& apvts = processor.getState();
        const auto id = [this] (const char* suffix) { return juce::String (schema::bandParamId (slot, suffix)); };
        const auto name = "Band " + juce::String (slot);
        const auto colour = theme::slotColour (slot);

        fLo = apvts.getRawParameterValue (id (schema::band::fLo));
        fHi = apvts.getRawParameterValue (id (schema::band::fHi));
        detMode = apvts.getRawParameterValue (id (schema::band::detMode));

        lowFreq.setParameter  (apvts, id (schema::band::fLo),      "Low Freq",  name + " Low Frequency",  colour);
        highFreq.setParameter (apvts, id (schema::band::fHi),      "High Freq", name + " High Frequency", colour);
        knee.setParameter     (apvts, id (schema::band::knee),     "Knee",      name + " Knee",           colour);
        range.setParameter    (apvts, id (schema::band::range),    "Range",     name + " Range",          colour);
        makeup.setParameter   (apvts, id (schema::band::makeup),   "Makeup",    name + " Makeup",         colour);
        mix.setParameter      (apvts, id (schema::band::mix),      "Mix",       name + " Mix",            colour);
        detector.setParameter (apvts, id (schema::band::detMode),  "Detector",  name + " Detector");
        detBlend.setParameter (apvts, id (schema::band::detBlend), "RMS Blend", name + " Detector RMS Blend", colour);
        rmsTime.setParameter  (apvts, id (schema::band::rmsTime),  "RMS Time",  name + " RMS Time",       colour);
        link.setParameter     (apvts, id (schema::band::link),     "Link",      name + " Stereo Link",    colour);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &lowFreq, &highFreq, &knee, &range, &makeup, &mix, &detector, &detBlend, &rmsTime, &link })
        c->setVisible (bound);

    hint.setVisible (! bound);
    title.setVisible (bound);
    shownTitle.clear();
    refresh();
    repaint();
}

void BandInspector::refresh()
{
    if (slot == 0 || fLo == nullptr) return;

    const auto text = "BAND " + juce::String (slot) + "   "
                      + fmt::toText (fmt::Unit::Hz, (double) fLo->load (std::memory_order_relaxed)) + " - "
                      + fmt::toText (fmt::Unit::Hz, (double) fHi->load (std::memory_order_relaxed));
    if (text != shownTitle)
    {
        shownTitle = text;
        title.setText (text, juce::dontSendNotification);
    }

    // The blend amount only acts in Blend mode; the RMS window only in RMS and Blend modes.
    const int mode = juce::roundToInt (detMode->load (std::memory_order_relaxed));
    detBlend.setEnabled (mode == (int) dsp::DetectorMode::Blend);
    rmsTime.setEnabled (mode != (int) dsp::DetectorMode::Peak);
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
        g.fillRoundedRectangle (juce::Rectangle<float> (8.0f, 10.0f, 4.0f, 16.0f), 2.0f);
    }
}

void BandInspector::resized()
{
    auto r = getLocalBounds().reduced (theme::pad);
    hint.setBounds (r);

    auto head = r.removeFromTop (20);
    head.removeFromLeft (10);
    title.setBounds (head);
    r.removeFromTop (4);

    const int cell = juce::jlimit (72, 120, (r.getWidth() - 2 * theme::pad) / 10);
    const auto next = [&r, cell] { auto c = r.removeFromLeft (cell); return c.withSizeKeepingCentre (cell, juce::jmin (c.getHeight(), 92)); };

    lowFreq.setBounds (next());
    highFreq.setBounds (next());
    knee.setBounds (next());
    range.setBounds (next());
    makeup.setBounds (next());
    mix.setBounds (next());
    r.removeFromLeft (theme::pad);
    detector.setBounds (next().withTrimmedTop (16));
    detBlend.setBounds (next());
    rmsTime.setBounds (next());
    r.removeFromLeft (theme::pad);
    link.setBounds (next());
}
} // namespace tl::ui
