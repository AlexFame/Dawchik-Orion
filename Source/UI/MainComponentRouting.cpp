// Mixer routing for MainComponent: buses, sends, output routes, and the per-track insert FX
// chain (add / replace / remove / bypass / move / copy, and their editor windows).
//
// Split out of MainComponent.cpp purely to keep that file manageable — this is the SAME
// class, same members, identical logic; only the definitions live here. See MainComponent.h
// for the declarations.
#include "MainComponent.h"

#include "../Audio/PlaybackSources.h"
#include "OrionTheme.h"

#include <memory>
#include <vector>

namespace orion
{
std::vector<TrackState::InsertFx>* MainComponent::insertChainForId(int id)
{
    if (id == ArrangementPlaybackSource::kMasterInsertKey)
        return &projectState.getMasterInserts();
    if (id >= ArrangementPlaybackSource::kBusInsertKeyBase)
    {
        const auto b = id - ArrangementPlaybackSource::kBusInsertKeyBase;
        auto& buses = projectState.getBuses();
        return (b >= 0 && b < static_cast<int>(buses.size())) ? &buses[static_cast<std::size_t>(b)].inserts : nullptr;
    }
    auto& tracks = projectState.getTracks();
    return (id >= 0 && id < static_cast<int>(tracks.size())) ? &tracks[static_cast<std::size_t>(id)].inserts : nullptr;
}

juce::String MainComponent::insertOwnerName(int id) const
{
    if (id == ArrangementPlaybackSource::kMasterInsertKey)
        return "Master";
    if (id >= ArrangementPlaybackSource::kBusInsertKeyBase)
    {
        const auto b = id - ArrangementPlaybackSource::kBusInsertKeyBase;
        const auto& buses = projectState.getBuses();
        return (b >= 0 && b < static_cast<int>(buses.size())) ? buses[static_cast<std::size_t>(b)].name : juce::String("Bus");
    }
    const auto& tracks = projectState.getTracks();
    return (id >= 0 && id < static_cast<int>(tracks.size())) ? tracks[static_cast<std::size_t>(id)].name : juce::String("Track");
}

void MainComponent::addBus()
{
    auto& buses = projectState.getBuses();
    orion::BusState bus;
    bus.name = "Bus " + juce::String(static_cast<int>(buses.size()) + 1);
    buses.push_back(std::move(bus));
    if (mixerPanel.isVisible()) mixerPanel.refreshStrips();
}

void MainComponent::showSendMenuForTrack(int trackIndex, int sendIndex)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    auto& buses = projectState.getBuses();

    juce::PopupMenu menu;

    // Existing send row: change level or remove.
    if (sendIndex >= 0 && sendIndex < static_cast<int>(track.sends.size()))
    {
        const std::array<int, 5> pcts { 10, 25, 50, 75, 100 };
        const auto& send = track.sends[static_cast<std::size_t>(sendIndex)];
        const auto cur = juce::roundToInt(send.level * 100.0);
        for (int i = 0; i < 5; ++i)
            menu.addItem(10 + i, juce::String(pcts[static_cast<std::size_t>(i)]) + "%", true, cur == pcts[static_cast<std::size_t>(i)]);
        menu.addSeparator();
        menu.addItem(8, send.prefader ? "Pre-fader (on)" : "Pre-fader", true, send.prefader);
        menu.addItem(9, "Remove send");
        menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex, sendIndex, pcts](int r)
        {
            auto& tr = projectState.getTracks();
            if (trackIndex >= static_cast<int>(tr.size())) return;
            auto& sends = tr[static_cast<std::size_t>(trackIndex)].sends;
            if (sendIndex >= static_cast<int>(sends.size())) return;
            if (r == 8) sends[static_cast<std::size_t>(sendIndex)].prefader = ! sends[static_cast<std::size_t>(sendIndex)].prefader;
            else if (r == 9) sends.erase(sends.begin() + sendIndex);
            else if (r >= 10 && r < 15) sends[static_cast<std::size_t>(sendIndex)].level = pcts[static_cast<std::size_t>(r - 10)] / 100.0;
            if (mixerPanel.isVisible()) mixerPanel.refreshStrips();
        });
        return;
    }

    // Add slot: pick a bus to send to (or create one).
    if (buses.empty())
    {
        menu.addItem(200, "New FX Bus (send here)");
    }
    else
    {
        for (int b = 0; b < static_cast<int>(buses.size()); ++b)
            menu.addItem(100 + b, juce::String::fromUTF8("\xe2\x86\x92 ") + buses[static_cast<std::size_t>(b)].name);
        menu.addSeparator();
        menu.addItem(200, "New FX Bus");
    }
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex](int r)
    {
        auto& tr = projectState.getTracks();
        if (trackIndex >= static_cast<int>(tr.size())) return;
        int busIndex = -1;
        if (r == 200) { addBus(); busIndex = static_cast<int>(projectState.getBuses().size()) - 1; }
        else if (r >= 100) busIndex = r - 100;
        if (busIndex < 0 || busIndex >= static_cast<int>(projectState.getBuses().size())) return;
        TrackState::SendFx s; s.busIndex = busIndex; s.level = 0.25;
        tr[static_cast<std::size_t>(trackIndex)].sends.push_back(s);
        if (mixerPanel.isVisible()) mixerPanel.refreshStrips();
    });
}

