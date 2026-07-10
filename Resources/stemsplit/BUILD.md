# orion-stemsplit (Demucs 6-stem helper)

Offline stem separation, matching **Logic Pro 12 Stem Splitter** (6 stems: drums, bass, other,
vocals, guitar, piano). Built from **[sevagh/demucs.cpp](https://github.com/sevagh/demucs.cpp)**
(MIT) — a native C++17 inference of Meta's **Demucs htdemucs_6s** (MIT model + MIT-licensed weights,
so unlike ML chord models the weights are freely bundleable).

Orion runs it as a **separate process** (`Source/Audio/StemSeparator.cpp`): decodes the source to a
clean 44.1 kHz stereo 16-bit WAV (JUCE — sidesteps the helper's strict WAV loader), then invokes
`orion-stemsplit <model> <input.wav> <outDir> <numThreads>`. Output files are
`target_{0..5}_{drums,bass,other,vocals,guitar,piano}.wav`. Progress is parsed from the helper's
`[THREAD i] ( NN.N%)` stdout lines.

## Layout
- `bin/orion-stemsplit` — arm64 binary (`demucs_mt.cpp.main`, self-contained: only Accelerate/libc++/libSystem).
- `models/htdemucs-6s.bin` — 6-source ggml f16 weights (~52 MB).

CMake copies this folder into `Orion.app/Contents/Resources/stemsplit/` before the `--deep` codesign.

## Rebuild (Apple Silicon)
```sh
git clone --recurse-submodules https://github.com/sevagh/demucs.cpp
cd demucs.cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j8
cp build/demucs_mt.cpp.main <repo>/Resources/stemsplit/bin/orion-stemsplit
strip -x <repo>/Resources/stemsplit/bin/orion-stemsplit
# 6-source ggml weights:
curl -L -o <repo>/Resources/stemsplit/models/htdemucs-6s.bin \
  https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/ggml-model-htdemucs-6s-f16.bin
```

Perf note: ~2.4× realtime on this arm64 Mac (≈100 s for a 42 s file, 8 threads). Runs offline in a
background thread with a progress banner.
