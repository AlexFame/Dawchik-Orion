#include "CameraChordWheelComponent.h"
#include "../UI/OrionTheme.h"
#include "../Core/ChordTheory.h"

namespace orion
{
CameraChordWheelComponent::CameraChordWheelComponent()
{
    setOpaque (false);
    wheel = std::make_unique<chordwheel::Component>();
    wheel->onRootPointed = [this] (const chords::ChordSpec& chord)
    {
        if (onChordPointed) onChordPointed (chord);
    };
    wheel->onRootSelected = [this] (const chords::ChordSpec& chord)
    {
        if (onChordSelected) onChordSelected (chord);
    };
    addAndMakeVisible (*wheel);

    titleLabel.setText ("Camera Chords", juce::dontSendNotification);
    titleLabel.setColour (juce::Label::textColourId, theme::text::primary);
    titleLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    statusLabel.setText ("Point your index finger at a chord", juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, theme::text::secondary);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    closeButton.setTooltip ("Close camera chord mode");
    closeButton.setColour (juce::TextButton::buttonColourId, theme::surface::primary);
    closeButton.setColour (juce::TextButton::textColourOffId, theme::text::primary);
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);
}

CameraChordWheelComponent::~CameraChordWheelComponent()
{
    stopCamera();
}

void CameraChordWheelComponent::setProjectKey (int rootPc, const std::array<int, 7>& pattern,
                                               const juce::String& name)
{
    keyRootPc = (rootPc % 12 + 12) % 12;
    keyPattern = pattern;
    keyName = name;
    wheel->setKey (keyRootPc, keyPattern);
    repaint();
}

void CameraChordWheelComponent::setChord (const chords::ChordSpec& chord, bool audition)
{
    wheel->setChord (chord);
    if (audition && onChordSelected)
        onChordSelected (chord);
}

bool CameraChordWheelComponent::startCamera()
{
    if (camera != nullptr || cameraOpening)
        return true;
    const auto devices = juce::CameraDevice::getAvailableDevices();
    if (devices.isEmpty())
    {
        statusLabel.setText ("No camera found", juce::dontSendNotification);
        return false;
    }
    cameraOpening = true;
    statusLabel.setText ("Opening camera...", juce::dontSendNotification);
    juce::Component::SafePointer<CameraChordWheelComponent> safeThis (this);
    juce::CameraDevice::openDeviceAsync (0,
        [safeThis] (juce::CameraDevice* device, const juce::String& error)
        {
            if (safeThis != nullptr) safeThis->cameraOpened (device, error);
            else delete device;
        }, 640, 480, 1280, 720, false);
    return true;
}

void CameraChordWheelComponent::cameraOpened (juce::CameraDevice* device, const juce::String& error)
{
    cameraOpening = false;
    camera.reset (device);
    if (camera == nullptr)
    {
        statusLabel.setText (error.isNotEmpty() ? error : "Camera unavailable", juce::dontSendNotification);
        return;
    }
    camera->addListener (this);
    statusLabel.setText ("Point your index finger at a chord", juce::dontSendNotification);
    startTimerHz (15);
}

void CameraChordWheelComponent::stopCamera()
{
    stopTimer();
    if (camera != nullptr)
    {
        camera->removeListener (this);
        camera->stopRecording();
        camera.reset();
    }
    cameraOpening = false;
}

void CameraChordWheelComponent::imageReceived (const juce::Image& image)
{
    const juce::ScopedLock lock (frameLock);
    latestFrame = image.createCopy();
}

void CameraChordWheelComponent::timerCallback()
{
    updatePointingFromFrame();
    repaint();
}

void CameraChordWheelComponent::updatePointingFromFrame()
{
    juce::Image frame;
    {
        const juce::ScopedLock lock (frameLock);
        frame = latestFrame.createCopy();
    }
    const auto tip = camera::detectIndexTip (frame);
    if (! tip.has_value())
    {
        wheel->clearPointing();
        return;
    }

    // The preview is mirrored for the user, so mirror the hand horizontally too.
    const auto area = wheel->getBounds().toFloat();
    const auto local = juce::Point<float> (area.getX() + (1.0f - tip->x) * area.getWidth(),
                                           area.getY() + tip->y * area.getHeight());
    wheel->setPointingPosition (local - wheel->getPosition().toFloat());
}

void CameraChordWheelComponent::paint (juce::Graphics& g)
{
    g.setColour (theme::core::studio);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::metrics::panelRadius);
    g.setColour (theme::cool::cyan.withAlpha (0.32f));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.6f), theme::metrics::panelRadius, 1.2f);

    auto preview = getLocalBounds().toFloat().reduced (18.0f);
    preview.removeFromTop (46.0f);
    preview.removeFromBottom (30.0f);
    preview = preview.removeFromLeft (preview.getWidth() * 0.43f);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.fillRoundedRectangle (preview, 12.0f);
    {
        const juce::ScopedLock lock (frameLock);
        if (latestFrame.isValid())
            g.drawImageWithin (latestFrame, static_cast<int> (preview.getX()), static_cast<int> (preview.getY()),
                               static_cast<int> (preview.getWidth()), static_cast<int> (preview.getHeight()),
                               juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
    }
    g.setColour (theme::text::primary.withAlpha (0.55f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (keyName, preview.toNearestInt().withTrimmedTop (8), juce::Justification::centredTop);
}

void CameraChordWheelComponent::resized()
{
    auto area = getLocalBounds().reduced (18);
    titleLabel.setBounds (area.removeFromTop (30));
    closeButton.setBounds (area.removeFromTop (30).removeFromRight (68));
    statusLabel.setBounds (area.removeFromBottom (24));
    area.removeFromBottom (6);
    const auto preview = area.removeFromLeft (static_cast<int> (area.getWidth() * 0.43f));
    area.removeFromLeft (18);
    wheel->setBounds (area.withSizeKeepingCentre (juce::jmin (area.getWidth(), area.getHeight()),
                                                   juce::jmin (area.getWidth(), area.getHeight())));
    juce::ignoreUnused (preview);
}
}