void MainComponent::showOutputRouteMenuForTrack(int trackIndex)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    const auto& buses = projectState.getBuses();

    juce::PopupMenu menu;
    menu.addItem(1, "Master", true, track.outputBus < 0);
    if (! buses.empty())
    {
        menu.addSeparator();
        for (int b = 0; b < static_cast<int>(buses.size()); ++b)
            menu.addItem(100 + b, buses[static_cast<std::size_t>(b)].name, true, track.outputBus == b);
    }
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex](int r)
    {
        auto& tr = projectState.getTracks();
        if (trackIndex >= static_cast<int>(tr.size())) return;
        if (r == 1)            tr[static_cast<std::size_t>(trackIndex)].outputBus = -1;
        else if (r >= 100)     tr[static_cast<std::size_t>(trackIndex)].outputBus = r - 100;
        if (mixerPanel.isVisible()) mixerPanel.repaint();
    });
}

void MainComponent::showInsertMenuForTrack(int trackIndex, int insertIndex)
{
    auto* chainPtr = insertChainForId(trackIndex);
    if (chainPtr == nullptr)
        return;
    auto& chain = *chainPtr;

    juce::PopupMenu menu;

    // VST3 effects list (shared by add + replace).
    juce::Array<juce::PluginDescription> effects;
    for (const auto& d : pluginManager.getAllDescriptions())
        if (! d.isInstrument)
            effects.add(d);

    // Clicking an existing insert chip: open / bypass / replace / remove.
    if (insertIndex >= 0 && insertIndex < static_cast<int>(chain.size()))
    {
        const auto& fx = chain[static_cast<std::size_t>(insertIndex)];
        menu.addItem(1, "Open " + fx.pluginName);
        menu.addItem(2, fx.bypassed ? "Un-bypass" : "Bypass", true, fx.bypassed);

        juce::PopupMenu replaceMenu;
        {
            int id = 2000;
            for (const auto& d : effects)
            {
                auto label = d.name;
                if (d.manufacturerName.isNotEmpty()) label += " - " + d.manufacturerName;
                replaceMenu.addItem(id++, label);
            }
        }
        menu.addSubMenu("Replace with", replaceMenu, ! effects.isEmpty());
        menu.addItem(3, "Remove");

        const auto list = effects;
        menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex, insertIndex, list](int r)
        {
            if (r == 1) openInsertEditor(trackIndex, insertIndex);
            else if (r == 2) toggleInsertBypass(trackIndex, insertIndex);
            else if (r == 3) removeInsertFromTrack(trackIndex, insertIndex);
            else if (r >= 2000)
            {
                const auto i = r - 2000;
                if (i >= 0 && i < list.size())
                    replaceInsertOnTrack(trackIndex, insertIndex, list.getReference(i));
            }
        });
        return;
    }

    // The "+" add slot: pick a VST3 effect to append.
    juce::PopupMenu addMenu;
    if (effects.isEmpty())
        addMenu.addItem(900, pluginManager.isScanning() ? "Scanning..." : "No VST3 effects found — Rescan...", ! pluginManager.isScanning());
    else
    {
        int id = 1000;
        for (const auto& d : effects)
        {
            auto label = d.name;
            if (d.manufacturerName.isNotEmpty()) label += " - " + d.manufacturerName;
            addMenu.addItem(id++, label);
        }
    }
    menu.addSubMenu("Add effect", addMenu);
    menu.addSeparator();
    menu.addItem(901, pluginManager.isScanning() ? "Scanning..." : "Rescan VST3 plugins...", ! pluginManager.isScanning());

    const auto list = effects;
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex, list](int r)
    {
        if (r == 900 || r == 901) { scanPluginsInteractively(); return; }
        if (r >= 1000)
        {
            const auto i = r - 1000;
            if (i >= 0 && i < list.size())
                addInsertOnTrack(trackIndex, list.getReference(i));
        }
    });
}

