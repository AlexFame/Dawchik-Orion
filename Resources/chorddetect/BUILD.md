# orion-chorddetect (Chordino helper)

Self-contained chord-detection helper, built from QMUL's **NNLS-Chroma / Chordino**
(<https://github.com/c4dm/nnls-chroma>, GPL v2+ — see `src/COPYING`).

Orion runs this as a **separate process** (`Source/Audio/ChordDetector.cpp` →
`detectViaChordino`), reads its `"<seconds>: <label>"` stdout, and maps the timeline onto the
per-beat chord grid. Process isolation keeps the GPL code cleanly separated from Orion's
proprietary source ("mere aggregation"). If the helper is missing/fails, ChordDetector falls
back to its native template detector.

## Layout
- `bin/orion-chorddetect` — the arm64 binary (uses `@loader_path/../libs`).
- `libs/` — bundled libsndfile + codec dylibs (via `dylibbundler`).
- `src/` — the GPL source it was built from.

CMake copies this whole folder into `Orion.app/Contents/Resources/chorddetect/` (before the
`--deep` codesign pass).

## Rebuild (Apple Silicon)
```sh
brew install vamp-plugin-sdk libsndfile boost dylibbundler
cd src
g++ -D_VAMP_PLUGIN_IN_HOST_NAMESPACE -O3 -ffast-math -std=c++14 \
  -I/opt/homebrew/opt/vamp-plugin-sdk/include \
  -I/opt/homebrew/opt/libsndfile/include \
  -I/opt/homebrew/opt/boost/include \
  chordextract.cpp Chordino.cpp NNLSBase.cpp chromamethods.cpp viterbi.cpp nnls.c \
  /opt/homebrew/opt/vamp-plugin-sdk/lib/libvamp-hostsdk.a \
  -L/opt/homebrew/opt/libsndfile/lib -lsndfile -framework Accelerate \
  -o ../bin/orion-chorddetect
cd ../bin
dylibbundler -od -b -x ./orion-chorddetect -d ../libs -p @loader_path/../libs
```
`chordextract.cpp` and `chromamethods.cpp` are in `src/` too (not listed above only because the
compile line references them by name).
