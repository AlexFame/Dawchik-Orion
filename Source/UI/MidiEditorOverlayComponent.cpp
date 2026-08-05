#include "MidiEditorOverlayComponent.h"

#include "OrionTheme.h"
#include "OrionPopupMenu.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <vector>

namespace
{
// Match the main-screen control language: Avenir Next, compact 6px controls and a
// light coral active state shared with the Browser and left rail.
class PianoRollButtonLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool highlighted, bool down) override
    {
        const auto b = button.getLocalBounds().toFloat();
        const bool on = button.getToggleState();
        // Clearly-visible pill so the label reads against the dark red header (the previous fill blended in).
        const auto fill = on ? orion::theme::accent::activeCoral.withAlpha(0.90f)
                        : (down        ? orion::theme::surface::hover
                        : (highlighted ? orion::theme::surface::elevated
                                       : orion::theme::surface::primary));
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillRoundedRectangle(b.translated(0.0f, 1.0f), 6.0f);
        g.setColour(fill);
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(on ? orion::theme::accent::activeCoralLight.withAlpha(0.9f)
                       : juce::Colours::white.withAlpha(0.16f));
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);
    }

    juce::Font getTextButtonFont(juce::TextButton&, int) override
    {
        return juce::Font(juce::FontOptions("Avenir Next", 13.0f, juce::Font::bold));
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        g.setColour(juce::Colours::white.withAlpha(0.94f));
        g.setFont(juce::FontOptions("Avenir Next", 13.0f, juce::Font::bold));
        g.drawText(button.getButtonText(), button.getLocalBounds().reduced(8, 0), juce::Justification::centred, true);
    }
};

juce::LookAndFeel& pianoRollButtonLookAndFeel()
{
    static PianoRollButtonLookAndFeel lnf;
    return lnf;
}

// Locks the shared audio-edit CriticalSection (if wired) for the scope — used around any add/remove of
// notes/slides so the audio thread can't read the vector while it's being reallocated.
struct OptionalAudioLock
{
    juce::CriticalSection* l;
    explicit OptionalAudioLock(juce::CriticalSection* lock) : l(lock) { if (l) l->enter(); }
    ~OptionalAudioLock() { if (l) l->exit(); }
};

const auto overlayBackground = orion::theme::core::canvas.withAlpha(0.96f);
const auto panelBackground = orion::theme::surface::primary;
const auto panelStroke = orion::theme::line::normal;
const auto mutedText = orion::theme::text::tertiary.withAlpha(0.88f);
const auto playheadColour = orion::theme::warm::red;
const auto velocityLaneBackground = orion::theme::core::deepSpace;
// Lighter grid so it's easier on the eyes (was near-black .darker(0.58) + heavy black rows).
const auto pianoGridBase = orion::theme::surface::primary.darker(0.18f);
const auto pianoGridScaleRow = orion::theme::surface::elevated.brighter(0.16f).withAlpha(0.40f);
const auto pianoGridBlackRow = orion::theme::core::voidBlack.withAlpha(0.20f);
constexpr int resizeHandleWidth = 10;
constexpr double minimumNoteLengthInBeats = 0.03125;   // 1/128 — small enough that a 1/64 grid cell isn't clamped up
// Empty space (in beats) shown PAST the clip's end so you can keep writing the melody beyond
// the current clip length. Drawing a note into this area grows the clip to fit (4 bars).
constexpr double editingHeadroomBeats = 16.0;
constexpr int minimumNoteWidthPx = 4;
constexpr int marqueeThresholdPx = 4;
constexpr int noteDragThresholdPx = 2;
constexpr int slideDrawThresholdPx = 3;
constexpr float slideHandleRadiusPx = 6.0f;
constexpr int baseLaneHeightPx = 24;
constexpr int velocityLaneHeightPx = 112;
constexpr int topBarHeightPx = 122;
constexpr int totalPitchCount = 128;
constexpr int lowestPitch = 0;
constexpr int highestPitch = totalPitchCount - 1;
constexpr double beatEpsilon = 0.0001;
constexpr double minimumHorizontalZoom = 0.5;
constexpr double minimumVerticalZoom = 0.45;
constexpr double maximumHorizontalZoom = 6.0;
constexpr double maximumVerticalZoom = 3.0;
constexpr double maximumAutoFocusVerticalZoom = 1.45;
constexpr double minimumSlidePointBeatDistance = 0.02;
constexpr double placedNoteAuditionMs = 180.0;
constexpr int pianoRollToolButtonCount = 9;

struct ScalePattern
{
    const char* name;
    std::array<int, 7> pitchClasses;
};

constexpr std::array<const char*, 12> rootNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
constexpr std::array<ScalePattern, 3> scalePatterns {{
    { "Minor", { 0, 2, 3, 5, 7, 8, 10 } },
    { "Major", { 0, 2, 4, 5, 7, 9, 11 } },
    { "Dorian", { 0, 2, 3, 5, 7, 9, 10 } },
}};

struct SnapSetting
{
    const char* name;
    double beats;
};

constexpr std::array<SnapSetting, 5> snapSettings {{
    { "1/4", 1.0 },
    { "1/8", 0.5 },
    { "1/16", 0.25 },
    { "1/32", 0.125 },
    { "1/64", 0.0625 },
}};

struct PianoRollToolInfo
{
    const char* name;
    const char* shortcut;
};

PianoRollToolInfo getPianoRollToolInfo(int index) noexcept
{
    static constexpr std::array<PianoRollToolInfo, pianoRollToolButtonCount> tools {{
        { "Select", "Cmd+V" },
        { "Draw", "Cmd+Shift+D" },
        { "Cut", "Cmd+B" },
        { "Erase", "Cmd+Shift+E" },
        { "Quantize", "Option+Q" },
        { "Velocity", "Cmd+Shift+V" },
        { "Slide", "Cmd+Shift+S" },
        { "Step", "Cmd+Shift+W" },
        { "Audition", "Cmd+Shift+A" }
    }};

    if (index < 0 || index >= static_cast<int>(tools.size()))
        return { "", "" };

    return tools[static_cast<std::size_t>(index)];
}

	std::optional<int> pitchForTypingKeyCode(int keyCode)
	{
	    static constexpr int lowerRowBasePitch = 48;
	    static constexpr int upperRowBasePitch = 60;
	    static constexpr std::array<std::pair<int, int>, 39> mapping {{
	        { 'z', lowerRowBasePitch + 0 }, { 's', lowerRowBasePitch + 1 }, { 'x', lowerRowBasePitch + 2 },
	        { 'd', lowerRowBasePitch + 3 }, { 'c', lowerRowBasePitch + 4 }, { 'v', lowerRowBasePitch + 5 },
	        { 'g', lowerRowBasePitch + 6 }, { 'b', lowerRowBasePitch + 7 }, { 'h', lowerRowBasePitch + 8 },
	        { 'n', lowerRowBasePitch + 9 }, { 'j', lowerRowBasePitch + 10 }, { 'm', lowerRowBasePitch + 11 },
	        { ',', lowerRowBasePitch + 12 }, { 'l', lowerRowBasePitch + 13 }, { '.', lowerRowBasePitch + 14 },
	        { ';', lowerRowBasePitch + 15 }, { '/', lowerRowBasePitch + 16 }, { '\'', lowerRowBasePitch + 17 },
	        { 'q', upperRowBasePitch + 0 }, { '2', upperRowBasePitch + 1 }, { 'w', upperRowBasePitch + 2 },
	        { '3', upperRowBasePitch + 3 }, { 'e', upperRowBasePitch + 4 }, { 'r', upperRowBasePitch + 5 },
	        { '5', upperRowBasePitch + 6 }, { 't', upperRowBasePitch + 7 }, { '6', upperRowBasePitch + 8 },
	        { 'y', upperRowBasePitch + 9 }, { '7', upperRowBasePitch + 10 }, { 'u', upperRowBasePitch + 11 },
	        { 'i', upperRowBasePitch + 12 }, { '9', upperRowBasePitch + 13 }, { 'o', upperRowBasePitch + 14 },
	        { '0', upperRowBasePitch + 15 }, { 'p', upperRowBasePitch + 16 }, { '[', upperRowBasePitch + 17 },
	        { ']', upperRowBasePitch + 18 }, { '+', upperRowBasePitch + 19 }, { '=', upperRowBasePitch + 19 },
	    }};

	    const auto lowerKeyCode = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(keyCode));
	    for (const auto& [mappedKey, pitch] : mapping)
	        if (lowerKeyCode == mappedKey)
	            return pitch;

	    return std::nullopt;
	}

    std::optional<int> diatonicPitchForTypingKeyCode(int keyCode, int scaleRoot, int scalePatternIndex)
    {
        static const std::array<std::pair<int, int>, 12> upperRow {{
            {'q', 0}, {'w', 1}, {'e', 2}, {'r', 3}, {'t', 4}, {'y', 5},
            {'u', 6}, {'i', 7}, {'o', 8}, {'p', 9}, {'[', 10}, {']', 11},
        }};
        static const std::array<std::pair<int, int>, 11> lowerRow {{
            {'z', 0}, {'x', 1}, {'c', 2}, {'v', 3}, {'b', 4}, {'n', 5},
            {'m', 6}, {',', 7}, {'.', 8}, {'/', 9}, {'\'', 10},
        }};

        const auto lowerKeyCode = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(keyCode));
        int diatonicIndex = -1;
        bool isLowerRow = false;
        for (const auto& [mappedKey, index] : upperRow)
            if (lowerKeyCode == mappedKey) { diatonicIndex = index; break; }
        if (diatonicIndex < 0)
        {
            for (const auto& [mappedKey, index] : lowerRow)
                if (lowerKeyCode == mappedKey) { diatonicIndex = index; isLowerRow = true; break; }
        }
        if (diatonicIndex < 0)
            return std::nullopt;

        const auto& pattern = scalePatterns[static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(scalePatterns.size()) - 1, scalePatternIndex))].pitchClasses;
        const auto root = ((scaleRoot % 12) + 12) % 12;
        const auto rowBase = isLowerRow ? 48 : 60;
        const auto octaveShift = diatonicIndex / 7;
        const auto degree = diatonicIndex % 7;

        return juce::jlimit(0, 127, rowBase + root + octaveShift * 12 + pattern[static_cast<std::size_t>(degree)]);
    }

	juce::String noteNameForPitch(int pitch)
	{
	    static constexpr const char* noteNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	    const auto pitchClass = ((pitch % 12) + 12) % 12;
	    const auto octave = (pitch / 12) - 1;
	    return juce::String(noteNames[pitchClass]) + juce::String(octave);
	}

    juce::Colour midiAccentForTrack(juce::Colour sourceColour, bool selected = false)
    {
        const auto saturation = juce::jmin(sourceColour.getSaturation(), selected ? 0.74f : 0.66f);
        const auto brightness = juce::jlimit(selected ? 0.58f : 0.52f,
                                             selected ? 0.92f : 0.82f,
                                             sourceColour.getBrightness() + (selected ? 0.14f : 0.08f));
        return sourceColour.withSaturation(saturation).withBrightness(brightness);
    }
}  // namespace

namespace orion
{
MidiEditorOverlayComponent::MidiEditorOverlayComponent()
{
    setWantsKeyboardFocus(true);
    setVisible(false);
    // Keep animation at the display cadence. Input edits repaint immediately, so a 120 Hz
    // timer only spent CPU redrawing the editor between actual frames.
    startTimerHz(60);

    // Unify all the toolbar TextButtons with the main-screen button style (child buttons inherit this).
    setLookAndFeel(&pianoRollButtonLookAndFeel());

    titleLabel.setColour(juce::Label::textColourId, theme::text::primary);
    titleLabel.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    addAndMakeVisible(titleLabel);

    subtitleLabel.setColour(juce::Label::textColourId, mutedText);
    subtitleLabel.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    addAndMakeVisible(subtitleLabel);

    contextLabel.setColour(juce::Label::textColourId, theme::text::primary.withAlpha(0.90f));
    contextLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    contextLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(contextLabel);

    for (auto* label : { &scaleLabel, &snapLabel })
    {
        label->setColour(juce::Label::textColourId, mutedText);
        label->setFont(juce::FontOptions(12.0f, juce::Font::plain));
        addChildComponent(*label);
    }

    scaleLabel.setText("Scale", juce::dontSendNotification);
    snapLabel.setText("Snap", juce::dontSendNotification);

    scaleButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    scaleButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    scaleButton.addListener(this);
    addAndMakeVisible(scaleButton);

    snapButton.setTooltip("Snap grid: where notes land and the grid cell size");
    snapButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    snapButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    snapButton.addListener(this);
    addAndMakeVisible(snapButton);

    quantizeButton.setButtonText("Quantize");
    quantizeButton.setTooltip("Quantize selected notes (or all if none selected) to the Snap grid  -  Option+Q");
    quantizeButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    quantizeButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    quantizeButton.addListener(this);
    addChildComponent(quantizeButton);

    slidePenButton.setButtonText("Slide Pen");
    slidePenButton.setClickingTogglesState(true);
    slidePenButton.setTooltip("Draw pitch slides directly in the piano roll");
    slidePenButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    slidePenButton.setColour(juce::TextButton::buttonOnColourId, theme::accent::activeCoral);
    slidePenButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    slidePenButton.setColour(juce::TextButton::textColourOnId, theme::text::primary);
    slidePenButton.addListener(this);
    addChildComponent(slidePenButton);

    slideVisibilityButton.setButtonText(getSlideVisibilityName());
    slideVisibilityButton.setTooltip("Cycle slide visibility: Ghost, Active, Off");
    slideVisibilityButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    slideVisibilityButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    slideVisibilityButton.addListener(this);
    addAndMakeVisible(slideVisibilityButton);

    stepWriteButton.setButtonText("Step Write");
    stepWriteButton.setClickingTogglesState(true);
    stepWriteButton.setTooltip("Step Write mode: type/play notes one grid step at a time");
    stepWriteButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    stepWriteButton.setColour(juce::TextButton::buttonOnColourId, theme::accent::activeCoral);
    stepWriteButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    stepWriteButton.setColour(juce::TextButton::textColourOnId, theme::text::primary);
    stepWriteButton.addListener(this);
    addChildComponent(stepWriteButton);

    joinButton.setButtonText("Join");
    joinButton.setTooltip("Join selected notes into one gliding voice (MPE-style)");
    joinButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    joinButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    joinButton.addListener(this);
    addAndMakeVisible(joinButton);

    splitButton.setButtonText("Split");
    splitButton.setTooltip("Split a selected glide back into separate notes");
    splitButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    splitButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    splitButton.addListener(this);
    addAndMakeVisible(splitButton);

    modButton.setButtonText("Mod");
    modButton.setTooltip("Cycle the selected glide's LFO/vibrato shape (Off → Sine → Tri → Saw → Square)");
    modButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    modButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    modButton.addListener(this);
    addAndMakeVisible(modButton);

    presetsButton.setButtonText("Presets");
    presetsButton.setTooltip("Apply a pitch-curve preset (scoop / fall / slide / vibrato) to the selected note(s)");
    presetsButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    presetsButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    presetsButton.addListener(this);
    addAndMakeVisible(presetsButton);

    stepLengthButton.setTooltip("Step Write length");
    stepLengthButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    stepLengthButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    stepLengthButton.addListener(this);
    addAndMakeVisible(stepLengthButton);

    stepRestButton.setButtonText("Rest");
    stepRestButton.setTooltip("Step Write rest: advance without a note");
    stepRestButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    stepRestButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    stepRestButton.addListener(this);
    addAndMakeVisible(stepRestButton);

    stepBackButton.setButtonText("Back");
    stepBackButton.setTooltip("Step Write backstep");
    stepBackButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    stepBackButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    stepBackButton.addListener(this);
    addAndMakeVisible(stepBackButton);

    stepTieButton.setButtonText("Tie");
    stepTieButton.setTooltip("Step Write tie: extend previous note/chord");
    stepTieButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    stepTieButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    stepTieButton.addListener(this);
    addAndMakeVisible(stepTieButton);

    glideButton.setButtonText("Glide");
    glideButton.setClickingTogglesState(true);
    glideButton.setTooltip("Glide: every note played slides from the previous pitch (mono portamento)");
    glideButton.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    glideButton.setColour(juce::TextButton::buttonOnColourId, theme::accent::activeCoral);
    glideButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    glideButton.setColour(juce::TextButton::textColourOnId, theme::core::voidBlack);
    glideButton.addListener(this);
    addAndMakeVisible(glideButton);

    scaleLockLabel.setText("Scale Lock", juce::dontSendNotification);
    scaleLockLabel.setColour(juce::Label::textColourId, theme::text::secondary.withAlpha(0.82f));
    scaleLockLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(scaleLockLabel);

    scaleLockToggle.setColour(juce::ToggleButton::textColourId, theme::text::primary);
    scaleLockToggle.setToggleState(scaleLockEnabled, juce::dontSendNotification);
    scaleLockToggle.onClick = [this]
    {
        scaleLockEnabled = scaleLockToggle.getToggleState();
        if (onScaleLockChanged)
            onScaleLockChanged(scaleLockEnabled);
        repaint();
    };
    addAndMakeVisible(scaleLockToggle);

    chordToggle.setButtonText("Chord");
    chordToggle.setClickingTogglesState(false);   // click opens a chord-size menu instead of a plain toggle
    chordToggle.setToggleState(chordModeEnabled, juce::dontSendNotification);
    chordToggle.setTooltip("Chord mode: one placed/played note becomes a diatonic chord in the project key. "
                           "Pick the chord size (triad / 7th / 9th / 11th / 13th) or turn it off.");
    chordToggle.setColour(juce::TextButton::buttonColourId, theme::surface::primary);
    chordToggle.setColour(juce::TextButton::buttonOnColourId, theme::accent::activeCoral);
    chordToggle.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    chordToggle.setColour(juce::TextButton::textColourOnId, theme::text::primary);
    chordToggle.setButtonText("Chords");
    chordToggle.onClick = [this]
    {
        if (chordSelector != nullptr && chordSelector->isVisible())
            closeChordSelector();
        else
            openChordSelector();
    };
    addAndMakeVisible(chordToggle);

    chordSelector = std::make_unique<ChordSelectorComponent>();
    chordSelector->onAudition     = [this](const std::vector<int>& p) { auditionChord(p); };
    chordSelector->onClose        = [this] { closeChordSelector(); };
    chordSelector->onDragChordOut = [this](const std::vector<int>& p) { beginChordDrag(p); };
    addChildComponent(*chordSelector);

    focusToggle.setVisible(false);

    closeButton.setButtonText("Back To Arrangement");
    closeButton.setColour(juce::TextButton::buttonColourId, theme::warm::red);
    closeButton.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    closeButton.addListener(this);
    addAndMakeVisible(closeButton);
}

MidiEditorOverlayComponent::~MidiEditorOverlayComponent()
{
    setLookAndFeel(nullptr);
}

void MidiEditorOverlayComponent::openClip(TrackState& trackState, TimelineClip& clipState,
                                          int projectKeyRoot, bool projectKeyIsMinor,
                                          bool initialScaleLock)
{
    activeTrack = &trackState;
    activeClip = &clipState;
    trackName = trackState.name;
    clipName = clipState.name;
    trackColour = trackState.colour;
    clipColour = clipState.colour;
    clearSelection();
    releaseLiveKeyboardPitches();
    releaseMousePreviewPitch();
    releasePlacedNotePreview();
    stopGlobalSpacePreview();
    hoverNote.reset();
    hoveredGridBeat.reset();
    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();
    slideDrawState.reset();
    slideEditState.reset();
    releaseLiveKeyboardPitches();
    undoStack.clear();
    redoStack.clear();
    horizontalZoom = 1.0;
    verticalZoom = 1.0;
    scrollX = 0.0;
    scrollY = 0.0;
    syncZoomScrollTargets();
    pendingMagnifyDelta = 0.0;
    ignoreWheelUntilMs = 0.0;
    // Inherit scale from project key. scalePatterns[0]=Minor, [1]=Major.
    scaleRoot         = ((projectKeyRoot % 12) + 12) % 12;
    scalePatternIndex = projectKeyIsMinor ? 0 : 1;
    scaleLockEnabled  = initialScaleLock;
    scaleLockToggle.setToggleState(scaleLockEnabled, juce::dontSendNotification);
    snapSizeInBeats = 0.25;
    stepWriteCursorBeat = 0.0;
    stepWriteStepLengthInBeats = 0.25;
    stepWriteEnabled = false;
    stepWriteLivePitches.clear();
    stepWriteLiveVoicing.clear();
    stepWritePendingVelocities.clear();
    focusModeEnabled = false;
    slidePenEnabled = false;
    currentTool = PianoRollTool::select;
    slideVisibilityMode = SlideVisibilityMode::ghost;
    hasStoredViewportBeforeFocus = false;
    ignoreNextMouseDown = false;
    focusToggle.setToggleState(false, juce::dontSendNotification);
    slidePenButton.setToggleState(false, juce::dontSendNotification);
    stepWriteButton.setToggleState(false, juce::dontSendNotification);
    slideVisibilityButton.setButtonText(getSlideVisibilityName());

    titleLabel.setText("Piano Roll", juce::dontSendNotification);
    updateSubtitle();
    setVisible(true);
    toFront(true);
    grabKeyboardFocus();

    focusViewportAroundClipNotes();
    clampScrollOffsets();
    repaint();
}

void MidiEditorOverlayComponent::closeEditor()
{
    commitStepWritePendingChord();
    stopGlobalSpacePreview();
    releaseLiveKeyboardPitches();
    releaseMousePreviewPitch();
    releasePlacedNotePreview();
    setVisible(false);
    clearSelection();
    hoverNote.reset();
    hoveredGridBeat.reset();
    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();
    slideDrawState.reset();

    if (onClose)
        onClose();
}

void MidiEditorOverlayComponent::setProjectKey(int rootSemitones, bool minor)
{
    scaleRoot         = ((rootSemitones % 12) + 12) % 12;
    scalePatternIndex = minor ? 0 : 1;
    updateSubtitle();
    repaint();
}

void MidiEditorOverlayComponent::setScaleLockExternally(bool enabled)
{
    scaleLockEnabled = enabled;
    scaleLockToggle.setToggleState(enabled, juce::dontSendNotification);
    repaint();
}

void MidiEditorOverlayComponent::setChordModeExternally(bool enabled, int chordSize)
{
    chordModeEnabled = enabled;
    chordSizeNotes   = juce::jlimit(3, 7, chordSize);
    static const std::array<const char*, 5> label { "Chord 3", "Chord 7", "Chord 9", "Chord 11", "Chord 13" };
    chordToggle.setButtonText(label[static_cast<std::size_t>(chordSizeNotes - 3)]);
    chordToggle.setToggleState(enabled, juce::dontSendNotification);
    repaint();
}

std::vector<int> MidiEditorOverlayComponent::chordPitchesForNote(int basePitch) const
{
    if (! chordModeEnabled)
        return { basePitch };

    const auto& pattern = scalePatterns[static_cast<std::size_t>(scalePatternIndex)].pitchClasses;
    const int root = ((scaleRoot % 12) + 12) % 12;
    const int snapped = snapPitchToScale(basePitch);
    const int pc = (((snapped - root) % 12) + 12) % 12;

    int degree = 0;
    for (int i = 0; i < 7; ++i)
        if (pattern[static_cast<std::size_t>(i)] == pc) { degree = i; break; }
    const int octave = (snapped - root - pattern[static_cast<std::size_t>(degree)]) / 12;

    const int size = juce::jlimit(3, 7, chordSizeNotes);
    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(size));
    for (int k = 0; k < size; ++k)
    {
        const int d = degree + 2 * k;
        const int o = octave + d / 7;
        out.push_back(juce::jlimit(0, 127, root + 12 * o + pattern[static_cast<std::size_t>(d % 7)]));
    }
    return out;
}

std::set<int> MidiEditorOverlayComponent::chordPitchSetForNote(int basePitch) const
{
    const auto pitches = chordPitchesForNote(basePitch);
    return { pitches.begin(), pitches.end() };
}

