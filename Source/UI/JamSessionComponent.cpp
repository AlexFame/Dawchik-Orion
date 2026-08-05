#include "JamSessionComponent.h"

#include "MacCharacterPalette.h"
#include "OrionTheme.h"
#include "OrionPopupMenu.h"

namespace orion
{
namespace
{
const auto bg = juce::Colour(0xff0b1118);
const auto panel = juce::Colour(0xff121b26);
const auto elevated = juce::Colour(0xff1a2533);
const auto control = juce::Colour(0xff172231);
const auto coral = theme::accent::activeCoral;

void drawSlash(juce::Graphics& g, juce::Rectangle<float> icon, juce::Colour colour)
{
    juce::Path slash;
    slash.startNewSubPath(icon.getX() + 1.0f, icon.getBottom() - 1.5f);
    slash.lineTo(icon.getRight() - 1.0f, icon.getY() + 1.5f);
    g.setColour(colour);
    g.strokePath(slash, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawCallIcon(juce::Graphics& g, juce::Rectangle<float> area,
                  const juce::String& label, bool, juce::Colour colour)
{
    auto icon = area.withSizeKeepingCentre(22.0f, 22.0f);
    g.setColour(colour);

    if (label.containsIgnoreCase("mic") || label.containsIgnoreCase("muted"))
    {
        auto capsule = icon.withSizeKeepingCentre(8.0f, 15.0f).translated(0.0f, -3.0f);
        g.drawRoundedRectangle(capsule, 4.0f, 2.0f);
        g.drawLine(icon.getCentreX(), capsule.getBottom(), icon.getCentreX(), icon.getBottom() - 4.0f, 2.0f);
        g.drawLine(icon.getCentreX() - 5.0f, icon.getBottom() - 4.0f,
                   icon.getCentreX() + 5.0f, icon.getBottom() - 4.0f, 2.0f);
        juce::Path arc;
        arc.addCentredArc(icon.getCentreX(), icon.getCentreY() - 2.0f, 10.0f, 11.0f,
                          0.0f, juce::MathConstants<float>::pi * 0.18f,
                          juce::MathConstants<float>::pi * 0.82f, true);
        g.strokePath(arc, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (label.containsIgnoreCase("muted"))
            drawSlash(g, icon.expanded(2.0f), coral.brighter(0.18f));
        return;
    }

    if (label.containsIgnoreCase("cam"))
    {
        const auto cameraOn = label.containsIgnoreCase("on") || label.containsIgnoreCase("opening");
        auto body = icon.withSizeKeepingCentre(16.0f, 11.0f);
        body.setX(icon.getX() + 1.0f);
        g.drawRoundedRectangle(body, 2.8f, 2.0f);
        juce::Path lens;
        lens.startNewSubPath(body.getRight(), body.getY() + 3.0f);
        lens.lineTo(icon.getRight() - 1.0f, icon.getY() + 5.5f);
        lens.lineTo(icon.getRight() - 1.0f, icon.getBottom() - 5.5f);
        lens.lineTo(body.getRight(), body.getBottom() - 3.0f);
        g.strokePath(lens, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (! cameraOn)
            drawSlash(g, icon.expanded(2.0f), coral.brighter(0.18f));
        return;
    }

    if (label.containsIgnoreCase("share") || label.containsIgnoreCase("live"))
    {
        auto screen = icon.withSizeKeepingCentre(18.0f, 13.0f).translated(0.0f, 1.0f);
        g.drawRoundedRectangle(screen, 2.5f, 2.0f);
        g.drawLine(icon.getCentreX(), screen.getBottom(), icon.getCentreX(), icon.getBottom() - 2.0f, 2.0f);
        g.drawLine(icon.getCentreX() - 5.0f, icon.getBottom() - 2.0f,
                   icon.getCentreX() + 5.0f, icon.getBottom() - 2.0f, 2.0f);
        juce::Path arrow;
        arrow.startNewSubPath(icon.getCentreX(), icon.getY() + 2.0f);
        arrow.lineTo(icon.getCentreX(), icon.getY() + 10.0f);
        arrow.startNewSubPath(icon.getCentreX() - 4.0f, icon.getY() + 6.0f);
        arrow.lineTo(icon.getCentreX(), icon.getY() + 2.0f);
        arrow.lineTo(icon.getCentreX() + 4.0f, icon.getY() + 6.0f);
        g.strokePath(arrow, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }

    auto phone = icon.reduced(1.0f);
    juce::Path handset;
    handset.startNewSubPath(phone.getX() + 2.0f, phone.getCentreY() + 3.0f);
    handset.cubicTo(phone.getCentreX() - 3.0f, phone.getY() + 1.0f,
                    phone.getCentreX() + 3.0f, phone.getY() + 1.0f,
                    phone.getRight() - 2.0f, phone.getCentreY() + 3.0f);
    g.strokePath(handset, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

class JamButtonLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& background,
                              bool highlighted, bool down) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto role = button.getComponentID();
        const auto primary = role == "primary";
        const auto segment = role == "segment";
        const auto call = role == "call";
        const auto danger = role == "danger";
        const auto icon = role == "icon";
        const auto active = button.getToggleState();
        const auto enabled = button.isEnabled();

        auto fill = primary ? coral : background;
        auto stroke = primary ? coral.brighter(0.15f).withAlpha(0.72f) : theme::line::subtle.withAlpha(0.95f);

        if (icon)
        {
            fill = juce::Colour(0xff202a37);
            stroke = theme::line::strong.withAlpha(0.70f);
        }
        else if (call)
        {
            fill = active ? juce::Colour(0xff343b47) : juce::Colour(0xff242b35);
            stroke = active ? theme::cool::cyan.withAlpha(0.46f) : juce::Colour(0xff3a4351).withAlpha(0.72f);
        }
        else if (danger)
        {
            fill = active ? juce::Colour(0xffd93f4c) : juce::Colour(0xffe24956);
            stroke = juce::Colour(0xffff6b75).withAlpha(0.62f);
        }
        else if (segment)
        {
            fill = active ? juce::Colour(0xff2b3340) : control.withAlpha(0.42f);
            stroke = active ? theme::line::strong.withAlpha(0.72f) : theme::line::subtle.withAlpha(0.58f);
        }
        else if (active)
        {
            fill = coral.withAlpha(0.92f);
            stroke = coral.brighter(0.10f).withAlpha(0.78f);
        }

        if (! enabled)
        {
            fill = control.withAlpha(0.45f);
            stroke = theme::line::subtle.withAlpha(0.45f);
        }
        else if (down)
        {
            fill = fill.darker(0.14f);
        }
        else if (highlighted)
        {
            fill = fill.brighter(primary || active || danger ? 0.04f : 0.10f);
            stroke = stroke.brighter(0.10f);
        }

        const auto radius = (call || icon) ? 8.0f : 7.0f;
        g.setColour(juce::Colours::black.withAlpha(call ? 0.18f : 0.22f));
        g.fillRoundedRectangle(bounds.translated(0.0f, call ? 1.0f : 1.5f), radius);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, radius);
        g.setColour(stroke);
        g.drawRoundedRectangle(bounds, radius, call ? 1.15f : 1.0f);
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        const auto role = button.getComponentID();
        const auto active = button.getToggleState() || role == "primary" || role == "danger";
        if (role == "call" || role == "danger")
        {
            const auto colour = button.isEnabled()
                                    ? (role == "danger" ? juce::Colours::white : theme::text::primary.withAlpha(0.94f))
                                    : theme::text::muted.withAlpha(0.54f);
            drawCallIcon(g, button.getLocalBounds().toFloat(), button.getButtonText(), button.getToggleState(), colour);
            return;
        }

        g.setColour(button.isEnabled()
                        ? (active ? juce::Colours::white : theme::text::primary.withAlpha(0.92f))
                        : theme::text::muted.withAlpha(0.56f));
        const auto fontSize = role == "icon" ? 20.0f
                            : role == "segment" ? 12.0f
                            : role == "utility" ? 12.0f
                            : role == "call" || role == "danger" ? 12.0f
                            : 12.5f;
        g.setFont(juce::FontOptions(fontSize, juce::Font::bold));
        g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(8, 2),
                         juce::Justification::centred, 1);
    }
};

JamButtonLookAndFeel& jamButtonLookAndFeel()
{
    static JamButtonLookAndFeel lnf;
    return lnf;
}

void styleButton(juce::TextButton& button, const juce::String& role = "secondary")
{
    button.setLookAndFeel(&jamButtonLookAndFeel());
    button.setComponentID(role);
    button.setColour(juce::TextButton::buttonColourId, control);
    button.setColour(juce::TextButton::buttonOnColourId, coral);
    button.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    button.setClickingTogglesState(false);
}

juce::Colour colourForName(const juce::String& name)
{
    if (name == "Maya L.") return juce::Colour(0xffb59cff);
    if (name == "Jordan T.") return juce::Colour(0xffffb36c);
    if (name == "Sophie K.") return juce::Colour(0xffffc9b2);
    if (name == "You") return theme::cool::cyan;
    if (name == "Orion") return theme::text::muted;
    return juce::Colour(0xffff8b96);
}

void drawPill(juce::Graphics& g, juce::Rectangle<float> bounds, const juce::String& text,
              juce::Colour fill, juce::Colour stroke, juce::Colour textColour)
{
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, 5.0f);
    g.setColour(stroke);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.0f);
    g.setColour(textColour);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(text, bounds, juce::Justification::centred);
}
}

JamSessionComponent::JamSessionComponent()
{
    setOpaque(true);
    addMouseListener(this, true);
    chatMessages.clear();

    for (auto* button : { &createButton, &joinButton, &exitButton, &chatTab, &participantsTab, &setupTab,
                          &micButton, &cameraButton, &shareButton, &tipButton, &emoteButton, &settingsButton,
                          &sendButton })
    {
        button->addListener(this);
        button->setWantsKeyboardFocus(false);
        addAndMakeVisible(*button);
    }
    styleButton(createButton, "primary");
    styleButton(joinButton);
    styleButton(exitButton, "danger");
    styleButton(chatTab, "segment");
    styleButton(participantsTab, "segment");
    styleButton(setupTab, "segment");
    styleButton(micButton, "call");
    styleButton(cameraButton, "call");
    styleButton(shareButton, "call");
    styleButton(tipButton, "icon");
    styleButton(emoteButton, "utility");
    styleButton(settingsButton, "utility");
    styleButton(sendButton, "primary");

    // Shown centred in the video strip once you've hung up, so there's an obvious way back into the call.
    rejoinButton.addListener(this);
    rejoinButton.setWantsKeyboardFocus(false);
    styleButton(rejoinButton, "primary");
    rejoinButton.setTooltip("Rejoin the video call");
    addChildComponent(rejoinButton);   // visibility toggled in refreshControls

    // Tooltips — the call-control icons aren't self-explanatory (esp. the red "leave" one).
    micButton.setTooltip("Microphone - talk to everyone in the jam");
    cameraButton.setTooltip("Camera - share your video");
    shareButton.setTooltip("Share your screen");
    exitButton.setTooltip("Leave the call - your video tile disappears for everyone (you stay in the jam)");
    createButton.setTooltip("Host a session others can join");
    joinButton.setTooltip("Join / leave a session");
    tipButton.setTooltip("Attach a file or sample to the chat");
    emoteButton.setTooltip("Send a reaction");

    tipButton.setButtonText("+");
    emoteButton.setButtonText("Emoji");
    settingsButton.setButtonText("");
    sendButton.setButtonText("Send");

    for (auto* button : { &micButton, &cameraButton, &shareButton, &exitButton })
        button->setAlwaysOnTop(true);

    for (auto* button : { &chatTab, &participantsTab, &setupTab, &micButton, &cameraButton, &shareButton })
        button->setClickingTogglesState(true);

    auto configureCombo = [this](juce::ComboBox& box, const juce::String& name)
    {
        box.setName(name);
        box.setColour(juce::ComboBox::backgroundColourId, elevated);
        box.setColour(juce::ComboBox::outlineColourId, theme::line::subtle);
        box.setColour(juce::ComboBox::textColourId, theme::text::primary);
        box.setColour(juce::ComboBox::arrowColourId, theme::text::secondary);
        addAndMakeVisible(box);
    };
    configureCombo(audioInputBox, "Audio input");
    configureCombo(audioOutputBox, "Audio output");
    configureCombo(cameraDeviceBox, "Camera");
    configureCombo(latencyBox, "Latency");
    audioInputBox.addItem("System microphone", 1);
    audioInputBox.addItem("External input", 2);
    audioOutputBox.addItem("Master out", 1);
    audioOutputBox.addItem("Jam monitor bus", 2);
    cameraDeviceBox.addItem("FaceTime camera", 1);
    cameraDeviceBox.addItem("Camera off", 2);
    latencyBox.addItem("Low latency", 1);
    latencyBox.addItem("Balanced", 2);
    latencyBox.addItem("High stability", 3);
    audioInputBox.setSelectedId(1, juce::dontSendNotification);
    audioOutputBox.setSelectedId(1, juce::dontSendNotification);
    cameraDeviceBox.setSelectedId(2, juce::dontSendNotification);
    latencyBox.setSelectedId(2, juce::dontSendNotification);
    cameraDeviceBox.onChange = [this]
    {
        const auto requestedState = cameraDeviceBox.getSelectedId() != 2;
        if (requestedState)
            startCamera();
        else if (cameraEnabled || cameraOpening)
            stopCamera();
    };
    audioInputBox.onChange = [this] { repaint(); };
    audioOutputBox.onChange = [this] { repaint(); };
    latencyBox.onChange = [this] { repaint(); };

    chatEditor.setMultiLine(false);
    chatEditor.setReturnKeyStartsNewLine(false);
    chatEditor.setTextToShowWhenEmpty("Type a message...", theme::text::muted);
    chatEditor.setColour(juce::TextEditor::backgroundColourId, elevated);
    chatEditor.setColour(juce::TextEditor::outlineColourId, theme::line::subtle);
    chatEditor.setColour(juce::TextEditor::textColourId, theme::text::primary);
    chatEditor.setColour(juce::TextEditor::focusedOutlineColourId, theme::line::subtle);
    chatEditor.setFont(juce::FontOptions(16.0f));
    chatEditor.setIndents(12, 6);
    chatEditor.addListener(this);
    addAndMakeVisible(chatEditor);
    refreshControls();
    startTimerHz(24);
}

void JamSessionComponent::setEmbeddedArrangementMode(bool shouldUseEmbeddedMode)
{
    if (embeddedArrangementMode == shouldUseEmbeddedMode)
        return;

    embeddedArrangementMode = shouldUseEmbeddedMode;
    setOpaque(! embeddedArrangementMode);
    setInterceptsMouseClicks(true, true);
    resized();
    repaint();
}

juce::Rectangle<int> JamSessionComponent::embeddedChatBounds() const
{
    auto area = getLocalBounds().reduced(14);
    return area.removeFromRight(326);
}

juce::Rectangle<int> JamSessionComponent::embeddedVideoBounds() const
{
    auto area = getLocalBounds().reduced(14);
    area.removeFromRight(326);
    area.removeFromRight(14);
    return area.removeFromTop(156);
}

juce::Rectangle<int> JamSessionComponent::localCameraPreviewBounds() const
{
    // The camera preview is a NATIVE video layer that draws on top of any JUCE component, so the
    // mic/camera controls and name can't overlay it — they'd be hidden. Reserve a strip at the
    // bottom for them and let the video fill everything above it, edge to edge.
    return localParticipantTileBounds().reduced(1).withTrimmedBottom(40);
}

juce::Rectangle<int> JamSessionComponent::localParticipantTileBounds() const
{
    auto tiles = embeddedVideoBounds().reduced(12);
    if (! embeddedArrangementMode)
    {
        auto area = getLocalBounds().reduced(18);
        area.removeFromTop(64);
        auto main = area.reduced(0, 10);
        main.removeFromRight(318);
        main.removeFromRight(18);
        tiles = main.reduced(16).removeFromTop(188);
    }

    const int tileGap = 10;
    const int tileW = (tiles.getWidth() - tileGap * 3) / 4;
    return tiles.removeFromLeft(tileW);
}

void JamSessionComponent::attachCallControlsTo(juce::Component& parent)
{
    for (auto* button : { &micButton, &cameraButton, &shareButton, &exitButton })
    {
        if (button->getParentComponent() != &parent)
            parent.addAndMakeVisible(*button);
        button->setVisible(callControlsVisible);
        button->toFront(false);
    }
}

void JamSessionComponent::layoutCallControls(juce::Rectangle<int> bounds)
{
    auto callControls = bounds.withSizeKeepingCentre(juce::jmin(bounds.getWidth() - 12, 244), 30);
    callControls.setY(bounds.getBottom() - 36);
    micButton.setBounds(callControls.removeFromLeft(50));
    callControls.removeFromLeft(8);
    cameraButton.setBounds(callControls.removeFromLeft(50));
    callControls.removeFromLeft(8);
    shareButton.setBounds(callControls.removeFromLeft(60));
    callControls.removeFromLeft(8);
    exitButton.setBounds(callControls);
}

void JamSessionComponent::timerCallback()
{
    updateCallControlsVisibility();
}

void JamSessionComponent::updateCallControlsVisibility()
{
    auto mouse = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
    const auto hoverArea = localAreaToGlobal(localParticipantTileBounds());
    // No local tile when we've left the call, so there's nothing to hover controls over.
    const auto shouldShow = callActive && hoverArea.contains(mouse);

    if (callControlsVisible == shouldShow)
        return;

    callControlsVisible = shouldShow;
    for (auto* button : { &micButton, &cameraButton, &shareButton, &exitButton })
        button->setVisible(callControlsVisible);
}

void JamSessionComponent::positionRejoinButton(juce::Rectangle<int> stripInner, int tileW, int tileGap)
{
    if (callActive)
        return;   // in the call — button hidden (refreshControls also toggles visibility)

    const int joinSlot = static_cast<int>(visibleTileIndices().size());
    if (joinSlot >= 4)
        return;   // strip full of peer tiles, no room — the caption overlay isn't shown either

    auto slot = stripInner;
    for (int s = 0; s < joinSlot; ++s)
    {
        slot.removeFromLeft(tileW);
        slot.removeFromLeft(tileGap);
    }
    auto joinTile = slot.removeFromLeft(tileW);

    // Lower half of the join tile, matching the placeholder caption drawn on top.
    auto lower = joinTile.removeFromBottom(joinTile.getHeight() / 2).reduced(10, 0);
    rejoinButton.setBounds(juce::Rectangle<int>(0, 0, juce::jmin(120, lower.getWidth()), 30)
                               .withCentre({ lower.getCentreX(), lower.getCentreY() }));
    rejoinButton.toFront(false);
}

std::vector<int> JamSessionComponent::visibleTileIndices() const
{
    // Peers who are in the call always get a tile — even if WE'VE left — so a rejoin is visible to
    // everyone (otherwise a host who ended the call would never see a guest come back). Our own tile
    // (index 0) shows only while we're in the call.
    std::vector<int> idx;
    for (int i = 0; i < static_cast<int>(participants.size()); ++i)
    {
        const bool isLocal = (i == 0);
        if (isLocal ? callActive : participants[static_cast<std::size_t>(i)].inCall)
            idx.push_back(i);
    }
    return idx;
}

void JamSessionComponent::mouseDown(const juce::MouseEvent& event)
{
    auto* clickedComponent = event.eventComponent;
    if (clickedComponent != &chatEditor && ! chatEditor.isParentOf(clickedComponent))
        chatEditor.giveAwayKeyboardFocus();
}

bool JamSessionComponent::hitTest(int x, int y)
{
    if (! embeddedArrangementMode)
        return true;

    const juce::Point<int> point { x, y };
    if (embeddedChatBounds().contains(point) || embeddedVideoBounds().contains(point))
        return true;

    for (auto* child : getChildren())
        if (child->isVisible() && child->getBounds().contains(point))
            return true;

    return false;
}

bool JamSessionComponent::isInterestedInDragSource(const SourceDetails& dragSourceDetails)
{
    const auto* payload = dragSourceDetails.description.getDynamicObject();
    return payload != nullptr && payload->getProperty("type").toString() == "playlist-blocks";
}

void JamSessionComponent::itemDragEnter(const SourceDetails&)
{
    playlistDragOver = true;
    repaint();
}

void JamSessionComponent::itemDragExit(const SourceDetails&)
{
    playlistDragOver = false;
    repaint();
}

void JamSessionComponent::itemDropped(const SourceDetails& dragSourceDetails)
{
    playlistDragOver = false;
    addPlaylistAttachment(dragSourceDetails.description);
    repaint();
}

void JamSessionComponent::addPlaylistAttachment(const juce::var& payload)
{
    const auto* object = payload.getDynamicObject();
    if (object == nullptr)
        return;

    const auto clipCount = static_cast<int>(object->getProperty("clipCount"));
    const auto trackCount = static_cast<int>(object->getProperty("trackCount"));
    const auto startBeat = static_cast<double>(object->getProperty("startBeat"));
    const auto endBeat = static_cast<double>(object->getProperty("endBeat"));
    const auto title = object->getProperty("title").toString();

    juce::String message;
    message << "Attached " << (clipCount == 1 ? "clip" : "playlist block")
            << " \"" << title << "\"";
    if (clipCount > 1)
        message << " (" << clipCount << " clips";
    if (trackCount > 1)
        message << ", " << trackCount << " tracks";
    if (clipCount > 1)
        message << ")";
    message << " · bars " << juce::String(startBeat / 4.0 + 1.0, 1)
            << "-" << juce::String(endBeat / 4.0 + 1.0, 1);

    addChatMessage("Orion", message);
}

void JamSessionComponent::openFileAttachmentChooser(bool foldersOnly)
{
    attachmentChooser = std::make_unique<juce::FileChooser>(foldersOnly ? "Add folders to Jam chat"
                                                                         : "Add files or photos to Jam chat",
                                                            juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                                                            juce::String());

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | (foldersOnly ? juce::FileBrowserComponent::canSelectDirectories
                                           : juce::FileBrowserComponent::canSelectFiles)
                            | juce::FileBrowserComponent::canSelectMultipleItems;

    juce::Component::SafePointer<JamSessionComponent> safeThis(this);
    attachmentChooser->launchAsync(chooserFlags,
        [safeThis](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;

            const auto results = chooser.getResults();
            safeThis->attachmentChooser.reset();
            safeThis->addFileAttachments(results);
        });
}

void JamSessionComponent::addFileAttachments(const juce::Array<juce::File>& files)
{
    if (files.isEmpty())
        return;

    if (files.size() == 1)
    {
        const auto file = files.getFirst();
        addChatMessage("Orion",
                       juce::String(file.isDirectory() ? "Attached folder \"" : "Attached file \"")
                           + file.getFileName() + "\".");
        return;
    }

    bool foldersOnly = true;
    for (const auto& file : files)
    {
        if (! file.isDirectory())
        {
            foldersOnly = false;
            break;
        }
    }
    addChatMessage("Orion", "Attached " + juce::String(files.size()) + (foldersOnly ? " folders." : " files."));
}

void JamSessionComponent::paint(juce::Graphics& g)
{
    if (embeddedArrangementMode)
    {
        auto chat = embeddedChatBounds().toFloat();
        auto videoStrip = embeddedVideoBounds().toFloat();

        g.setColour(panel.withAlpha(0.94f));
        g.fillRoundedRectangle(chat, 8.0f);
        g.setColour(playlistDragOver ? theme::cool::cyan.withAlpha(0.95f) : theme::line::subtle);
        g.drawRoundedRectangle(chat.reduced(0.5f), 8.0f, playlistDragOver ? 2.0f : 1.0f);

        g.setColour(bg.withAlpha(0.92f));
        g.fillRoundedRectangle(videoStrip, 8.0f);
        g.setColour(theme::line::subtle.withAlpha(0.82f));
        g.drawRoundedRectangle(videoStrip.reduced(0.5f), 8.0f, 1.0f);

        auto tiles = videoStrip.reduced(12.0f);
        const auto tileGap = 10.0f;
        const auto tileW = (tiles.getWidth() - tileGap * 3.0f) / 4.0f;
        const auto vis = visibleTileIndices();
        const int joinSlot = callActive ? -1 : static_cast<int>(vis.size());
        for (int slot = 0; slot < 4; ++slot)
        {
            auto tile = tiles.removeFromLeft(tileW);
            if (slot != 3) tiles.removeFromLeft(tileGap);
            if (slot < static_cast<int>(vis.size()))
            {
                const int pi = vis[static_cast<std::size_t>(slot)];
                drawParticipantTile(g, tile, participants[static_cast<std::size_t>(pi)], false);
                if (pi == 0 && cameraEnabled)
                    drawLocalCameraFrame(g, tile);
            }
            else if (slot == joinSlot)
            {
                drawCallEndedPlaceholder(g, tile);   // "You left" + the Join-call button lives here
            }
            else
            {
                g.setColour(theme::surface::primary.withAlpha(0.35f));
                g.fillRoundedRectangle(tile, 8.0f);
            }
        }

        auto chatArea = chat.reduced(16.0f);
        auto chatHeader = chatArea.removeFromTop(30.0f);
        g.setColour(theme::text::primary);
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawText("Jam", chatHeader.removeFromLeft(90.0f), juce::Justification::centredLeft);
        g.setColour(connectionState == ConnectionState::live ? theme::status::success : theme::text::muted);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(connectionState == ConnectionState::live ? "LIVE" : "READY", chatHeader, juce::Justification::centredRight);

        if (diagnosticsText.isNotEmpty())
        {
            g.setColour(theme::text::muted);
            g.setFont(juce::FontOptions(10.0f, juce::Font::plain));
            g.drawText(diagnosticsText, chatArea.removeFromTop(13.0f), juce::Justification::centredLeft);
        }

        auto content = chatArea.reduced(0.0f, 12.0f);
        content.removeFromTop(87.0f);
        if (panelMode == PanelMode::chat)
            content.removeFromBottom(98.0f);
        if (panelMode == PanelMode::chat)
            drawChatPanel(g, content, true);
        else if (panelMode == PanelMode::participants)
            drawParticipantsPanel(g, content, true);
        else
            drawSetupPanel(g, content, true);
        return;
    }

    g.fillAll(bg);
    auto area = getLocalBounds().toFloat().reduced(18.0f);
    auto header = area.removeFromTop(64.0f);
    g.setColour(theme::text::primary);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText("ORION JAM", header.removeFromLeft(190.0f), juce::Justification::centredLeft);
    g.setColour(theme::status::success);
    g.fillEllipse(header.getX(), header.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setColour(theme::text::secondary);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    const auto headerStatus = networkStatus.isNotEmpty()
                                  ? networkStatus
                                  : (connectionState == ConnectionState::live
                                         ? "LIVE  ·  SESSION " + inviteCode
                                         : juce::String("READY TO JAM"));
    g.drawText(headerStatus, header.withTrimmedLeft(16.0f), juce::Justification::centredLeft);

    if (diagnosticsText.isNotEmpty())
    {
        g.setColour(theme::text::muted);
        g.setFont(juce::FontOptions(11.0f, juce::Font::plain));
        g.drawText(diagnosticsText, header.withTrimmedLeft(16.0f), juce::Justification::centredRight);
    }

    auto main = area.reduced(0.0f, 10.0f);
    auto chat = main.removeFromRight(318.0f);
    main.removeFromRight(18.0f);
    auto stage = main;

    g.setColour(panel);
    g.fillRoundedRectangle(stage, 10.0f);
    g.setColour(theme::line::subtle);
    g.drawRoundedRectangle(stage.reduced(0.5f), 10.0f, 1.0f);
    g.setColour(panel);
    g.fillRoundedRectangle(chat, 10.0f);
    g.setColour(playlistDragOver ? theme::cool::cyan.withAlpha(0.95f) : theme::line::subtle);
    g.drawRoundedRectangle(chat.reduced(0.5f), 10.0f, playlistDragOver ? 2.0f : 1.0f);

    auto videoStrip = stage.reduced(16.0f).removeFromTop(188.0f);
    const auto tileGap = 10.0f;
    const auto tileW = (videoStrip.getWidth() - tileGap * 3.0f) / 4.0f;
    const auto vis = visibleTileIndices();
    const int joinSlot = callActive ? -1 : static_cast<int>(vis.size());
    for (int slot = 0; slot < 4; ++slot)
    {
        auto tile = videoStrip.removeFromLeft(tileW);
        if (slot != 3) videoStrip.removeFromLeft(tileGap);
        if (slot < static_cast<int>(vis.size()))
        {
            const int pi = vis[static_cast<std::size_t>(slot)];
            drawParticipantTile(g, tile, participants[static_cast<std::size_t>(pi)], false);
            if (pi == 0 && cameraEnabled)
                drawLocalCameraFrame(g, tile);
        }
        else if (slot == joinSlot)
        {
            drawCallEndedPlaceholder(g, tile);
        }
        else
        {
            g.setColour(theme::surface::primary.withAlpha(0.35f));
            g.fillRoundedRectangle(tile, 8.0f);
        }
    }

    auto workspace = stage.reduced(16.0f);
    workspace.removeFromTop(206.0f);
    g.setColour(juce::Colour(0xff101923));
    g.fillRoundedRectangle(workspace, 7.0f);
    g.setColour(theme::line::subtle.withAlpha(0.8f));
    g.drawRoundedRectangle(workspace.reduced(0.5f), 7.0f, 1.0f);
    g.setColour(theme::text::muted);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("Shared Orion workspace", workspace.reduced(16.0f).removeFromTop(22.0f), juce::Justification::centredLeft);
    for (int i = 1; i < 12; ++i)
    {
        const auto x = workspace.getX() + workspace.getWidth() * static_cast<float>(i) / 12.0f;
        g.setColour(theme::line::subtle.withAlpha(0.32f));
        g.drawVerticalLine(static_cast<int>(x), workspace.getY() + 38.0f, workspace.getBottom());
    }
    g.setColour(coral.withAlpha(0.85f));
    g.drawLine(workspace.getX() + workspace.getWidth() * 0.22f, workspace.getY() + 34.0f,
               workspace.getX() + workspace.getWidth() * 0.22f, workspace.getBottom(), 1.5f);

    auto chatArea = chat.reduced(16.0f);
    auto chatHeader = chatArea.removeFromTop(42.0f);
    g.setColour(theme::text::primary);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Jam", chatHeader.removeFromLeft(90.0f), juce::Justification::centredLeft);
    g.setColour(connectionState == ConnectionState::live ? theme::status::success : theme::text::muted);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(connectionState == ConnectionState::live ? "LIVE" : "READY", chatHeader, juce::Justification::centredRight);

    if (diagnosticsText.isNotEmpty())
    {
        g.setColour(theme::text::muted);
        g.setFont(juce::FontOptions(10.0f, juce::Font::plain));
        g.drawText(diagnosticsText, chatArea.removeFromTop(13.0f), juce::Justification::centredLeft);
    }

    auto content = chatArea.reduced(0.0f, 12.0f);
    content.removeFromTop(58.0f);
    if (panelMode == PanelMode::chat)
        content.removeFromBottom(98.0f);
    if (panelMode == PanelMode::chat)
        drawChatPanel(g, content, false);
    else if (panelMode == PanelMode::participants)
        drawParticipantsPanel(g, content, false);
    else
        drawSetupPanel(g, content, false);
}

void JamSessionComponent::resized()
{
    refreshControls();

    if (embeddedArrangementMode)
    {
        auto area = getLocalBounds().reduced(14);
        auto chat = area.removeFromRight(326).reduced(16);

        chat.removeFromTop(34);
        auto actions = chat.removeFromTop(34);
        const auto actionW = (actions.getWidth() - 8) / 2;
        createButton.setBounds(actions.removeFromLeft(actionW));
        actions.removeFromLeft(8);
        joinButton.setBounds(actions);

        chat.removeFromTop(7);
        auto tabs = chat.removeFromTop(34);
        const auto tabW = tabs.getWidth() / 3;
        chatTab.setBounds(tabs.removeFromLeft(tabW));
        participantsTab.setBounds(tabs.removeFromLeft(tabW));
        setupTab.setBounds(tabs);

        chat.removeFromTop(12);

        auto tiles = embeddedVideoBounds().reduced(12);
        const auto tileGap = 10;
        const auto tileW = (tiles.getWidth() - tileGap * 3) / 4;
        auto localTile = tiles.removeFromLeft(tileW);
        attachCallControlsTo(*this);
        layoutCallControls(localTile);

        // "Join call" button, in the lower half of the join-slot tile (after any peer tiles).
        positionRejoinButton(embeddedVideoBounds().reduced(12), tileW, tileGap);

        auto panelBody = chat;
        auto compose = panelBody.removeFromBottom(92);
        auto input = compose.removeFromTop(42);
        chatEditor.setBounds(input);
        compose.removeFromTop(10);
        auto tools = compose.removeFromTop(40);
        tipButton.setBounds(tools.removeFromLeft(40));
        tools.removeFromLeft(8);
        emoteButton.setBounds(tools.removeFromLeft(82));
        sendButton.setBounds(tools.removeFromRight(76));
        settingsButton.setBounds({});

        auto setup = panelBody.reduced(0, 8);
        const auto comboH = 32;
        audioInputBox.setBounds(setup.removeFromTop(comboH).toNearestInt());
        setup.removeFromTop(8);
        audioOutputBox.setBounds(setup.removeFromTop(comboH).toNearestInt());
        setup.removeFromTop(8);
        cameraDeviceBox.setBounds(setup.removeFromTop(comboH).toNearestInt());
        setup.removeFromTop(8);
        latencyBox.setBounds(setup.removeFromTop(comboH).toNearestInt());
        refreshControls();
        if (cameraEnabled)
        {
            attachCallControlsTo(*this);
            layoutCallControls(localParticipantTileBounds());
        }
        return;
    }

    auto area = getLocalBounds().reduced(18).removeFromBottom(82);
    auto chat = area.removeFromRight(318);
    auto chatInner = chat.reduced(16);
    chatInner.removeFromTop(76);
    auto tabs = chatInner.removeFromTop(36);
    const auto tabW = tabs.getWidth() / 3;
    chatTab.setBounds(tabs.removeFromLeft(tabW));
    participantsTab.setBounds(tabs.removeFromLeft(tabW));
    setupTab.setBounds(tabs);

    chatInner.removeFromTop(12);

    auto panelBody = chatInner;
    auto compose = chat.reduced(16).removeFromBottom(92);
    auto input = compose.removeFromTop(42);
    chatEditor.setBounds(input);
    compose.removeFromTop(10);
    auto tools = compose.removeFromTop(40);
    tipButton.setBounds(tools.removeFromLeft(40));
    tools.removeFromLeft(8);
    emoteButton.setBounds(tools.removeFromLeft(82));
    sendButton.setBounds(tools.removeFromRight(76));
    settingsButton.setBounds({});

    auto actions = juce::Rectangle<int>(getWidth() - 268, 28, 232, 34);
    const auto actionW = (actions.getWidth() - 8) / 2;
    createButton.setBounds(actions.removeFromLeft(actionW));
    actions.removeFromLeft(8);
    joinButton.setBounds(actions);

    auto layout = getLocalBounds().reduced(18);
    layout.removeFromTop(64);
    auto main = layout.reduced(0, 10);
    main.removeFromRight(318);
    main.removeFromRight(18);
    auto stage = main;
    auto videoStrip = stage.reduced(16).removeFromTop(188);
    auto tiles = videoStrip;
    const auto tileGap = 10;
    const auto tileW = (tiles.getWidth() - tileGap * 3) / 4;
    auto localTile = tiles.removeFromLeft(tileW);
    attachCallControlsTo(*this);
    layoutCallControls(localTile.withTrimmedBottom(28));

    positionRejoinButton(videoStrip, tileW, tileGap);

    auto setup = panelBody.reduced(0, 0);
    const auto comboH = 32;
    audioInputBox.setBounds(setup.removeFromTop(comboH).toNearestInt());
    setup.removeFromTop(10);
    audioOutputBox.setBounds(setup.removeFromTop(comboH).toNearestInt());
    setup.removeFromTop(10);
    cameraDeviceBox.setBounds(setup.removeFromTop(comboH).toNearestInt());
    setup.removeFromTop(10);
    latencyBox.setBounds(setup.removeFromTop(comboH).toNearestInt());
    refreshControls();
}

void JamSessionComponent::endCall()
{
    // "Hang up" (Zoom Leave-call): drop out of the video call entirely — your tile disappears for you
    // AND for peers (via presence) — but STAY in the jam. A misclick here never drops the collab session.
    const bool wasInCall = callActive;
    if (cameraEnabled)
        stopCamera();
    if (micEnabled)
    {
        micEnabled = false;
        if (! participants.empty())
            participants[0].muted = true;
        if (onMicEnabledChanged)
            onMicEnabledChanged(false);
    }
    sharingEnabled = false;
    callActive = false;
    if (! participants.empty())
        participants[0].inCall = false;
    // Always give visible feedback so the press never feels dead, even if you'd already left the call.
    addChatMessage("SYS", wasInCall ? "You left the call - your video is off for everyone (still in the jam)."
                                    : "You're not in the call - still in the jam.");
    refreshControls();
    resized();      // re-place the "Join call" button now that the tile is gone
    repaint();

    // Let the host broadcast "end call for everyone"; a guest hang-up affects only itself (the host
    // gates on whether it's actually hosting).
    if (onLocalCallEnded)
        onLocalCallEnded();
}

void JamSessionComponent::joinCall()
{
    if (callActive)
        return;
    callActive = true;
    if (! participants.empty())
        participants[0].inCall = true;
    addChatMessage("SYS", "You joined the call.");
    refreshControls();
    resized();
    repaint();
}

void JamSessionComponent::leaveSession()
{
    // Deliberately leave the whole session: end the call, then disconnect the collab connection.
    endCall();
    connectionState = ConnectionState::ready;
    networkStatus = {};
    if (onLeaveSessionRequested)
        onLeaveSessionRequested();
    refreshControls();
    repaint();
}

void JamSessionComponent::buttonClicked(juce::Button* button)
{
    if (button == &createButton)
    {
        // Only the real session result flips us to "live" (see setSessionStatus) — clicking must
        // never claim a connection that isn't there.
        if (connectionState == ConnectionState::live)
            addChatMessage("Orion", "Invite " + inviteCode + " copied.");
        else if (onCreateSessionRequested)
            onCreateSessionRequested();

        refreshControls();
        repaint();
    }
    else if (button == &joinButton)
    {
        if (connectionState == ConnectionState::live)
            leaveSession();             // header "Leave" = deliberately disconnect the session
        else if (onJoinSessionRequested)
            onJoinSessionRequested();

        refreshControls();
        repaint();
    }
    else if (button == &exitButton)
    {
        endCall();                      // red hang-up = leave the call (tile gone); stay in the jam
    }
    else if (button == &rejoinButton)
    {
        joinCall();                     // "Join call" in the empty strip = re-enter the video call
    }
    else if (button == &chatTab)
    {
        panelMode = PanelMode::chat;
        refreshControls();
        repaint();
    }
    else if (button == &participantsTab)
    {
        panelMode = PanelMode::participants;
        refreshControls();
        repaint();
    }
    else if (button == &setupTab)
    {
        panelMode = PanelMode::setup;
        refreshControls();
        repaint();
    }
    else if (button == &micButton)
    {
        const auto requestedState = ! micEnabled;
        if (requestedState && ! callActive)
            joinCall();                 // turning the mic on re-enters the call (Zoom-style)
        if (onMicEnabledChanged && ! onMicEnabledChanged(requestedState))
            return;
        micEnabled = requestedState;
        participants[0].muted = ! micEnabled;
        refreshControls();
        repaint();
    }
    else if (button == &cameraButton)
    {
        const auto requestedState = ! cameraEnabled;
        if (requestedState)
        {
            if (! callActive)
                joinCall();             // turning the camera on re-enters the call
            startCamera();
        }
        else
            stopCamera();
    }
    else if (button == &shareButton)
    {
        sharingEnabled = ! sharingEnabled;
        participants[0].sharing = sharingEnabled;
        if (onShareEnabledChanged)
            onShareEnabledChanged(sharingEnabled);
        addChatMessage("Orion", sharingEnabled ? "Shared Orion workspace." : "Stopped sharing.");
        refreshControls();
        repaint();
    }
    else if (button == &tipButton)
    {
        juce::PopupMenu menu;
        ui::stylePopupMenu(menu);
        menu.addItem(1, "Add files or photos");
        menu.addItem(2, "Add folders");
        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withTargetComponent(tipButton)
                               .withMinimumWidth(190),
                           [this](int result)
                           {
                               if (result == 1)
                                   openFileAttachmentChooser(false);
                               else if (result == 2)
                                   openFileAttachmentChooser(true);
                           });
    }
    else if (button == &emoteButton)
    {
        chatEditor.grabKeyboardFocus();
        showSystemEmojiPalette();
    }
    else if (button == &settingsButton)
    {
        chatEditor.grabKeyboardFocus();
    }
    else if (button == &sendButton)
    {
        sendChatMessage();
    }
}

void JamSessionComponent::textEditorReturnKeyPressed(juce::TextEditor& editor)
{
    if (&editor == &chatEditor)
        sendChatMessage();
}

void JamSessionComponent::sendChatMessage()
{
    const auto text = chatEditor.getText().trim();
    if (text.isNotEmpty())
    {
        addChatMessage("You", text);   // instant local echo; our own network echo is filtered out
        if (onSendChat)
            onSendChat(text);
        chatEditor.clear();
    }
}

void JamSessionComponent::addChatMessage(const juce::String& name, const juce::String& message)
{
    chatMessages.push_back({ name, message });
    if (chatMessages.size() > 40)
        chatMessages.erase(chatMessages.begin());
    repaint();
}

void JamSessionComponent::receiveChat(const juce::String& name, const juce::String& text)
{
    addChatMessage(name.isEmpty() ? juce::String("Peer") : name, text);
}

void JamSessionComponent::setRoster(const std::vector<RosterMember>& members)
{
    std::vector<Participant> next;
    next.reserve(members.size());
    for (const auto& m : members)
    {
        Participant p;
        p.name = m.name;
        p.colour = juce::Colour(m.colourArgb);
        if (m.isLocal)
        {
            // The local row mirrors this machine's real mic/camera/share state.
            p.muted = ! micEnabled;
            p.cameraOff = ! cameraEnabled;
            p.sharing = sharingEnabled;
            p.inCall = callActive;
        }
        else
        {
            // Peer mic/cam state isn't transmitted yet — show them present.
            p.muted = false;
            p.cameraOff = true;
            p.sharing = false;
            p.inCall = m.inCall;   // only peers actually in the call get a tile
        }
        next.push_back(std::move(p));
    }

    if (next.empty())   // never leave the local row absent; other code indexes participants[0]
        next.push_back(Participant { "You", juce::Colour(0xffd9785f), ! micEnabled, ! cameraEnabled, sharingEnabled, callActive });

    if (next != participants)
    {
        participants = std::move(next);
        // When we're out of the call the "Join call" slot sits after the peer tiles, so a peer
        // joining/leaving the call must re-place the button.
        if (! callActive)
            resized();
        repaint();
    }
}

void JamSessionComponent::setDiagnostics(const juce::String& text)
{
    if (diagnosticsText == text)
        return;

    diagnosticsText = text;
    repaint();
}

void JamSessionComponent::setSessionStatus(bool live, const juce::String& detail)
{
    const bool wasLive = connectionState == ConnectionState::live;
    connectionState = live ? ConnectionState::live : ConnectionState::ready;
    networkStatus = detail;

    // Freshly connecting (or reconnecting) puts you in the call by default, like joining a meeting.
    if (live && ! wasLive)
    {
        callActive = true;
        if (! participants.empty())
            participants[0].inCall = true;
    }

    // Echo it into the chat too: it's the one place in this panel that's always on screen, so
    // connection results and errors can't go unnoticed.
    if (detail.isNotEmpty())
        addChatMessage("Orion", detail);

    refreshControls();
    repaint();
}

void JamSessionComponent::refreshControls()
{
    const auto live = connectionState == ConnectionState::live;
    createButton.setButtonText(live ? "Invite" : "Start");
    // Second slot doubles as the way OUT of a session — there was no way to leave one before.
    joinButton.setButtonText(live ? "Leave" : "Join");
    joinButton.setEnabled(true);
    exitButton.setButtonText("Leave");

    chatTab.setToggleState(panelMode == PanelMode::chat, juce::dontSendNotification);
    participantsTab.setToggleState(panelMode == PanelMode::participants, juce::dontSendNotification);
    setupTab.setToggleState(panelMode == PanelMode::setup, juce::dontSendNotification);
    micButton.setToggleState(! micEnabled, juce::dontSendNotification);
    cameraButton.setToggleState(! cameraEnabled, juce::dontSendNotification);
    shareButton.setToggleState(sharingEnabled, juce::dontSendNotification);
    micButton.setButtonText(micEnabled ? "Mic" : "Muted");
    cameraButton.setButtonText(cameraOpening ? "Opening" : (cameraEnabled ? "Cam on" : "Cam"));
    shareButton.setButtonText(sharingEnabled ? "Live" : "Share");

    const auto setupVisible = panelMode == PanelMode::setup;
    for (auto* box : { &audioInputBox, &audioOutputBox, &cameraDeviceBox, &latencyBox })
        box->setVisible(setupVisible);
    // "Join call" only when we've actually left the call, so the empty strip has a way back.
    rejoinButton.setVisible(! callActive);

    chatEditor.setVisible(panelMode == PanelMode::chat);
    tipButton.setVisible(panelMode == PanelMode::chat);
    emoteButton.setVisible(panelMode == PanelMode::chat);
    settingsButton.setVisible(false);
    sendButton.setVisible(panelMode == PanelMode::chat);
}

void JamSessionComponent::startCamera()
{
    if (cameraEnabled || cameraOpening)
        return;

    if (onCameraEnabledChanged && ! onCameraEnabledChanged(true))
        return;

    const auto devices = juce::CameraDevice::getAvailableDevices();
    if (devices.isEmpty())
    {
        addChatMessage("Orion", "No camera device found.");
        return;
    }

    cameraOpening = true;
    refreshControls();
    juce::Component::SafePointer<JamSessionComponent> safeThis(this);
    juce::CameraDevice::openDeviceAsync(0,
        [safeThis](juce::CameraDevice* device, const juce::String& error)
        {
            if (safeThis == nullptr)
            {
                delete device;
                return;
            }

            safeThis->cameraOpening = false;
            safeThis->cameraDevice.reset(device);
            if (safeThis->cameraDevice == nullptr)
            {
                safeThis->cameraEnabled = false;
                safeThis->participants[0].cameraOff = true;
                safeThis->cameraDeviceBox.setSelectedId(2, juce::dontSendNotification);
                safeThis->addChatMessage("Orion", error.isNotEmpty() ? error : "Camera could not be opened.");
                safeThis->refreshControls();
                safeThis->repaint();
                return;
            }

            // Take the frames ourselves and paint them (cover-scaled) instead of embedding the
            // native viewer, so the video fills the tile exactly and controls can sit on top.
            safeThis->cameraDevice->addListener(safeThis);
            safeThis->attachCallControlsTo(*safeThis);
            safeThis->layoutCallControls(safeThis->localParticipantTileBounds());

            safeThis->cameraEnabled = true;
            safeThis->participants[0].cameraOff = false;
            safeThis->cameraDeviceBox.setSelectedId(1, juce::dontSendNotification);
            safeThis->refreshControls();
            safeThis->resized();
            safeThis->repaint();
        },
        640, 360, 1920, 1080, true);
}

void JamSessionComponent::imageReceived(const juce::Image& image)
{
    // Called on the camera's background thread. Stash a copy and repaint on the message thread.
    {
        const juce::ScopedLock sl(frameLock);
        latestFrame = image.createCopy();
    }
    // Repaint only the local tile, not the whole panel — a full-panel repaint 30x/s would be a
    // needless software-render load (chat, timeline strip, etc.).
    juce::Component::SafePointer<JamSessionComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->repaint(safeThis->localParticipantTileBounds().expanded(2));
    });
}

