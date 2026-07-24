#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_video/juce_video.h>

#include <array>
#include <functional>
#include <vector>

namespace orion
{
class JamSessionComponent final : public juce::Component,
                                  public juce::DragAndDropTarget,
                                  private juce::Button::Listener,
                                  private juce::TextEditor::Listener,
                                  private juce::Timer
{
public:
    JamSessionComponent();

    std::function<void()> onClose;
    // Fired when the user actually wants a multiplayer session opened/joined. The host handles the
    // networking (Source/Collab); this panel only owns the presentation.
    std::function<void()> onCreateSessionRequested;
    std::function<void()> onJoinSessionRequested;
    std::function<bool(bool)> onMicEnabledChanged;
    std::function<bool(bool)> onCameraEnabledChanged;
    std::function<void(bool)> onShareEnabledChanged;

    void setEmbeddedArrangementMode(bool shouldUseEmbeddedMode);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool hitTest(int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override;
    void itemDragEnter(const SourceDetails& dragSourceDetails) override;
    void itemDragExit(const SourceDetails& dragSourceDetails) override;
    void itemDropped(const SourceDetails& dragSourceDetails) override;

private:
    enum class PanelMode
    {
        chat,
        participants,
        setup
    };

    enum class ConnectionState
    {
        ready,
        live
    };

    struct Participant
    {
        juce::String name;
        juce::Colour colour;
        bool muted { false };
        bool cameraOff { false };
        bool sharing { false };
    };

    struct BeatLane
    {
        juce::String name;
        juce::Colour colour;
        std::array<bool, 16> steps {};
        int ownerIndex { 0 };
    };

    void buttonClicked(juce::Button*) override;
    void textEditorReturnKeyPressed(juce::TextEditor&) override;
    void timerCallback() override;
    void sendChatMessage();
    void addChatMessage(const juce::String& name, const juce::String& message);
    void openFileAttachmentChooser(bool foldersOnly);
    void addFileAttachments(const juce::Array<juce::File>& files);
    void refreshControls();
    void updateCallControlsVisibility();
    void addPlaylistAttachment(const juce::var& payload);
    void startCamera();
    void stopCamera();
    void attachCallControlsTo(juce::Component&);
    void layoutCallControls(juce::Rectangle<int>);
    juce::Rectangle<int> localParticipantTileBounds() const;
    juce::Rectangle<int> embeddedChatBounds() const;
    juce::Rectangle<int> embeddedVideoBounds() const;
    juce::Rectangle<int> localCameraPreviewBounds() const;
    void drawParticipantTile(juce::Graphics&, juce::Rectangle<float>, const Participant&, bool active) const;
    void drawAvatar(juce::Graphics&, juce::Rectangle<float>, const Participant&) const;
    void drawChatPanel(juce::Graphics&, juce::Rectangle<float>, bool compact);
    void drawParticipantsPanel(juce::Graphics&, juce::Rectangle<float>, bool compact);
    void drawSetupPanel(juce::Graphics&, juce::Rectangle<float>, bool compact);

    std::array<Participant, 4> participants {
        Participant { "Alex R.", juce::Colour(0xffd9785f), true, true, false },
        Participant { "Maya L.", juce::Colour(0xff6957a8), true, true, false },
        Participant { "Jordan T.", juce::Colour(0xffa8774f), true, true, false },
        Participant { "Sophie K.", juce::Colour(0xffd2a98e), true, true, false }
    };
    std::vector<std::pair<juce::String, juce::String>> chatMessages;
    bool playlistDragOver { false };

    juce::TextButton createButton { "Create session" };
    juce::TextButton joinButton { "Join" };
    juce::TextButton exitButton { "Exit Jam" };
    juce::TextButton chatTab { "Chat" };
    juce::TextButton participantsTab { "Participants" };
    juce::TextButton setupTab { "Setup" };
    juce::TextButton micButton { "Mic" };
    juce::TextButton cameraButton { "Camera" };
    juce::TextButton shareButton { "Share" };
    juce::TextButton tipButton { "Tip" };
    juce::TextButton emoteButton { "Emote" };
    juce::TextButton settingsButton { "Settings" };
    juce::TextButton sendButton { "Chat" };
    juce::ComboBox audioInputBox;
    juce::ComboBox audioOutputBox;
    juce::ComboBox cameraDeviceBox;
    juce::ComboBox latencyBox;
    juce::TextEditor chatEditor;
    std::unique_ptr<juce::FileChooser> attachmentChooser;
    std::unique_ptr<juce::CameraDevice> cameraDevice;
    std::unique_ptr<juce::Component> cameraPreview;
    PanelMode panelMode { PanelMode::chat };
    ConnectionState connectionState { ConnectionState::ready };
    juce::String inviteCode { "7K4M" };
    bool micEnabled { false };
    bool cameraEnabled { false };
    bool cameraOpening { false };
    bool sharingEnabled { false };
    bool embeddedArrangementMode { false };
    bool callControlsVisible { false };
};
} // namespace orion