void MidiEditorOverlayComponent::timerCallback()
{
    if (placedNotePreviewPitch.has_value()
        && juce::Time::getMillisecondCounterHiRes() >= placedNotePreviewOffMs)
    {
        releasePlacedNotePreview();
    }

    if (! chordPreviewPitches.empty()
        && juce::Time::getMillisecondCounterHiRes() >= chordPreviewOffMs)
    {
        releaseChordPreview();
    }

    // Hold-Command preview, polled here so it works even without keyboard focus (just
    // hovering the editor). Cmd down arms a debounced start; Cmd up ends it; a Cmd+key
    // shortcut cancels it via keyPressed.
    {
        const bool cmdDown = isShowing() && juce::ModifierKeys::getCurrentModifiers().isCommandDown();
        if (cmdDown && ! cmdWasDown)
        {
            cmdDownMs = juce::Time::getMillisecondCounter();
            cmdPreviewPending = true;
        }
        else if (! cmdDown)
        {
            cmdPreviewPending = false;
            if (globalSpacePreviewActive)
                stopGlobalSpacePreview();
        }
        cmdWasDown = cmdDown;

        if (cmdPreviewPending && ! globalSpacePreviewActive && cmdDown
            && juce::Time::getMillisecondCounter() - cmdDownMs >= 180)
        {
            cmdPreviewPending = false;
            startGlobalSpacePreviewHeld();
        }
    }

    if (! isVisible())
        return;

    // FL-style smooth glide: ease zoom + scroll toward their targets each tick.
    bool moved = false;      // needs a full repaint (zoom or vertical scroll → the keyboard changes too)
    bool gridOnly = false;   // horizontal pan only → the left keyboard is unchanged, skip repainting it
    if (zoomAnimating)
    {
        bool done = true;
        const double dh = targetHorizontalZoom - horizontalZoom;
        if (std::abs(dh) > horizontalZoom * 0.0025) { horizontalZoom += dh * 0.35; done = false; }
        else horizontalZoom = targetHorizontalZoom;
        const double dv = targetVerticalZoom - verticalZoom;
        if (std::abs(dv) > verticalZoom * 0.0025) { verticalZoom += dv * 0.35; done = false; }
        else verticalZoom = targetVerticalZoom;
        // Re-pin the focus point under the cursor while the zoom eases.
        const double ppb = getPixelsPerBeat();
        const double lane = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
        scrollX = zoomFocusBeat * ppb - zoomFocusXInView;
        scrollY = zoomFocusLane * lane - zoomFocusYInView;
        clampScrollOffsets();
        targetScrollX = scrollX;   // the zoom anchor owns scroll while animating
        targetScrollY = scrollY;
        if (done)
            zoomAnimating = false;
        updateSubtitle();
        moved = true;
    }
    else
    {
        const double dx = targetScrollX - scrollX;
        const double dy = targetScrollY - scrollY;
        const bool hMove = std::abs(dx) > 0.5;
        const bool vMove = std::abs(dy) > 0.5;
        if (hMove || vMove)
        {
            scrollX += dx * 0.35;
            scrollY += dy * 0.35;
            clampScrollOffsets();
            if (vMove) moved = true;   // vertical scroll moves the keyboard rows → full repaint
            else       gridOnly = true;
        }
        else if (dx != 0.0 || dy != 0.0)
        {
            const bool onlyH = std::abs(dy) < 0.001;
            scrollX = targetScrollX;
            scrollY = targetScrollY;
            if (onlyH) gridOnly = true;
            else       moved = true;
        }
    }

    // Only repaint the whole editor when the viewport itself moves. While playing, redraw the
    // playhead strip and rows whose active-note highlight changed instead of the full piano roll.
    const bool playing = onRequestPlayingState && onRequestPlayingState();
    const bool previewsActive = placedNotePreviewPitch.has_value() || globalSpacePreviewActive
                             || cmdPreviewPending || cmdWasDown
                             || ! liveKeyboardPitches.empty() || ! stepWriteLivePitches.empty()
                             || mousePreviewPitch.has_value();
    if (moved)
    {
        repaint();
    }
    else if (playing)
    {
        std::set<int> activePitches;
        const auto playheadBeat = onRequestPlayheadBeat ? onRequestPlayheadBeat() : 0.0;
        const auto clipStart = activeClip != nullptr ? activeClip->startBeat : 0.0;
        const auto clipLength = activeClip != nullptr ? activeClip->lengthInBeats : 0.0;
        const auto localBeat = playheadBeat - clipStart;
        const bool insideClip = activeClip != nullptr && localBeat >= 0.0 && localBeat <= clipLength + 0.0001;
        if (insideClip)
            for (const auto& note : activeClip->midiNotes)
                if (localBeat >= note.startBeat && localBeat < note.startBeat + juce::jmax(0.01, note.lengthInBeats))
                    activePitches.insert(note.pitch);
        activePitches.insert(liveKeyboardPitches.begin(), liveKeyboardPitches.end());
        activePitches.insert(stepWriteLivePitches.begin(), stepWriteLivePitches.end());
        if (mousePreviewPitch.has_value())
        {
            const auto chord = chordPitchSetForNote(*mousePreviewPitch);
            activePitches.insert(chord.begin(), chord.end());
        }

        const auto visibleGrid = getVisibleGridViewport();
        const auto gridArea = getGridBounds();
        const auto playheadX = juce::roundToInt(gridArea.getX() + localBeat * getPixelsPerBeat() - scrollX);
        const auto previousX = lastTimerPlayheadX.value_or(playheadX);
        if (insideClip || lastTimerPlayheadX.has_value())
        {
            const auto left = juce::jmin(previousX, playheadX) - 10;
            const auto right = juce::jmax(previousX, playheadX) + 11;
            repaint(juce::Rectangle<int>(left, visibleGrid.getY(), right - left, visibleGrid.getHeight())
                        .getIntersection(getLocalBounds()));
        }

        if (activePitches != lastTimerActivePlaybackPitches)
        {
            // Piano-key shapes can overlap neighbouring lanes, so repaint that compact column once.
            repaint(getKeyboardBounds());
            std::set<int> changedPitches = activePitches;
            changedPitches.insert(lastTimerActivePlaybackPitches.begin(), lastTimerActivePlaybackPitches.end());
            const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
            for (const auto pitch : changedPitches)
            {
                const auto y = gridArea.getY() + juce::roundToInt(pitchToLane(pitch) * laneHeight);
                repaint(juce::Rectangle<int>(visibleGrid.getX(), y - 2, visibleGrid.getWidth(),
                                            juce::roundToInt(laneHeight) + 4)
                            .getIntersection(visibleGrid));
            }
        }

        lastTimerPlayheadX = insideClip ? std::optional<int>(playheadX) : std::nullopt;
        lastTimerActivePlaybackPitches = std::move(activePitches);
    }
    else if (previewsActive)
    {
        repaint();
    }
    else if (lastTimerPlayheadX.has_value() || ! lastTimerActivePlaybackPitches.empty())
    {
        // One final full frame clears the old playhead and any held-key highlights after Stop.
        lastTimerPlayheadX.reset();
        lastTimerActivePlaybackPitches.clear();
        repaint();
    }
    else if (gridOnly)
    {
        // Horizontal pan: only the grid + velocity lane (right of the keyboard) changed. Repainting just
        // that strip lets the keyboard draws get clipped-rejected instead of rasterised every frame.
        const auto kb = getKeyboardBounds();
        const auto grid = getGridBounds();
        repaint(kb.getRight(), grid.getY(), getWidth() - kb.getRight(), getHeight() - grid.getY());
    }
}

void MidiEditorOverlayComponent::syncZoomScrollTargets()
{
    targetHorizontalZoom = horizontalZoom;
    targetVerticalZoom = verticalZoom;
    targetScrollX = scrollX;
    targetScrollY = scrollY;
    zoomAnimating = false;
}