void JamSessionComponent::drawLocalCameraFrame(juce::Graphics& g, juce::Rectangle<float> tile)
{
    juce::Image frame;
    { const juce::ScopedLock sl(frameLock); frame = latestFrame; }
    if (! frame.isValid())
        return;

    const auto iw = static_cast<float>(frame.getWidth());
    const auto ih = static_cast<float>(frame.getHeight());
    if (iw <= 0.0f || ih <= 0.0f)
        return;

    juce::Graphics::ScopedSaveState state(g);
    juce::Path clip;
    clip.addRoundedRectangle(tile, 8.0f);
    g.reduceClipRegion(clip);

    // COVER: scale so the frame fills the tile on both axes, centre, crop the overflow.
    const auto scale = juce::jmax(tile.getWidth() / iw, tile.getHeight() / ih);
    const auto transform = juce::AffineTransform::scale(scale)
                               .translated(tile.getCentreX() - iw * scale * 0.5f,
                                           tile.getCentreY() - ih * scale * 0.5f);
    g.drawImageTransformed(frame, transform);

    // Name chip, top-left, over the video (like a video call). Only the local tile draws the frame,
    // so participants[0] is us.
    if (! participants.empty())
    {
        const auto name = participants[0].name;
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        const auto textW = juce::jmin(tile.getWidth() - 12.0f,
                                      static_cast<float>(g.getCurrentFont().getStringWidth(name)) + 16.0f);
        juce::Rectangle<float> chip(tile.getX() + 6.0f, tile.getY() + 6.0f, textW, 20.0f);
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(chip, 5.0f);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawText(name, chip.reduced(8.0f, 0.0f), juce::Justification::centredLeft, true);
    }
}

