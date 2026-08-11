# Orion — Feature Map

The complete functionality of Orion, a fast-creation DAW for macOS (Apple Silicon), built with
JUCE 8 / C++20. Reference targets: **Ableton (workflow) + Bitwig (depth) + Studio One (arrangement)
+ FL (piano roll)**. Ethos: *maximally simple for beatmakers; depth on demand.*

Status legend: ✅ done · 🟡 partial / staged · 🔴 pending or known issue.

---

## 1. Workspaces (top toolbar tabs)

| Tab | What it is | Status |
|-----|------------|--------|
| **PROJECT / Arrangement** | Studio One-style playlist timeline (tracks, clips, chord lane) | ✅ |
| **MIXER** | Channel strips: inserts, sends, buses, master | ✅ |
| **CLIP EDITOR** | Audio clip editor (trim, warp preview, fades, pitch) | ✅ |
| **STEPS** | Step sequencer (drum grid → MIDI clip) | ✅ |
| **MPC SAMPLE** | Akai MPC-style pad sampler + hardware bridge | ✅ |
| **ORION JAM** | Real-time multiplayer session (video/voice/chat) | 🟡 LAN only |

---

## 2. Arrangement (`ArrangementTimelineComponent`)

- ✅ Tracks: audio / MIDI / folder; per-track header (M/S/R, instrument, **FX insert stack**,
  volume slider + dB, **pan slider + L/R readout**), resizable height, colour palette (Studio One).
- ✅ Clips: drag from browser, move (in-place), trim edges (Ableton-style, no accidental stretch),
  time-stretch on Alt-drag, fades (in/out + curves), per-clip gain, split/join, mute/solo.
- ✅ Grid & snap: Ableton-faithful adaptive grid, Cmd-bypass, grid-step readout, default density.
- ✅ **Warp**: RubberBand time-stretch, autowarp on drop, warp cache (bounded). 🟡 Warp markers
  Stage 1 (clip-editor preview) done; Stage 2 (arrangement playback of markers) pending.
- ✅ **Chord lane** ("CHORDS"): place/move/replace chord blocks, drag chord from the wheel onto the
  lane, drag progression down onto a track to bake it into a MIDI clip. Chord octave control.
- ✅ Selection inspector, undo/redo, marquee select, multi-select.
- 🔴 Drum warp lacks punch vs Ableton (phase-vocoder smears attack; real fix = slice/transient mode).

## 3. Piano roll (`MidiEditorOverlayComponent`)

- ✅ FL-style note editing: draw, move (H + V), resize, velocity lane, cut/split/join, marquee.
- ✅ **Alt+drag duplicate** notes (Ableton); clip grows to fit when notes move right.
- ✅ Scale lock, key-aware editing, chord tool + **Circle of Fifths** selector, glide (pitch slides,
  MPE-style per-voice channels), presets, step input.
- ✅ MIDI recording; loop-repeat note ordering fixed (offs before ons — sample players survive).

## 4. Chords & harmony

- ✅ **Circle of Fifths chord wheel** (`ChordSelectorComponent`, Cubase-style): outer ring = majors
  by fifths, inner ring = relative minors, tonic centre, roman numerals on diatonic chords.
- ✅ Quality (Maj/Min/Aug/Dim/sus/5th/Bass), extensions (6/7/maj7/9/11/13/alterations), slash bass.
- ✅ Clickable **Key** label → project key menu; diatonic voicing keeps every chord at/above the
  tonic (no octave drop) across all 24 keys; live keyboard preview; drag chord to grid.
- ✅ Chord mode (one key = diatonic chord), chord track drives MPC pad highlight.
- ✅ **Audio → chords**: `ChordDetector` (bundled Chordino, GPL, separate process) detects a
  progression from a dropped loop and lays it on the chord lane.
- 🟡 **Camera chord control** (`ChordSelectorComponent` + `HandPoseTracker`/Vision): point a
  fingertip at the wheel (background-thread detection, on-wheel cursor + self-view). Novelty; gestures
  (fist = next, palm = stop) not built; perf/accuracy parked in favour of the click workflow.

## 5. Sampler (`SamplerEngine`, `SamplerPanelComponent`)

- ✅ Single-sample sampler: Classic (note-length gated, key held) + One-Shot (808s) modes; slicing,
  auto-chop, pitch, per-slice mapping; browser double-click opens the sampler track.
- ✅ MIDI input routing (hardware MIDI via `MidiInput::openDevice`).

## 6. MPC Sample (`MpcSamplePanelComponent`, `MpcSampleHardwareBridge`)

- ✅ 16-pad MPC-style kit: drop samples on pads, one-shot playback (`renderMpcKitClip`), key-scale
  pad highlighting, LCD note/chord/KEY readout, MPC-local key.
- ✅ **Akai MPC hardware bridge**: pads over USB (notes 36–51 + aftertouch). 🔴 kit not serialized yet.

## 7. Mixer (`MixerPanelComponent`)