void MidiEditorOverlayComponent::paint(juce::Graphics& g)
{
    g.fillAll(overlayBackground);

    const auto topBar = getTopBarBounds();
    const auto keyboardArea = getKeyboardBounds();
    const auto visibleGrid = getVisibleGridViewport();
    const auto dirtyGrid = visibleGrid.getIntersection(g.getClipBounds());
    const auto gridArea = getGridBounds();
    const auto velocityLane = getVelocityLaneBounds();

    // Flat dark header (no red gradient) so the toolbar matches the main screen's colour scheme.
    g.setColour(theme::core::studio);
    g.fillRoundedRectangle(topBar.toFloat(), theme::metrics::panelRadius);
    g.setColour(panelBackground);
    g.fillRoundedRectangle(keyboardArea.toFloat(), 18.0f);
    g.fillRoundedRectangle(visibleGrid.toFloat(), 18.0f);
    g.setColour(velocityLaneBackground);
    g.fillRoundedRectangle(velocityLane.toFloat(), 16.0f);

    g.setColour(panelStroke.withAlpha(0.92f));
    g.drawRoundedRectangle(topBar.toFloat(), theme::metrics::panelRadius, 1.0f);
    g.drawRoundedRectangle(keyboardArea.toFloat(), 18.0f, 1.0f);
    g.drawRoundedRectangle(visibleGrid.toFloat(), 18.0f, 1.0f);
    g.drawRoundedRectangle(velocityLane.toFloat(), 16.0f, 1.0f);

    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto visibleBeatRange = getVisibleBeatRange();
    const auto pixelsPerBeat = getPixelsPerBeat();
    const auto steps = juce::jmax(16, static_cast<int>(std::ceil(visibleBeatRange / snapSizeInBeats)));
    const auto displayedLaneCount = getDisplayedLaneCount();

    const auto firstVisibleLane = juce::jmax(0, static_cast<int>(std::floor((visibleGrid.getY() - gridArea.getY()) / laneHeight)));
    const auto lastVisibleLane = juce::jmin(displayedLaneCount - 1, static_cast<int>(std::ceil((visibleGrid.getBottom() - gridArea.getY()) / laneHeight)));

    const auto livePlayheadBeat = onRequestPlayheadBeat ? onRequestPlayheadBeat() : 0.0;
    const auto livePlayingState = onRequestPlayingState ? onRequestPlayingState() : false;
    const auto clipStart  = activeClip != nullptr ? activeClip->startBeat     : 0.0;
    const auto clipLength = activeClip != nullptr ? activeClip->lengthInBeats : 8.0;
    const auto localBeat  = livePlayheadBeat - clipStart;
    const bool playheadInsideClip = localBeat >= 0.0 && localBeat <= clipLength + 0.0001;

    std::set<int> activePlaybackPitches;
    if (activeClip != nullptr && livePlayingState && playheadInsideClip)
    {
        for (const auto& note : activeClip->midiNotes)
        {
            const auto noteEnd = note.startBeat + juce::jmax(0.01, note.lengthInBeats);
            if (localBeat >= note.startBeat && localBeat < noteEnd)
                activePlaybackPitches.insert(note.pitch);
        }
    }
    activePlaybackPitches.insert(liveKeyboardPitches.begin(), liveKeyboardPitches.end());
    activePlaybackPitches.insert(stepWriteLivePitches.begin(), stepWriteLivePitches.end());
    if (mousePreviewPitch.has_value())
    {
        const auto pitches = chordPitchSetForNote(*mousePreviewPitch);
        activePlaybackPitches.insert(pitches.begin(), pitches.end());
    }

    // === Real piano keyboard: ONE continuous white bed + black-key overlay ======
    const auto kbF       = keyboardArea.toFloat().reduced(1.0f);
    const float blackW   = kbF.getWidth() * 0.72f;                 // black-key length
    const auto laneTopY  = [&](int lane) { return static_cast<float>(gridArea.getY() + lane * laneHeight); };
    // Keyboard highlight uses the physical key shape. The grid highlight below stays
    // lane-sized, so note rows remain precise while the piano keys still feel real.
    const auto physicalLaneBounds = [&](int lane, float x, float width)
    {
        const auto pitch = laneIndexToPitch(lane);
        const auto top = laneTopY(lane);
        const auto h = static_cast<float>(laneHeight);
        const float yTop = (! isBlackKey(pitch) && isBlackKey(pitch + 1)) ? top - h * 0.5f : top;
        const float yBottom = (! isBlackKey(pitch) && isBlackKey(pitch - 1)) ? top + h * 1.5f : top + h;
        return juce::Rectangle<float>(x, yTop, width, yBottom - yTop);
    };

    // Physical keyboard: white key bed + black key overlay. The grid rows below
    // use matching vertical extents, so the pretty piano shape does not lie alone.
    {
        // Solid bed instead of two full-height gradients per frame (cheaper software render).
        g.setColour(juce::Colour(0xfff2f4f6));
        g.fillRoundedRectangle(kbF, 17.0f);
    }

    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        if (isBlackKey(pitch))
            continue;

        const auto top = laneTopY(lane);
        const auto h = static_cast<float>(laneHeight);
        const auto y = top + (isBlackKey(pitch - 1) ? h * 1.5f : h);
        if (y < kbF.getY() + 2.0f || y > kbF.getBottom() - 2.0f)
            continue;

        g.setColour(theme::core::canvas.withAlpha(0.54f));
        g.drawLine(kbF.getX(), y, kbF.getRight(), y, 1.35f);
        g.setColour(theme::text::primary.withAlpha(0.32f));
        g.drawLine(kbF.getX() + 1.0f, y + 1.0f, kbF.getRight() - 1.0f, y + 1.0f, 1.0f);
    }

    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        if (isBlackKey(pitch) || ! activePlaybackPitches.contains(pitch))
            continue;

        auto keyRect = physicalLaneBounds(lane, kbF.getX(), kbF.getWidth()).getIntersection(kbF);
        if (keyRect.isEmpty())
            continue;

        const auto mousePressed = mousePreviewPitch.has_value() && *mousePreviewPitch == pitch;
        const auto keyAccent = midiAccentForTrack(clipColour, true);
        const auto shadow = keyRect.translated(mousePressed ? 2.4f : 1.4f, mousePressed ? 1.2f : 0.6f)
                                   .withTrimmedRight(mousePressed ? 2.4f : 1.2f)
                                   .reduced(1.0f, 1.0f);

        g.setColour(theme::core::canvas.withAlpha(mousePressed ? 0.34f : 0.25f));
        g.fillRoundedRectangle(shadow, 5.0f);

        g.saveState();
        juce::Path keyboardClip;
        keyboardClip.addRoundedRectangle(kbF, 17.0f);
        g.reduceClipRegion(keyboardClip);

        juce::ColourGradient press(keyAccent.brighter(0.28f).withAlpha(mousePressed ? 0.94f : 0.80f),
                                   keyRect.getX(), keyRect.getY(),
                                   keyAccent.darker(0.28f).withAlpha(mousePressed ? 0.96f : 0.82f),
                                   keyRect.getRight(), keyRect.getY(),
                                   false);
        press.addColour(0.55, keyAccent.withAlpha(mousePressed ? 0.82f : 0.68f));
        g.setGradientFill(press);
        g.fillRect(keyRect);

        const auto insetArea = keyRect.reduced(mousePressed ? 2.0f : 1.2f, mousePressed ? 1.4f : 0.9f);
        juce::ColourGradient inset(juce::Colours::black.withAlpha(mousePressed ? 0.32f : 0.22f),
                                   insetArea.getX(), insetArea.getY(),
                                   juce::Colours::transparentBlack,
                                   insetArea.getX(), insetArea.getBottom(),
                                   false);
        g.setGradientFill(inset);
        g.fillRect(insetArea);

        g.setColour(theme::text::primary.withAlpha(mousePressed ? 0.30f : 0.22f));
        g.drawLine(keyRect.getX() + 4.0f, keyRect.getBottom() - 2.0f,
                   keyRect.getRight() - 4.0f, keyRect.getBottom() - 2.0f, 1.2f);
        g.restoreState();
    }

    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        if (! isBlackKey(pitch))
            continue;

        const auto top = laneTopY(lane);
        const auto h = static_cast<float>(laneHeight);
        juce::Rectangle<float> bk(kbF.getX() - 2.0f, top + 1.5f, blackW + 2.0f, h - 3.0f);
        if (bk.getBottom() < kbF.getY() || bk.getY() > kbF.getBottom())
            continue;

        const bool isActive = activePlaybackPitches.contains(pitch);
        const bool mousePressed = mousePreviewPitch.has_value() && *mousePreviewPitch == pitch;
        const auto keyAccent = midiAccentForTrack(clipColour, true);
        if (isActive)
            bk = bk.translated(mousePressed ? 2.8f : 1.6f, mousePressed ? 1.4f : 0.8f)
                   .withTrimmedRight(mousePressed ? 2.8f : 1.4f)
                   .reduced(0.0f, mousePressed ? 0.5f : 0.2f);

        const float r = juce::jmin(7.0f, bk.getHeight() * 0.5f);
        // Cheap solid fills instead of per-key paths + gradient + stroke (that was the paint bottleneck
        // — this block runs for every visible black key on every repaint).
        g.setColour(theme::core::canvas.withAlpha(isActive ? 0.22f : 0.42f));
        g.fillRoundedRectangle(bk.translated(isActive ? 0.7f : 1.5f, isActive ? 1.0f : 2.5f), r);
        g.setColour(isActive ? keyAccent.darker(0.10f) : theme::core::voidBlack);
        g.fillRoundedRectangle(bk, r);
        g.setColour(theme::core::canvas.withAlpha(0.5f));
        g.drawRoundedRectangle(bk, r, 1.0f);
    }

    // Note labels: quiet text only, no row separators. Keep the keyboard shape dominant.
    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        auto keyRow = keyboardArea.withY(static_cast<int>(std::round(laneTopY(lane))))
                                  .withHeight(static_cast<int>(std::ceil(laneHeight)));
        if (keyRow.getBottom() < keyboardArea.getY() || keyRow.getY() > keyboardArea.getBottom())
            continue;

        keyRow = keyRow.getIntersection(keyboardArea);
        const auto black = isBlackKey(pitch);
        const auto active = activePlaybackPitches.contains(pitch);
        const auto pitchClass = ((pitch % 12) + 12) % 12;
        const auto root = pitchClass == scaleRoot;
        g.setColour(active ? theme::text::primary.withAlpha(0.96f)
                           : (black ? theme::text::secondary.withAlpha(0.62f)
                                    : theme::text::inverse.withAlpha(0.50f)));
        g.setFont(juce::FontOptions(active ? 10.5f : 9.0f, (active || root) ? juce::Font::bold : juce::Font::plain));
        g.drawText(noteNameForPitch(pitch), keyRow.reduced(9, 0), juce::Justification::centredLeft);
    }

    g.saveState();
    g.reduceClipRegion(visibleGrid);

    for (int lane = firstVisibleLane; lane <= lastVisibleLane + 1; ++lane)
    {
        const auto y = static_cast<float>(gridArea.getY() + lane * laneHeight);
        g.setColour(theme::line::subtle.withAlpha(0.16f));
        g.drawLine(static_cast<float>(visibleGrid.getX()), y, static_cast<float>(visibleGrid.getRight()), y, 1.0f);
    }

    g.setColour(pianoGridBase);
    g.fillRect(visibleGrid);

    // Uniform lane rect helper — must match the click→pitch mapping (yToPitch) and
    // the note rectangles, so everything lines up on the grid.
    const auto laneRowRect = [&](int lane)
    {
        return juce::Rectangle<float>(
                   static_cast<float>(visibleGrid.getX()),
                   static_cast<float>(gridArea.getY() + lane * laneHeight),
                   static_cast<float>(visibleGrid.getWidth()),
                   static_cast<float>(laneHeight))
                   .getIntersection(visibleGrid.toFloat());
    };

    // 1) Black-key rows first (darker base).
    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        if (! isBlackKey(pitch))
            continue;
        const auto row = laneRowRect(lane);
        if (row.isEmpty())
            continue;
        g.setColour(pianoGridBlackRow);
        g.fillRect(row);
    }

    // 2) In-scale rows ON TOP — so scale notes that fall on BLACK keys (e.g. D#, G#,
    //    A# in C minor) are highlighted too, not hidden under the black-row fill.
    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        if (! isPitchInScale(pitch))
            continue;
        const auto row = laneRowRect(lane);
        if (row.isEmpty())
            continue;
        g.setColour(pianoGridScaleRow);
        g.fillRect(row);
    }

    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        auto row = juce::Rectangle<float>(
                       static_cast<float>(visibleGrid.getX()),
                       static_cast<float>(gridArea.getY() + lane * laneHeight),
                       static_cast<float>(visibleGrid.getWidth()),
                       static_cast<float>(laneHeight))
                       .getIntersection(visibleGrid.toFloat());
        if (row.isEmpty())
            continue;

        if (activePlaybackPitches.contains(pitch))
        {
            auto laneGlow = row.reduced(0.0f, 1.0f);
            const auto keyAccent = midiAccentForTrack(clipColour, true);
            juce::ColourGradient glow(keyAccent.brighter(0.22f).withAlpha(0.30f),
                                      laneGlow.getX(), laneGlow.getCentreY(),
                                      keyAccent.darker(0.42f).withAlpha(0.015f),
                                      laneGlow.getRight(), laneGlow.getCentreY(),
                                      false);
            glow.addColour(0.18, keyAccent.withAlpha(0.22f));
            glow.addColour(0.48, keyAccent.withAlpha(0.085f));
            g.setGradientFill(glow);
            g.fillRect(laneGlow);

            juce::ColourGradient edge(keyAccent.brighter(0.26f).withAlpha(0.42f),
                                      laneGlow.getX(), laneGlow.getY(),
                                      keyAccent.withAlpha(0.02f),
                                      laneGlow.getRight(), laneGlow.getY(),
                                      false);
            g.setGradientFill(edge);
            g.fillRect(laneGlow.withHeight(1.4f));
            g.fillRect(laneGlow.withY(laneGlow.getBottom() - 1.4f).withHeight(1.4f));
        }
    }

    // Draw horizontal pitch-cell boundaries after row fills so each note cell reads
    // clearly without making the piano roll feel like a bright spreadsheet.
    for (int lane = firstVisibleLane; lane <= lastVisibleLane + 1; ++lane)
    {
        const auto y = static_cast<float>(gridArea.getY() + lane * laneHeight);
        if (y < static_cast<float>(visibleGrid.getY() - 1) || y > static_cast<float>(visibleGrid.getBottom() + 1))
            continue;

        const auto pitchBelow = laneIndexToPitch(juce::jlimit(0, displayedLaneCount - 1, lane));
        const auto octaveBoundary = (pitchBelow % 12) == 0;
        g.setColour(octaveBoundary ? theme::line::strong.withAlpha(0.34f)
                                   : theme::line::normal.withAlpha(0.24f));
        g.drawLine(static_cast<float>(visibleGrid.getX()), y,
                   static_cast<float>(visibleGrid.getRight()), y,
                   octaveBoundary ? 1.2f : 1.0f);
    }

    const auto stepsPerBeat = juce::jmax(1, static_cast<int>(std::round(1.0 / juce::jmax(0.001, snapSizeInBeats))));
    const auto beatsPerBar  = 4; // visual bar — independent of project's time-sig numerator for now
    const auto stepsPerBar  = stepsPerBeat * beatsPerBar;

    // Adaptive density (Ableton/FL-style): don't draw a grid level once its lines get denser than a few
    // pixels — that "picket fence" of sub-pixel lines is what tanked the frame rate when zoomed out.
    const double subPx  = snapSizeInBeats * pixelsPerBeat;
    const double beatPx = pixelsPerBeat;
    const bool drawSub  = subPx  >= 6.0;    // 1/16, 1/32… subdivisions
    const bool drawBeat = beatPx >= 6.0;    // beat lines

    // Iterate only the steps within (a little past) the visible window instead of the whole clip.
    const double leftBeat  = (static_cast<double>(visibleGrid.getX() - 8) - static_cast<double>(gridArea.getX()) + scrollX) / juce::jmax(1.0e-6, pixelsPerBeat);
    const double rightBeat = (static_cast<double>(visibleGrid.getRight() + 8) - static_cast<double>(gridArea.getX()) + scrollX) / juce::jmax(1.0e-6, pixelsPerBeat);
    const int firstStep = juce::jmax(0, static_cast<int>(std::floor(leftBeat / juce::jmax(0.001, snapSizeInBeats))));
    const int lastStep  = juce::jmin(steps, static_cast<int>(std::ceil(rightBeat / juce::jmax(0.001, snapSizeInBeats))));

    // FL-style: faintly shade alternate beat cells so the grid reads clearly for placing notes.
    if (drawBeat)
    {
        for (int b = juce::jmax(0, static_cast<int>(std::floor(leftBeat))); b <= static_cast<int>(std::ceil(rightBeat)); ++b)
        {
            if ((b & 1) == 0)
                continue;
            const float bx = static_cast<float>(gridArea.getX() + (static_cast<double>(b) * pixelsPerBeat) - scrollX);
            const float bx2 = bx + static_cast<float>(pixelsPerBeat);
            const float x0 = juce::jmax(bx, static_cast<float>(visibleGrid.getX()));
            const float x1 = juce::jmin(bx2, static_cast<float>(visibleGrid.getRight()));
            if (x1 <= x0)
                continue;
            g.setColour(juce::Colours::white.withAlpha(0.025f));
            g.fillRect(juce::Rectangle<float>(x0, static_cast<float>(visibleGrid.getY()), x1 - x0, static_cast<float>(visibleGrid.getHeight())));
        }
    }

    for (int step = firstStep; step <= lastStep; ++step)
    {
        const bool isBarLine  = (step % stepsPerBar)  == 0;
        const bool isBeatLine = (step % stepsPerBeat) == 0;

        // Skip levels that are too dense to read (keeps only the meaningful lines when zoomed out).
        if (! isBarLine && (isBeatLine ? ! drawBeat : ! drawSub))
            continue;

        const auto beat = static_cast<double>(step) * snapSizeInBeats;
        const auto x = static_cast<float>(gridArea.getX() + (beat * pixelsPerBeat) - scrollX);
        if (x < static_cast<float>(visibleGrid.getX() - 8) || x > static_cast<float>(visibleGrid.getRight() + 8))
            continue;

        // Clear hierarchy so bar boundaries (every 4 beats) pop: bars bright, beats medium, subdivisions faint.
        if (isBarLine)
            g.setColour(juce::Colours::white.withAlpha(0.6f));
        else if (isBeatLine)
            g.setColour(juce::Colours::white.withAlpha(0.26f));
        else
            g.setColour(juce::Colours::white.withAlpha(0.10f));   // snap subdivisions (1/16, 1/32…)

        g.drawLine(x, static_cast<float>(visibleGrid.getY()), x, static_cast<float>(visibleGrid.getBottom()),
                   isBarLine ? 2.2f : (isBeatLine ? 1.3f : 1.0f));
    }

    // Bar numbers along the top of the grid.
    {
        const auto rulerHeight = 16.0f;
        g.setColour(theme::core::canvas.withAlpha(0.42f));
        g.fillRect(juce::Rectangle<float>(static_cast<float>(visibleGrid.getX()),
                                          static_cast<float>(visibleGrid.getY()),
                                          static_cast<float>(visibleGrid.getWidth()),
                                          rulerHeight));
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.setColour(theme::text::secondary.withAlpha(0.86f));
        for (int step = 0; step <= steps; step += stepsPerBar)
        {
            const auto beat = static_cast<double>(step) * snapSizeInBeats;
            const auto x = static_cast<float>(gridArea.getX() + (beat * pixelsPerBeat) - scrollX);
            if (x < static_cast<float>(visibleGrid.getX() - 30) || x > static_cast<float>(visibleGrid.getRight() + 4))
                continue;
            const auto barNumber = step / stepsPerBar + 1; // 1-based for display
            g.drawText(juce::String(barNumber),
                       static_cast<int>(x + 3),
                       static_cast<int>(visibleGrid.getY() + 1),
                       40, static_cast<int>(rulerHeight) - 2,
                       juce::Justification::topLeft);
        }
    }

    // No special clip-end line — all grid lines stay uniform (like FL). The pattern length is still
    // conveyed by the faint dim past the clip end above.

    if (activeClip != nullptr)
    {
        for (int noteIndex = 0; noteIndex < static_cast<int>(activeClip->midiNotes.size()); ++noteIndex)
        {
            const auto& note = activeClip->midiNotes[static_cast<std::size_t>(noteIndex)];
            if (focusModeEnabled && ! isPitchInScale(note.pitch))
                continue;
            const auto noteBounds = getNoteBounds(note);
            // The grid is already clipped; skip notes outside it before issuing any draw calls.
            // Large MIDI clips otherwise redraw every off-screen note on each playback frame.
            if (! noteBounds.intersects(dirtyGrid))
                continue;
            const auto isSelected = isNoteSelected(noteIndex);

            const auto noteColour = midiAccentForTrack(clipColour, isSelected);
            g.setColour(noteColour);
            g.fillRoundedRectangle(noteBounds.toFloat(), 6.0f);
            g.setColour(noteColour.darker(0.45f).withAlpha(0.68f));
            g.drawRoundedRectangle(noteBounds.toFloat().reduced(0.5f), 5.5f, 1.0f);

            if (isSelected)
            {
                g.setColour(theme::text::primary.withAlpha(0.80f));
                g.drawRoundedRectangle(noteBounds.toFloat(), 6.0f, 1.4f);
            }
        }

        auto drawSlide = [this, &g, gridArea, pixelsPerBeat](const PitchSlide& slide, bool selected, bool drawingPreview)
        {
            if (slide.points.size() < 2)
                return;

            const auto slideX = [&](double beat) { return static_cast<float>(gridArea.getX() + (beat * pixelsPerBeat) - scrollX); };

            juce::Path path;
            path.startNewSubPath(slideX(slide.points.front().beat), pitchToCentreY(slide.points.front().pitch));
            for (std::size_t i = 1; i < slide.points.size(); ++i)
            {
                const auto& a = slide.points[i - 1];
                const auto& b = slide.points[i];
                // Sample the segment so curve + LFO wiggle are visible (denser when modulated).
                const int steps = a.lfoShape != 0 ? 64 : 18;
                for (int s = 1; s <= steps; ++s)
                {
                    const auto t = static_cast<double>(s) / steps;
                    const auto beat = a.beat + (b.beat - a.beat) * t;
                    const auto pitch = pitchSlideSegmentPitch(a, b, beat - a.beat, t);
                    path.lineTo(slideX(beat), pitchToCentreY(pitch));
                }
            }

            const auto ghosted = ! selected && ! drawingPreview;
            const auto bodyAlpha = ghosted ? 0.34f : 0.92f;
            const auto shadowAlpha = ghosted ? 0.16f : 0.42f;
            const auto highlightAlpha = ghosted ? 0.16f : 0.54f;

            g.setColour(theme::core::canvas.withAlpha(shadowAlpha));
            g.strokePath(path, juce::PathStrokeType(selected ? 8.0f : 6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour((selected ? theme::cool::aqua : theme::warm::red).withAlpha(bodyAlpha));
            g.strokePath(path, juce::PathStrokeType(selected ? 4.0f : 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(theme::text::primary.withAlpha(selected ? 0.72f : highlightAlpha));
            g.strokePath(path, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (selected)
            {
                const auto drawHandle = [&](const PitchSlidePoint& point)
                {
                    const auto x = static_cast<float>(gridArea.getX() + (point.beat * pixelsPerBeat) - scrollX);
                    const auto y = pitchToCentreY(point.pitch);
                    juce::Rectangle<float> handle(x - slideHandleRadiusPx, y - slideHandleRadiusPx,
                                                  slideHandleRadiusPx * 2.0f, slideHandleRadiusPx * 2.0f);
                    g.setColour(theme::core::canvas.withAlpha(0.45f));
                    g.fillEllipse(handle.translated(1.0f, 1.0f));
                    g.setColour(theme::cool::aqua);
                    g.fillEllipse(handle);
                    g.setColour(theme::text::primary.withAlpha(0.82f));
                    g.drawEllipse(handle.reduced(1.0f), 1.2f);
                };

                drawHandle(slide.points.front());
                drawHandle(slide.points.back());

                // Curve handles: a small grip at each segment's midpoint — drag vertically
                // to bend that glide segment (bezier-like).
                for (std::size_t i = 1; i < slide.points.size(); ++i)
                {
                    const auto& a = slide.points[i - 1];
                    const auto& b = slide.points[i];
                    if (std::abs(b.beat - a.beat) < 1.0e-4)
                        continue;
                    const auto midBeat  = (a.beat + b.beat) * 0.5;
                    const auto midPitch = a.pitch + (b.pitch - a.pitch) * pitchSlideCurveShape(0.5, a.curve);
                    const auto x = slideX(midBeat);
                    const auto y = pitchToCentreY(midPitch);
                    const auto r = slideHandleRadiusPx * 0.7f;
                    g.setColour(theme::core::canvas.withAlpha(0.5f));
                    g.fillEllipse(x - r, y - r, r * 2.0f, r * 2.0f);
                    g.setColour(juce::Colour(0xfffbbf24).withAlpha(0.95f));  // amber grip
                    g.fillEllipse(x - r + 1.0f, y - r + 1.0f, (r - 1.0f) * 2.0f, (r - 1.0f) * 2.0f);
                }
            }
        };

        for (int slideIndex = 0; slideIndex < static_cast<int>(activeClip->pitchSlides.size()); ++slideIndex)
        {
            const auto& slide = activeClip->pitchSlides[static_cast<std::size_t>(slideIndex)];
            if (shouldDrawSlide(slide, slideIndex, false))
                drawSlide(slide, selectedSlide == slideIndex || slideTouchesSelectedNotes(slide), false);
        }

        if (slideDrawState.has_value())
            drawSlide(slideDrawState->slide, true, true);
    }

    // Playhead in clip-local coordinates: project playhead minus clip start.
    // Only draw when the project playhead is INSIDE this clip's range — otherwise
    // the piano-roll cursor used to mirror the project loop wrap-around and slide
    // silently back to bar 1 as soon as the clip ended.
    if (playheadInsideClip)
    {
        const auto playheadX = static_cast<float>(gridArea.getX())
            + static_cast<float>((localBeat * pixelsPerBeat) - scrollX);
        const auto lineColour = globalSpacePreviewActive ? theme::cool::aqua : playheadColour;

        juce::ColourGradient gradient(lineColour.withAlpha(0.0f), playheadX - 8.0f, 0.0f,
                                      lineColour.withAlpha(0.0f), playheadX + 8.0f, 0.0f, false);
        gradient.addColour(0.5, lineColour.withAlpha(globalSpacePreviewActive ? 0.42f : (livePlayingState ? 0.35f : 0.15f)));
        g.setGradientFill(gradient);
        g.fillRect(playheadX - 8.0f, static_cast<float>(visibleGrid.getY()), 16.0f, static_cast<float>(visibleGrid.getHeight()));

        g.setColour(lineColour.withAlpha(globalSpacePreviewActive ? 0.98f : (livePlayingState ? 0.95f : 0.75f)));
        g.drawLine(playheadX,
                   static_cast<float>(visibleGrid.getY()),
                   playheadX,
                   static_cast<float>(visibleGrid.getBottom()),
                   globalSpacePreviewActive ? 2.4f : 2.0f);
    }

    if (stepWriteEnabled && activeClip != nullptr)
    {
        const auto cursorBeat = getSnappedStepWriteCursorBeat();
        const auto cursorX = static_cast<float>(gridArea.getX())
            + static_cast<float>((cursorBeat * pixelsPerBeat) - scrollX);

        if (cursorX >= static_cast<float>(visibleGrid.getX() - 4)
            && cursorX <= static_cast<float>(visibleGrid.getRight() + 4))
        {
            g.setColour(theme::cool::aqua.withAlpha(0.18f));
            const auto stepWidth = static_cast<float>(stepWriteStepLengthInBeats * pixelsPerBeat);
            g.fillRect(juce::Rectangle<float>(cursorX,
                                              static_cast<float>(visibleGrid.getY()),
                                              juce::jmax(2.0f, stepWidth),
                                              static_cast<float>(visibleGrid.getHeight())));
            g.setColour(theme::cool::aqua.withAlpha(0.96f));
            g.drawLine(cursorX,
                       static_cast<float>(visibleGrid.getY()),
                       cursorX,
                       static_cast<float>(visibleGrid.getBottom()),
                       2.0f);
        }
    }

    // Cut tool: show a vertical guide at the exact split point over the hovered note (FL-style),
    // so it's clear WHERE the note will be sliced. Cyan contrasts against the orange notes.
    if (currentTool == PianoRollTool::cut && hoverNote.has_value())
    {
        const auto mouse = getMouseXYRelative();
        if (visibleGrid.contains(mouse))
        {
            const auto cutBeat = snapBeatNearest(xToBeat(static_cast<double>(mouse.x)));
            const auto cutX = static_cast<float>(gridArea.getX())
                + static_cast<float>((cutBeat * pixelsPerBeat) - scrollX);
            const auto nb = hoverNote->bounds;
            const bool valid = cutX > static_cast<float>(nb.getX()) + 1.5f
                            && cutX < static_cast<float>(nb.getRight()) - 1.5f;
            const auto col = valid ? theme::cool::cyan : juce::Colours::white;

            g.setColour(col.withAlpha(0.20f));   // faint full-height guide
            g.drawLine(cutX, static_cast<float>(visibleGrid.getY()),
                       cutX, static_cast<float>(visibleGrid.getBottom()), 1.0f);
            g.setColour(col.withAlpha(valid ? 0.98f : 0.45f));   // bright segment through the note
            g.drawLine(cutX, static_cast<float>(nb.getY()) - 2.0f,
                       cutX, static_cast<float>(nb.getBottom()) + 2.0f, 2.0f);
        }
    }

    // Ghost preview of a chord being dragged in from the selector.
    if (chordDropPreviewPos.has_value() && ! pendingChordDrag.empty() && activeClip != nullptr)
    {
        const auto pos = *chordDropPreviewPos;
        const auto beat = snapBeat(xToBeat(static_cast<double>(pos.x)));
        int dropPitch = yToPitch(pos.y);
        if (scaleLockEnabled && ! isPitchInScale(dropPitch))
            dropPitch = snapPitchToScale(dropPitch);
        const int offset = dropPitch - pendingChordDrag.front();
        const auto len = juce::jlimit(minimumNoteLengthInBeats,
                                      juce::jmax(minimumNoteLengthInBeats, getContentBeats() - beat),
                                      lastNoteLengthInBeats);
        for (int p : pendingChordDrag)
        {
            MidiNote ghost { juce::jlimit(lowestPitch, highestPitch, p + offset), beat, len, 100 };
            const auto r = getNoteBounds(ghost).toFloat();
            g.setColour(theme::cool::cyan.withAlpha(0.32f));
            g.fillRoundedRectangle(r, 5.0f);
            g.setColour(theme::cool::cyan.withAlpha(0.85f));
            g.drawRoundedRectangle(r, 5.0f, 1.2f);
        }
    }

    if (marqueeState.has_value() && marqueeState->movedEnough)
    {
        const auto clippedMarquee = marqueeState->bounds.getIntersection(visibleGrid);
        g.setColour(theme::cool::cyan.withAlpha(0.12f));
        g.fillRect(clippedMarquee);
        g.setColour(theme::cool::cyan.withAlpha(0.62f));
        g.drawRect(clippedMarquee, 1);
    }
    g.restoreState();

    {
        juce::Graphics::ScopedSaveState saveState(g);

        g.excludeClipRegion(visibleGrid);
        g.reduceClipRegion(velocityLane);

        for (int step = firstStep; step <= lastStep; ++step)
        {
            const bool isBarLine  = (step % stepsPerBar)  == 0;
            const bool isBeatLine = (step % stepsPerBeat) == 0;
            if (! isBarLine && (isBeatLine ? ! drawBeat : ! drawSub))
                continue;
            const auto beat = static_cast<double>(step) * snapSizeInBeats;
            const auto x = static_cast<float>(gridArea.getX() + (beat * pixelsPerBeat) - scrollX);
            if (x < static_cast<float>(velocityLane.getX() - 8) || x > static_cast<float>(velocityLane.getRight() + 8))
                continue;
            g.setColour(step % 4 == 0 ? theme::line::normal.withAlpha(0.24f) : theme::line::subtle.withAlpha(0.12f));
            g.drawLine(x, static_cast<float>(velocityLane.getY()), x, static_cast<float>(velocityLane.getBottom()), step % 4 == 0 ? 1.4f : 1.0f);
        }

        g.setColour(mutedText);
        g.setFont(juce::FontOptions(12.0f, juce::Font::plain));
        g.drawText("Velocity", velocityLane.reduced(12, 8).removeFromTop(18), juce::Justification::centredLeft);

        if (activeClip != nullptr)
        {
            for (int noteIndex = 0; noteIndex < static_cast<int>(activeClip->midiNotes.size()); ++noteIndex)
            {
                if (focusModeEnabled && ! isPitchInScale(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].pitch))
                    continue;
                const auto barBounds = getVelocityBarBounds(noteIndex);
                const auto selected = isNoteSelected(noteIndex);
                const auto accent = midiAccentForTrack(clipColour, selected);
                g.setColour(accent.withAlpha(selected ? 0.95f : 0.58f));
                g.fillRoundedRectangle(barBounds.toFloat(), 2.5f);
                // Bright cap on top so the exact velocity value reads clearly and is easy to grab.
                const auto cap = barBounds.toFloat().withHeight(3.0f);
                g.setColour(accent.withAlpha(selected ? 1.0f : 0.9f));
                g.fillRoundedRectangle(cap, 1.5f);
            }

            // While dragging a velocity bar, show a readout (value + %) so it's obvious how much
            // louder/quieter it got.
            if (velocityDragState.has_value() && ! velocityDragState->targetIndices.empty())
            {
                const int idx = velocityDragState->targetIndices.front();
                if (idx >= 0 && idx < static_cast<int>(activeClip->midiNotes.size()))
                {
                    const int vel = juce::jlimit(0, 127, activeClip->midiNotes[static_cast<std::size_t>(idx)].velocity);
                    const int pct = juce::roundToInt(vel / 127.0 * 100.0);
                    const juce::String text = juce::String(vel) + "  ·  " + juce::String(pct) + "%";

                    g.setFont(juce::Font(12.5f, juce::Font::bold));
                    const auto bar = getVelocityBarBounds(idx);
                    const int tw = g.getCurrentFont().getStringWidth(text) + 18;
                    const int th = 21;
                    int lx = juce::jlimit(velocityLane.getX() + 6, velocityLane.getRight() - tw - 6, bar.getCentreX() - tw / 2);
                    int ly = juce::jmax(velocityLane.getY() + 4, bar.getY() - th - 5);
                    const juce::Rectangle<int> pill(lx, ly, tw, th);

                    g.setColour(theme::core::studio.withAlpha(0.96f));
                    g.fillRoundedRectangle(pill.toFloat(), 6.0f);
                    g.setColour(theme::cool::cyan.withAlpha(0.75f));
                    g.drawRoundedRectangle(pill.toFloat(), 6.0f, 1.2f);
                    g.setColour(juce::Colours::white);
                    g.drawText(text, pill, juce::Justification::centred);
                }
            }
        }
    }

    paintEditToolbar(g);
}

void MidiEditorOverlayComponent::resized()
{
    auto bounds = getLocalBounds().reduced(8);
    auto topBar = bounds.removeFromTop(topBarHeightPx).reduced(20, 12);

    topBar.removeFromRight(210);   // reserve the space so the other rows don't run under the button
    // Centre the Back button in the VISUAL header panel (getTopBarBounds), not the layout topBar —
    // their origins differ, which was pinning it to the top edge.
    const auto panel = getTopBarBounds();
    constexpr int backW = 190;
    constexpr int backH = 46;
    closeButton.setBounds(panel.getRight() - 20 - backW, panel.getCentreY() - backH / 2, backW, backH);

    auto titleRow = topBar.removeFromTop(28);
    titleLabel.setBounds(titleRow.removeFromLeft(220));
    contextLabel.setBounds(titleRow.reduced(8, 0));
    subtitleLabel.setBounds(0, 0, 0, 0);
    topBar.removeFromTop(8);                       // breathing room between the title and the buttons
    auto controlsArea = topBar.removeFromTop(32);

    // Uniform gaps + vertical air so the pills don't feel cramped. Text is centred by the LnF.
    constexpr int gap = 10;
    const auto pill = [&](int w) { controlsArea.removeFromLeft(gap); return controlsArea.removeFromLeft(w).reduced(0, 4); };
    scaleButton.setBounds(controlsArea.removeFromLeft(128).reduced(0, 4));   // first has no leading gap
    snapButton.setBounds(pill(92));
    slideVisibilityButton.setBounds(pill(112));
    stepLengthButton.setBounds(pill(74));
    stepRestButton.setBounds(pill(56));
    stepBackButton.setBounds(pill(56));
    stepTieButton.setBounds(pill(48));
    controlsArea.removeFromLeft(gap + 4);
    scaleLockToggle.setBounds(controlsArea.removeFromLeft(26).reduced(0, 5));
    scaleLockLabel.setBounds(controlsArea.removeFromLeft(80).reduced(0, 4));
    chordToggle.setBounds(pill(66));
    glideButton.setBounds(pill(62));
    joinButton.setBounds(pill(56));
    splitButton.setBounds(pill(56));
    modButton.setBounds(pill(66));
    presetsButton.setBounds(pill(74));
    quantizeButton.setBounds({});
    slidePenButton.setBounds({});
    stepWriteButton.setBounds({});

    if (chordSelector != nullptr && chordSelector->isVisible())
    {
        const auto grid = getVisibleGridViewport();
        constexpr int w = 620, h = 400;
        const int x = juce::jlimit(grid.getX(), juce::jmax(grid.getX(), grid.getRight() - w),
                                   grid.getCentreX() - w / 2);
        chordSelector->setBounds(x, grid.getY() + 14, w, h);
    }

    clampScrollOffsets();
}

bool MidiEditorOverlayComponent::keyPressed(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    const auto lowerKeyCode = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    const auto commandShortcut = modifiers.isCommandDown() && ! modifiers.isCtrlDown() && ! modifiers.isAltDown();
    const auto shiftedCommandShortcut = commandShortcut && modifiers.isShiftDown();

    // Any real key press cancels a pending hold-Command preview and ends a running one, so
    // Cmd+key shortcuts (undo/copy/…) don't trigger or keep the preview.
    cmdPreviewPending = false;
    if (globalSpacePreviewActive)
        stopGlobalSpacePreview();

    // Number keys 1..9 pick the toolbar tools (FL/Ableton style), when no modifier is held.
    if (! modifiers.isCommandDown() && ! modifiers.isCtrlDown() && ! modifiers.isAltDown())
    {
        const auto kc = key.getKeyCode();
        if (kc >= '1' && kc <= '9' && (kc - '1') < pianoRollToolButtonCount)
        {
            activateToolByIndex(kc - '1');
            return true;
        }
    }

    // Shift + arrows move selected notes by a full octave. Keep the plain arrows free for
    // navigation, and make the opposite direction available for a predictable pair of shortcuts.
    if (modifiers.isShiftDown() && ! modifiers.isCommandDown() && ! modifiers.isCtrlDown()
        && ! modifiers.isAltDown())
    {
        if (key.getKeyCode() == juce::KeyPress::downKey)
        {
            transposeSelectedNotes(-12);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::upKey)
        {
            transposeSelectedNotes(12);
            return true;
        }
    }

    if (! key.getModifiers().isCommandDown()
        && ! key.getModifiers().isCtrlDown()
        && ! key.getModifiers().isAltDown()
        && pitchForTypingKeyCode(key.getKeyCode()).has_value())
    {
        if (stepWriteEnabled)
        {
            updateStepWriteKeyboardPitches();
            return true;
        }

        updateLiveKeyboardPitches();
        // Consume the event so macOS doesn't play the system alert beep when the
        // key isn't otherwise handled (e.g. a MIDI track with no instrument loaded).
        return true;
    }

    if (commandShortcut && ! shiftedCommandShortcut && lowerKeyCode == 'z')
    {
        if (undoStack.empty())
            return true;

        redoStack.push_back(NoteSnapshot { activeClip != nullptr ? activeClip->midiNotes : std::vector<MidiNote> {},
                                           activeClip != nullptr ? activeClip->pitchSlides : std::vector<PitchSlide> {},
                                           selectedNotes, selectedSlide, horizontalZoom, verticalZoom, scaleRoot, scalePatternIndex,
                                           snapSizeInBeats, stepWriteCursorBeat, stepWriteStepLengthInBeats, false });
        restoreSnapshot(undoStack.back());
        undoStack.pop_back();
        repaint();
        return true;
    }

    if (commandShortcut && ((shiftedCommandShortcut && lowerKeyCode == 'z') || lowerKeyCode == 'y'))
    {
        if (redoStack.empty())
            return true;

        undoStack.push_back(NoteSnapshot { activeClip != nullptr ? activeClip->midiNotes : std::vector<MidiNote> {},
                                           activeClip != nullptr ? activeClip->pitchSlides : std::vector<PitchSlide> {},
                                           selectedNotes, selectedSlide, horizontalZoom, verticalZoom, scaleRoot, scalePatternIndex,
                                           snapSizeInBeats, stepWriteCursorBeat, stepWriteStepLengthInBeats, false });
        restoreSnapshot(redoStack.back());
        redoStack.pop_back();
        repaint();
        return true;
    }

    if (key == juce::KeyPress::spaceKey)
    {
        // Play from the clip start; Stop rewinds back to the clip start so the next play
        // begins from the beginning (not where it was paused). Momentary preview is hold-Cmd.
        const bool playing = onRequestPlayingState && onRequestPlayingState();
        if (playing)
        {
            if (onStopAndRewindToClipStart)
                onStopAndRewindToClipStart();
            else if (onTogglePlayback)
                onTogglePlayback();
        }
        else if (onTogglePlayback)
        {
            onTogglePlayback();   // playhead is already at the clip start after a stop
        }
        return true;
    }

    if (commandShortcut && ! shiftedCommandShortcut && lowerKeyCode == 'a' && activeClip != nullptr)
    {
        selectedNotes.clear();
        for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
            selectedNotes.insert(i);
        repaint();
        return true;
    }

    if (commandShortcut && ! shiftedCommandShortcut && lowerKeyCode == 'd' && ! selectedNotes.empty())
    {
        duplicateSelectedNotes();
        return true;
    }

    if ((key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey) && ! selectedNotes.empty())
    {
        deleteSelectedNotes();
        return true;
    }

    if ((key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey) && selectedSlide.has_value())
    {
        deleteSelectedSlide();
        return true;
    }

    // Option+Q — quick quantize. Q alone would collide with the laptop-keyboard
    // note input (QWERTY row plays notes), and Cmd+Q is the OS quit shortcut.
    if (key == juce::KeyPress('q', juce::ModifierKeys::altModifier, 0))
    {
        quantizeSelectedNotes();
        currentTool = PianoRollTool::quantize;
        return true;
    }

    if (shiftedCommandShortcut && lowerKeyCode == 'v')
        return setPianoRollTool(PianoRollTool::velocity), true;

    if (commandShortcut && ! shiftedCommandShortcut && lowerKeyCode == 'v')
        return setPianoRollTool(PianoRollTool::select), true;

    if (shiftedCommandShortcut && lowerKeyCode == 'd')
        return setPianoRollTool(PianoRollTool::draw), true;

    if (commandShortcut && ! shiftedCommandShortcut && lowerKeyCode == 'b')
        return setPianoRollTool(PianoRollTool::cut), true;

    if (shiftedCommandShortcut && lowerKeyCode == 'e')
        return setPianoRollTool(PianoRollTool::erase), true;

    if (shiftedCommandShortcut && lowerKeyCode == 'a')
        return setPianoRollTool(PianoRollTool::audition), true;

    if (shiftedCommandShortcut && lowerKeyCode == 's')
    {
        setPianoRollTool(PianoRollTool::slide);
        slidePenEnabled = true;
        slidePenButton.setToggleState(true, juce::dontSendNotification);
        repaint();
        return true;
    }

    if (shiftedCommandShortcut && lowerKeyCode == 'w')
    {
        setPianoRollTool(PianoRollTool::step);
        stepWriteEnabled = true;
        stepWriteButton.setToggleState(true, juce::dontSendNotification);
        repaint();
        return true;
    }

    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::escapeKey)
    {
        closeEditor();
        return true;
    }

    return false;
}

bool MidiEditorOverlayComponent::keyStateChanged(bool)
{
    // (Hold-Command preview is handled by the timer poll so it works without focus.)

    if (stepWriteEnabled)
    {
        updateStepWriteKeyboardPitches();
        return true;
    }

    updateLiveKeyboardPitches();
    return false;
}

void MidiEditorOverlayComponent::focusLost(FocusChangeType)
{
    ignoreNextMouseDown = false;
    stopGlobalSpacePreview();
    commitStepWritePendingChord();
    releaseLiveKeyboardPitches();
    releasePlacedNotePreview();
    repaint();
}

void MidiEditorOverlayComponent::mouseMove(const juce::MouseEvent& event)
{
    if (getEditToolbarBounds().contains(event.getPosition()))
    {
        bool overButton = false;
        for (int i = 0; i < pianoRollToolButtonCount; ++i)
            overButton = overButton || getToolButtonBounds(i).contains(event.getPosition());
        setMouseCursor(overButton ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

    hoverNote = hitTestNote(event.getPosition());
    hoveredGridBeat = getVisibleGridViewport().contains(event.getPosition())
        ? std::optional<double>(xToBeat(event.getPosition().x))
        : std::nullopt;
    if (currentTool == PianoRollTool::draw
        || currentTool == PianoRollTool::cut
        || currentTool == PianoRollTool::erase)
        setMouseCursor(toolCursor(currentTool));   // pointer becomes the tool glyph (FL-style)
    else if (currentTool == PianoRollTool::audition)
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    else
        setMouseCursor(hoverNote.has_value() && hoverNote->overResizeHandle
                           ? juce::MouseCursor::LeftRightResizeCursor
                           : juce::MouseCursor::NormalCursor);

    if (currentTool == PianoRollTool::cut)
        repaint(getVisibleGridViewport());   // keep the cut guide glued to the pointer
}

void MidiEditorOverlayComponent::mouseExit(const juce::MouseEvent&)
{
    hoverNote.reset();
    hoveredGridBeat.reset();
    releaseMousePreviewPitch();
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void MidiEditorOverlayComponent::mouseDown(const juce::MouseEvent& event)
{
    if (activeClip == nullptr)
        return;

    ignoreNextMouseDown = false;
    grabKeyboardFocus();

    if (handleEditToolbarClick(event.getPosition()))
        return;

    if (getKeyboardBounds().contains(event.getPosition()))
    {
        setMousePreviewPitch(keyboardPitchForPoint(event.getPosition()));
        return;
    }

    if (ignoreNextMouseDown || shouldConsumeFocusClick())
    {
        ignoreNextMouseDown = false;
        grabKeyboardFocus();
    }

    if (getVelocityLaneBounds().contains(event.getPosition()))
    {
        const auto hitVelocityIndex = hitTestVelocityBar(event.getPosition());
        if (hitVelocityIndex.has_value() && ! isNoteSelected(*hitVelocityIndex))
            selectSingleNote(*hitVelocityIndex);

        std::vector<int> targetIndices;
        if (selectedNotes.empty())
        {
            if (! hitVelocityIndex.has_value())
                return;

            targetIndices.push_back(*hitVelocityIndex);
        }
        else
        {
            targetIndices.assign(selectedNotes.begin(), selectedNotes.end());
        }

        std::vector<int> originalVelocities;
        originalVelocities.reserve(targetIndices.size());
        for (const auto index : targetIndices)
            originalVelocities.push_back(activeClip->midiNotes[static_cast<std::size_t>(index)].velocity);

        velocityDragState = VelocityDragState { targetIndices, originalVelocities, event.getPosition(), false };
        pushUndoSnapshot();
        velocityDragState->historyCaptured = true;
        updateVelocityFromPosition(event.getPosition().y);
        repaint();
        return;
    }

    if (! getVisibleGridViewport().contains(event.getPosition()))
        return;

    const auto hit = hitTestNote(event.getPosition());

    if (currentTool == PianoRollTool::cut)
    {
        splitNoteAtPoint(event.getPosition());
        return;
    }

    if (currentTool == PianoRollTool::erase)
    {
        if (hit.has_value())
        {
            selectSingleNote(hit->selected.noteIndex);
            deleteSelectedNotes();
        }
        else if (const auto hitSlide = hitTestSlide(event.getPosition()))
        {
            selectedSlide = hitSlide->slideIndex;
            deleteSelectedSlide();
        }
        return;
    }

    if (currentTool == PianoRollTool::audition)
    {
        auditionNoteAtPoint(event.getPosition());
        return;
    }

    if (slidePenEnabled)
    {
        if (auto slide = makeSlideAt(event.getPosition()))
        {
            clearSelection();
            selectedSlide.reset();
            slideDrawState = SlideDrawState { *slide, false };
            repaint();
        }
        return;
    }

    const auto hitSlide = hitTestSlide(event.getPosition());
    const auto slideHandleHit = hitSlide.has_value() && hitSlide->mode != SlideEditMode::move;

    if (event.mods.isRightButtonDown())
    {
        if (slideHandleHit)
        {
            selectedSlide = hitSlide->slideIndex;
            deleteSelectedSlide();
        }
        else if (hit.has_value())
        {
            selectSingleNote(hit->selected.noteIndex);
            deleteSelectedNotes();
        }
        else if (hitSlide.has_value())
        {
            selectedSlide = hitSlide->slideIndex;
            deleteSelectedSlide();
        }
        return;
    }

    if (hitSlide.has_value() && (! hit.has_value() || slideHandleHit))
    {
        clearSelection();
        selectedSlide = hitSlide->slideIndex;
        slideEditState = SlideEditState {
            hitSlide->slideIndex,
            hitSlide->mode,
            hitSlide->segmentIndex,
            event.getPosition(),
            activeClip->pitchSlides[static_cast<std::size_t>(hitSlide->slideIndex)],
            false
        };
        repaint();
        return;
    }

    if (hit.has_value())
    {
        selectedSlide.reset();
        if (! isNoteSelected(hit->selected.noteIndex))
            selectSingleNote(hit->selected.noteIndex);

        std::vector<int> selectedIndices(selectedNotes.begin(), selectedNotes.end());
        std::vector<MidiNote> originalSelectedNotes;
        originalSelectedNotes.reserve(selectedIndices.size());
        for (const auto index : selectedIndices)
            originalSelectedNotes.push_back(activeClip->midiNotes[static_cast<std::size_t>(index)]);

        const auto& note = activeClip->midiNotes[static_cast<std::size_t>(hit->selected.noteIndex)];
        noteDragState = NoteDragState {
            hit->selected,
            hit->overResizeHandle ? NoteDragMode::resizeRight : NoteDragMode::move,
            event.getPosition(),
            note.startBeat,
            note.lengthInBeats,
            note.pitch,
            originalSelectedNotes,
            selectedIndices,
            false
        };
        repaint();
        return;
    }

    clearSelection();
    marqueeState = MarqueeState { event.getPosition(), juce::Rectangle<int>(event.getPosition(), { 1, 1 }), false };
    repaint();
}

void MidiEditorOverlayComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (slideDrawState.has_value())
    {
        slideDrawState->movedEnough = slideDrawState->movedEnough
            || std::abs(event.getDistanceFromDragStartX()) > slideDrawThresholdPx
            || std::abs(event.getDistanceFromDragStartY()) > slideDrawThresholdPx;
        appendSlidePoint(slideDrawState->slide, event.getPosition());
        repaint();
        return;
    }

    if (slideEditState.has_value() && activeClip != nullptr)
    {
        const auto dragDistanceX = event.getDistanceFromDragStartX();
        const auto dragDistanceY = event.getDistanceFromDragStartY();
        if ((dragDistanceX * dragDistanceX) + (dragDistanceY * dragDistanceY) < noteDragThresholdPx * noteDragThresholdPx)
            return;

        if (slideEditState->slideIndex < 0
            || slideEditState->slideIndex >= static_cast<int>(activeClip->pitchSlides.size()))
            return;

        if (! slideEditState->historyCaptured)
        {
            pushUndoSnapshot();
            slideEditState->historyCaptured = true;
        }

        auto edited = slideEditState->originalSlide;
        if (edited.points.size() >= 2)
        {
            const auto pixelsPerBeat = getPixelsPerBeat();
            const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
            const auto beatDelta = static_cast<double>(event.position.x - slideEditState->mouseDownPosition.x) / juce::jmax(1.0, pixelsPerBeat);
            const auto pitchDelta = static_cast<double>(slideEditState->mouseDownPosition.y - event.position.y) / laneHeight;

            if (slideEditState->mode == SlideEditMode::curve)
            {
                const auto si = slideEditState->segmentIndex;
                if (si >= 0 && si + 1 < static_cast<int>(edited.points.size()))
                {
                    const auto dyPixels = static_cast<double>(slideEditState->mouseDownPosition.y - event.position.y);
                    // ~50 px of drag = full bend (was 120, which felt like the handle barely moved).
                    edited.points[static_cast<std::size_t>(si)].curve =
                        juce::jlimit(-1.0, 1.0, slideEditState->originalSlide.points[static_cast<std::size_t>(si)].curve + dyPixels / 50.0);
                }
                activeClip->pitchSlides[static_cast<std::size_t>(slideEditState->slideIndex)] = std::move(edited);
                repaint();
                return;
            }

            if (slideEditState->mode == SlideEditMode::move)
            {
                auto minBeat = edited.points.front().beat;
                auto maxBeat = edited.points.front().beat;
                auto minPitch = edited.points.front().pitch;
                auto maxPitch = edited.points.front().pitch;
                for (const auto& point : edited.points)
                {
                    minBeat = juce::jmin(minBeat, point.beat);
                    maxBeat = juce::jmax(maxBeat, point.beat);
                    minPitch = juce::jmin(minPitch, point.pitch);
                    maxPitch = juce::jmax(maxPitch, point.pitch);
                }

                const auto safeBeatDelta = juce::jlimit(-minBeat, activeClip->lengthInBeats - maxBeat, beatDelta);
                const auto safePitchDelta = juce::jlimit(static_cast<double>(lowestPitch) - minPitch,
                                                         static_cast<double>(highestPitch) - maxPitch,
                                                         pitchDelta);
                for (auto& point : edited.points)
                {
                    point.beat += safeBeatDelta;
                    point.pitch += safePitchDelta;
                }
            }
            else
            {
                const auto originalStart = edited.points.front().beat;
                const auto originalEnd = edited.points.back().beat;
                const auto originalSpan = juce::jmax(minimumSlidePointBeatDistance, originalEnd - originalStart);

                if (slideEditState->mode == SlideEditMode::resizeStart)
                {
                    const auto anchorBeat = originalEnd;
                    const auto newStart = juce::jlimit(0.0, anchorBeat - minimumSlidePointBeatDistance,
                                                       xToBeat(static_cast<double>(event.position.x)));
                    const auto newSpan = juce::jmax(minimumSlidePointBeatDistance, anchorBeat - newStart);
                    const auto targetPitch = yToContinuousPitch(static_cast<double>(event.position.y));
                    const auto pitchOffset = targetPitch - edited.points.front().pitch;

                    for (auto& point : edited.points)
                    {
                        const auto t = juce::jlimit(0.0, 1.0, (point.beat - originalStart) / originalSpan);
                        point.beat = newStart + t * newSpan;
                        point.pitch = juce::jlimit(static_cast<double>(lowestPitch),
                                                   static_cast<double>(highestPitch),
                                                   point.pitch + pitchOffset * (1.0 - t));
                    }
                }
                else
                {
                    const auto anchorBeat = originalStart;
                    const auto newEnd = juce::jlimit(anchorBeat + minimumSlidePointBeatDistance, activeClip->lengthInBeats,
                                                     xToBeat(static_cast<double>(event.position.x)));
                    const auto newSpan = juce::jmax(minimumSlidePointBeatDistance, newEnd - anchorBeat);
                    const auto targetPitch = yToContinuousPitch(static_cast<double>(event.position.y));
                    const auto pitchOffset = targetPitch - edited.points.back().pitch;

                    for (auto& point : edited.points)
                    {
                        const auto t = juce::jlimit(0.0, 1.0, (point.beat - originalStart) / originalSpan);
                        point.beat = anchorBeat + t * newSpan;
                        point.pitch = juce::jlimit(static_cast<double>(lowestPitch),
                                                   static_cast<double>(highestPitch),
                                                   point.pitch + pitchOffset * t);
                    }
                }
            }

            activeClip->pitchSlides[static_cast<std::size_t>(slideEditState->slideIndex)] = std::move(edited);
            repaint();
        }
        return;
    }

    if (mousePreviewPitch.has_value())
    {
        setMousePreviewPitch(keyboardPitchForPoint(event.getPosition()));
        return;
    }

    if (velocityDragState.has_value())
    {
        updateVelocityFromPosition(event.getPosition().y);
        repaint();
        return;
    }

    if (! noteDragState.has_value() || activeClip == nullptr)
    {
        if (marqueeState.has_value())
        {
            marqueeState->bounds = juce::Rectangle<int>(marqueeState->origin, event.getPosition()).getSmallestIntegerContainer();
            marqueeState->movedEnough = marqueeState->bounds.getWidth() > marqueeThresholdPx || marqueeState->bounds.getHeight() > marqueeThresholdPx;
            selectedNotes = hitTestNotesInRect(marqueeState->bounds);
            repaint();
        }
        return;
    }

    const auto dragDistanceX = event.getDistanceFromDragStartX();
    const auto dragDistanceY = event.getDistanceFromDragStartY();
    if ((dragDistanceX * dragDistanceX) + (dragDistanceY * dragDistanceY) < noteDragThresholdPx * noteDragThresholdPx)
        return;

    if (! noteDragState->historyCaptured)
    {
        pushUndoSnapshot();
        noteDragState->historyCaptured = true;
    }

    const auto pixelsPerBeat = getPixelsPerBeat();
    const auto beatDelta = (event.position.x - noteDragState->mouseDownPosition.x) / pixelsPerBeat;
    const auto snappedBeatDelta = snapBeatNearest(beatDelta);
    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);

    if (noteDragState->mode == NoteDragMode::move)
    {
        const auto pitchDelta = static_cast<int>(std::round((noteDragState->mouseDownPosition.y - event.position.y) / laneHeight));

        // Clamp the horizontal shift for the SELECTION AS A WHOLE (by its earliest/latest note) so
        // that hitting a boundary stops the whole group together — clamping each note individually
        // would collapse the relative spacing and pile notes on top of each other.
        double minStart = std::numeric_limits<double>::max();
        double maxEnd   = std::numeric_limits<double>::lowest();
        for (const auto& orig : noteDragState->originalSelectedNotes)
        {
            minStart = juce::jmin(minStart, orig.startBeat);
            maxEnd   = juce::jmax(maxEnd, orig.startBeat + orig.lengthInBeats);
        }
        const double lowerDelta = -minStart;                              // can't push the first note before 0
        const double upperDelta = activeClip->lengthInBeats - maxEnd;     // can't push the last note past the clip
        const double groupDelta = juce::jlimit(lowerDelta, juce::jmax(lowerDelta, upperDelta), snappedBeatDelta);

        for (std::size_t i = 0; i < noteDragState->selectedIndices.size(); ++i)
        {
            auto& movedNote = activeClip->midiNotes[static_cast<std::size_t>(noteDragState->selectedIndices[i])];
            const auto& originalNote = noteDragState->originalSelectedNotes[i];
            movedNote.startBeat = originalNote.startBeat + groupDelta;     // uniform shift keeps spacing intact
            auto newPitch = juce::jlimit(lowestPitch, highestPitch, originalNote.pitch + pitchDelta);
            if (scaleLockEnabled && ! isPitchInScale(newPitch))
                newPitch = snapPitchToScale(newPitch);
            movedNote.pitch = newPitch;
        }
    }
    else
    {
        for (std::size_t i = 0; i < noteDragState->selectedIndices.size(); ++i)
        {
            auto& resizedNote = activeClip->midiNotes[static_cast<std::size_t>(noteDragState->selectedIndices[i])];
            const auto& originalNote = noteDragState->originalSelectedNotes[i];
            resizedNote.lengthInBeats = juce::jlimit(
                minimumNoteLengthInBeats,
                getContentBeats() - resizedNote.startBeat,   // allow stretching into the headroom
                originalNote.lengthInBeats + snappedBeatDelta);
            growClipToFit(resizedNote.startBeat + resizedNote.lengthInBeats);
        }
    }

    repaint();
}

void MidiEditorOverlayComponent::mouseUp(const juce::MouseEvent&)
{
    OptionalAudioLock lk(audioEditLock);   // may add notes/slides — guard vs the audio thread
    releaseMousePreviewPitch();
    if (slideDrawState.has_value())
    {
        if (activeClip != nullptr && slideDrawState->movedEnough && slideDrawState->slide.points.size() >= 2)
        {
            smoothSlide(slideDrawState->slide);
            pushUndoSnapshot();
            activeClip->pitchSlides.push_back(std::move(slideDrawState->slide));
            selectedSlide = static_cast<int>(activeClip->pitchSlides.size()) - 1;
        }
        slideDrawState.reset();
        repaint();
        return;
    }

    if (slideEditState.has_value())
    {
        if (slideEditState->historyCaptured && ! undoStack.empty() && ! notesChangedSince(undoStack.back()))
            undoStack.pop_back();
        slideEditState.reset();
        repaint();
        return;
    }

    if (activeClip != nullptr && marqueeState.has_value() && ! marqueeState->movedEnough)
    {
        pushUndoSnapshot();
        const auto snappedBeat = snapBeat(xToBeat(static_cast<double>(marqueeState->origin.x)));
        auto pitch = yToPitch(marqueeState->origin.y);
        // Scale lock: snap new notes to the closest in-scale pitch when enabled.
        if (scaleLockEnabled && ! isPitchInScale(pitch))
            pitch = snapPitchToScale(pitch);
        // A freshly placed note takes the remembered length (4 bars by default, or whatever
        // the user last stretched a note to). Notes may be drawn into the headroom past the
        // clip end; the clip then grows to fit so you can keep writing the melody.
        const auto maxLen = juce::jmax(minimumNoteLengthInBeats, getContentBeats() - snappedBeat);
        const auto newLength = juce::jlimit(minimumNoteLengthInBeats, maxLen, lastNoteLengthInBeats);
        // Chord mode: one click writes the whole diatonic chord (each voice a separate note).
        const auto chordPitches = chordPitchesForNote(pitch);
        selectedNotes.clear();
        for (const auto chordPitch : chordPitches)
        {
            activeClip->midiNotes.push_back(MidiNote { chordPitch, snappedBeat, newLength, 100 });
            selectedNotes.insert(static_cast<int>(activeClip->midiNotes.size()) - 1);
        }
        growClipToFit(snappedBeat + newLength);
        auditionPlacedNote(chordPitches.front(), 100);
    }

    if (noteDragState.has_value() && activeClip != nullptr && noteDragState->historyCaptured)
    {
        if (noteDragState->mode == NoteDragMode::move)
        {
            for (const auto index : noteDragState->selectedIndices)
            {
                auto& movedNote = activeClip->midiNotes[static_cast<std::size_t>(index)];
                movedNote.startBeat = juce::jlimit(0.0,
                                                   activeClip->lengthInBeats - movedNote.lengthInBeats,
                                                   snapBeatNearest(movedNote.startBeat));
            }
        }
        else
        {
            for (const auto index : noteDragState->selectedIndices)
            {
                if (index < 0 || index >= static_cast<int>(activeClip->midiNotes.size()))
                    continue;

                auto& resizedNote = activeClip->midiNotes[static_cast<std::size_t>(index)];
                resizedNote.lengthInBeats = juce::jlimit(minimumNoteLengthInBeats,
                                                         getContentBeats() - resizedNote.startBeat,
                                                         snapBeatNearest(resizedNote.lengthInBeats));
                growClipToFit(resizedNote.startBeat + resizedNote.lengthInBeats);
                // Remember the stretched length so the next drawn note matches it.
                lastNoteLengthInBeats = resizedNote.lengthInBeats;
            }
        }

        // Shortening / moving a note back can leave the clip overlong — pull it back to fit the content
        // (symmetric with growClipToFit; only ever shrinks, never below one bar).
        shrinkClipToFitNotes();

        if (! undoStack.empty() && ! notesChangedSince(undoStack.back()))
            undoStack.pop_back();
    }

    if (velocityDragState.has_value() && velocityDragState->historyCaptured && ! undoStack.empty() && ! notesChangedSince(undoStack.back()))
        undoStack.pop_back();

    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();
    repaint();
}

void MidiEditorOverlayComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (! getVisibleGridViewport().contains(event.getPosition()))
        return;

    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto totalDelta = std::abs(wheel.deltaX) + std::abs(wheel.deltaY);

    if (nowMs < ignoreWheelUntilMs && totalDelta < 0.045f)
        return;

    // Hold Cmd/Ctrl to zoom into the point under the cursor (for mouse-wheel users).
    if (event.mods.isCommandDown() || event.mods.isCtrlDown())
    {
        const auto zoomDelta = static_cast<double>(wheel.deltaY) * 1.35;
        if (std::abs(zoomDelta) > 0.0001)
            adjustZoom(zoomDelta, zoomDelta, event.getPosition());
        return;
    }

    // Plain two-finger scroll / wheel = PAN (vertical + horizontal), like a native
    // Mac trackpad. Pinch (mouseMagnify) is what zooms into the cursor.
    if (std::abs(wheel.deltaX) > 0.0001f || std::abs(wheel.deltaY) > 0.0001f)
    {
        // Move the SCROLL TARGET; the timer eases toward it for a smooth glide.
        zoomAnimating = false;
        targetScrollX -= static_cast<double>(wheel.deltaX) * 600.0;
        targetScrollY -= static_cast<double>(wheel.deltaY) * 600.0;
        clampScrollOffsets();
    }
}

void MidiEditorOverlayComponent::mouseMagnify(const juce::MouseEvent& event, float scaleFactor)
{
    if (! getVisibleGridViewport().contains(event.getPosition()))
        return;

    const auto rawScale = juce::jlimit(0.85, 1.15, static_cast<double>(scaleFactor));
    pendingMagnifyDelta = pendingMagnifyDelta == 0.0 ? rawScale : (pendingMagnifyDelta * rawScale);
    ignoreWheelUntilMs = juce::Time::getMillisecondCounterHiRes() + 120.0;

    if (std::abs(pendingMagnifyDelta - 1.0) < 0.003)
        return;

    // Exact zoom-to-cursor: keep the SAME beat (x) and SAME lane (y) under the
    // pointer fixed. Anchoring by beat/lane (not by content-size ratios) stays
    // correct even when the grid is shorter than the viewport.
    const auto visible = getVisibleGridViewport();
    const double focusXInView = static_cast<double>(event.getPosition().x - visible.getX());
    const double focusYInView = static_cast<double>(event.getPosition().y - visible.getY());

    const double ppbOld  = getPixelsPerBeat();
    const double laneOld = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);

    // Pin the point under the cursor, set a zoom target; the timer eases toward it (smooth zoom).
    zoomFocusXInView = focusXInView;
    zoomFocusYInView = focusYInView;
    zoomFocusBeat = (scrollX + focusXInView) / juce::jmax(1.0e-6, ppbOld);
    zoomFocusLane = (scrollY + focusYInView) / juce::jmax(1.0, laneOld);
    targetHorizontalZoom = juce::jlimit(minimumHorizontalZoom, maximumHorizontalZoom, targetHorizontalZoom * pendingMagnifyDelta);
    targetVerticalZoom = juce::jlimit(minimumVerticalZoom, maximumVerticalZoom, targetVerticalZoom * pendingMagnifyDelta);
    pendingMagnifyDelta = 1.0;
    zoomAnimating = true;
    updateSubtitle();
}

void MidiEditorOverlayComponent::buttonClicked(juce::Button* button)
{
    if (button == &closeButton)
        closeEditor();
    else if (button == &scaleButton)
        showScaleMenu();
    else if (button == &snapButton)
        showSnapMenu();
    else if (button == &quantizeButton)
        quantizeSelectedNotes();
    else if (button == &stepWriteButton)
    {
        stepWriteEnabled = stepWriteButton.getToggleState();
        currentTool = stepWriteEnabled ? PianoRollTool::step : PianoRollTool::select;
        commitStepWritePendingChord();
        releaseLiveKeyboardPitches();
        releasePlacedNotePreview();

        if (stepWriteEnabled)
        {
            slidePenEnabled = false;
            slidePenButton.setToggleState(false, juce::dontSendNotification);

            if (activeClip != nullptr)
            {
                const auto playheadBeat = onRequestPlayheadBeat ? onRequestPlayheadBeat() : activeClip->startBeat;
                const auto localBeat = playheadBeat - activeClip->startBeat;
                if (localBeat >= 0.0 && localBeat <= activeClip->lengthInBeats)
                    stepWriteCursorBeat = std::round(localBeat / stepWriteStepLengthInBeats) * stepWriteStepLengthInBeats;
                else
                    stepWriteCursorBeat = getSnappedStepWriteCursorBeat();

                stepWriteCursorBeat = juce::jlimit(0.0, activeClip->lengthInBeats, stepWriteCursorBeat);
            }
        }

        repaint();
    }
    else if (button == &stepLengthButton)
        showStepLengthMenu();
    else if (button == &stepRestButton)
        restStepWrite();
    else if (button == &stepBackButton)
        backstepStepWrite();
    else if (button == &stepTieButton)
        extendStepWritePreviousNotes();
    else if (button == &slidePenButton)
    {
        slidePenEnabled = slidePenButton.getToggleState();
        currentTool = slidePenEnabled ? PianoRollTool::slide : PianoRollTool::select;
        if (slidePenEnabled)
        {
            stepWriteEnabled = false;
            stepWriteButton.setToggleState(false, juce::dontSendNotification);
            commitStepWritePendingChord();
        }
        selectedSlide.reset();
        clearSelection();
        repaint();
    }
    else if (button == &glideButton)
    {
        glideEnabled = glideButton.getToggleState();
        if (onGlideChanged)
            onGlideChanged(glideEnabled);
        repaint();
    }
    else if (button == &joinButton)
    {
        joinSelectedNotes();
    }
    else if (button == &splitButton)
    {
        splitSelectedSlide();
    }
    else if (button == &modButton)
    {
        if (activeClip != nullptr && selectedSlide.has_value()
            && *selectedSlide >= 0 && *selectedSlide < static_cast<int>(activeClip->pitchSlides.size()))
        {
            pushUndoSnapshot();
            auto& sl = activeClip->pitchSlides[static_cast<std::size_t>(*selectedSlide)];
            const int next = sl.points.empty() ? 0 : (sl.points.front().lfoShape + 1) % 5;
            for (auto& p : sl.points)
            {
                p.lfoShape = next;
                if (next != 0 && p.lfoDepth == 0.0) p.lfoDepth = 1.0;  // default ±1 st vibrato
                if (p.lfoRate <= 0.0)               p.lfoRate = 3.0;   // cycles per beat
            }
            static const char* names[] = { "Mod", "Sine", "Tri", "Saw", "Sqr" };
            modButton.setButtonText(names[next]);
            repaint();
        }
    }
    else if (button == &presetsButton)
    {
        showSlidePresetMenu();
    }
    else if (button == &slideVisibilityButton)
    {
        switch (slideVisibilityMode)
        {
            case SlideVisibilityMode::ghost:  slideVisibilityMode = SlideVisibilityMode::active; break;
            case SlideVisibilityMode::active: slideVisibilityMode = SlideVisibilityMode::off;    break;
            case SlideVisibilityMode::off:    slideVisibilityMode = SlideVisibilityMode::ghost;  break;
        }
        slideVisibilityButton.setButtonText(getSlideVisibilityName());
        repaint();
    }
}

bool MidiEditorOverlayComponent::updateLiveKeyboardPitches()
{
    static constexpr std::array<int, 39> keyCodes {
        'q', '2', 'w', '3', 'e', 'r', '5', 't', '6', 'y', '7', 'u', 'i', '9', 'o', '0', 'p', '[', ']', '+', '=',
        'z', 's', 'x', 'd', 'c', 'v', 'g', 'b', 'h', 'n', 'j', 'm', ',', 'l', '.', ';', '/', '\''
    };

    std::set<int> nextBasePitches;
    const auto modifiers = juce::ModifierKeys::getCurrentModifiers();
    if (! modifiers.isCommandDown() && ! modifiers.isCtrlDown() && ! modifiers.isAltDown())
    {
        for (const auto keyCode : keyCodes)
        {
            if (! juce::KeyPress::isKeyCurrentlyDown(keyCode))
                continue;

            auto pitch = scaleLockEnabled
                ? diatonicPitchForTypingKeyCode(keyCode, scaleRoot, scalePatternIndex)
                : pitchForTypingKeyCode(keyCode);
            if (! pitch.has_value())
                continue;

            auto playablePitch = *pitch;
            if (activeTrack != nullptr)
                playablePitch += activeTrack->samplerKeyboardOctaveOffset * 12
                               + activeTrack->samplerTransposeSemitones;

            nextBasePitches.insert(juce::jlimit(lowestPitch, highestPitch, playablePitch));
        }
    }

    std::set<int> nextPitches;
    if (nextBasePitches.size() == 1)
    {
        for (const auto chordPitch : chordPitchesForNote(*nextBasePitches.begin()))
            nextPitches.insert(juce::jlimit(lowestPitch, highestPitch, chordPitch));
    }
    else
    {
        nextPitches = std::move(nextBasePitches);
    }

    const auto previousPitches = liveKeyboardPitches;
    const auto previousBasePitches = liveKeyboardBasePitches;
    if (nextPitches == previousPitches)
        return false;

    if (onPreviewNoteOff)
        for (const auto pitch : previousPitches)
            if (! nextPitches.contains(pitch))
                onPreviewNoteOff(pitch);

    const bool retriggerChordGesture = chordModeEnabled
                                    && previousBasePitches.size() == 1
                                    && nextBasePitches.size() == 1
                                    && previousBasePitches != nextBasePitches
                                    && ! previousPitches.empty()
                                    && ! nextPitches.empty();
    if (retriggerChordGesture && onPreviewChordRetrigger)
        onPreviewChordRetrigger();

    if (onPreviewNoteOn)
        for (const auto pitch : nextPitches)
            if (retriggerChordGesture || ! previousPitches.contains(pitch))
                onPreviewNoteOn(pitch, 108);

    liveKeyboardPitches = std::move(nextPitches);
    liveKeyboardBasePitches = std::move(nextBasePitches);
    repaint();
    return true;
}

bool MidiEditorOverlayComponent::stepWriteMidiNoteOn(int midiNote, int velocity)
{
    if (! stepWriteEnabled || activeClip == nullptr)
        return false;

    auto pitch = juce::jlimit(lowestPitch, highestPitch, midiNote);
    if (scaleLockEnabled && ! isPitchInScale(pitch))
        pitch = snapPitchToScale(pitch);

    const auto safeVelocity = juce::jlimit(1, 127, velocity > 0 ? velocity : defaultStepWriteVelocity());
    const auto chordPitches = chordPitchesForNote(pitch);
    stepWriteLiveVoicing[pitch] = chordPitches;
    for (const auto chordPitch : chordPitches)
        stepWriteLivePitches.insert(juce::jlimit(lowestPitch, highestPitch, chordPitch));
    stepWritePendingVelocities[pitch] = safeVelocity;

    if (onPreviewNoteOn)
        for (const auto chordPitch : chordPitches)
            onPreviewNoteOn(juce::jlimit(lowestPitch, highestPitch, chordPitch), safeVelocity);

    repaint();
    return true;
}

bool MidiEditorOverlayComponent::stepWriteMidiNoteOff(int midiNote)
{
    if (! stepWriteEnabled)
        return false;

    auto pitch = juce::jlimit(lowestPitch, highestPitch, midiNote);
    if (scaleLockEnabled && ! isPitchInScale(pitch))
        pitch = snapPitchToScale(pitch);

    auto pitches = chordPitchesForNote(pitch);
    if (const auto it = stepWriteLiveVoicing.find(pitch); it != stepWriteLiveVoicing.end())
    {
        pitches = it->second;
        stepWriteLiveVoicing.erase(it);
    }

    for (const auto chordPitch : pitches)
    {
        const auto clamped = juce::jlimit(lowestPitch, highestPitch, chordPitch);
        stepWriteLivePitches.erase(clamped);
        if (onPreviewNoteOff)
            onPreviewNoteOff(clamped);
    }

    if (stepWriteLivePitches.empty())
        commitStepWritePendingChord();

    repaint();
    return true;
}

bool MidiEditorOverlayComponent::updateStepWriteKeyboardPitches()
{
    static constexpr std::array<int, 39> keyCodes {
        'q', '2', 'w', '3', 'e', 'r', '5', 't', '6', 'y', '7', 'u', 'i', '9', 'o', '0', 'p', '[', ']', '+', '=',
        'z', 's', 'x', 'd', 'c', 'v', 'g', 'b', 'h', 'n', 'j', 'm', ',', 'l', '.', ';', '/', '\''
    };

    std::set<int> nextBasePitches;
    std::map<int, int> nextVelocities;
    const auto modifiers = juce::ModifierKeys::getCurrentModifiers();
    if (! modifiers.isCommandDown() && ! modifiers.isCtrlDown() && ! modifiers.isAltDown())
    {
        for (const auto keyCode : keyCodes)
        {
            if (! juce::KeyPress::isKeyCurrentlyDown(keyCode))
                continue;

            auto pitch = scaleLockEnabled
                ? diatonicPitchForTypingKeyCode(keyCode, scaleRoot, scalePatternIndex)
                : pitchForTypingKeyCode(keyCode);
            if (! pitch.has_value())
                continue;

            auto playablePitch = *pitch;
            if (activeTrack != nullptr)
                playablePitch += activeTrack->samplerKeyboardOctaveOffset * 12
                               + activeTrack->samplerTransposeSemitones;

            playablePitch = juce::jlimit(lowestPitch, highestPitch, playablePitch);
            nextBasePitches.insert(playablePitch);
            nextVelocities[playablePitch] = defaultStepWriteVelocity();
        }
    }

    std::set<int> nextPitches;
    if (nextBasePitches.size() == 1)
    {
        for (const auto chordPitch : chordPitchesForNote(*nextBasePitches.begin()))
            nextPitches.insert(juce::jlimit(lowestPitch, highestPitch, chordPitch));
    }
    else
    {
        nextPitches = nextBasePitches;
    }

    const auto previousPitches = stepWriteLivePitches;
    if (nextPitches == previousPitches)
        return false;

    if (onPreviewNoteOff)
        for (const auto pitch : previousPitches)
            if (! nextPitches.contains(pitch))
                onPreviewNoteOff(pitch);

    if (onPreviewNoteOn)
        for (const auto pitch : nextPitches)
            if (! previousPitches.contains(pitch))
                onPreviewNoteOn(pitch, nextVelocities[pitch]);

    for (const auto& [pitch, velocity] : nextVelocities)
        stepWritePendingVelocities[pitch] = velocity;

    stepWriteLivePitches = std::move(nextPitches);

    if (stepWriteLivePitches.empty())
        commitStepWritePendingChord();

    repaint();
    return true;
}

void MidiEditorOverlayComponent::commitStepWritePendingChord()
{
    OptionalAudioLock lk(audioEditLock);
    if (! stepWriteLivePitches.empty())
    {
        if (onPreviewNoteOff)
            for (const auto pitch : stepWriteLivePitches)
                onPreviewNoteOff(pitch);

        stepWriteLivePitches.clear();
        stepWriteLiveVoicing.clear();
    }

    if (activeClip == nullptr || stepWritePendingVelocities.empty())
    {
        stepWritePendingVelocities.clear();
        return;
    }

    const auto startBeat = getSnappedStepWriteCursorBeat();
    if (startBeat >= activeClip->lengthInBeats - beatEpsilon)
    {
        stepWritePendingVelocities.clear();
        return;
    }

    const auto remainingBeats = activeClip->lengthInBeats - startBeat;
    if (remainingBeats <= beatEpsilon)
    {
        stepWritePendingVelocities.clear();
        return;
    }

    const auto noteLength = juce::jmin(stepWriteStepLengthInBeats, remainingBeats);

    pushUndoSnapshot();
    selectedNotes.clear();
    selectedSlide.reset();

    // Chord mode means "one key becomes a chord". If the player is already holding multiple
    // keys, keep that manual chord as-is instead of expanding every note into its own chord.
    std::map<int, int> pitchToVelocity;   // pitch → velocity (kept sorted, unique)
    const bool expandSingleKey = stepWritePendingVelocities.size() == 1;
    for (const auto& [pitch, velocity] : stepWritePendingVelocities)
    {
        const auto pitches = expandSingleKey ? chordPitchesForNote(pitch) : std::vector<int> { pitch };
        for (const auto chordPitch : pitches)
        {
            auto& stored = pitchToVelocity[chordPitch];
            stored = juce::jmax(stored, juce::jlimit(1, 127, velocity));
        }
    }

    for (const auto& [pitch, velocity] : pitchToVelocity)
    {
        activeClip->midiNotes.push_back(MidiNote { pitch, startBeat, noteLength, velocity });
        selectedNotes.insert(static_cast<int>(activeClip->midiNotes.size()) - 1);
    }

    stepWritePendingVelocities.clear();
    advanceStepWriteCursor(1);
    repaint();
}

void MidiEditorOverlayComponent::advanceStepWriteCursor(int stepCount)
{
    if (activeClip == nullptr)
        return;

    const auto nextBeat = getSnappedStepWriteCursorBeat() + static_cast<double>(stepCount) * stepWriteStepLengthInBeats;
    const auto step = juce::jmax(0.001, stepWriteStepLengthInBeats);
    stepWriteCursorBeat = juce::jlimit(0.0, activeClip->lengthInBeats, std::round(nextBeat / step) * step);
}

void MidiEditorOverlayComponent::restStepWrite()
{
    if (! stepWriteEnabled || activeClip == nullptr)
        return;

    commitStepWritePendingChord();
    pushUndoSnapshot();
    advanceStepWriteCursor(1);
    repaint();
}

void MidiEditorOverlayComponent::backstepStepWrite()
{
    if (! stepWriteEnabled || activeClip == nullptr)
        return;

    commitStepWritePendingChord();
    pushUndoSnapshot();
    advanceStepWriteCursor(-1);
    repaint();
}

void MidiEditorOverlayComponent::extendStepWritePreviousNotes()
{
    if (! stepWriteEnabled || activeClip == nullptr || activeClip->midiNotes.empty())
        return;

    commitStepWritePendingChord();
    const auto cursorBeat = getSnappedStepWriteCursorBeat();
    const auto targetEnd = juce::jlimit(0.0, activeClip->lengthInBeats, cursorBeat + stepWriteStepLengthInBeats);
    bool changed = false;

    pushUndoSnapshot();

    if (! selectedNotes.empty())
    {
        for (const auto index : selectedNotes)
        {
            if (index < 0 || index >= static_cast<int>(activeClip->midiNotes.size()))
                continue;

            auto& note = activeClip->midiNotes[static_cast<std::size_t>(index)];
            const auto noteEnd = note.startBeat + note.lengthInBeats;
            if (std::abs(noteEnd - cursorBeat) > stepWriteStepLengthInBeats + beatEpsilon
                && noteEnd > cursorBeat + beatEpsilon)
                continue;

            const auto newLength = juce::jlimit(minimumNoteLengthInBeats,
                                                activeClip->lengthInBeats - note.startBeat,
                                                targetEnd - note.startBeat);
            if (std::abs(newLength - note.lengthInBeats) > beatEpsilon)
            {
                note.lengthInBeats = newLength;
                changed = true;
            }
        }
    }

    if (! changed)
    {
        double latestStart = -1.0;
        for (const auto& note : activeClip->midiNotes)
            if (note.startBeat <= cursorBeat + beatEpsilon)
                latestStart = juce::jmax(latestStart, note.startBeat);

        if (latestStart >= 0.0)
        {
            selectedNotes.clear();
            for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
            {
                auto& note = activeClip->midiNotes[static_cast<std::size_t>(i)];
                if (std::abs(note.startBeat - latestStart) > beatEpsilon)
                    continue;

                const auto newLength = juce::jlimit(minimumNoteLengthInBeats,
                                                    activeClip->lengthInBeats - note.startBeat,
                                                    targetEnd - note.startBeat);
                if (std::abs(newLength - note.lengthInBeats) > beatEpsilon)
                {
                    note.lengthInBeats = newLength;
                    changed = true;
                }
                selectedNotes.insert(i);
            }
        }
    }

    if (changed)
        advanceStepWriteCursor(1);
    else if (! undoStack.empty())
        undoStack.pop_back();

    repaint();
}

double MidiEditorOverlayComponent::getSnappedStepWriteCursorBeat() const noexcept
{
    if (activeClip == nullptr)
        return 0.0;

    const auto step = juce::jmax(0.001, stepWriteStepLengthInBeats);
    const auto snapped = std::round(stepWriteCursorBeat / step) * step;
    return juce::jlimit(0.0, activeClip->lengthInBeats, snapped);
}

int MidiEditorOverlayComponent::defaultStepWriteVelocity() const noexcept
{
    if (activeClip != nullptr && ! selectedNotes.empty())
    {
        const auto index = *selectedNotes.rbegin();
        if (index >= 0 && index < static_cast<int>(activeClip->midiNotes.size()))
            return juce::jlimit(1, 127, activeClip->midiNotes[static_cast<std::size_t>(index)].velocity);
    }

    return 100;
}

std::optional<PitchSlide> MidiEditorOverlayComponent::makeSlideAt(juce::Point<int> position) const
{
    if (activeClip == nullptr || activeClip->midiNotes.empty())
        return std::nullopt;

    const auto beat = juce::jlimit(0.0, activeClip->lengthInBeats, xToBeat(static_cast<double>(position.x)));
    const auto pitch = yToContinuousPitch(static_cast<double>(position.y));
    int sourcePitch = static_cast<int>(std::round(pitch));
    double sourceStart = 0.0;
    bool foundSource = false;

    for (const auto& note : activeClip->midiNotes)
    {
        const auto noteEnd = note.startBeat + juce::jmax(0.01, note.lengthInBeats);
        if (beat >= note.startBeat && beat <= noteEnd)
        {
            sourcePitch = note.pitch;
            sourceStart = note.startBeat;
            foundSource = true;
            break;
        }
    }

    if (! foundSource)
        return std::nullopt;

    PitchSlide slide;
    slide.sourcePitch = sourcePitch;
    slide.sourceNoteStartBeat = sourceStart;
    slide.points.push_back(PitchSlidePoint { beat, pitch });
    return slide;
}

bool MidiEditorOverlayComponent::appendSlidePoint(PitchSlide& slide, juce::Point<int> position) const
{
    if (activeClip == nullptr)
        return false;

    const auto beat = juce::jlimit(0.0, activeClip->lengthInBeats, xToBeat(static_cast<double>(position.x)));
    const auto pitch = yToContinuousPitch(static_cast<double>(position.y));
    if (! slide.points.empty())
    {
        auto& last = slide.points.back();
        if (std::abs(last.beat - beat) < 0.015 && std::abs(last.pitch - pitch) < 0.15)
            return false;
        if (beat < last.beat)
            return false;
    }

    slide.points.push_back(PitchSlidePoint { beat, pitch });
    return true;
}

void MidiEditorOverlayComponent::smoothSlide(PitchSlide& slide) const
{
    if (slide.points.size() < 3)
        return;

    std::vector<PitchSlidePoint> cleaned;
    cleaned.reserve(slide.points.size());
    cleaned.push_back(slide.points.front());

    for (std::size_t i = 1; i + 1 < slide.points.size(); ++i)
    {
        const auto& point = slide.points[i];
        const auto& last = cleaned.back();
        if (std::abs(point.beat - last.beat) < minimumSlidePointBeatDistance
            && std::abs(point.pitch - last.pitch) < 0.08)
            continue;
        if (point.beat <= last.beat)
            continue;
        cleaned.push_back(point);
    }

    if (slide.points.back().beat > cleaned.back().beat)
        cleaned.push_back(slide.points.back());

    if (cleaned.size() < 3)
    {
        slide.points = std::move(cleaned);
        return;
    }

    auto smoothed = cleaned;
    for (int pass = 0; pass < 8; ++pass)
    {
        auto next = smoothed;
        for (std::size_t i = 1; i + 1 < smoothed.size(); ++i)
        {
            next[i].pitch = smoothed[i - 1].pitch * 0.25
                          + smoothed[i].pitch * 0.50
                          + smoothed[i + 1].pitch * 0.25;
        }
        smoothed = std::move(next);
    }

    const auto startBeat = smoothed.front().beat;
    const auto endBeat = smoothed.back().beat;
    const auto span = juce::jmax(minimumSlidePointBeatDistance, endBeat - startBeat);
    const auto targetCount = juce::jlimit<std::size_t>(
        4,
        96,
        static_cast<std::size_t>(std::ceil(span / 0.035)) + 1);

    auto pitchAt = [&smoothed](double beat)
    {
        const auto n = smoothed.size();
        if (beat <= smoothed.front().beat)
            return smoothed.front().pitch;
        if (beat >= smoothed.back().beat)
            return smoothed.back().pitch;

        for (std::size_t i = 1; i < n; ++i)
        {
            const auto& a = smoothed[i - 1];
            const auto& b = smoothed[i];
            if (beat < a.beat || beat > b.beat)
                continue;

            const auto segment = juce::jmax(minimumSlidePointBeatDistance, b.beat - a.beat);
            const auto t = juce::jlimit(0.0, 1.0, (beat - a.beat) / segment);

            // Catmull-Rom spline through the (de-jittered) points → a genuinely
            // smooth flowing curve instead of straight segments.
            const double p0 = smoothed[i >= 2 ? i - 2 : i - 1].pitch;
            const double p1 = a.pitch;
            const double p2 = b.pitch;
            const double p3 = smoothed[i + 1 < n ? i + 1 : i].pitch;
            const double t2 = t * t;
            const double t3 = t2 * t;
            return 0.5 * ((2.0 * p1)
                          + (-p0 + p2) * t
                          + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2
                          + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
        }

        return smoothed.back().pitch;
    };

    std::vector<PitchSlidePoint> resampled;
    resampled.reserve(targetCount);
    for (std::size_t i = 0; i < targetCount; ++i)
    {
        const auto t = targetCount <= 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(targetCount - 1);
        const auto beat = startBeat + span * t;
        resampled.push_back(PitchSlidePoint {
            beat,
            juce::jlimit(static_cast<double>(lowestPitch), static_cast<double>(highestPitch), pitchAt(beat))
        });
    }

    resampled.front() = slide.points.front();
    resampled.back() = slide.points.back();
    slide.points = std::move(resampled);
}

bool MidiEditorOverlayComponent::slideTouchesSelectedNotes(const PitchSlide& slide) const noexcept
{
    if (activeClip == nullptr || selectedNotes.empty() || slide.points.empty())
        return false;

    const auto slideStart = slide.points.front().beat;
    const auto slideEnd = slide.points.back().beat;
    for (const auto noteIndex : selectedNotes)
    {
        if (noteIndex < 0 || noteIndex >= static_cast<int>(activeClip->midiNotes.size()))
            continue;

        const auto& note = activeClip->midiNotes[static_cast<std::size_t>(noteIndex)];
        const auto noteEnd = note.startBeat + juce::jmax(0.01, note.lengthInBeats);
        const auto overlapsTime = slideStart <= noteEnd && slideEnd >= note.startBeat;
        const auto startsInside = slideStart >= note.startBeat && slideStart <= noteEnd;
        if (overlapsTime && (startsInside || std::abs(slide.sourceNoteStartBeat - note.startBeat) < 0.0001))
            return true;
    }

    return false;
}

bool MidiEditorOverlayComponent::shouldDrawSlide(const PitchSlide& slide, int slideIndex, bool drawingPreview) const noexcept
{
    if (drawingPreview)
        return true;

    switch (slideVisibilityMode)
    {
        case SlideVisibilityMode::off:
            return false;
        case SlideVisibilityMode::ghost:
            return true;
        case SlideVisibilityMode::active:
            return selectedSlide == slideIndex || slideTouchesSelectedNotes(slide);
    }

    return true;
}

juce::String MidiEditorOverlayComponent::getSlideVisibilityName() const
{
    switch (slideVisibilityMode)
    {
        case SlideVisibilityMode::off:    return "Slides Off";
        case SlideVisibilityMode::ghost:  return "Slides Ghost";
        case SlideVisibilityMode::active: return "Slides Active";
    }

    return "Slides Ghost";
}

std::optional<int> MidiEditorOverlayComponent::keyboardPitchForPoint(juce::Point<int> position) const noexcept
{
    if (! getKeyboardBounds().contains(position))
        return std::nullopt;

    return yToPitch(position.y);
}

void MidiEditorOverlayComponent::setMousePreviewPitch(std::optional<int> pitch)
{
    if (pitch == mousePreviewPitch)
        return;

    releaseMousePreviewPitch();
    if (! pitch.has_value())
        return;

    mousePreviewPitch = *pitch;
    if (chordModeEnabled && onPreviewChordRetrigger)
        onPreviewChordRetrigger();

    if (onPreviewNoteOn)
        for (const auto chordPitch : chordPitchesForNote(*pitch))
            onPreviewNoteOn(juce::jlimit(lowestPitch, highestPitch, chordPitch), 110);
    repaint();
}

void MidiEditorOverlayComponent::releaseMousePreviewPitch()
{
    if (! mousePreviewPitch.has_value())
        return;

    const auto pitch = *mousePreviewPitch;
    mousePreviewPitch.reset();
    if (onPreviewNoteOff)
        for (const auto chordPitch : chordPitchesForNote(pitch))
            onPreviewNoteOff(juce::jlimit(lowestPitch, highestPitch, chordPitch));
    repaint();
}

void MidiEditorOverlayComponent::releaseLiveKeyboardPitches()
{
    if (liveKeyboardPitches.empty())
        return;

    if (onPreviewNoteOff)
        for (const auto pitch : liveKeyboardPitches)
            onPreviewNoteOff(pitch);

    liveKeyboardPitches.clear();
    liveKeyboardBasePitches.clear();
    repaint();
}

void MidiEditorOverlayComponent::auditionPlacedNote(int pitch, int velocity)
{
    releasePlacedNotePreview();

    placedNotePreviewPitch = pitch;
    placedNotePreviewOffMs = juce::Time::getMillisecondCounterHiRes() + placedNoteAuditionMs;
    if (onPreviewNoteOn)
        onPreviewNoteOn(pitch, velocity);
}

void MidiEditorOverlayComponent::releasePlacedNotePreview()
{
    if (! placedNotePreviewPitch.has_value())
        return;

    const auto pitch = *placedNotePreviewPitch;
    placedNotePreviewPitch.reset();
    placedNotePreviewOffMs = 0.0;
    if (onPreviewNoteOff)
        onPreviewNoteOff(pitch);
}

void MidiEditorOverlayComponent::auditionChord(const std::vector<int>& pitches)
{
    releaseChordPreview();
    chordPreviewPitches = pitches;
    chordPreviewOffMs = juce::Time::getMillisecondCounterHiRes() + 650.0;
    if (onPreviewNoteOn)
        for (int p : chordPreviewPitches)
            onPreviewNoteOn(juce::jlimit(0, 127, p), 104);
}

void MidiEditorOverlayComponent::releaseChordPreview()
{
    if (chordPreviewPitches.empty())
        return;
    if (onPreviewNoteOff)
        for (int p : chordPreviewPitches)
            onPreviewNoteOff(juce::jlimit(0, 127, p));
    chordPreviewPitches.clear();
    chordPreviewOffMs = 0.0;
}

void MidiEditorOverlayComponent::openChordSelector()
{
    if (chordSelector == nullptr)
        return;

    const int patternIndex = juce::jlimit(0, static_cast<int>(scalePatterns.size()) - 1, scalePatternIndex);
    const juce::String keyName = juce::String(rootNames[static_cast<std::size_t>(((scaleRoot % 12) + 12) % 12)])
                               + " " + scalePatterns[static_cast<std::size_t>(patternIndex)].name;
    chordSelector->setProjectKey(scaleRoot, scalePatterns[static_cast<std::size_t>(patternIndex)].pitchClasses, keyName);

    // Centre the panel over the grid, just below the toolbar.
    const auto grid = getVisibleGridViewport();
    constexpr int w = 620, h = 400;
    const int x = juce::jlimit(grid.getX(), juce::jmax(grid.getX(), grid.getRight() - w),
                               grid.getCentreX() - w / 2);
    const int y = grid.getY() + 14;
    chordSelector->setBounds(x, y, w, h);
    chordSelector->setVisible(true);
    chordSelector->toFront(false);
    chordToggle.setToggleState(true, juce::dontSendNotification);
}

void MidiEditorOverlayComponent::closeChordSelector()
{
    releaseChordPreview();
    if (chordSelector != nullptr)
        chordSelector->setVisible(false);
    chordToggle.setToggleState(false, juce::dontSendNotification);
}

void MidiEditorOverlayComponent::beginChordDrag(const std::vector<int>& pitches)
{
    if (pitches.empty() || chordSelector == nullptr)
        return;
    pendingChordDrag = pitches;

    // Small labelled chip that follows the cursor while dragging.
    const juce::String name = orion::chords::chordName(chordSelector->getChord());
    juce::Image img(juce::Image::ARGB, 104, 30, true);
    {
        juce::Graphics g(img);
        g.setColour(theme::cool::cyan.withAlpha(0.92f));
        g.fillRoundedRectangle(img.getBounds().toFloat().reduced(1.0f), 6.0f);
        g.setColour(juce::Colours::black.withAlpha(0.9f));
        g.setFont(juce::Font(14.0f, juce::Font::bold));
        g.drawText(name, img.getBounds(), juce::Justification::centred);
    }
    startDragging("chord", chordSelector.get(), juce::ScaledImage(img), true);
}

void MidiEditorOverlayComponent::placeChordNotes(const std::vector<int>& pitches, juce::Point<int> pos)
{
    if (activeClip == nullptr || pitches.empty())
        return;
    if (! getVisibleGridViewport().contains(pos))
        return;

    OptionalAudioLock lk(audioEditLock);
    pushUndoSnapshot();

    const auto beat = snapBeat(xToBeat(static_cast<double>(pos.x)));
    int dropPitch = yToPitch(pos.y);
    if (scaleLockEnabled && ! isPitchInScale(dropPitch))
        dropPitch = snapPitchToScale(dropPitch);
    // Anchor the chord so its root lands on the drop lane, keeping the voicing intervals intact.
    const int offset = dropPitch - pitches.front();

    const auto maxLen = juce::jmax(minimumNoteLengthInBeats, getContentBeats() - beat);
    const auto len = juce::jlimit(minimumNoteLengthInBeats, maxLen, lastNoteLengthInBeats);

    selectedNotes.clear();
    for (int p : pitches)
    {
        const int np = juce::jlimit(lowestPitch, highestPitch, p + offset);
        activeClip->midiNotes.push_back(MidiNote { np, beat, len, 100 });
        selectedNotes.insert(static_cast<int>(activeClip->midiNotes.size()) - 1);
    }
    growClipToFit(beat + len);
    repaint();
}

bool MidiEditorOverlayComponent::isInterestedInDragSource(const SourceDetails& details)
{
    return details.description.toString() == "chord";
}

void MidiEditorOverlayComponent::itemDragMove(const SourceDetails& details)
{
    chordDropPreviewPos = getVisibleGridViewport().contains(details.localPosition)
                              ? std::optional<juce::Point<int>>(details.localPosition)
                              : std::nullopt;
    repaint();
}

void MidiEditorOverlayComponent::itemDragExit(const SourceDetails&)
{
    chordDropPreviewPos.reset();
    repaint();
}

void MidiEditorOverlayComponent::itemDropped(const SourceDetails& details)
{
    chordDropPreviewPos.reset();
    if (! pendingChordDrag.empty())
        placeChordNotes(pendingChordDrag, details.localPosition);
    pendingChordDrag.clear();
    repaint();
}

bool MidiEditorOverlayComponent::isTextInputFocused() const noexcept
{
    return dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr;
}

bool MidiEditorOverlayComponent::canStartGlobalSpacePreview() const noexcept
{
    return activeClip != nullptr
        && hoveredGridBeat.has_value()
        && hasKeyboardFocus(true)
        && ! isTextInputFocused();
}

bool MidiEditorOverlayComponent::startGlobalSpacePreviewFromMouse()
{
    // Already previewing → swallow the key-repeat (holding space fires keyPressed over and
    // over; without this guard each repeat restarted playback at the start, so it looked
    // stuck looping a tiny range).
    if (globalSpacePreviewActive)
        return true;

    if (! canStartGlobalSpacePreview())
        return false;

    const auto localBeat = juce::jlimit(0.0, activeClip->lengthInBeats, *hoveredGridBeat);
    const auto globalBeat = activeClip->startBeat + localBeat;

    if (onStartGlobalSpacePreview)
        onStartGlobalSpacePreview(globalBeat);

    globalSpacePreviewActive = true;
    repaint();
    return true;
}

bool MidiEditorOverlayComponent::startGlobalSpacePreviewHeld()
{
    if (globalSpacePreviewActive)
        return true;
    if (activeClip == nullptr || ! isShowing() || isTextInputFocused())
        return false;

    // Prefer the hovered beat; otherwise play from the current playhead.
    double globalBeat;
    if (hoveredGridBeat.has_value())
        globalBeat = activeClip->startBeat + juce::jlimit(0.0, activeClip->lengthInBeats, *hoveredGridBeat);
    else
        globalBeat = onRequestPlayheadBeat ? onRequestPlayheadBeat() : activeClip->startBeat;

    if (onStartGlobalSpacePreview)
        onStartGlobalSpacePreview(globalBeat);

    globalSpacePreviewActive = true;
    repaint();
    return true;
}

juce::Rectangle<int> MidiEditorOverlayComponent::getEditToolbarBounds() const noexcept
{
    auto topBar = getTopBarBounds().reduced(20, 12);
    topBar.removeFromRight(210);
    auto toolbar = topBar.removeFromBottom(28);
    toolbar.setLeft(getVisibleGridViewport().getX());
    return toolbar;
}

juce::Rectangle<int> MidiEditorOverlayComponent::getToolButtonBounds(int index) const noexcept
{
    auto toolbar = getEditToolbarBounds();
    constexpr int w = 28, h = 28, gap = 5;
    return juce::Rectangle<int>(toolbar.getX() + index * (w + gap),
                                toolbar.getCentreY() - h / 2,
                                w,
                                h);
}

const juce::MouseCursor& MidiEditorOverlayComponent::toolCursor(PianoRollTool tool)
{
    if (auto it = toolCursorCache.find(tool); it != toolCursorCache.end())
        return it->second;

    // Render the tool glyph into a 2x image, white with a dark outline for contrast on the dark grid.
    constexpr int logical = 24;
    constexpr float scale = 2.0f;
    juce::Image img(juce::Image::ARGB, (int) (logical * scale), (int) (logical * scale), true);
    {
        juce::Graphics g(img);
        g.addTransform(juce::AffineTransform::scale(scale));

        juce::Path p;
        int hotX = logical / 2, hotY = logical / 2;

        switch (tool)
        {
            case PianoRollTool::draw:
            {
                // Pencil pointing down-left; hotspot = the tip.
                p.startNewSubPath(17.0f, 3.0f);
                p.lineTo(21.0f, 7.0f);
                p.lineTo(8.0f, 20.0f);
                p.lineTo(4.0f, 16.0f);
                p.closeSubPath();
                juce::Path tip;
                tip.startNewSubPath(4.0f, 16.0f);
                tip.lineTo(8.0f, 20.0f);
                tip.lineTo(2.0f, 22.0f);
                tip.closeSubPath();
                p.addPath(tip);
                hotX = 2; hotY = 22;
                break;
            }
            case PianoRollTool::cut:
            {
                // Scissors: two blades + two finger loops; hotspot at the blade crossing.
                juce::Path blades;
                blades.startNewSubPath(4.0f, 4.0f);   blades.lineTo(19.0f, 15.0f);
                blades.startNewSubPath(19.0f, 4.0f);  blades.lineTo(4.0f, 15.0f);
                g.setColour(juce::Colours::black.withAlpha(0.85f));
                g.strokePath(blades, juce::PathStrokeType(3.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setColour(juce::Colours::white);
                g.strokePath(blades, juce::PathStrokeType(1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                auto loop = [&](float cx, float cy)
                {
                    g.setColour(juce::Colours::black.withAlpha(0.85f));
                    g.drawEllipse(cx - 3.2f, cy - 3.2f, 6.4f, 6.4f, 3.0f);
                    g.setColour(juce::Colours::white);
                    g.drawEllipse(cx - 3.2f, cy - 3.2f, 6.4f, 6.4f, 1.6f);
                };
                loop(5.0f, 18.0f);
                loop(18.0f, 18.0f);
                toolCursorCache.emplace(tool, juce::MouseCursor(juce::ScaledImage(img, scale), juce::Point<int>(11, 10)));
                return toolCursorCache.at(tool);
            }
            case PianoRollTool::erase:
            {
                // Eraser block (tilted); hotspot at the working corner.
                p.startNewSubPath(6.0f, 15.0f);
                p.lineTo(14.0f, 4.0f);
                p.lineTo(21.0f, 9.0f);
                p.lineTo(13.0f, 20.0f);
                p.closeSubPath();
                hotX = 8; hotY = 18;
                break;
            }
            default:
                // No custom glyph for this tool — signal the caller to fall back to a system cursor.
                toolCursorCache.emplace(tool, juce::MouseCursor(juce::MouseCursor::NormalCursor));
                return toolCursorCache.at(tool);
        }

        g.setColour(juce::Colours::black.withAlpha(0.85f));
        g.strokePath(p, juce::PathStrokeType(3.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(juce::Colours::white);
        g.fillPath(p);
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.strokePath(p, juce::PathStrokeType(0.8f));

        toolCursorCache.emplace(tool, juce::MouseCursor(juce::ScaledImage(img, scale), juce::Point<int>(hotX, hotY)));
    }
    return toolCursorCache.at(tool);
}

void MidiEditorOverlayComponent::paintEditToolbar(juce::Graphics& g)
{
    const auto toolbar = getEditToolbarBounds();
    if (toolbar.isEmpty())
        return;

    const auto toolForIndex = [](int index)
    {
        switch (index)
        {
            case 0:  return PianoRollTool::select;
            case 1:  return PianoRollTool::draw;
            case 2:  return PianoRollTool::cut;
            case 3:  return PianoRollTool::erase;
            case 4:  return PianoRollTool::quantize;
            case 5:  return PianoRollTool::velocity;
            case 6:  return PianoRollTool::slide;
            case 7:  return PianoRollTool::step;
            case 8:  return PianoRollTool::audition;
            default: return PianoRollTool::select;
        }
    };

    const auto mouse = getMouseXYRelative();
    int hoveredIndex = -1;
    for (int i = 0; i < pianoRollToolButtonCount; ++i)
        if (getToolButtonBounds(i).contains(mouse))
            hoveredIndex = i;

    // No wrapper panel — match the arrangement's tool row (individual button shells only).
    auto isActive = [&](PianoRollTool tool)
    {
        if (tool == PianoRollTool::slide)
            return slidePenEnabled;
        if (tool == PianoRollTool::step)
            return stepWriteEnabled;
        return currentTool == tool;
    };

    for (int i = 0; i < pianoRollToolButtonCount; ++i)
    {
        const auto tool = toolForIndex(i);
        const auto active = isActive(tool);
        const auto b = getToolButtonBounds(i).toFloat();
        const auto hover = hoveredIndex == i;
        // Selected tool uses the shared coral active state; idle icons stay quiet.
        const auto iconColour = active ? theme::accent::activeCoral.withAlpha(0.98f)
                                       : juce::Colours::white.withAlpha(0.72f);
        const auto accentColour = iconColour;
        const auto stroke = juce::PathStrokeType(1.75f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        g.setColour(hover ? theme::surface::primary.withAlpha(0.78f)
                          : theme::surface::primary.withAlpha(active ? 0.62f : 0.34f));
        g.fillRoundedRectangle(b, 4.0f);
        g.setColour(active ? theme::accent::activeCoral.withAlpha(0.72f) : juce::Colours::white.withAlpha(0.16f));
        g.drawRoundedRectangle(b, 4.0f, active ? 1.4f : 1.0f);
        g.setColour(iconColour);

        const auto cx = b.getCentreX();
        const auto cy = b.getCentreY();
        if (i == 0)
        {
            juce::Path p;
            const auto x = b.getX() + 8.0f;
            const auto y = b.getY() + 4.5f;
            p.startNewSubPath(x, y);
            p.lineTo(x + 1.1f, y + 17.0f);
            p.lineTo(x + 6.1f, y + 11.4f);
            p.lineTo(x + 9.6f, y + 17.0f);
            p.lineTo(x + 12.6f, y + 15.2f);
            p.lineTo(x + 9.1f, y + 10.1f);
            p.lineTo(x + 15.5f, y + 10.1f);
            p.closeSubPath();
            g.strokePath(p, stroke);
        }
        else if (i == 1)
        {
            juce::Path pen;
            pen.startNewSubPath(b.getX() + 8.0f, b.getBottom() - 5.0f);
            pen.lineTo(b.getRight() - 8.0f, b.getY() + 5.0f);
            pen.lineTo(b.getRight() - 4.5f, b.getY() + 8.5f);
            pen.lineTo(b.getX() + 11.5f, b.getBottom() - 3.0f);
            pen.closeSubPath();
            g.strokePath(pen, stroke);
            g.setColour(accentColour);
            g.drawLine(b.getX() + 7.0f, b.getBottom() - 3.0f, b.getX() + 13.0f, b.getBottom() - 5.7f, 1.8f);
        }
        else if (i == 2)
        {
            g.drawEllipse(cx - 11.0f, cy + 2.0f, 7.5f, 7.5f, 1.8f);
            g.drawEllipse(cx + 3.5f, cy + 2.0f, 7.5f, 7.5f, 1.8f);
            g.drawLine(cx - 5.0f, cy + 5.0f, cx + 1.0f, cy - 1.0f, 2.0f);
            g.drawLine(cx + 5.0f, cy + 5.0f, cx - 1.0f, cy - 1.0f, 2.0f);
            g.drawLine(cx - 1.5f, cy - 1.0f, cx - 9.0f, cy - 10.0f, 2.0f);
            g.drawLine(cx + 1.5f, cy - 1.0f, cx + 9.0f, cy - 10.0f, 2.0f);
            g.setColour(accentColour);
            g.fillEllipse(cx - 2.1f, cy - 2.1f, 4.2f, 4.2f);
        }
        else if (i == 3)
        {
            auto eraser = b.reduced(7.0f, 5.5f);
            juce::Path e;
            e.startNewSubPath(eraser.getX() + 4.0f, eraser.getBottom() - 1.0f);
            e.lineTo(eraser.getX(), eraser.getBottom() - 6.0f);
            e.lineTo(eraser.getRight() - 7.0f, eraser.getY());
            e.lineTo(eraser.getRight(), eraser.getY() + 6.0f);
            e.lineTo(eraser.getX() + 11.0f, eraser.getBottom() - 1.0f);
            e.closeSubPath();
            g.strokePath(e, stroke);
            g.setColour(accentColour);
            g.drawLine(eraser.getX() + 2.0f, eraser.getBottom() + 1.0f, eraser.getRight(), eraser.getBottom() + 1.0f, 1.8f);
        }
        else if (i == 4)
        {
            for (int n = 0; n < 4; ++n)
            {
                const auto x = b.getX() + 7.0f + static_cast<float>(n) * 4.3f;
                const auto h = 5.0f + static_cast<float>((n + 1) % 3) * 3.0f;
                g.fillRoundedRectangle(x, cy + 8.0f - h, 2.4f, h, 1.0f);
            }
            g.setColour(accentColour);
            g.drawLine(b.getX() + 6.0f, cy - 7.0f, b.getRight() - 6.0f, cy - 7.0f, 1.7f);
            g.drawLine(b.getX() + 6.0f, cy - 2.0f, b.getRight() - 6.0f, cy - 2.0f, 1.7f);
        }
        else if (i == 5)
        {
            g.drawLine(b.getX() + 8.0f, b.getBottom() - 6.0f, b.getX() + 8.0f, b.getY() + 7.0f, 1.7f);
            g.drawLine(b.getX() + 8.0f, b.getBottom() - 6.0f, b.getRight() - 6.0f, b.getBottom() - 6.0f, 1.7f);
            g.setColour(accentColour);
            g.fillRoundedRectangle(b.getX() + 12.0f, cy + 2.0f, 3.0f, 5.0f, 1.0f);
            g.fillRoundedRectangle(b.getX() + 17.0f, cy - 4.0f, 3.0f, 11.0f, 1.0f);
            g.fillRoundedRectangle(b.getX() + 22.0f, cy - 8.0f, 3.0f, 15.0f, 1.0f);
        }
        else if (i == 6)
        {
            juce::Path curve;
            curve.startNewSubPath(b.getX() + 6.0f, cy + 5.0f);
            curve.cubicTo(b.getX() + 11.0f, cy - 11.0f, b.getRight() - 13.0f, cy + 11.0f, b.getRight() - 6.0f, cy - 5.0f);
            g.strokePath(curve, stroke);
            g.setColour(accentColour);
            g.fillEllipse(b.getX() + 5.0f, cy + 3.0f, 4.0f, 4.0f);
            g.fillEllipse(b.getRight() - 8.0f, cy - 7.0f, 4.0f, 4.0f);
        }
        else if (i == 7)
        {
            for (int n = 0; n < 4; ++n)
            {
                const auto x = b.getX() + 7.0f + static_cast<float>(n) * 5.0f;
                g.drawLine(x, b.getY() + 6.0f, x, b.getBottom() - 5.0f, 1.5f);
            }
            g.setColour(accentColour);
            juce::Path arrow;
            arrow.addTriangle(b.getRight() - 10.0f, cy - 4.0f, b.getRight() - 10.0f, cy + 4.0f, b.getRight() - 4.0f, cy);
            g.fillPath(arrow);
        }
        else if (i == 8)
        {
            juce::Path ear;
            ear.startNewSubPath(cx + 4.0f, cy - 8.5f);
            ear.cubicTo(cx - 2.5f, cy - 13.0f, cx - 10.0f, cy - 6.5f, cx - 8.0f, cy);
            ear.cubicTo(cx - 6.0f, cy + 6.0f, cx - 1.0f, cy + 4.0f, cx, cy + 9.0f);
            g.strokePath(ear, stroke);
            g.drawLine(b.getX() + 6.0f, cy + 1.0f, b.getX() + 6.0f, cy + 7.0f, 1.5f);
            g.drawLine(b.getX() + 10.0f, cy - 3.0f, b.getX() + 10.0f, cy + 7.0f, 1.5f);
            g.setColour(accentColour);
            juce::Path play;
            play.addTriangle(b.getRight() - 7.0f, cy - 4.0f, b.getRight() - 7.0f, cy + 4.0f, b.getRight() - 1.5f, cy);
            g.fillPath(play);
        }
    }

    if (hoveredIndex >= 0)
    {
        const auto info = getPianoRollToolInfo(hoveredIndex);
        const auto text = juce::String(info.name) + "  " + juce::String(info.shortcut);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        const auto textW = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text);
        auto tip = juce::Rectangle<float>(0.0f, 0.0f, textW + 20.0f, 22.0f);
        const auto target = getToolButtonBounds(hoveredIndex);
        const auto topBar = getTopBarBounds().reduced(20, 12);
        tip.setCentre(static_cast<float>(target.getCentreX()), static_cast<float>(toolbar.getBottom() + 14));
        tip.setX(juce::jlimit(static_cast<float>(topBar.getX()),
                              static_cast<float>(topBar.getRight()) - tip.getWidth(),
                              tip.getX()));
        g.setColour(theme::core::voidBlack.withAlpha(0.48f));
        g.fillRoundedRectangle(tip.translated(0.0f, 1.5f), 6.0f);
        g.setColour(theme::surface::primary.withAlpha(0.96f));
        g.fillRoundedRectangle(tip, 6.0f);
        g.setColour(theme::line::normal.withAlpha(0.72f));
        g.drawRoundedRectangle(tip, 6.0f, 1.0f);
        g.setColour(theme::text::primary);
        g.drawText(text, tip.reduced(9.0f, 0.0f), juce::Justification::centred);
    }
}

void MidiEditorOverlayComponent::setPianoRollTool(PianoRollTool tool)
{
    currentTool = tool;
    if (tool != PianoRollTool::slide && slidePenEnabled)
    {
        slidePenEnabled = false;
        slidePenButton.setToggleState(false, juce::dontSendNotification);
    }
    if (tool != PianoRollTool::step && stepWriteEnabled)
    {
        stepWriteEnabled = false;
        stepWriteButton.setToggleState(false, juce::dontSendNotification);
        commitStepWritePendingChord();
    }

    repaint();
}

void MidiEditorOverlayComponent::activateToolByIndex(int index)
{
    switch (index)
    {
        case 0: setPianoRollTool(PianoRollTool::select); break;
        case 1: setPianoRollTool(PianoRollTool::draw); break;
        case 2: setPianoRollTool(PianoRollTool::cut); break;
        case 3: setPianoRollTool(PianoRollTool::erase); break;
        case 4: quantizeSelectedNotes(); currentTool = PianoRollTool::quantize; repaint(); break;
        case 5: setPianoRollTool(PianoRollTool::velocity); break;
        case 6:
            setPianoRollTool(PianoRollTool::slide);
            slidePenEnabled = true;
            slidePenButton.setToggleState(true, juce::dontSendNotification);
            stepWriteEnabled = false;
            stepWriteButton.setToggleState(false, juce::dontSendNotification);
            commitStepWritePendingChord();
            clearSelection();
            selectedSlide.reset();
            repaint();
            break;
        case 7:
            setPianoRollTool(PianoRollTool::step);
            stepWriteEnabled = true;
            stepWriteButton.setToggleState(true, juce::dontSendNotification);
            slidePenEnabled = false;
            slidePenButton.setToggleState(false, juce::dontSendNotification);
            repaint();
            break;
        case 8: setPianoRollTool(PianoRollTool::audition); break;
        default: break;
    }
}

bool MidiEditorOverlayComponent::handleEditToolbarClick(juce::Point<int> position)
{
    if (! getEditToolbarBounds().contains(position))
        return false;

    for (int i = 0; i < pianoRollToolButtonCount; ++i)
    {
        if (! getToolButtonBounds(i).contains(position))
            continue;

        activateToolByIndex(i);
        grabKeyboardFocus();
        return true;
    }

    return true;
}

bool MidiEditorOverlayComponent::splitNoteAtPoint(juce::Point<int> position)
{
    if (activeClip == nullptr)
        return false;
    OptionalAudioLock lk(audioEditLock);

    const auto hit = hitTestNote(position);
    if (! hit.has_value())
        return false;

    const auto index = hit->selected.noteIndex;
    if (index < 0 || index >= static_cast<int>(activeClip->midiNotes.size()))
        return false;

    auto note = activeClip->midiNotes[static_cast<std::size_t>(index)];
    const auto splitBeat = snapBeatNearest(xToBeat(static_cast<double>(position.x)));
    const auto noteEnd = note.startBeat + note.lengthInBeats;
    if (splitBeat <= note.startBeat + beatEpsilon || splitBeat >= noteEnd - beatEpsilon)
        return false;

    pushUndoSnapshot();
    auto right = note;
    note.lengthInBeats = splitBeat - note.startBeat;
    right.startBeat = splitBeat;
    right.lengthInBeats = noteEnd - splitBeat;
    activeClip->midiNotes[static_cast<std::size_t>(index)] = note;
    activeClip->midiNotes.insert(activeClip->midiNotes.begin() + index + 1, right);
    selectedNotes.clear();
    selectedNotes.insert(index + 1);
    repaint();
    return true;
}

bool MidiEditorOverlayComponent::auditionNoteAtPoint(juce::Point<int> position)
{
    if (activeClip == nullptr)
        return false;

    if (const auto hit = hitTestNote(position))
    {
        const auto index = hit->selected.noteIndex;
        if (index >= 0 && index < static_cast<int>(activeClip->midiNotes.size()))
        {
            const auto& note = activeClip->midiNotes[static_cast<std::size_t>(index)];
            auditionPlacedNote(note.pitch, note.velocity);
            selectSingleNote(index);
            return true;
        }
    }

    if (getKeyboardBounds().contains(position))
    {
        setMousePreviewPitch(keyboardPitchForPoint(position));
        return true;
    }

    return false;
}

void MidiEditorOverlayComponent::stopGlobalSpacePreview()
{
    if (! globalSpacePreviewActive)
        return;

    globalSpacePreviewActive = false;
    if (onStopGlobalSpacePreview)
        onStopGlobalSpacePreview();
    repaint();
}

juce::Rectangle<int> MidiEditorOverlayComponent::getTopBarBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(24);
    return bounds.removeFromTop(topBarHeightPx);
}

juce::Rectangle<int> MidiEditorOverlayComponent::getKeyboardBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(24);
    bounds.removeFromTop(topBarHeightPx);
    bounds.removeFromBottom(velocityLaneHeightPx + 10);
    return bounds.removeFromLeft(92);
}

juce::Rectangle<int> MidiEditorOverlayComponent::getVisibleGridViewport() const noexcept
{
    auto bounds = getLocalBounds().reduced(24);
    bounds.removeFromTop(topBarHeightPx);
    bounds.removeFromBottom(velocityLaneHeightPx + 10);
    bounds.removeFromLeft(92);
    return bounds;
}

juce::Rectangle<int> MidiEditorOverlayComponent::getVelocityLaneBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(24);
    bounds.removeFromTop(topBarHeightPx);
    auto velocityArea = bounds.removeFromBottom(velocityLaneHeightPx);
    velocityArea.removeFromLeft(92);
    return velocityArea;
}

juce::Rectangle<int> MidiEditorOverlayComponent::getGridBounds() const noexcept
{
    auto bounds = getVisibleGridViewport();
    const auto fullWidth = bounds.getWidth();
    const auto zoomedWidth = fullWidth;
    const auto fullHeight = bounds.getHeight();
    const auto naturalHeight = getDisplayedLaneCount() * baseLaneHeightPx;
    const auto zoomedHeight = juce::jmax(fullHeight, static_cast<int>(std::round(static_cast<double>(naturalHeight) * verticalZoom)));
    // NOTE: horizontal scroll is applied by the drawing/hit-test code itself (it
    // subtracts scrollX), so we must NOT also translate X here — doing both made
    // scrollX count twice, which broke zoom-to-cursor when scrolled. Y is applied
    // only here, so it keeps its translation.
    return bounds.withWidth(zoomedWidth).withHeight(zoomedHeight).translated(0, -static_cast<int>(std::round(scrollY)));
}

double MidiEditorOverlayComponent::getPixelsPerBeat() const noexcept
{
    const auto visible = getVisibleGridViewport();
    return (static_cast<double>(visible.getWidth()) * horizontalZoom) / juce::jmax(1.0, getContentBeats());
}

double MidiEditorOverlayComponent::getVisibleBeatRange() const noexcept
{
    const auto contentBeats = getContentBeats();
    return juce::jmax(contentBeats, juce::jmax(1.0, contentBeats) / juce::jmax(minimumHorizontalZoom, horizontalZoom));
}

double MidiEditorOverlayComponent::getContentBeats() const noexcept
{
    // Clip length + headroom so there's always empty grid after the clip to keep writing into.
    const auto clipLength = activeClip != nullptr ? activeClip->lengthInBeats : 8.0;
    return juce::jmax(1.0, clipLength) + editingHeadroomBeats;
}

// getPixelsPerBeat() divides the view width by getContentBeats(), so changing the clip length
// (grow/shrink to fit) would silently rescale the whole grid — the piano roll appeared to zoom
// in/out while you were only moving or deleting notes. Rescale horizontalZoom by the same ratio
// so pixels-per-beat is unchanged: the clip length only affects the scrollable extent, not zoom.
void MidiEditorOverlayComponent::preserveHorizontalScaleAcross(double oldContentBeats)
{
    const auto newContentBeats = getContentBeats();
    if (oldContentBeats <= 0.0 || newContentBeats <= 0.0)
        return;
    const auto ratio = newContentBeats / oldContentBeats;
    horizontalZoom       = juce::jlimit(minimumHorizontalZoom, maximumHorizontalZoom, horizontalZoom * ratio);
    targetHorizontalZoom = juce::jlimit(minimumHorizontalZoom, maximumHorizontalZoom, targetHorizontalZoom * ratio);
}

void MidiEditorOverlayComponent::growClipToFit(double endBeat)
{
    if (activeClip == nullptr || endBeat <= activeClip->lengthInBeats + beatEpsilon)
        return;
    const auto oldContentBeats = getContentBeats();
    constexpr double barBeats = 4.0;   // visual bar (matches the grid)
    activeClip->lengthInBeats = std::ceil((endBeat - beatEpsilon) / barBeats) * barBeats;
    preserveHorizontalScaleAcross(oldContentBeats);
    clampScrollOffsets();
}

void MidiEditorOverlayComponent::shrinkClipToFitNotes()
{
    if (activeClip == nullptr)
        return;
    constexpr double barBeats = 4.0;   // one visual bar = minimum pattern length
    double maxEnd = 0.0;
    for (const auto& n : activeClip->midiNotes)
        maxEnd = juce::jmax(maxEnd, n.startBeat + juce::jmax(0.01, n.lengthInBeats));
    // Snap up to the next bar; never below one bar. Only shrink (growth stays with growClipToFit),
    // so deleting a stray long note pulls the clip — and the loop — back to the real content.
    const auto fitted = juce::jmax(barBeats, std::ceil((maxEnd - beatEpsilon) / barBeats) * barBeats);
    if (fitted < activeClip->lengthInBeats - beatEpsilon)
    {
        const auto oldContentBeats = getContentBeats();
        activeClip->lengthInBeats = fitted;
        preserveHorizontalScaleAcross(oldContentBeats);
        clampScrollOffsets();
    }
}

juce::Rectangle<int> MidiEditorOverlayComponent::getNoteBounds(const MidiNote& note) const noexcept
{
    const auto grid = getGridBounds();
    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto pixelsPerBeat = getPixelsPerBeat();
    const auto lane = pitchToLane(note.pitch);
    return juce::Rectangle<int>(
        grid.getX() + static_cast<int>(std::round((note.startBeat * pixelsPerBeat) - scrollX)) + 1,
        grid.getY() + static_cast<int>(std::round(lane * laneHeight)) + 3,
        juce::jmax(minimumNoteWidthPx, static_cast<int>(std::round(note.lengthInBeats * pixelsPerBeat)) - 2),
        juce::jmax(10, static_cast<int>(std::round(laneHeight)) - 6));
}

std::optional<MidiEditorOverlayComponent::NoteHit> MidiEditorOverlayComponent::hitTestNote(juce::Point<int> position) const
{
    if (activeClip == nullptr)
        return std::nullopt;

    for (int noteIndex = static_cast<int>(activeClip->midiNotes.size()) - 1; noteIndex >= 0; --noteIndex)
    {
        if (focusModeEnabled && ! isPitchInScale(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].pitch))
            continue;
        const auto bounds = getNoteBounds(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)]);
        if (! bounds.contains(position))
            continue;

        return NoteHit {
            SelectedNote { noteIndex },
            bounds,
            position.x >= bounds.getRight() - resizeHandleWidth
        };
    }

    return std::nullopt;
}

std::optional<MidiEditorOverlayComponent::SlideHit> MidiEditorOverlayComponent::hitTestSlide(juce::Point<int> position) const
{
    if (activeClip == nullptr)
        return std::nullopt;

    const auto grid = getGridBounds();
    const auto pixelsPerBeat = getPixelsPerBeat();
    const auto pointFor = [&](const PitchSlidePoint& point)
    {
        return juce::Point<float>(
            static_cast<float>(grid.getX() + (point.beat * pixelsPerBeat) - scrollX),
            pitchToCentreY(point.pitch));
    };

    for (int slideIndex = static_cast<int>(activeClip->pitchSlides.size()) - 1; slideIndex >= 0; --slideIndex)
    {
        const auto& slide = activeClip->pitchSlides[static_cast<std::size_t>(slideIndex)];
        if (! shouldDrawSlide(slide, slideIndex, false))
            continue;

        // Curve grips (segment midpoints) — only on the selected slide, take priority.
        if (selectedSlide == slideIndex || slideTouchesSelectedNotes(slide))
        {
            for (std::size_t i = 1; i < slide.points.size(); ++i)
            {
                const auto& a = slide.points[i - 1];
                const auto& b = slide.points[i];
                if (std::abs(b.beat - a.beat) < 1.0e-4)
                    continue;
                const auto midBeat  = (a.beat + b.beat) * 0.5;
                const auto midPitch = a.pitch + (b.pitch - a.pitch) * pitchSlideCurveShape(0.5, a.curve);
                const juce::Point<float> h(static_cast<float>(grid.getX() + (midBeat * pixelsPerBeat) - scrollX),
                                           pitchToCentreY(midPitch));
                if (h.getDistanceFrom(position.toFloat()) <= slideHandleRadiusPx + 8.0f)   // easier to grab
                    return SlideHit { slideIndex, SlideEditMode::curve, static_cast<int>(i - 1) };
            }
        }

        if (! slide.points.empty())
        {
            const auto start = pointFor(slide.points.front());
            const auto end = pointFor(slide.points.back());
            if (start.getDistanceFrom(position.toFloat()) <= slideHandleRadiusPx + 4.0f)
                return SlideHit { slideIndex, SlideEditMode::resizeStart };
            if (end.getDistanceFrom(position.toFloat()) <= slideHandleRadiusPx + 4.0f)
                return SlideHit { slideIndex, SlideEditMode::resizeEnd };
        }

        for (std::size_t i = 1; i < slide.points.size(); ++i)
        {
            const auto a = pointFor(slide.points[i - 1]);
            const auto b = pointFor(slide.points[i]);
            const auto ab = b - a;
            const auto ap = position.toFloat() - a;
            const auto denom = juce::jmax(0.0001f, ab.getDistanceSquaredFromOrigin());
            const auto t = juce::jlimit(0.0f, 1.0f, (ap.x * ab.x + ap.y * ab.y) / denom);
            const auto closest = a + ab * t;
            if (closest.getDistanceFrom(position.toFloat()) <= 7.0f)
                return SlideHit { slideIndex, SlideEditMode::move };
        }
    }

    return std::nullopt;
}

juce::Rectangle<int> MidiEditorOverlayComponent::getVelocityBarBounds(int noteIndex) const noexcept
{
    const auto velocityLane = getVelocityLaneBounds().reduced(10, 10);
    const auto note = activeClip->midiNotes[static_cast<std::size_t>(noteIndex)];
    const auto grid = getGridBounds();
    const auto pixelsPerBeat = getPixelsPerBeat();
    // Narrow column anchored at the note START (FL/Ableton-style velocity stalk) — easy to grab and
    // adjust one note at a time, instead of a wide bar spanning the whole note that overlaps neighbours.
    constexpr int barWidth = 7;
    const auto x = grid.getX() + static_cast<int>(std::round((note.startBeat * pixelsPerBeat) - scrollX)) + 1;
    const auto normalisedVelocity = juce::jlimit(0.0, 1.0, static_cast<double>(note.velocity) / 127.0);
    const auto height = juce::jmax(8, static_cast<int>(std::round(normalisedVelocity * static_cast<double>(velocityLane.getHeight() - 20))));

    return juce::Rectangle<int>(x, velocityLane.getBottom() - height, barWidth, height).getIntersection(getVelocityLaneBounds().reduced(8, 8));
}

std::optional<int> MidiEditorOverlayComponent::hitTestVelocityBar(juce::Point<int> position) const
{
    if (activeClip == nullptr)
        return std::nullopt;

    // Grab target = the whole vertical column above the narrow stalk (padded a little sideways), so a
    // thin bar — or a very low-velocity short one — is still easy to click and drag.
    const auto lane = getVelocityLaneBounds().reduced(8, 8);
    for (int noteIndex = static_cast<int>(activeClip->midiNotes.size()) - 1; noteIndex >= 0; --noteIndex)
    {
        if (focusModeEnabled && ! isPitchInScale(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].pitch))
            continue;
        const auto bar = getVelocityBarBounds(noteIndex);
        const auto column = juce::Rectangle<int>(bar.getX() - 3, lane.getY(), bar.getWidth() + 6, lane.getHeight());
        if (column.contains(position))
            return noteIndex;
    }

    return std::nullopt;
}

std::set<int> MidiEditorOverlayComponent::hitTestNotesInRect(juce::Rectangle<int> selection) const
{
    std::set<int> hits;

    if (activeClip == nullptr)
        return hits;

    for (int noteIndex = 0; noteIndex < static_cast<int>(activeClip->midiNotes.size()); ++noteIndex)
    {
        if (focusModeEnabled && ! isPitchInScale(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].pitch))
            continue;
        if (selection.intersects(getNoteBounds(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)])))
            hits.insert(noteIndex);
    }

    return hits;
}

