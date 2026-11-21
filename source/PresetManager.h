#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace tl
{
class PluginProcessor;

/** Presets and the A/B comparison. MESSAGE THREAD ONLY (file access, state loading).

    A preset is the plugin state as readable XML (the same document the host saves, see
    StateSerialization.h), so presets go through the same validation and migration as host
    state. Starter presets are built in; user presets are `.tlmb` files in one folder.
    The name of the last loaded / saved preset is kept in the state (NonParam). */
class PresetManager
{
public:
    struct Entry
    {
        juce::String name;
        bool builtIn = false;
        juce::File file;        // user presets only
        bool operator== (const Entry& o) const { return name == o.name && builtIn == o.builtIn; }
    };

    /** `userDirectory` is created on the first save. */
    explicit PresetManager (PluginProcessor&, juce::File userDirectory = defaultUserDirectory());

    static juce::File defaultUserDirectory();
    static constexpr const char* fileExtension = ".tlmb";
    juce::File getUserDirectory() const { return directory; }

    /** Starter presets first (fixed order), then user presets by name. */
    std::vector<Entry> list() const;

    /** Loads a preset. On failure the current state is untouched and the reason is returned. */
    juce::Result load (const Entry&);
    juce::Result loadByName (const juce::String& name);

    /** Saves the current state as a user preset (overwrites one with the same name). The name
        is cleaned of characters that are not allowed in file names; empty names are refused. */
    juce::Result saveUser (const juce::String& name);
    juce::Result deleteUser (const juce::String& name);

    /** Steps through list() (wraps). With no current preset, next() loads the first. */
    juce::Result loadNext();
    juce::Result loadPrevious();

    /** Empty when the state did not come from a preset. */
    juce::String getCurrentName() const;

    /** A new instance: no bands, every parameter at its default. */
    void loadEmpty();

    static juce::String cleanName (const juce::String&);

    // ---- A/B -----------------------------------------------------------------------------------
    /** Two state snapshots for comparing. Switching stores the current state in the active
        slot and loads the other one (the first switch copies A to B). Not saved with the project. */
    int getActiveSlot() const noexcept { return activeSlot; }   // 0 = A, 1 = B
    void switchTo (int slot);
    void copyActiveToOther();

private:
    juce::Result step (int direction);
    juce::Result apply (const juce::MemoryBlock& state, const juce::String& name);
    void setCurrentName (const juce::String&);

    PluginProcessor& processor;
    juce::File directory;
    int activeSlot = 0;
    juce::MemoryBlock slots[2];
};
} // namespace tl