void JamSessionComponent::stopCamera()
{
    cameraOpening = false;
    attachCallControlsTo(*this);
    if (cameraDevice != nullptr)
        cameraDevice->removeListener(this);
    cameraDevice.reset();
    { const juce::ScopedLock sl(frameLock); latestFrame = {}; }
    cameraEnabled = false;
    participants[0].cameraOff = true;
    cameraDeviceBox.setSelectedId(2, juce::dontSendNotification);
    if (onCameraEnabledChanged)
        onCameraEnabledChanged(false);
    refreshControls();
    repaint();
}

void JamSessionComponent::drawChatPanel(juce::Graphics& g, juce::Rectangle<float> area, bool compact)
{
    auto messages = area.reduced(0.0f, compact ? 2.0f : 3.0f);
    const auto rowH = compact ? 108.0f : 112.0f;
    const auto systemRowH = compact ? 70.0f : 74.0f;
    const auto maxRows = juce::jmax(1, static_cast<int>(messages.getHeight() / rowH));
    const auto first = juce::jmax(0, static_cast<int>(chatMessages.size()) - maxRows - 1);

    for (int i = first; i < static_cast<int>(chatMessages.size()); ++i)
    {
        const auto& [name, text] = chatMessages[static_cast<std::size_t>(i)];
        const auto isSystem = name == "Orion";
        auto row = messages.removeFromTop(isSystem ? systemRowH : rowH);
        if (row.isEmpty())
            break;

        if (isSystem)
        {
            auto notice = row.reduced(0.0f, 3.0f);
            g.setColour(elevated.withAlpha(0.62f));
            g.fillRoundedRectangle(notice, 5.0f);
            g.setColour(theme::line::subtle.withAlpha(0.55f));
            g.drawRoundedRectangle(notice.reduced(0.5f), 5.0f, 1.0f);

            auto badge = notice.removeFromLeft(62.0f).reduced(6.0f, 7.0f);
            g.setColour(theme::cool::cyan.withAlpha(0.18f));
            g.fillRoundedRectangle(badge, 4.0f);
            g.setColour(theme::cool::cyan.withAlpha(0.9f));
            g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
            g.drawText("SYS", badge, juce::Justification::centred);

            g.setColour(theme::text::secondary);
            g.setFont(juce::FontOptions(24.0f));
            g.drawFittedText(text, notice.reduced(4.0f, 0.0f).toNearestInt(),
                             juce::Justification::centredLeft, 2);
            continue;
        }

        auto line = row.reduced(0.0f, 8.0f);
        auto nameLine = line.removeFromTop(34.0f);
        const auto user = name + ":";
        auto userFont = juce::Font(juce::FontOptions(27.0f, juce::Font::bold));
        auto textFont = juce::Font(juce::FontOptions(27.0f));

        g.setFont(userFont);
        g.setColour(colourForName(name));
        g.drawText(user, nameLine, juce::Justification::centredLeft, false);

        g.setFont(textFont);
        g.setColour(theme::text::primary.withAlpha(0.92f));
        g.drawFittedText(text, line.toNearestInt(), juce::Justification::topLeft, 2);
    }
}

