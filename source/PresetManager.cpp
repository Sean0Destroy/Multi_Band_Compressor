#include "PresetManager.h"

#include "BandSlots.h"
#include "ParameterSchema.h"
#include "PluginProcessor.h"
#include "StateSerialization.h"

namespace tl
{
namespace
{
const juce::Identifier kNonParamId { "NonParam" };
const juce::Identifier kPresetNameId { "presetName" };

// ---- Starter presets ---------------------------------------------------------------------------
// Written as data, turned into a state document on demand. They are starting points chosen by
// calculation, not by ear: nobody has listened to them yet.
struct StarterBand
{
    float lo, hi, threshold, ratio, knee, attack, release, range;
    std::vector<std::pair<const char*, float>> extra;   // further band parameters by suffix
};

struct Starter
{
    const char* name;
    std::vector<StarterBand> bands;
    std::vector<std::pair<const char*, float>> global;
};

const std::vector<Starter>& starters()
{
    using namespace schema;
    static const std::vector<Starter> s {
        { "Starter - Gentle 3 Band",
          { { 20.0f, 200.0f, -24.0f, 2.0f, 9.0f, 30.0f, 200.0f, 12.0f, {} },
            { 200.0f, 2000.0f, -22.0f, 2.0f, 9.0f, 15.0f, 150.0f, 12.0f, {} },
            { 2000.0f, 20000.0f, -26.0f, 2.0f, 9.0f, 5.0f, 100.0f, 12.0f, {} } },
          {} },
        { "Starter - Low End Control",
          { { 30.0f, 150.0f, -24.0f, 4.0f, 6.0f, 20.0f, 180.0f, 18.0f, { { band::freqRel, 50.0f }, { band::adaptRel, 30.0f } } } },
          {} },
        { "Starter - De-Ess",
          { { 5000.0f, 10000.0f, -30.0f, 6.0f, 3.0f, 0.5f, 60.0f, 9.0f, { { band::lookahead, 1.0f } } } },
          {} },
        { "Starter - Drum Bus 4 Band",
          { { 30.0f, 120.0f, -20.0f, 3.0f, 6.0f, 25.0f, 160.0f, 12.0f, { { band::transient, 40.0f } } },
            { 120.0f, 800.0f, -22.0f, 2.5f, 6.0f, 15.0f, 140.0f, 12.0f, { { band::transient, 50.0f } } },
            { 800.0f, 5000.0f, -24.0f, 2.5f, 6.0f, 8.0f, 110.0f, 12.0f, { { band::transient, 60.0f } } },
            { 5000.0f, 18000.0f, -28.0f, 2.0f, 6.0f, 3.0f, 90.0f, 9.0f, {} } },
          {} },
        { "Starter - External Sidechain Duck",
          { { 1000.0f, 5000.0f, -35.0f, 3.0f, 6.0f, 10.0f, 150.0f, 6.0f, { { band::scSource, 1.0f } } } },
          {} },
    };
    return s;
}

juce::String numberText (float v) { return juce::String (v, 6); }

juce::MemoryBlock starterState (const Starter& starter)
{
    juce::XmlElement root ("TL_STATE");
    root.setAttribute ("schemaVersion", state::currentSchemaVersion);

    const auto addParam = [&root] (const std::string& id, float value)
    {
        auto* e = root.createNewChildElement ("PARAM");
        e->setAttribute ("id", juce::String (id));
        e->setAttribute ("value", numberText (value));
    };

    Topology topology;
    for (size_t i = 0; i < starter.bands.size() && i < (size_t) schema::kActiveSlots; ++i)
    {
        const int slot = (int) i + 1;
        const auto& b = starter.bands[i];
        using namespace schema;
        addParam (bandParamId (slot, band::fLo), b.lo);
        addParam (bandParamId (slot, band::fHi), b.hi);
        addParam (bandParamId (slot, band::thresh), b.threshold);
        addParam (bandParamId (slot, band::ratio), b.ratio);
        addParam (bandParamId (slot, band::knee), b.knee);
        addParam (bandParamId (slot, band::attack), b.attack);
        addParam (bandParamId (slot, band::release), b.release);
        addParam (bandParamId (slot, band::range), b.range);
        for (auto& [suffix, value] : b.extra)
            addParam (bandParamId (slot, suffix), value);

        topology.used[i] = true;
        topology.everUsed[i] = true;
        topology.lastUse[i] = ++topology.counter;
    }

    for (auto& [id, value] : starter.global)
        addParam (id, value);

    if (auto x = topologyToTree (topology).createXml())
        root.addChildElement (x.release());

    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (root, block);
    return block;
}
} // namespace

PresetManager::PresetManager (PluginProcessor& p, juce::File userDirectory)
    : processor (p), directory (std::move (userDirectory))
{
}

juce::File PresetManager::defaultUserDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Tone Legion").getChildFile ("Multiband").getChildFile ("Presets");
}

juce::String PresetManager::cleanName (const juce::String& name)
{
    auto cleaned = juce::File::createLegalFileName (name.trim()).trim();
    while (cleaned.endsWithChar ('.')) cleaned = cleaned.dropLastCharacters (1).trim();
    return cleaned.substring (0, 64).trim();
}