int MidiEditorOverlayComponent::yToPitch(int y) const noexcept
{
    const auto grid = getGridBounds();
    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto lane = juce::jlimit(0, getDisplayedLaneCount() - 1, static_cast<int>((y - grid.getY()) / laneHeight));
    return laneIndexToPitch(lane);
}

double MidiEditorOverlayComponent::yToContinuousPitch(double y) const noexcept
{
    const auto grid = getGridBounds();
    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto lane = ((y - static_cast<double>(grid.getY())) / laneHeight) - 0.5;
    return juce::jlimit(static_cast<double>(lowestPitch),
                        static_cast<double>(highestPitch),
                        static_cast<double>(highestPitch) - lane);
}

float MidiEditorOverlayComponent::pitchToCentreY(double pitch) const noexcept
{
    const auto grid = getGridBounds();
    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto clampedPitch = juce::jlimit(static_cast<double>(lowestPitch),
                                          static_cast<double>(highestPitch),
                                          pitch);
    const auto lane = static_cast<double>(highestPitch) - clampedPitch;
    return static_cast<float>(static_cast<double>(grid.getY()) + lane * laneHeight + laneHeight * 0.5);
}

int MidiEditorOverlayComponent::laneIndexToPitch(int laneIndex) const noexcept
{
    laneIndex = juce::jlimit(0, getDisplayedLaneCount() - 1, laneIndex);
    return highestPitch - laneIndex;
}