void JamSessionComponent::drawParticipantsPanel(juce::Graphics& g, juce::Rectangle<float> area, bool compact)
{
    const auto rowH = compact ? 42.0f : 46.0f;
    for (int i = 0; i < static_cast<int>(participants.size()); ++i)
    {
        auto row = area.removeFromTop(rowH);
        if (row.isEmpty())
            break;

        const auto& participant = participants[static_cast<std::size_t>(i)];
        auto dot = row.removeFromLeft(22.0f).withSizeKeepingCentre(10.0f, 10.0f);
        g.setColour(i == 0 ? theme::status::success : theme::text::muted);
        g.fillEllipse(dot);

        auto text = row.removeFromLeft(row.getWidth() - 74.0f);
        g.setColour(theme::text::primary);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(participant.name, text.removeFromTop(17.0f), juce::Justification::centredLeft);
        g.setColour(theme::text::secondary);
        g.setFont(juce::FontOptions(11.0f));
        const auto status = juce::String(participant.muted ? "mic off" : "mic on")
            + " / " + (participant.cameraOff ? "cam off" : "cam on")
            + (participant.sharing ? " / sharing" : "");
        g.drawText(status, text, juce::Justification::centredLeft);

        g.setColour(participant.muted ? coral : theme::status::success);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(participant.muted ? "MUTE" : "LIVE", row, juce::Justification::centredRight);
    }
}

