#!/usr/bin/env python3
"""Orion audio -> MIDI transcriber (Spotify Basic Pitch, polyphonic).

Usage:
    transcribe.py <input_audio> <output_json> [--onset F] [--frame F]
                  [--min-note-ms N] [--min-freq HZ] [--max-freq HZ]
                  [--multi-bends]

Writes a JSON object:
    {"ok": true, "notes": [{"start": sec, "end": sec, "pitch": midi,
                            "amp": 0..1}, ...]}
On failure writes {"ok": false, "error": "..."} and exits non-zero.

Notes are in SECONDS so the caller maps to beats itself (tempo-agnostic).
"""
import sys
import os
import json
import argparse
import warnings

warnings.filterwarnings("ignore")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--onset", type=float, default=0.5)
    ap.add_argument("--frame", type=float, default=0.3)
    ap.add_argument("--min-note-ms", type=float, default=90.0)
    ap.add_argument("--min-freq", type=float, default=None)
    ap.add_argument("--max-freq", type=float, default=None)
    ap.add_argument("--multi-bends", action="store_true")
    args = ap.parse_args()

    try:
        # Keep the heavy import inside try so import errors land in JSON too.
        from basic_pitch.inference import predict
        from basic_pitch import ICASSP_2022_MODEL_PATH

        _model_out, _midi, note_events = predict(
            args.input,
            ICASSP_2022_MODEL_PATH,
            onset_threshold=args.onset,
            frame_threshold=args.frame,
            minimum_note_length=args.min_note_ms,
            minimum_frequency=args.min_freq,
            maximum_frequency=args.max_freq,
            multiple_pitch_bends=args.multi_bends,
            melodia_trick=True,
        )

        notes = []
        for ev in note_events:
            start, end, pitch, amp = ev[0], ev[1], ev[2], ev[3]
            notes.append({
                "start": float(start),
                "end": float(end),
                "pitch": int(pitch),
                "amp": float(max(0.0, min(1.0, amp))),
            })
        notes.sort(key=lambda n: (n["start"], n["pitch"]))

        with open(args.output, "w") as f:
            json.dump({"ok": True, "notes": notes}, f)
        return 0
    except Exception as e:  # noqa: BLE001
        try:
            with open(args.output, "w") as f:
                json.dump({"ok": False, "error": str(e)}, f)
        except Exception:
            pass
        sys.stderr.write("transcribe error: %s\n" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
