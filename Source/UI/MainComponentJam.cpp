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

// The actor id identifies a CONNECTION, not a person, and must be unique per instance: incoming
// ops whose actor matches ours are discarded as our own server echo. Using the display name here
// meant two Orions run by the same macOS user shared an id, so each threw away the other's edits
// as if they were its own — the session looked connected but nothing ever synced.
juce::String localActorId()
{
    static const juce::String id = localDisplayName() + "#" + juce::Uuid().toDashedString();
    return id;
}

// Per-instance salt so two clients never mint colliding entity ids.
collab::EntityId randomActorSalt()
{
    return static_cast<collab::EntityId>(juce::Random::getSystemRandom().nextInt(0xffff));
}

}  // namespace

// A stable colour per collaborator, derived from their actor id so both ends agree without any
// negotiation and the same person keeps the same colour all session.
static juce::uint32 colourForActor(const juce::String& actor)
{
    static const juce::uint32 palette[] = {
        0xffd9785f, 0xff6957a8, 0xffa8774f, 0xffd2a98e, 0xff4f9dd2, 0xff5fbf8a, 0xffc25fa8, 0xffd2c25f
    };
    const auto hash = static_cast<juce::uint32>(std::abs(static_cast<int>(actor.hashCode())));
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

void MainComponent::publishJamPresence()
{
    if (! collabController.isActive())
        return;

    // Where our pointer is over the arrangement, in musical coordinates. Polling the component
    // beats hooking every mouse handler: no existing edit path has to change.
    double beat = 0.0;
    double contentY = 0.0;
    const auto overTimeline = arrangementTimeline.pointToProjectPosition(
        arrangementTimeline.getMouseXYRelative(), beat, contentY);

    const auto me = localActorId();
    collabController.publishPresence(localDisplayName(), colourForActor(me), beat, contentY, overTimeline);

    // And hand the peers' cursors to the timeline, which draws them without knowing what collab is.
    std::vector<ArrangementTimelineComponent::RemoteCursor> cursors;
    for (const auto& p : collabController.peers())
        if (p.overTimeline)
            cursors.push_back({ p.name, juce::Colour(p.colourArgb), p.beat, p.contentY });

    arrangementTimeline.setRemoteCursors(std::move(cursors));
}

void MainComponent::sendJamChat(const juce::String& text)
{
    collabController.sendChat(localDisplayName(), text);
}

void MainComponent::updateJamDiagnostics()
{
    jamSession.setDiagnostics(collabController.diagnosticsLine());

    // Real roster: this machine first, then every live peer with the colour it uses for its cursor.
    std::vector<JamSessionComponent::RosterMember> roster;
    const auto me = localActorId();
    roster.push_back({ localDisplayName() + " (you)", colourForActor(me), true });
    for (const auto& p : collabController.peers())
        roster.push_back({ p.name, p.colourArgb, false });
    jamSession.setRoster(roster);
}

void MainComponent::applyRemoteJamTransport(bool playing, double beat)
{
    // A peer hit play/stop. Match it, driving our transport through the same UI path so audio
    // sources spin up correctly — not a bare TransportEngine::play() which wouldn't start playback.
    // The guard stops our own timer poll from re-broadcasting this as a local change (feedback loop).
    jamApplyingRemoteTransport = true;

    if (playing)
    {
        transportEngine.setPlayheadBeat(beat);
        if (! transportEngine.isPlaying())
            toggleTransportFromUi();
    }
    else
    {
        if (transportEngine.isPlaying())
            toggleTransportFromUi();
        transportEngine.setPlayheadBeat(beat);
    }

    jamLastSentPlaying = transportEngine.isPlaying();
    jamApplyingRemoteTransport = false;
    updateTransportLabels();
}

void MainComponent::syncJamTransportOut()
{
    // Broadcast only the play/stop TRANSITION (not every position tick — that would flood the wire).
    // The start position rides with the play event, so peers begin from the same beat.
    if (! collabController.isActive() || jamApplyingRemoteTransport)
        return;

    const bool playing = transportEngine.isPlaying();
    if (playing != jamLastSentPlaying)
    {
        jamLastSentPlaying = playing;
        collabController.sendTransport(playing, transportEngine.getPlayheadBeat());
    }
}

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
    collabController.onRemoteTransport = [this](bool playing, double beat) { applyRemoteJamTransport(playing, beat); };
    collabController.onRemoteChat = [this](const juce::String& name, const juce::String& text) { jamSession.receiveChat(name, text); };
    jamLastSentPlaying = transportEngine.isPlaying();

    if (! collabController.hostSession(jamDefaultPort, localActorId(), randomActorSalt()))
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
        collabController.onRemoteTransport = [this](bool playing, double beat) { applyRemoteJamTransport(playing, beat); };
        collabController.onRemoteChat = [this](const juce::String& name, const juce::String& text) { jamSession.receiveChat(name, text); };
        jamLastSentPlaying = transportEngine.isPlaying();

        if (! collabController.joinSession(address, jamDefaultPort, localActorId(), randomActorSalt()))
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
