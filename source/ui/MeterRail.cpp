#include "MeterRail.h"

namespace tl::ui
{
MeterRail::MeterRail (PluginProcessor& p) : processor (p)
{
    setTitle ("Level meters");
    setDescription ("Input, output and sidechain levels in dBFS.");
    setInterceptsMouseClicks (false, true);      // only the reset button takes clicks
    setOpaque (true);

    resetButton.setButtonText ("Reset");
    resetButton.setComponentID ("action_resetLoudness");
    resetButton.setTitle ("Reset loudness and true peak");
    resetButton.setTooltip ("Start the integrated loudness and the true-peak maximum again");
    resetButton.onClick = [this] { processor.getLoudnessMeter().requestReset(); };
    addAndMakeVisible (resetButton);

    for (auto& t : loudnessText) t = "-";
}

void MeterRail::resized()
{
    auto column = getLocalBounds().reduced (6).removeFromRight (loudnessColumnWidth - 6);
    resetButton.setBounds (column.removeFromBottom (24));
}

juce::String MeterRail::sidechainStateText (SidechainState s)
{
    switch (s)
    {
        case SidechainState::Off:       return "SC off";
        case SidechainState::Silent:    return "SC silent";
        case SidechainState::Receiving: return "SC live";
    }
    return {};
}

void MeterRail::update (Reading& r, const rt::LevelMeter& meter, double nowMs, bool audioRunning)
{
    r.channels = meter.getChannelCount();

    for (int c = 0; c < 2; ++c)
    {
        // Always consume the peak hold so that nothing stale is shown later.
        const float peak = meter.readAndResetPeakDb (c);
        const bool live = audioRunning && c < r.channels;
        r.peakDb[c] = live ? peak : rt::LevelMeter::silenceDb;
        r.rmsDb[c] = live ? meter.getRmsDb (c) : rt::LevelMeter::silenceDb;
    }

    const float now = juce::jmax (r.peakDb[0], r.peakDb[1]);
    if (now >= r.heldDb || nowMs - r.heldAtMs >= holdMs)
    {
        r.heldDb = now;
        r.heldAtMs = nowMs;
    }
    r.clipped = r.heldDb > 0.0f;
}

void MeterRail::refresh (double nowMs, bool audioRunning)
{
    update (in, processor.getInputMeter(), nowMs, audioRunning);
    update (out, processor.getOutputMeter(), nowMs, audioRunning);
    update (sc, processor.getSidechainMeter(), nowMs, audioRunning);

    // Loudness of the output (measured while the editor is open).
    const auto& loudness = processor.getLoudnessMeter();
    const auto text = [] (float v) { return v <= rt::LoudnessMeter::silenceLufs + 1.0f ? juce::String ("-") : juce::String (v, 1); };
    loudnessText[0] = audioRunning ? text (loudness.getMomentaryLufs()) : juce::String ("-");
    loudnessText[1] = audioRunning ? text (loudness.getShortTermLufs()) : juce::String ("-");
    loudnessText[2] = text (loudness.getIntegratedLufs());       // integrated and true-peak maximum are kept
    loudnessText[3] = text (loudness.getTruePeakDb());

    // Sidechain state. "Off" is what the HOST configured (bus disabled); the plugin cannot know
    // which track, if any, feeds an enabled bus. "Silent" after 300 ms below -90 dBFS.
    if (processor.getSidechainChannelCount() == 0)
    {
        sidechainState = SidechainState::Off;
        silentSinceMs = -1.0;
    }
    else if (juce::jmax (sc.peakDb[0], sc.peakDb[1]) > silentBelowDb)
    {
        sidechainState = SidechainState::Receiving;
        silentSinceMs = -1.0;
    }
    else
    {
        if (silentSinceMs < 0.0) silentSinceMs = nowMs;
        if (sidechainState != SidechainState::Receiving || nowMs - silentSinceMs >= 300.0)
            sidechainState = SidechainState::Silent;
    }

    repaint();
}

float MeterRail::yFor (juce::Rectangle<float> bar, float db) const noexcept
{
    const float t = (topDb - juce::jlimit (bottomDb, topDb, db)) / (topDb - bottomDb);
    return bar.getY() + t * bar.getHeight();
}

void MeterRail::drawMeter (juce::Graphics& g, juce::Rectangle<float> area, const Reading& r, const juce::String& title, bool dimmed) const
{
    const float alpha = dimmed ? 0.4f : 1.0f;

    g.setColour (theme::textMid.withMultipliedAlpha (alpha));
    g.setFont (theme::font (10.0f, true));
    g.drawText (title, area.removeFromTop (12.0f), juce::Justification::centred, false);

    auto value = area.removeFromBottom (14.0f);
    area.removeFromBottom (2.0f);

    const int shown = juce::jmax (1, r.channels);
    const float gap = 2.0f;
    const float barWidth = juce::jmin (9.0f, (area.getWidth() - gap * (float) (shown - 1)) / (float) shown);
    float x = area.getCentreX() - 0.5f * (barWidth * (float) shown + gap * (float) (shown - 1));

    for (int c = 0; c < shown; ++c)
    {
        const juce::Rectangle<float> bar (x, area.getY(), barWidth, area.getHeight());
        g.setColour (theme::background);
        g.fillRoundedRectangle (bar, 2.0f);

        const float peakY = yFor (bar, r.peakDb[c]);
        if (r.peakDb[c] > bottomDb)
        {
            // Above 0 dBFS the top of the bar is amber (a warning); below it the accent colour.
            const float zeroY = yFor (bar, 0.0f);
            g.setColour (theme::accent.withMultipliedAlpha (alpha));
            g.fillRect (juce::Rectangle<float> (bar.getX(), juce::jmax (peakY, zeroY), bar.getWidth(), bar.getBottom() - juce::jmax (peakY, zeroY)));
            if (peakY < zeroY)
            {
                g.setColour (theme::warning.withMultipliedAlpha (alpha));
                g.fillRect (juce::Rectangle<float> (bar.getX(), peakY, bar.getWidth(), zeroY - peakY));
            }
        }

        if (r.rmsDb[c] > bottomDb)
        {
            g.setColour (theme::textHi.withMultipliedAlpha (alpha));
            g.fillRect (juce::Rectangle<float> (bar.getX(), yFor (bar, r.rmsDb[c]) - 0.75f, bar.getWidth(), 1.5f));
        }

        x += barWidth + gap;
    }

    g.setFont (theme::font (10.5f));
    g.setColour ((r.clipped ? theme::warning : theme::textHi).withMultipliedAlpha (alpha));
    g.drawText (r.heldDb <= bottomDb ? juce::String ("-inf") : juce::String (r.heldDb, 1), value, juce::Justification::centred, false);
}

void MeterRail::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    auto r = getLocalBounds().toFloat().reduced (6.0f);