double MidiEditorOverlayComponent::xToBeat(double x) const noexcept
{
    const auto grid = getGridBounds();
    const auto pixelsPerBeat = getPixelsPerBeat();
    return juce::jmax(0.0, (x - static_cast<double>(grid.getX()) + scrollX) / juce::jmax(1.0, pixelsPerBeat));
}

double MidiEditorOverlayComponent::snapBeat(double beat) const noexcept
{
    return std::floor(beat / snapSizeInBeats) * snapSizeInBeats;
}

double MidiEditorOverlayComponent::snapBeatNearest(double beat) const noexcept
{
    return std::round(beat / snapSizeInBeats) * snapSizeInBeats;
}

int MidiEditorOverlayComponent::pitchToLane(int pitch) const noexcept
{
    return juce::jlimit(0, totalPitchCount - 1, highestPitch - pitch);
}

int MidiEditorOverlayComponent::getDisplayedLaneCount() const noexcept
{
    return totalPitchCount;
}

bool MidiEditorOverlayComponent::isPitchInScale(int pitch) const noexcept
{
    const int pitchClass = ((pitch % 12) + 12) % 12;
    const int scaleRelative = (pitchClass - scaleRoot + 12) % 12;
    const auto& pattern = scalePatterns[static_cast<std::size_t>(scalePatternIndex)].pitchClasses;
    return std::find(pattern.begin(), pattern.end(), scaleRelative) != pattern.end();
}