void MainComponent::addInsertOnTrack(int trackIndex, const juce::PluginDescription& description)
{
    auto* chainPtr = insertChainForId(trackIndex);
    if (chainPtr == nullptr || arrangementPlaybackSource == nullptr)
        return;

    juce::String error;
    auto instance = pluginManager.createInstance(description, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
    if (instance == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not load effect",
                                               error.isNotEmpty() ? error : juce::String("Unknown error."));
        return;
    }

    const auto idx = arrangementPlaybackSource->addInsert(trackIndex, std::move(instance), false);
    if (idx < 0)
        return;

    TrackState::InsertFx fx;
    fx.pluginId = description.createIdentifierString();
    fx.pluginName = description.name;
    chainPtr->push_back(std::move(fx));

    statusLabel.setText("Insert added: " + description.name, juce::dontSendNotification);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
    arrangementTimeline.repaint();
    openInsertEditor(trackIndex, idx);
}

void MainComponent::replaceInsertOnTrack(int trackIndex, int insertIndex, const juce::PluginDescription& description)
{
    auto* chainPtr = insertChainForId(trackIndex);
    if (chainPtr == nullptr || arrangementPlaybackSource == nullptr)
        return;
    auto& chain = *chainPtr;
    if (insertIndex < 0 || insertIndex >= static_cast<int>(chain.size()))
        return;

    juce::String error;
    auto instance = pluginManager.createInstance(description, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
    if (instance == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not load effect",
                                               error.isNotEmpty() ? error : juce::String("Unknown error."));
        return;
    }

    const auto bypassed = chain[static_cast<std::size_t>(insertIndex)].bypassed;
    insertEditorWindows.erase({ trackIndex, insertIndex });

    // Engine: drop the old instance, add the new one and move it to the same slot.
    arrangementPlaybackSource->removeInsert(trackIndex, insertIndex);
    const auto appended = arrangementPlaybackSource->addInsert(trackIndex, std::move(instance), bypassed);
    if (appended != insertIndex)
        arrangementPlaybackSource->moveInsert(trackIndex, appended, trackIndex, insertIndex);

    // Model: replace the entry in place.
    TrackState::InsertFx fx;
    fx.pluginId = description.createIdentifierString();
    fx.pluginName = description.name;
    fx.bypassed = bypassed;
    chain[static_cast<std::size_t>(insertIndex)] = std::move(fx);

    statusLabel.setText("Replaced with " + description.name, juce::dontSendNotification);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
    openInsertEditor(trackIndex, insertIndex);
}

void MainComponent::removeInsertFromTrack(int trackIndex, int insertIndex)
{
    insertEditorWindows.erase({ trackIndex, insertIndex });
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->removeInsert(trackIndex, insertIndex);

    if (auto* chainPtr = insertChainForId(trackIndex))
        if (insertIndex >= 0 && insertIndex < static_cast<int>(chainPtr->size()))
            chainPtr->erase(chainPtr->begin() + insertIndex);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
}

void MainComponent::toggleInsertBypass(int trackIndex, int insertIndex)
{
    auto* chainPtr = insertChainForId(trackIndex);
    if (chainPtr == nullptr)
        return;
    auto& chain = *chainPtr;
    if (insertIndex < 0 || insertIndex >= static_cast<int>(chain.size()))
        return;
    chain[static_cast<std::size_t>(insertIndex)].bypassed = ! chain[static_cast<std::size_t>(insertIndex)].bypassed;
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->setInsertBypass(trackIndex, insertIndex, chain[static_cast<std::size_t>(insertIndex)].bypassed);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
}