    // Loudness column on the right: LUFS momentary / short-term / integrated, true-peak maximum.
    {
        auto column = r.removeFromRight ((float) loudnessColumnWidth - 6.0f);
        r.removeFromRight (6.0f);
        column.removeFromBottom (28.0f);                                   // the reset button
        g.setFont (theme::font (10.0f, true));
        g.setColour (theme::textMid);
        g.drawText ("LUFS", column.removeFromTop (12.0f), juce::Justification::centred, false);

        const char* labels[4] { "M", "S", "I", "TP" };
        const float rowHeight = juce::jmin (26.0f, column.getHeight() / 4.0f);
        for (int row = 0; row < 4; ++row)
        {
            auto line = column.removeFromTop (rowHeight);
            if (row == 3)
            {
                g.setColour (theme::border);
                g.fillRect (line.removeFromTop (1.0f));
            }
            g.setFont (theme::font (10.0f, true));
            g.setColour (theme::textLow);
            g.drawText (labels[row], line.removeFromLeft (20.0f), juce::Justification::centredLeft, false);
            g.setFont (theme::font (12.0f));
            const bool over = row == 3 && loudnessText[3] != "-" && loudnessText[3].getFloatValue() > 0.0f;
            g.setColour (over ? theme::warning : theme::textHi);           // a true peak above 0 dBTP is a warning
            g.drawText (loudnessText[(size_t) row], line, juce::Justification::centredRight, false);
        }
    }

    auto status = r.removeFromBottom (14.0f);
    r.removeFromBottom (2.0f);

    // Scale labels on the left.
    auto scale = r.removeFromLeft (20.0f);
    const auto barArea = juce::Rectangle<float> (scale.getX(), r.getY() + 12.0f, scale.getWidth(), r.getHeight() - 12.0f - 16.0f);
    g.setFont (theme::font (9.5f));
    g.setColour (theme::textLow);
    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f })
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (scale.getX(), yFor (barArea, db) - 6.0f, scale.getWidth() - 2.0f, 12.0f),
                    juce::Justification::centredRight, false);

    const float column = r.getWidth() / 3.0f;
    drawMeter (g, r.removeFromLeft (column), in, "IN", false);
    drawMeter (g, r.removeFromLeft (column), out, "OUT", false);
    drawMeter (g, r, sc, "SC", sidechainState == SidechainState::Off);

    g.setFont (theme::font (10.0f));
    g.setColour (sidechainState == SidechainState::Receiving ? theme::textHi : theme::textLow);
    g.drawText (sidechainStateText (sidechainState), status, juce::Justification::centredRight, false);
}
} // namespace tl::ui
