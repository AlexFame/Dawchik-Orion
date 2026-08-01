# Orion — Automation (progress journal)

Philosophy: **Ableton outside, Bitwig inside.** Press A, pick a parameter, draw a line — simple.
Depth (automation clips, relative mode, modulators) appears only on demand. Do NOT build all of
Bitwig up front. Model is `Source/Automation/AutomationLane.h` (standalone module; engine + UI depend
on it, not each other). Values stored in natural units (dB / -1..+1 / 0..1). Per-segment tension curves.

## Done
- **Phase 1 — engine + inline UI** (commits `01be060`, `c5946ff`)
  - `AutomationLane` data model: params `trackVolume/trackPan/trackSend/instrumentParam/insertParam`,
    breakpoints with per-segment `curve`, `valueAt(beat, fallback)`, tension shaping, to/fromVar.
  - `TrackState.automation` vector + `automatedVolumeDb(beat)` / `automatedPan(beat)`; single source of truth.
  - Playback applies **volume + pan** envelopes at both render sites (PlaybackSources track mix + processInstruments).
  - Serialization round-trips lanes (ProjectSerializer).
  - Inline overlay editor in the arrangement timeline: 'A' toggles automation mode, Shift+A cycles param,
    param chip, add/drag/delete points, curved segments drawn.

- **Phase 2 — VST instrument params + touch-to-map** (user-verified 2026-07-31: "все работет")
  - Engine applies `instrumentParam` (Pass 1) and `insertParam` (`processTrackInserts`) at block rate
    via `applyPluginParamAutomation()` → `AudioProcessorParameter::setValue`.
  - Lane identity extended to (param, targetIndex, paramIndex); `findAutomation`/`laneFor` overloads,
    backward compatible (old vol/pan lanes = -1/-1).
  - **Touch-to-map (Ableton-style):** grabbing a knob in an open instrument editor auto-maps the lane
    to that param. `PluginTouchListener` (gesture-begin only, ignores playback value changes) attached
    on `openInstrumentEditor`, detached on close. Handler `onInstrumentParamTouched`.
  - AUTO chip menu also offers Volume / Pan / Instrument ▸ <params> / <InsertName> ▸ <params> (manual path).

## Done (cont.)
- **Automation UI — Bitwig/Ableton overlay** (commit 19e0a1a, user-approved "Отлично"): overlays the clip
  with a track-coloured fill + bright white thick line (no track darkening — Bitwig doesn't darken); param
  selector is an opaque header chip replacing the fader row; value readout on drag/hover; click adds a point
  ON the curve and only grabs when on the line; double-click deletes; Clear-automation menu item; fader-like
  volume taper (0 dB ~80% up). See [[midi-loop-note-ordering]] for the loop-mute fix found alongside.

## Pending polish (this session)
- **Clip drag badge** — the JUCE DnD "MIDI Clip / N clips" badge (ArrangementTimeline ~3621) shows during
  a plain move; user wants the clip to move in place with no badge (in-place move at ~3844 already exists).
  User chose to do this AFTER automation — now unblocked.
- **Insert touch-to-map** — same as instrument, but insert editor windows are erased in several places;
  wire listener lifetime carefully to avoid a dangling listener on a destroyed instance.
- **Colours**: user loves Bitwig's palette. Possible follow-ups: tune Orion's default track-colour palette,
  or PNG-palette import like Bitwig (manual §3.2.5 — colours are swappable palettes, not fixed hex).

## Next (endorsed order)
1. **Sends** — apply `trackSend` envelopes.
3. **Record modes** — Read / Write / Touch / Latch (write automation from live fader/knob moves).
4. **Automation clips + modulators** — Bitwig depth, on demand (LFO / env-follower mapped to any param).

## Discarded / rejected
- Full Bitwig modulation system up front — too much; gated behind "on demand".
- FL-style event automation as the base model — user: Bitwig + Ableton are the reference, not FL.
- The first raw overlay (unlabeled lane, no param name) — user couldn't tell what it automated; replaced
  with param chip + named target.

## Notes
- Keep the surface dead-simple; every added knob must justify itself against Orion's "maximally simple" ethos.
- No premature "done": build + tests green, then user verifies on their side. See memory/working-agreements.md.