void MainComponent::openInsertEditor(int trackIndex, int insertIndex)
{
    if (arrangementPlaybackSource == nullptr)
        return;
    auto* instance = arrangementPlaybackSource->getInsertInstance(trackIndex, insertIndex);
    if (instance == nullptr)
        return;

    const std::pair<int, int> key { trackIndex, insertIndex };
    if (auto it = insertEditorWindows.find(key); it != insertEditorWindows.end() && it->second != nullptr)
    {
        it->second->toFront(true);
        return;
    }

    juce::String title = insertOwnerName(trackIndex);
    if (auto* chain = insertChainForId(trackIndex); chain != nullptr
        && insertIndex >= 0 && insertIndex < static_cast<int>(chain->size()))
        title += " - " + (*chain)[static_cast<std::size_t>(insertIndex)].pluginName;

    insertEditorWindows[key] = std::make_unique<PluginEditorWindow>(
        *instance, title,
        [this, key]() { insertEditorWindows.erase(key); },
        [this](const juce::KeyPress& k) { return keyPressed(k); },
        [this](bool down) { return keyStateChanged(down); });
}

void MainComponent::moveInsert(int fromTrack, int fromIndex, int toTrack, int toIndex)
{
    auto& tracks = projectState.getTracks();
    if (fromTrack < 0 || fromTrack >= static_cast<int>(tracks.size())
        || toTrack < 0 || toTrack >= static_cast<int>(tracks.size()))
        return;
    auto& fromChain = tracks[static_cast<std::size_t>(fromTrack)].inserts;
    if (fromIndex < 0 || fromIndex >= static_cast<int>(fromChain.size()))
        return;
    if (fromTrack == toTrack && (toIndex == fromIndex || toIndex == fromIndex + 1))
        return;   // no-op move

    // Editor windows are keyed by (track,index); indices shift, so just close them.
    insertEditorWindows.clear();

    // Move in the engine (the live instance).
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->moveInsert(fromTrack, fromIndex, toTrack, toIndex);

    // Mirror in the model.
    auto fx = fromChain[static_cast<std::size_t>(fromIndex)];
    fromChain.erase(fromChain.begin() + fromIndex);
    auto& destChain = tracks[static_cast<std::size_t>(toTrack)].inserts;
    auto clamped = juce::jlimit(0, static_cast<int>(destChain.size()), toIndex);
    destChain.insert(destChain.begin() + clamped, std::move(fx));

    if (mixerPanel.isVisible()) mixerPanel.repaint();
}

void MainComponent::copyInsertToTrack(int fromTrack, int fromIndex, int toTrack)
{
    auto& tracks = projectState.getTracks();
    if (fromTrack < 0 || fromTrack >= static_cast<int>(tracks.size())
        || toTrack < 0 || toTrack >= static_cast<int>(tracks.size())
        || arrangementPlaybackSource == nullptr)
        return;
    auto& fromChain = tracks[static_cast<std::size_t>(fromTrack)].inserts;
    if (fromIndex < 0 || fromIndex >= static_cast<int>(fromChain.size()))
        return;

    const auto src = fromChain[static_cast<std::size_t>(fromIndex)];   // copy of the model entry
    const auto desc = pluginManager.findDescription(src.pluginId);
    if (! desc.has_value())
        return;

    juce::String error;
    auto instance = pluginManager.createInstance(*desc, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
    if (instance == nullptr)
        return;

    // Copy the source plugin's current settings onto the new instance.
    if (auto* srcInst = arrangementPlaybackSource->getInsertInstance(fromTrack, fromIndex))
    {
        juce::MemoryBlock state;
        srcInst->getStateInformation(state);
        if (state.getSize() > 0)
            instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }

    arrangementPlaybackSource->addInsert(toTrack, std::move(instance), src.bypassed);
    tracks[static_cast<std::size_t>(toTrack)].inserts.push_back(src);   // append copy to target

    statusLabel.setText("Copied " + src.pluginName + " to another track", juce::dontSendNotification);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
}
} // namespace orion