// Snap a chromatic pitch to the closest in-scale pitch. Ties prefer the lower one
// (musically the diatonic neighbour below is usually a safer guess).
int MidiEditorOverlayComponent::snapPitchToScale(int pitch) const noexcept
{
    if (isPitchInScale(pitch)) return pitch;
    for (int dist = 1; dist <= 6; ++dist)
    {
        if (isPitchInScale(pitch - dist)) return pitch - dist;
        if (isPitchInScale(pitch + dist)) return pitch + dist;
    }
    return pitch;
}

bool MidiEditorOverlayComponent::isBlackKey(int pitch) const noexcept
{
    const int pitchClass = ((pitch % 12) + 12) % 12;
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;
}

bool MidiEditorOverlayComponent::isNoteSelected(int noteIndex) const noexcept
{
    return selectedNotes.contains(noteIndex);
}

void MidiEditorOverlayComponent::selectSingleNote(int noteIndex)
{
    selectedNotes.clear();
    selectedNotes.insert(noteIndex);
    // FL-style: clicking a note adopts its length, so the next note you draw matches it.
    if (activeClip != nullptr && noteIndex >= 0 && noteIndex < static_cast<int>(activeClip->midiNotes.size()))
        lastNoteLengthInBeats = juce::jmax(minimumNoteLengthInBeats,
                                           activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].lengthInBeats);
}