void JamSessionComponent::drawSetupPanel(juce::Graphics& g, juce::Rectangle<float> area, bool compact)
{
    const auto labelH = compact ? 18.0f : 20.0f;
    const auto comboGap = compact ? 38.0f : 42.0f;
    const std::array<juce::String, 4> labels { "Audio input", "Audio output", "Camera", "Sync mode" };

    for (const auto& label : labels)
    {
        auto row = area.removeFromTop(comboGap);
        g.setColour(theme::text::secondary);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(label, row.removeFromTop(labelH), juce::Justification::centredLeft);
    }
}

void JamSessionComponent::drawParticipantTile(juce::Graphics& g, juce::Rectangle<float> tile,
                                               const Participant& participant, bool active) const
{
    g.setColour(participant.colour.darker(0.55f));
    g.fillRoundedRectangle(tile, 8.0f);
    g.setColour(active ? coral : theme::line::subtle);
    g.drawRoundedRectangle(tile.reduced(0.5f), 8.0f, active ? 2.0f : 1.0f);

    auto body = tile.reduced(12.0f);
    body.removeFromBottom(28.0f);
    drawAvatar(g, body, participant);

    auto footer = tile.removeFromBottom(28.0f).reduced(10.0f, 5.0f);
    g.setColour(juce::Colours::black.withAlpha(0.38f));
    g.fillRoundedRectangle(footer.expanded(2.0f, 0.0f), 4.0f);
    g.setColour(theme::text::primary);
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    auto nameArea = footer.removeFromLeft(juce::jmin(86.0f, footer.getWidth() * 0.56f));
    g.drawText(participant.name, nameArea, juce::Justification::centredLeft);
    const auto tileState = participant.sharing ? "sharing" : (participant.muted ? "mic off" : "live");
    drawPill(g, footer.reduced(0.0f, 1.0f), tileState,
             participant.muted ? coral.withAlpha(0.14f) : theme::status::success.withAlpha(0.12f),
             participant.muted ? coral.withAlpha(0.55f) : theme::status::success.withAlpha(0.45f),
             participant.muted ? coral : theme::status::success);
}

