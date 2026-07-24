#include "MainComponent.h"

// Multiplayer Jam wiring — the whole seam between Orion and the Collab module lives here.
//
// The app's edit code is deliberately untouched: MainComponent never calls broadcast() from an edit
// path. Instead CollabReconciler (polled from timerCallback) diffs the project against its shadow
// and emits the ops. All this file does is start/stop a session and refresh the views when a peer's
// edit lands.

namespace orion
{
namespace
{
// One well-known port keeps "host / join" a two-click affair on a LAN. Remote sessions will point
// at a standalone server instead, which is the same SocketTransport with a different address.
constexpr int jamDefaultPort = 7777;

juce::String localDisplayName()
{
    auto name = juce::SystemStats::getFullUserName();
    if (name.isEmpty())
        name = juce::SystemStats::getLogonName();
    return name.isEmpty() ? juce::String("Producer") : name;
}

// Per-instance salt so two clients never mint colliding entity ids.
collab::EntityId randomActorSalt()
{
    return static_cast<collab::EntityId>(juce::Random::getSystemRandom().nextInt(0xffff));
}
}  // namespace

void MainComponent::refreshAfterRemoteJamEdit()
{
    // A peer's op (or the joining snapshot) has already mutated projectState. Re-baseline FIRST so
    // the reconciler doesn't diff the peer's change as one of ours and send it straight back.
    collabReconciler.captureBaseline();

    // Then refresh the views that read the project. Deliberately gentler than the project-load
    // path: no resetForNewProject(), which would drop the local user's selection and scroll
    // position mid-jam.
    refreshAudioClipWarpLengths();
    refreshClipInspector();
    refreshClipEditor();
    updateTransportLabels();
    arrangementTimeline.resized();
    arrangementTimeline.repaint();
    stepSequencer.repaint();
    mixerPanel.repaint();
}

void MainComponent::startJamHosting()
{
    if (collabController.isActive())
    {
        jamSession.setSessionStatus(true, "Already in a session.");
        return;
    }

    collabController.onProjectChanged = [this] { refreshAfterRemoteJamEdit(); };

    if (! collabController.hostSession(jamDefaultPort, localDisplayName(), randomActorSalt()))
    {
        collabController.onProjectChanged = nullptr;
        jamSession.setSessionStatus(false, "Could not host on port " + juce::String(jamDefaultPort)
                                               + " - already in use?");
        return;
    }

    // The reconciler starts from the project as it is right now, so nothing already on the timeline
    // is mistaken for a new local edit.
    collabReconciler.captureBaseline();

    const auto address = juce::IPAddress::getLocalAddress().toString();
    jamSession.setSessionStatus(true, "HOSTING  -  " + address + ":" + juce::String(jamDefaultPort)
                                          + "  (same Mac: 127.0.0.1)");
}

void MainComponent::joinJamSession()
{
    if (collabController.isActive())
    {
        jamSession.setSessionStatus(true, "Already in a session.");
        return;
    }

    auto* window = new juce::AlertWindow("Join Jam",
                                         "Address of the host (their Orion must be hosting):",
                                         juce::MessageBoxIconType::QuestionIcon);
    window->addTextEditor("address", "127.0.0.1", "Host address:");
    window->addButton("Join", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    // TopLevelWindow adds itself to the desktop but leaves the component invisible, and
    // enterModalState doesn't show it either — without this the dialog exists but is never drawn,
    // so clicking Join appears to do nothing at all.
    window->setVisible(true);
    window->toFront(true);

    window->enterModalState(true, juce::ModalCallbackFunction::create([this, window](int result)
    {
        const auto address = window->getTextEditorContents("address").trim();
        delete window;

        if (result != 1 || address.isEmpty())
            return;

        collabController.onProjectChanged = [this] { refreshAfterRemoteJamEdit(); };

        if (! collabController.joinSession(address, jamDefaultPort, localDisplayName(), randomActorSalt()))
        {
            collabController.onProjectChanged = nullptr;
            jamSession.setSessionStatus(false, "Could not reach " + address + ":"
                                                   + juce::String(jamDefaultPort)
                                                   + " - is the host up?");
            return;
        }

        // The host's project arrives as a snapshot moments later and re-baselines us again.
        collabReconciler.captureBaseline();
        jamSession.setSessionStatus(true, "JOINED  -  " + address + ":" + juce::String(jamDefaultPort));
    }), false);
}

void MainComponent::leaveJamSession()
{
    if (! collabController.isActive())
        return;

    collabController.disconnect();
    collabController.onProjectChanged = nullptr;
    jamSession.setSessionStatus(false, "Left the session.");
}
}  // namespace orion