void MidiEditorOverlayComponent::transposeSelectedNotes(int semitones)
{
    if (activeClip == nullptr || selectedNotes.empty() || semitones == 0)
        return;

    OptionalAudioLock lk(audioEditLock);
    pushUndoSnapshot();

    // Remember the original pitch/start pairs so pitch slides remain attached to the
    // notes they belong to after the note values are changed.
    std::vector<std::pair<int, double>> movedNotes;
    movedNotes.reserve(selectedNotes.size());
    for (const auto index : selectedNotes)
    {
        if (index < 0 || index >= static_cast<int>(activeClip->midiNotes.size()))
            continue;
        const auto& note = activeClip->midiNotes[static_cast<std::size_t>(index)];
        movedNotes.emplace_back(note.pitch, note.startBeat);
        auto& moved = activeClip->midiNotes[static_cast<std::size_t>(index)];
        moved.pitch = juce::jlimit(0, 127, moved.pitch + semitones);
    }

    for (auto& slide : activeClip->pitchSlides)
    {
        const auto belongsToMovedNote = std::any_of(movedNotes.begin(), movedNotes.end(),
            [&slide](const auto& source)
            {
                return slide.sourcePitch == source.first
                    && std::abs(slide.sourceNoteStartBeat - source.second) < 0.0001;
            });
        if (! belongsToMovedNote)
            continue;

        slide.sourcePitch = juce::jlimit(0, 127, slide.sourcePitch + semitones);
        for (auto& point : slide.points)
            point.pitch = juce::jlimit(0.0, 127.0, point.pitch + static_cast<double>(semitones));
    }

    repaint();
}

void MidiEditorOverlayComponent::duplicateSelectedNotes()
{
    if (activeClip == nullptr || selectedNotes.empty())
        return;
    OptionalAudioLock lk(audioEditLock);

    pushUndoSnapshot();

    std::vector<int> indices(selectedNotes.begin(), selectedNotes.end());
    double minStart = std::numeric_limits<double>::max();
    double maxEnd = 0.0;

    for (const auto index : indices)
    {
        const auto& note = activeClip->midiNotes[static_cast<std::size_t>(index)];
        minStart = juce::jmin(minStart, note.startBeat);
        maxEnd = juce::jmax(maxEnd, note.startBeat + note.lengthInBeats);
    }

    const auto offset = juce::jmax(snapSizeInBeats, snapBeat(maxEnd - minStart));
    std::vector<int> duplicatedIndices;

    for (const auto index : indices)
    {
        auto note = activeClip->midiNotes[static_cast<std::size_t>(index)];
        note.startBeat = juce::jlimit(0.0, activeClip->lengthInBeats - note.lengthInBeats, note.startBeat + offset);
        activeClip->midiNotes.push_back(note);
        duplicatedIndices.push_back(static_cast<int>(activeClip->midiNotes.size()) - 1);
    }

    selectedNotes.clear();
    selectedNotes.insert(duplicatedIndices.begin(), duplicatedIndices.end());
    repaint();
}

void MidiEditorOverlayComponent::joinSelectedNotes()
{
    if (activeClip == nullptr || selectedNotes.size() < 2)
        return;
    OptionalAudioLock lk(audioEditLock);

    // Selected notes in time order.
    std::vector<MidiNote> sel;
    for (const auto idx : selectedNotes)
        if (idx >= 0 && idx < static_cast<int>(activeClip->midiNotes.size()))
            sel.push_back(activeClip->midiNotes[static_cast<std::size_t>(idx)]);
    std::sort(sel.begin(), sel.end(),
              [](const MidiNote& a, const MidiNote& b) { return a.startBeat < b.startBeat; });

    // Group notes that start together into chords (one trigger position each).
    constexpr double eps = 0.0001;
    std::vector<std::vector<MidiNote>> chords;
    for (const auto& n : sel)
    {
        if (chords.empty() || std::abs(n.startBeat - chords.back().front().startBeat) > eps)
            chords.push_back({ n });
        else
            chords.back().push_back(n);
    }
    if (chords.size() < 2)
        return;  // need at least two time positions to glide between

    // Voice-lead: sort each chord low→high and pair by rank across chords.
    for (auto& c : chords)
        std::sort(c.begin(), c.end(), [](const MidiNote& a, const MidiNote& b) { return a.pitch < b.pitch; });
    std::size_t voices = 0;
    for (const auto& c : chords)
        voices = juce::jmax(voices, c.size());
    if (voices == 0)
        return;

    pushUndoSnapshot();

    // Drop the original selected notes.
    std::vector<MidiNote> kept;
    kept.reserve(activeClip->midiNotes.size());
    for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
        if (! selectedNotes.contains(i))
            kept.push_back(activeClip->midiNotes[static_cast<std::size_t>(i)]);

    // One gliding voice (merged note + slide) per voice rank.
    for (std::size_t v = 0; v < voices; ++v)
    {
        std::vector<MidiNote> voiceNotes;
        for (const auto& c : chords)
            voiceNotes.push_back(c[juce::jmin(v, c.size() - 1)]);  // extra voices clamp to top

        PitchSlide slide;
        for (const auto& n : voiceNotes)
        {
            const auto end = n.startBeat + juce::jmax(0.01, n.lengthInBeats);
            slide.points.push_back({ n.startBeat, static_cast<double>(n.pitch) });
            slide.points.push_back({ end,         static_cast<double>(n.pitch) });
        }
        slide.sourcePitch = voiceNotes.front().pitch;
        slide.sourceNoteStartBeat = voiceNotes.front().startBeat;

        MidiNote merged = voiceNotes.front();
        merged.lengthInBeats = (voiceNotes.back().startBeat + juce::jmax(0.01, voiceNotes.back().lengthInBeats))
                               - voiceNotes.front().startBeat;
        kept.push_back(merged);
        activeClip->pitchSlides.push_back(slide);
    }

    activeClip->midiNotes = std::move(kept);
    clearSelection();
    selectedSlide = static_cast<int>(activeClip->pitchSlides.size()) - 1;
    repaint();
}

