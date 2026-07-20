# Orion Agent Notes

## Working Style

- User prefers direct, proactive implementation in Russian.
- Keep fixes focused. Do not revert unrelated dirty files.
- After code changes, build Orion and restart the app so the user can test immediately.
- Restart requires host app access:
  - Quit: `osascript -e 'tell application "Orion" to quit'`
  - Open: `open -n build-local/Orion_artefacts/Orion.app`

## Build

- Main local build command:
  - `cmake --build build-local --target Orion -j 4`
- The app bundle used for manual testing is:
  - `build-local/Orion_artefacts/Orion.app`
- Warnings are acceptable unless they relate to the current change.

## Architecture Map

- `Source/Core/ProjectState.*`: project model, tracks, clips, MPC kit/chop/tune state.
- `Source/Core/ProjectSerializer.*`: save/load of project state. Add new persisted fields here when `ProjectState` changes.
- `Source/Audio/PlaybackSources.h`: arrangement playback, live/record routing, transport-driven rendering.
- `Source/Sampler/SamplerEngine.*`: sample playback, MPC kit rendering, tune/chop/slice playback.
- `Source/UI/MainComponent.*`: top-level app coordination, routing between UI/audio/project/MIDI.
- `Source/UI/TransportBarComponent.*`: top transport, Orion/Jam layout, note/chord MIDI display.
- `Source/UI/MpcSamplePanelComponent.*`: MPC Sample visual shell, pad/command hit zones, LCD/waveform/chop display.
- `Source/UI/MpcSampleHardwareBridge.*`: MIDI device bridge for MPC Sample hardware.
- `Source/UI/MpcSampleMapping.*`: MPC Sample note/CC/button mapping.
- `Source/UI/JamSessionComponent.*`: Orion Jam camera/chat/session UI.

## MPC Sample Rules

- Hardware pads must route through `MpcSampleHardwareBridge` and `MpcSampleMapping`; keep mapping isolated from generic UI.
- 16 Levels / tune mode should play one selected sample chromatically across pads.
- Chop mode should play equal slices of the selected sample and update the LCD with the active slice.
- Replacing a sample on a selected pad must update the active chop/tune source immediately.
- Prevent aftertouch/pressure retrigger loops: a held physical pad should not retrigger until a real release and press.
- Keep MIDI monitor UI readable: show current note or chord, not a tiny signal bar.

## UI Guidelines

- Keep Jam chat simple: attach, emoji, text input, send.
- Avoid visible focus rings that make the app uncomfortable to work in.
- Use modern compact controls, consistent sizing, and no overlapping buttons/text.
- Camera controls should be overlay controls on the participant video and hide when not hovered.

## Testing Checklist

- Build succeeds.
- Orion restarts from the freshly built app bundle.
- For MPC changes:
  - Mouse pad click works.
  - Hardware pad press works.
  - MIDI display updates.
  - 16 Levels and Chop modes do not fight each other.
  - Replacing a pad sample replaces playback and screen state.
