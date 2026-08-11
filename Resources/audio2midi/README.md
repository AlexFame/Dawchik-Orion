# Orion Audio → MIDI (Spotify Basic Pitch)

Polyphonic audio-to-MIDI transcription behind the arrangement clip menu
**"Convert to MIDI (melody)"**. The C++ side (`Source/Audio/AudioToMidi.*`)
writes the clip region to a temp WAV, runs `transcribe.py` in the venv below,
and reads back a JSON note list (times in **seconds**).

## Recreate the venv (not committed — ~593 MB)

Requires **Python 3.11** (3.13/3.14 lack prebuilt wheels for numba/llvmlite):

```bash
brew install python@3.11
cd Resources/audio2midi
/opt/homebrew/bin/python3.11 -m venv venv
venv/bin/pip install --upgrade pip "setuptools<81" wheel
venv/bin/pip install basic-pitch onnxruntime
```

`setuptools<81` is required — `resampy` still imports the removed `pkg_resources`.

The CoreML model ships inside the `basic-pitch` package, so no extra download or
network is needed at runtime. Transcription of a 4-bar loop takes ~3–4 s.

## Overrides

- `ORION_A2M_PYTHON` — path to the venv's python
- `ORION_A2M_SCRIPT` — path to `transcribe.py`

## transcribe.py

```
transcribe.py <input.wav> <out.json> [--onset F] [--frame F]
              [--min-note-ms N] [--min-freq HZ] [--max-freq HZ] [--multi-bends]
```