std::vector<PresetManager::Entry> PresetManager::list() const
{
    std::vector<Entry> entries;
    for (auto& s : starters())
        entries.push_back ({ s.name, true, {} });

    juce::Array<juce::File> files;
    if (directory.isDirectory())
        files = directory.findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);

    juce::StringArray names;
    for (auto& f : files) names.add (f.getFileNameWithoutExtension());
    names.sortNatural();

    for (auto& n : names)
        entries.push_back ({ n, false, directory.getChildFile (n + fileExtension) });
    return entries;
}

juce::String PresetManager::getCurrentName() const
{
    return processor.getState().state.getChildWithName (kNonParamId).getProperty (kPresetNameId).toString();
}

void PresetManager::setCurrentName (const juce::String& name)
{
    auto& stateTree = processor.getState().state;
    auto nonParam = stateTree.getChildWithName (kNonParamId);
    if (! nonParam.isValid())
    {
        nonParam = juce::ValueTree (kNonParamId);
        stateTree.appendChild (nonParam, nullptr);
    }

    if (name.isEmpty()) nonParam.removeProperty (kPresetNameId, nullptr);
    else                nonParam.setProperty (kPresetNameId, name, nullptr);
}

juce::Result PresetManager::apply (const juce::MemoryBlock& block, const juce::String& name)
{
    // Check the document before touching anything: a preset must be a valid state.
    juce::String error;
    auto parsed = state::parse (block.getData(), block.getSize(), processor.getState().state.getType(), error);
    if (! parsed.has_value())                 return juce::Result::fail (error.isNotEmpty() ? error : "Not a preset");
    if (! state::validate (*parsed, error))   return juce::Result::fail (error.isNotEmpty() ? error : "Invalid preset");

    processor.setStateKeepingSession (block.getData(), (int) block.getSize());   // window size and engine stay
    setCurrentName (name);
    processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    return juce::Result::ok();
}

juce::Result PresetManager::load (const Entry& entry)
{
    if (entry.builtIn)
    {
        for (auto& s : starters())
            if (entry.name == s.name)
                return apply (starterState (s), entry.name);
        return juce::Result::fail ("Unknown starter preset");
    }

    if (! entry.file.existsAsFile())
        return juce::Result::fail ("Preset file not found");

    const auto xml = juce::parseXML (entry.file);
    if (xml == nullptr)
        return juce::Result::fail ("The preset file could not be read");

    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (*xml, block);
    return apply (block, entry.name);
}

juce::Result PresetManager::loadByName (const juce::String& name)
{
    for (auto& e : list())
        if (e.name == name)
            return load (e);
    return juce::Result::fail ("No preset with that name");
}

juce::Result PresetManager::saveUser (const juce::String& requestedName)
{
    const auto name = cleanName (requestedName);
    if (name.isEmpty())
        return juce::Result::fail ("Enter a name for the preset");

    for (auto& s : starters())
        if (name.equalsIgnoreCase (s.name))
            return juce::Result::fail ("That name belongs to a starter preset");

    if (! directory.isDirectory() && ! directory.createDirectory())
        return juce::Result::fail ("The preset folder could not be created");

    setCurrentName (name);                       // saved inside the preset as well

    juce::MemoryBlock block;
    processor.getStateInformation (block);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
    if (xml == nullptr)
        return juce::Result::fail ("The state could not be written");

    const auto file = directory.getChildFile (name + fileExtension);
    if (! file.replaceWithText (xml->toString()))
        return juce::Result::fail ("The preset file could not be written");

    processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    return juce::Result::ok();
}

juce::Result PresetManager::deleteUser (const juce::String& name)
{
    const auto file = directory.getChildFile (cleanName (name) + fileExtension);
    if (! file.existsAsFile())  return juce::Result::fail ("No user preset with that name");
    if (! file.deleteFile())    return juce::Result::fail ("The preset file could not be deleted");
    if (getCurrentName() == cleanName (name)) setCurrentName ({});
    return juce::Result::ok();
}

juce::Result PresetManager::step (int direction)
{
    const auto entries = list();
    if (entries.empty())
        return juce::Result::fail ("No presets");

    const auto current = getCurrentName();
    int index = -1;
    for (int i = 0; i < (int) entries.size(); ++i)
        if (entries[(size_t) i].name == current) { index = i; break; }

    const int count = (int) entries.size();
    const int next = index < 0 ? (direction > 0 ? 0 : count - 1) : (index + direction + count) % count;
    return load (entries[(size_t) next]);
}

juce::Result PresetManager::loadNext()     { return step (+1); }
juce::Result PresetManager::loadPrevious() { return step (-1); }

void PresetManager::loadEmpty()
{
    juce::XmlElement root ("TL_STATE");
    root.setAttribute ("schemaVersion", state::currentSchemaVersion);
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (root, block);
    apply (block, {});
}

// ---- A/B ---------------------------------------------------------------------------------------
void PresetManager::switchTo (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == activeSlot)
        return;

    processor.getStateInformation (slots[activeSlot]);

    if (slots[slot].getSize() == 0)
        slots[slot] = slots[activeSlot];         // first visit: B starts as a copy of A
    else
        processor.setStateKeepingSession (slots[slot].getData(), (int) slots[slot].getSize());

    activeSlot = slot;
    processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
}

void PresetManager::copyActiveToOther()
{
    processor.getStateInformation (slots[activeSlot]);
    slots[1 - activeSlot] = slots[activeSlot];
}
} // namespace tl