void MidiEditorOverlayComponent::splitSelectedSlide()
{
    if (activeClip == nullptr || ! selectedSlide.has_value())
        return;
    OptionalAudioLock lk(audioEditLock);
    const auto si = *selectedSlide;
    if (si < 0 || si >= static_cast<int>(activeClip->pitchSlides.size()))
        return;

    pushUndoSnapshot();
    const auto slide = activeClip->pitchSlides[static_cast<std::size_t>(si)];

    // Reconstruct notes from the slide's hold/glide point pairs.
    std::vector<MidiNote> rebuilt;
    for (std::size_t i = 0; i + 1 < slide.points.size(); i += 2)
    {
        MidiNote n;
        n.startBeat = slide.points[i].beat;
        n.lengthInBeats = juce::jmax(0.05, slide.points[i + 1].beat - slide.points[i].beat);
        n.pitch = juce::jlimit(0, 127, static_cast<int>(std::round(slide.points[i].pitch)));
        n.velocity = 100;
        rebuilt.push_back(n);
    }

    // Drop the merged voice that owned this slide.
    std::vector<MidiNote> kept;
    bool removedSource = false;
    for (const auto& n : activeClip->midiNotes)
    {
        if (! removedSource && n.pitch == slide.sourcePitch
            && std::abs(n.startBeat - slide.sourceNoteStartBeat) < 0.0001)
        {
            removedSource = true;
            continue;
        }
        kept.push_back(n);
    }
    activeClip->midiNotes = std::move(kept);
    activeClip->pitchSlides.erase(activeClip->pitchSlides.begin() + si);

    for (const auto& n : rebuilt)
        activeClip->midiNotes.push_back(n);

    selectedSlide.reset();
    clearSelection();
    repaint();
}

void MidiEditorOverlayComponent::deleteSelectedNotes()
{
    if (activeClip == nullptr || selectedNotes.empty())
        return;

    OptionalAudioLock lk(audioEditLock);
    pushUndoSnapshot();

    std::vector<MidiNote> keptNotes;
    keptNotes.reserve(activeClip->midiNotes.size());

    for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
    {
        if (! selectedNotes.contains(i))
            keptNotes.push_back(activeClip->midiNotes[static_cast<std::size_t>(i)]);
    }

    activeClip->midiNotes = std::move(keptNotes);
    shrinkClipToFitNotes();   // deleting a long note pulls the clip/loop back to the real content
    clearSelection();
    hoverNote.reset();
    noteDragState.reset();
    repaint();
}

void MidiEditorOverlayComponent::deleteSelectedSlide()
{
    OptionalAudioLock lk(audioEditLock);
    if (activeClip == nullptr || ! selectedSlide.has_value())
        return;

    const auto index = *selectedSlide;
    if (index < 0 || index >= static_cast<int>(activeClip->pitchSlides.size()))
        return;

    pushUndoSnapshot();
    activeClip->pitchSlides.erase(activeClip->pitchSlides.begin() + index);
    selectedSlide.reset();
    repaint();
}

void MidiEditorOverlayComponent::showSlidePresetMenu()
{
    const bool haveNotes = activeClip != nullptr && ! selectedNotes.empty();
    const bool haveSlide = activeClip != nullptr && selectedSlide.has_value();

    juce::PopupMenu menu;
    orion::ui::stylePopupMenu(menu);
    menu.addSectionHeader("Pitch curve (on selected notes)");
    menu.addItem(1, "Scoop up into note", haveNotes);
    menu.addItem(2, "Slide down into note", haveNotes);
    menu.addItem(3, "Fall (drop at end)", haveNotes);
    menu.addItem(4, "Bend up", haveNotes);
    menu.addItem(5, "Dip & recover", haveNotes);
    menu.addItem(6, "Vibrato (whole note)", haveNotes);

    juce::PopupMenu depth;
    orion::ui::stylePopupMenu(depth);
    depth.addItem(20, "Subtle (±0.3 st)", haveSlide);
    depth.addItem(21, "Medium (±0.7 st)", haveSlide);
    depth.addItem(22, "Wide (±1.5 st)", haveSlide);
    menu.addSubMenu("Vibrato depth", depth, haveSlide);

    juce::PopupMenu rate;
    orion::ui::stylePopupMenu(rate);
    rate.addItem(30, "Slow (2 / beat)", haveSlide);
    rate.addItem(31, "Medium (4 / beat)", haveSlide);
    rate.addItem(32, "Fast (6 / beat)", haveSlide);
    rate.addItem(33, "Very fast (8 / beat)", haveSlide);
    menu.addSubMenu("Vibrato rate", rate, haveSlide);

    menu.addSeparator();
    menu.addItem(9, "Clear slide", haveSlide);

    juce::Component::SafePointer<MidiEditorOverlayComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetsButton),
        [safeThis](int result)
        {
            if (safeThis == nullptr || result == 0)
                return;
            if (result >= 1 && result <= 6)       safeThis->applySlidePreset(result);
            else if (result == 20)                safeThis->setSelectedSlideVibrato(0.3, -1.0, 1);
            else if (result == 21)                safeThis->setSelectedSlideVibrato(0.7, -1.0, 1);
            else if (result == 22)                safeThis->setSelectedSlideVibrato(1.5, -1.0, 1);
            else if (result == 30)                safeThis->setSelectedSlideVibrato(-1.0, 2.0, 1);
            else if (result == 31)                safeThis->setSelectedSlideVibrato(-1.0, 4.0, 1);
            else if (result == 32)                safeThis->setSelectedSlideVibrato(-1.0, 6.0, 1);
            else if (result == 33)                safeThis->setSelectedSlideVibrato(-1.0, 8.0, 1);
            else if (result == 9)                 safeThis->deleteSelectedSlide();
        });
}

void MidiEditorOverlayComponent::applySlidePreset(int presetId)
{
    if (activeClip == nullptr || selectedNotes.empty())
        return;

    pushUndoSnapshot();

    struct NoteRef { int pitch; double start; double len; };
    std::vector<NoteRef> notes;
    for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
        if (selectedNotes.count(i) > 0)
        {
            const auto& n = activeClip->midiNotes[static_cast<std::size_t>(i)];
            notes.push_back({ n.pitch, n.startBeat, juce::jmax(0.01, n.lengthInBeats) });
        }

    const auto pt = [](double b, double pitch) { return PitchSlidePoint { b, pitch }; };

    for (const auto& nt : notes)
    {
        // Replace any existing slide for this note so re-applying a preset doesn't stack.
        auto& slides = activeClip->pitchSlides;
        slides.erase(std::remove_if(slides.begin(), slides.end(), [&](const PitchSlide& s) {
            return s.sourcePitch == nt.pitch && std::abs(s.sourceNoteStartBeat - nt.start) < 1.0e-4;
        }), slides.end());

        const double p = nt.pitch, s = nt.start, L = nt.len, e = s + L;
        PitchSlide sl;
        sl.sourcePitch = nt.pitch;
        sl.sourceNoteStartBeat = s;

        switch (presetId)
        {
            case 1: // scoop up into note
                sl.points = { pt(s, p - 2.0), pt(s + L * 0.30, p), pt(e, p) };
                sl.points[0].curve = 0.5;
                break;
            case 2: // slide down into note
                sl.points = { pt(s, p + 2.0), pt(s + L * 0.30, p), pt(e, p) };
                sl.points[0].curve = 0.5;
                break;
            case 3: // fall: hold then drop a fifth at the end
                sl.points = { pt(s, p), pt(s + L * 0.6, p), pt(e, p - 7.0) };
                sl.points[1].curve = -0.4;
                break;
            case 4: // bend up across the note
                sl.points = { pt(s, p), pt(e, p + 2.0) };
                break;
            case 5: // dip & recover
                sl.points = { pt(s, p), pt(s + L * 0.20, p - 1.5), pt(s + L * 0.45, p), pt(e, p) };
                break;
            case 6: // vibrato across the whole note
                sl.points = { pt(s, p), pt(e, p) };
                for (auto& q : sl.points) { q.lfoShape = 1; q.lfoDepth = 0.7; q.lfoRate = 5.0; }
                break;
            default:
                continue;
        }

        slides.push_back(std::move(sl));
    }

    clearSelection();
    if (! activeClip->pitchSlides.empty())
        selectedSlide = static_cast<int>(activeClip->pitchSlides.size()) - 1;
    repaint();
}

void MidiEditorOverlayComponent::setSelectedSlideVibrato(double depthSemitones, double rateCyclesPerBeat, int shape)
{
    if (activeClip == nullptr || ! selectedSlide.has_value())
        return;
    const auto si = *selectedSlide;
    if (si < 0 || si >= static_cast<int>(activeClip->pitchSlides.size()))
        return;

    pushUndoSnapshot();
    auto& sl = activeClip->pitchSlides[static_cast<std::size_t>(si)];
    for (auto& q : sl.points)
    {
        if (q.lfoShape == 0)                       q.lfoShape = shape > 0 ? shape : 1;   // turn vibrato on
        if (depthSemitones >= 0.0)                 q.lfoDepth = depthSemitones;
        else if (q.lfoDepth == 0.0)                q.lfoDepth = 0.7;                      // sensible default
        if (rateCyclesPerBeat > 0.0)               q.lfoRate = rateCyclesPerBeat;
        else if (q.lfoRate <= 0.0)                 q.lfoRate = 4.0;
    }

    static const char* names[] = { "Mod", "Sine", "Tri", "Saw", "Sqr" };
    const auto shapeIdx = juce::jlimit(0, 4, sl.points.empty() ? 0 : sl.points.front().lfoShape);
    modButton.setButtonText(names[shapeIdx]);
    repaint();
}

void MidiEditorOverlayComponent::quantizeSelectedNotes()
{
    if (activeClip == nullptr || activeClip->midiNotes.empty())
        return;

    // Simple Ableton-style quick quantize:
    //   * Grid     — current Snap value
    //   * Start    — pulled to the NEAREST grid line (round, not floor)
    //   * Length   — preserved; end rides with start
    //   * Target   — selected notes; if no selection, ALL notes in the clip.
    const auto snap    = juce::jmax(0.001, snapSizeInBeats);
    const auto clipLen = activeClip->lengthInBeats;
    const bool quantizeAll = selectedNotes.empty();

    auto computeNewStart = [&](const MidiNote& n)
    {
        const auto snapped = std::round(n.startBeat / snap) * snap;
        const auto maxStart = juce::jmax(0.0, clipLen - n.lengthInBeats);
        return juce::jlimit(0.0, maxStart, snapped);
    };

    bool anyChange = false;
    for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
    {
        if (! quantizeAll && ! selectedNotes.contains(i))
            continue;
        const auto& n = activeClip->midiNotes[static_cast<std::size_t>(i)];
        if (std::abs(computeNewStart(n) - n.startBeat) > 1.0e-6)
        {
            anyChange = true;
            break;
        }
    }
    if (! anyChange)
        return;

    pushUndoSnapshot();

    for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
    {
        if (! quantizeAll && ! selectedNotes.contains(i))
            continue;
        auto& n = activeClip->midiNotes[static_cast<std::size_t>(i)];
        n.startBeat = computeNewStart(n);
    }

    repaint();
}

void MidiEditorOverlayComponent::updateSubtitle()
{
    scaleButton.setButtonText(getScaleName());
    // Prefix so the two division menus aren't confused: "Grid" = snap/placement grid,
    // "Len" = Step Write note length. They can be set independently (e.g. 1/16 grid, 1/32 notes).
    snapButton.setButtonText("Grid " + getSnapName());
    const auto stepText = [this]
    {
        if (std::abs(stepWriteStepLengthInBeats - 1.0) < beatEpsilon) return juce::String("1/4");
        if (std::abs(stepWriteStepLengthInBeats - 0.5) < beatEpsilon) return juce::String("1/8");
        if (std::abs(stepWriteStepLengthInBeats - 0.25) < beatEpsilon) return juce::String("1/16");
        if (std::abs(stepWriteStepLengthInBeats - 0.125) < beatEpsilon) return juce::String("1/32");
        return juce::String(stepWriteStepLengthInBeats, 3);
    }();
    stepLengthButton.setButtonText("Len " + stepText);
    contextLabel.setText(trackName, juce::dontSendNotification);
}

void MidiEditorOverlayComponent::clearSelection()
{
    selectedNotes.clear();
    selectedSlide.reset();
}

void MidiEditorOverlayComponent::pushUndoSnapshot()
{
    if (activeClip == nullptr)
        return;

    undoStack.push_back(NoteSnapshot { activeClip->midiNotes, activeClip->pitchSlides, selectedNotes, selectedSlide,
                                       horizontalZoom, verticalZoom, scaleRoot, scalePatternIndex, snapSizeInBeats,
                                       stepWriteCursorBeat, stepWriteStepLengthInBeats, false });
    redoStack.clear();
}

void MidiEditorOverlayComponent::restoreSnapshot(const NoteSnapshot& snapshot)
{
    OptionalAudioLock lk(audioEditLock);
    if (activeClip == nullptr)
        return;

    activeClip->midiNotes = snapshot.midiNotes;
    activeClip->pitchSlides = snapshot.pitchSlides;
    selectedNotes = snapshot.selectedNotes;
    selectedSlide = snapshot.selectedSlide;
    // Do NOT restore the zoom/scroll from the snapshot — undo should only change the notes, never jump
    // or zoom the view (FL keeps the view put through undo/redo).
    scaleRoot = snapshot.scaleRoot;
    scalePatternIndex = snapshot.scalePatternIndex;
    snapSizeInBeats = snapshot.snapSizeInBeats;
    stepWriteCursorBeat = snapshot.stepWriteCursorBeat;
    stepWriteStepLengthInBeats = snapshot.stepWriteStepLengthInBeats;
    focusModeEnabled = false;
    hoverNote.reset();
    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();
    clampScrollOffsets();
    focusToggle.setToggleState(false, juce::dontSendNotification);
    updateSubtitle();
}

bool MidiEditorOverlayComponent::notesChangedSince(const NoteSnapshot& snapshot) const noexcept
{
    if (activeClip == nullptr)
        return false;

    if (activeClip->midiNotes.size() != snapshot.midiNotes.size())
        return true;
    if (activeClip->pitchSlides.size() != snapshot.pitchSlides.size())
        return true;

    for (std::size_t i = 0; i < activeClip->midiNotes.size(); ++i)
    {
        const auto& current = activeClip->midiNotes[i];
        const auto& previous = snapshot.midiNotes[i];

        if (current.pitch != previous.pitch
            || std::abs(current.startBeat - previous.startBeat) > beatEpsilon
            || std::abs(current.lengthInBeats - previous.lengthInBeats) > beatEpsilon
            || current.velocity != previous.velocity)
        {
            return true;
        }
    }

    for (std::size_t i = 0; i < activeClip->pitchSlides.size(); ++i)
    {
        const auto& current = activeClip->pitchSlides[i];
        const auto& previous = snapshot.pitchSlides[i];
        if (current.sourcePitch != previous.sourcePitch
            || std::abs(current.sourceNoteStartBeat - previous.sourceNoteStartBeat) > beatEpsilon
            || current.points.size() != previous.points.size())
            return true;

        for (std::size_t point = 0; point < current.points.size(); ++point)
        {
            if (std::abs(current.points[point].beat - previous.points[point].beat) > beatEpsilon
                || std::abs(current.points[point].pitch - previous.points[point].pitch) > 1.0e-4)
                return true;
        }
    }

    return false;
}

void MidiEditorOverlayComponent::updateVelocityFromPosition(int y)
{
    if (activeClip == nullptr || ! velocityDragState.has_value())
        return;

    const auto lane = getVelocityLaneBounds().reduced(10, 10);
    const auto clampedY = juce::jlimit(lane.getY(), lane.getBottom(), y);
    const auto ratio = 1.0 - (static_cast<double>(clampedY - lane.getY()) / juce::jmax(1.0, static_cast<double>(lane.getHeight())));
    const auto velocity = juce::jlimit(1, 127, static_cast<int>(std::round(ratio * 127.0)));

    for (const auto index : velocityDragState->targetIndices)
        activeClip->midiNotes[static_cast<std::size_t>(index)].velocity = velocity;
}

void MidiEditorOverlayComponent::focusViewportAroundClipNotes()
{
    const auto visible = getVisibleGridViewport();

    // FL-style fixed default when the piano roll opens: a consistent, comfortable zoom (~90 px per beat,
    // natural lane height) rather than a tight fit to whatever notes exist. Open at bar 1, and centre
    // the pitch on the notes (or C4 when empty).
    verticalZoom = 1.0;
    constexpr double targetPixelsPerBeat = 90.0;
    horizontalZoom = juce::jlimit(minimumHorizontalZoom, maximumHorizontalZoom,
                                  targetPixelsPerBeat * getContentBeats()
                                      / juce::jmax(1.0, static_cast<double>(visible.getWidth())));
    scrollX = 0.0;

    int centerPitch = 60;   // C4
    if (activeClip != nullptr && ! activeClip->midiNotes.empty())
    {
        int minPitch = highestPitch;
        int maxPitch = lowestPitch;
        for (const auto& note : activeClip->midiNotes)
        {
            minPitch = juce::jmin(minPitch, note.pitch);
            maxPitch = juce::jmax(maxPitch, note.pitch);
        }
        centerPitch = (minPitch + maxPitch) / 2;
    }

    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    scrollY = (static_cast<double>(pitchToLane(centerPitch)) * laneHeight)
            - (static_cast<double>(visible.getHeight()) * 0.5);

    clampScrollOffsets();
    syncZoomScrollTargets();
}

bool MidiEditorOverlayComponent::shouldConsumeFocusClick() const noexcept
{
    return ! hasKeyboardFocus(true);
}

void MidiEditorOverlayComponent::adjustZoom(double horizontalDelta, double verticalDelta, std::optional<juce::Point<int>> focusPoint)
{
    const auto visible = getVisibleGridViewport();

    const double focusXInView = focusPoint.has_value()
        ? static_cast<double>(focusPoint->x - visible.getX()) : static_cast<double>(visible.getWidth()) * 0.5;
    const double focusYInView = focusPoint.has_value()
        ? static_cast<double>(focusPoint->y - visible.getY()) : static_cast<double>(visible.getHeight()) * 0.5;

    // Anchor by the beat (x) and lane (y) under the pointer so it stays put; ease toward the target.
    const double ppbOld  = getPixelsPerBeat();
    const double laneOld = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    zoomFocusXInView = focusXInView;
    zoomFocusYInView = focusYInView;
    zoomFocusBeat = (scrollX + focusXInView) / juce::jmax(1.0e-6, ppbOld);
    zoomFocusLane = (scrollY + focusYInView) / juce::jmax(1.0, laneOld);
    targetHorizontalZoom = juce::jlimit(minimumHorizontalZoom, maximumHorizontalZoom, targetHorizontalZoom + horizontalDelta);
    targetVerticalZoom = juce::jlimit(minimumVerticalZoom, maximumVerticalZoom, targetVerticalZoom + verticalDelta);
    zoomAnimating = true;
    updateSubtitle();
}

void MidiEditorOverlayComponent::clampScrollOffsets()
{
    const auto visible = getVisibleGridViewport();
    const auto fullWidth = static_cast<double>(visible.getWidth());
    const auto fullHeight = static_cast<double>(visible.getHeight());
    const auto zoomedWidth = fullWidth * horizontalZoom;
    const auto zoomedHeight = juce::jmax(fullHeight, static_cast<double>(getDisplayedLaneCount() * baseLaneHeightPx) * verticalZoom);

    // Allow a little overscroll past the content edge (half a viewport) so that
    // zoom-to-cursor can keep a point near the RIGHT / BOTTOM edge fixed under the
    // pointer. Without this the upper clamp pulls right-edge zooms back to centre.
    const auto maxScrollX = juce::jmax(0.0, zoomedWidth - fullWidth * 0.5);
    const auto maxScrollY = juce::jmax(0.0, zoomedHeight - fullHeight * 0.5);
    scrollX = juce::jlimit(0.0, maxScrollX, scrollX);
    scrollY = juce::jlimit(0.0, maxScrollY, scrollY);
    targetScrollX = juce::jlimit(0.0, maxScrollX, targetScrollX);
    targetScrollY = juce::jlimit(0.0, maxScrollY, targetScrollY);
}

juce::String MidiEditorOverlayComponent::getScaleName() const
{
    return juce::String(rootNames[static_cast<std::size_t>(scaleRoot)]) + " " + scalePatterns[static_cast<std::size_t>(scalePatternIndex)].name;
}

juce::String MidiEditorOverlayComponent::getSnapName() const
{
    for (const auto& setting : snapSettings)
    {
        if (std::abs(setting.beats - snapSizeInBeats) < beatEpsilon)
            return setting.name;
    }

    return juce::String(snapSizeInBeats, 3) + " beat";
}

void MidiEditorOverlayComponent::showScaleMenu()
{
    juce::PopupMenu menu;
    orion::ui::stylePopupMenu(menu);

    for (int rootIndex = 0; rootIndex < static_cast<int>(rootNames.size()); ++rootIndex)
    {
        for (int patternIndex = 0; patternIndex < static_cast<int>(scalePatterns.size()); ++patternIndex)
        {
            const auto itemId = 1 + rootIndex * static_cast<int>(scalePatterns.size()) + patternIndex;
            const auto selected = scaleRoot == rootIndex && scalePatternIndex == patternIndex;
            menu.addItem(itemId, juce::String(rootNames[static_cast<std::size_t>(rootIndex)]) + " " + scalePatterns[static_cast<std::size_t>(patternIndex)].name, true, selected);
        }
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&scaleButton),
                       [this](int selectedId)
                       {
                           if (selectedId <= 0)
                               return;

                           const auto index = selectedId - 1;
                           scaleRoot = index / static_cast<int>(scalePatterns.size());
                           scalePatternIndex = index % static_cast<int>(scalePatterns.size());
                           clampScrollOffsets();
                           updateSubtitle();
                           repaint();
                       });
}

void MidiEditorOverlayComponent::showSnapMenu()
{
    juce::PopupMenu menu;
    orion::ui::stylePopupMenu(menu);

    for (int i = 0; i < static_cast<int>(snapSettings.size()); ++i)
    {
        const auto selected = std::abs(snapSettings[static_cast<std::size_t>(i)].beats - snapSizeInBeats) < beatEpsilon;
        menu.addItem(i + 1, snapSettings[static_cast<std::size_t>(i)].name, true, selected);
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&snapButton),
                       [this](int selectedId)
                       {
                           if (selectedId <= 0)
                               return;

                           snapSizeInBeats = snapSettings[static_cast<std::size_t>(selectedId - 1)].beats;
                           // FL-style: the grid cell size becomes the default note size, so a freshly
                           // drawn note is exactly one cell of the chosen grid. "Len" follows too.
                           lastNoteLengthInBeats = snapSizeInBeats;
                           stepWriteStepLengthInBeats = snapSizeInBeats;
                           updateSubtitle();   // refreshes both the "Grid" and "Len" button labels
                           repaint();
                       });
}

void MidiEditorOverlayComponent::showStepLengthMenu()
{
    juce::PopupMenu menu;
    orion::ui::stylePopupMenu(menu);

    const std::array<SnapSetting, 4> stepLengths {{
        { "1/4", 1.0 },
        { "1/8", 0.5 },
        { "1/16", 0.25 },
        { "1/32", 0.125 },
    }};

    for (int i = 0; i < static_cast<int>(stepLengths.size()); ++i)
    {
        const auto selected = std::abs(stepLengths[static_cast<std::size_t>(i)].beats - stepWriteStepLengthInBeats) < beatEpsilon;
        menu.addItem(i + 1, stepLengths[static_cast<std::size_t>(i)].name, true, selected);
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&stepLengthButton),
                       [this, stepLengths](int selectedId)
                       {
                           if (selectedId <= 0)
                               return;

                           commitStepWritePendingChord();
                           stepWriteStepLengthInBeats = stepLengths[static_cast<std::size_t>(selectedId - 1)].beats;
                           // "Len" is also the default length for freshly drawn notes.
                           lastNoteLengthInBeats = juce::jmax(minimumNoteLengthInBeats, stepWriteStepLengthInBeats);
                           stepWriteCursorBeat = getSnappedStepWriteCursorBeat();
                           updateSubtitle();
                           repaint();
                       });
}
}  // namespace orion
