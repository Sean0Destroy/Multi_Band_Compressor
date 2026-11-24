#include "MeterRail.h"

namespace tl::ui
{
MeterRail::MeterRail (PluginProcessor& p) : processor (p)
{
    setTitle ("Level meters");
    setDescription ("Input, output and sidechain levels in dBFS, and the loudness of the output.");
    setInterceptsMouseClicks (false, true);      // only the reset button takes clicks

    resetButton.setButtonText ("Reset");
    resetButton.setComponentID ("action_resetLoudness");
    resetButton.setTitle ("Reset loudness and true peak");
    resetButton.setTooltip ("Start the integrated loudness and the true-peak maximum again");
    resetButton.getProperties().set (style::small, true);
    resetButton.onClick = [this] { processor.getLoudnessMeter().requestReset(); };
    addAndMakeVisible (resetButton);

    for (auto& t : loudnessText) t = "-";
}

void MeterRail::resized()
{
    auto head = getLocalBounds().removeFromTop (headHeight).reduced (12, 9);
    resetButton.setBounds (head.removeFromRight (58).withSizeKeepingCentre (58, 24));
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

float MeterRail::barFraction (float db) noexcept
{
    // The upper part of the scale gets the most room: -6 dB sits at 71 %, -24 dB at 34 %.
    if (! (db > -48.0f)) return 0.0f;
    return 1.0f - std::pow (juce::jlimit (0.0f, 48.0f, -db) / 48.0f, 0.6f);
}

void MeterRail::drawGroup (juce::Graphics& g, juce::Rectangle<float> area, const Reading& r, const juce::String& title, bool dimmed) const
{
    auto label = area.removeFromBottom (14.0f);
    area.removeFromBottom (4.0f);

    g.setColour (dimmed ? theme::textLow : theme::textMid);
    theme::drawTracked (g, title, label, juce::Justification::centred, theme::label (10.0f, 0.28f));

    const float barWidth = 9.0f, gap = 3.0f;
    float x = area.getCentreX() - barWidth - gap * 0.5f;
    for (int c = 0; c < 2; ++c, x += barWidth + gap)
    {
        const juce::Rectangle<float> bar (x, area.getY(), barWidth, area.getHeight());
        g.setColour (juce::Colour (0xff191d23));
        g.fillRect (bar);

        const bool live = c < juce::jmax (1, r.channels) && ! dimmed;
        const float level = live ? barFraction (r.peakDb[c]) : 0.0f;
        if (level > 0.0f)
        {
            // Gradient over the whole bar (deep ice, ice, pale ice, clip red at the top), revealed up to the level.
            juce::ColourGradient grad (juce::Colour (0xff6fa8bb), bar.getX(), bar.getBottom(), theme::clip, bar.getX(), bar.getY(), false);
            grad.addColour (0.70, theme::accent);
            grad.addColour (0.88, juce::Colour (0xffd6eef6));
            grad.addColour (0.96, theme::clip);
            g.setGradientFill (grad);
            g.fillRect (bar.withTop (bar.getBottom() - level * bar.getHeight()));
        }

        const float held = live ? barFraction (r.heldDb) : 0.0f;
        if (held > 0.0f)
        {
            g.setColour (r.clipped ? theme::clip : theme::textHi);
            g.fillRect (bar.getX(), bar.getBottom() - held * bar.getHeight(), bar.getWidth(), 2.0f);
        }
    }
}

void MeterRail::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    // Head: MASTER, the sidechain state, the loudness reset (a button).
    {
        auto head = r.removeFromTop ((float) headHeight);
        g.setColour (theme::lattice);
        g.fillRect (head.getX() + 1.0f, head.getBottom() - 1.0f, head.getWidth() - 2.0f, 1.0f);
        head = head.reduced (12.0f, 0.0f);
        g.setColour (theme::textHi);
        theme::drawTracked (g, "MASTER", head, juce::Justification::centredLeft, theme::display (13.0f, 0.3f));
        head.removeFromRight (66.0f);
        g.setColour (sidechainState == SidechainState::Receiving ? theme::accent : theme::textLow);
        theme::drawTracked (g, sidechainStateText (sidechainState).toUpperCase(), head, juce::Justification::centredRight,
                            theme::label (9.0f, 0.16f));
    }

    // Level bars in a well.
    {
        const int wellH = juce::jlimit (minWellHeight, wellHeight, getHeight() - (preferredHeight - wellHeight));
        auto well = r.removeFromTop ((float) wellH + 10.0f).withTrimmedTop (10.0f).reduced (12.0f, 0.0f);
        theme::drawWell (g, well);
        auto inner = well.reduced (10.0f, 8.0f);
        auto bars = inner.withTrimmedBottom (18.0f);

        auto scale = inner.removeFromLeft (26.0f);
        g.setFont (theme::mono (9.0f));
        g.setColour (theme::textLow);
        for (int db : { 0, -6, -12, -24, -48 })
        {
            const float y = bars.getBottom() - barFraction ((float) db + 0.001f) * bars.getHeight();
            const auto text = db == 0 ? juce::String ("0") : juce::String::fromUTF8 ("\xe2\x88\x92") + juce::String (-db);
            g.drawText (text, juce::Rectangle<float> (scale.getX(), y - 6.0f, scale.getWidth() - 4.0f, 12.0f),
                        juce::Justification::centredRight, false);
        }

        inner.removeFromLeft (6.0f);
        const float w = inner.getWidth() / 3.0f;
        drawGroup (g, inner.removeFromLeft (w), in, "IN", false);
        drawGroup (g, inner.removeFromLeft (w), out, "OUT", false);
        drawGroup (g, inner, sc, "SC", sidechainState == SidechainState::Off);
    }

    // Loudness cells: momentary, short-term, integrated (LUFS) and the true-peak maximum (dBTP).
    {
        auto row = r.withTrimmedTop (10.0f).reduced (12.0f, 0.0f).removeFromTop (36.0f);
        const char* labels[4] { "M", "S", "I", "TP" };
        const float gap = 4.0f, cw = (row.getWidth() - 3.0f * gap) / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            const auto cell = juce::Rectangle<float> (row.getX() + (float) i * (cw + gap), row.getY(), cw, row.getHeight());
            g.setColour (theme::background);
            g.fillRect (cell);
            g.setColour (theme::border);
            g.drawRect (cell, 1.0f);

            auto inner = cell.reduced (0.0f, 4.0f);
            g.setColour (theme::textMid);
            theme::drawTracked (g, labels[i], inner.removeFromTop (11.0f), juce::Justification::centred, theme::label (8.0f, 0.2f));
            const auto& text = loudnessText[(size_t) i];
            const bool over = i == 3 && text != "-" && text.getFloatValue() > 0.0f;      // above 0 dBTP: a warning
            g.setColour (over ? theme::warning : theme::accent);
            g.setFont (theme::mono (12.0f));
            g.drawText (text.replaceCharacter ('-', (juce::juce_wchar) 0x2212), inner, juce::Justification::centred, false);
        }
    }
}
} // namespace tl::ui