void JamSessionComponent::drawCallEndedPlaceholder(juce::Graphics& g, juce::Rectangle<float> tile) const
{
    // A dashed placeholder tile with a caption on top; the rejoinButton (a real child) sits over its
    // lower half (positioned in resized()).
    g.setColour(theme::surface::primary.withAlpha(0.35f));
    g.fillRoundedRectangle(tile, 8.0f);
    g.setColour(theme::line::subtle.withAlpha(0.85f));
    g.drawRoundedRectangle(tile.reduced(0.5f), 8.0f, 1.0f);

    auto top = tile.reduced(6.0f);
    top.removeFromBottom(tile.getHeight() * 0.48f);   // leave the lower half for the button
    g.setColour(theme::text::secondary);
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText("You left the call", top.removeFromTop(18.0f), juce::Justification::centred);
    g.setColour(theme::text::muted);
    g.setFont(juce::FontOptions(10.0f, juce::Font::plain));
    g.drawText("still in the jam", top.removeFromTop(14.0f), juce::Justification::centred, true);
}

void JamSessionComponent::drawAvatar(juce::Graphics& g, juce::Rectangle<float> area,
                                     const Participant& participant) const
{
    if (participant.cameraOff)
    {
        auto cameraArea = area.reduced(8.0f, 10.0f);
        g.setColour(juce::Colours::black.withAlpha(0.22f));
        g.fillRoundedRectangle(cameraArea, 8.0f);
        g.setColour(theme::line::subtle.withAlpha(0.75f));
        g.drawRoundedRectangle(cameraArea.reduced(0.5f), 8.0f, 1.0f);

        auto icon = cameraArea.withSizeKeepingCentre(42.0f, 28.0f).translated(0.0f, -8.0f);
        g.setColour(theme::text::secondary.withAlpha(0.82f));
        g.drawRoundedRectangle(icon.reduced(4.0f, 5.0f), 4.0f, 1.6f);
        juce::Path slash;
        slash.startNewSubPath(icon.getX() + 5.0f, icon.getBottom() - 2.0f);
        slash.lineTo(icon.getRight() - 5.0f, icon.getY() + 2.0f);
        g.strokePath(slash, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText("camera off", cameraArea.withTrimmedTop(cameraArea.getHeight() - 24.0f),
                   juce::Justification::centred);
        return;
    }

    if (participant.sharing)
    {
        auto shareArea = area.reduced(8.0f, 10.0f);
        g.setColour(theme::cool::cyan.withAlpha(0.10f));
        g.fillRoundedRectangle(shareArea, 8.0f);
        g.setColour(theme::cool::cyan.withAlpha(0.85f));
        g.drawRoundedRectangle(shareArea.reduced(0.5f), 8.0f, 1.2f);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText("shared workspace", shareArea.withTrimmedTop(shareArea.getHeight() - 24.0f),
                   juce::Justification::centred);
        area = shareArea.withTrimmedBottom(24.0f);
    }

    const auto size = juce::jmin(area.getWidth(), area.getHeight()) * 0.42f;
    auto avatar = juce::Rectangle<float>(size, size).withCentre({ area.getCentreX(), area.getCentreY() - 8.0f });
    g.setColour(participant.colour.brighter(0.35f));
    g.fillEllipse(avatar);
    g.setColour(juce::Colours::white.withAlpha(0.78f));
    g.fillEllipse(avatar.withSizeKeepingCentre(size * 0.42f, size * 0.42f).translated(0, -size * 0.12f));
    g.fillEllipse(avatar.withSizeKeepingCentre(size * 0.62f, size * 0.34f).translated(0, size * 0.25f));
}
} // namespace orion