- ✅ Channel strips: insert FX chain (add/replace/remove/bypass/reorder, drag between tracks),
  aux **sends** to FX buses (pre/post-fader, levels), output routing, volume/pan, stereo meters.
- ✅ Master strip + master inserts; FX buses; VST3 effect hosting via `PluginManager`.

## 8. Automation (`Source/Automation`, `PlaybackSources`)

- ✅ Model: `AutomationLane` (breakpoints, per-segment tension curves), params: trackVolume,
  trackPan, **trackSend**, instrumentParam, insertParam.
- ✅ Inline Bitwig/Ableton overlay editor: press **A**, param chip selector, draw/drag/delete points,
  value readout, Clear. Serialized with the project.
- ✅ **Touch-to-map**: grab a knob in an instrument/insert editor → auto-maps the lane (Ableton joker).
- 🔴 Record modes (Read/Write/Touch/Latch) — pending. 🔴 Automation clips + modulators (LFO/env) — pending.

## 9. Audio engine (`Source/Audio`)

- ✅ `TransportEngine`/`TransportController`: play/stop/record, loop, playhead, stop-declick tail.
- ✅ `PlaybackSources` (render graph): per-track clip/instrument render, inserts, sends, buses, master.
- ✅ `AudioRenderPool`: **multi-core VST render** (P-cores, Ableton model) — parallel instruments.
- ✅ `OrionStretchEngine` (RubberBand), `WarpEngine`, warp cache (bounded, deferred free).
- ✅ Recording: `AudioInputRecorder` + `IndependentAudioInput` (input≠output guarded vs CoreAudio hang).
- ✅ `ExportService`: fast WAV export. `LoudnessMeter` (LUFS), normalize/match-loudness.
- ✅ `StemSeparator`: 6-stem split via bundled demucs.cpp (MIT, separate process).
- 🔴 VST per-block spike crackle (Analog Lab/Juno) — buffer 1024 is a workaround, real fix pending.

## 10. Browser (`BrowserPanelComponent`, `Source/Analysis`)

- ✅ Files / Loops / One-Shots / VST / soundpack locations; search + tags.
- ✅ **Synced preview** (RubberBand, tempo + key sync, Ableton parity: -6 dB cue, launches on the bar).
- ✅ **ML sound tags**: `AudioTagger` / `SampleEmbedding` / `AppleSoundClassifier` (auto-tag samples).

## 11. Multiplayer — Orion Jam (`Source/Collab`, `JamSessionComponent`)

- ✅ Architecture: op-log with a **sequencer hub** (total order), `CollabController` seam,
  `CollabReconciler` (syncs without instrumenting edit code), stable entity IDs.
- ✅ Transports: `SocketTransport` (TCP), `LoopbackTransport` (test), embedded `CollabServer`.
- ✅ Synced: whole clips, tracks (VST/sampler/MPC/inserts), notes, pitch slides, mixer, shared
  transport, live cursors (Figma-style), chat (with history), participant roster.
- ✅ Asset transfer (content-addressed sample sync), collaborative undo/redo, auto-reconnect.
- ✅ Voice talkback (`VoiceChatEngine`) + Zoom-style video call layer.
- 🔴 **Remote (different cities/countries) needs a cloud-hosted server** (NAT/TLS/rooms) — Phase 3,
  not built. Currently host-as-server on LAN only.

## 12. Plugins (`Source/Plugins`)

- ✅ VST3 hosting: `PluginManager` (scan/instantiate), `PluginEditorWindow`, `PluginPickerComponent`
  (searchable), `PluginTouchListener` (parameter gesture → automation map). App portability: dylibs
  (RubberBand + deps) bundled into `.app` so it runs on any Mac.

## 13. Core / project

- ✅ `ProjectState` (single source of truth), `ProjectSerializer` (save/load, round-trips automation,
  chords, warp), `ChordTheory` (voicing, diatonic triads, key-aware tensions).
- ✅ Key/scale + time signature + tempo; audio-edit lock protects the audio thread on reallocation.

## 14. Testing & build

- ✅ Test suite (`Source/Tools`): SmokeTest, RenderPoolTest, CollabTest, LoudnessTest, KeyTest,
  ChordTest, SimTest, WarpCompare; `PluginScanner` (out-of-process VST scan).
- Build: `cmake --build build-local --target Orion -j8` → `build-local/Orion_artefacts/Orion.app`.
  (The user's build dir is **build-local**; `build-codex` is a separate parallel build.)

---

## Known issues / open items (rollup)

- 🔴 Drum warp lacks punch (needs slice/transient mode).
- 🔴 VST per-block crackle (real fix vs buffer-1024 workaround).
- 🔴 Warp markers Stage 2 (arrangement playback).
- 🔴 Automation record modes + clips/modulators.
- 🔴 Jam remote server (cloud hub for cross-country) + TLS + rooms.
- 🔴 MPC kit serialization.
- 🟡 Camera chord control is a parked novelty (perf/accuracy, gestures unbuilt).
- 🟡 No OpenGL on the dev Mac → software paint only; optimize paint, cache layers.
