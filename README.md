# Orion

Orion is a fast-creation DAW for macOS producers who want to move from idea to arrangement without getting lost in menus, plugins, and workflow overhead.

## Orion v1 focus

- FL-style piano roll workflow with scales and fast note editing
- Studio One-style playlist for quick arrangement
- Reliable autowarp for drag-and-drop loops
- VST3 hosting
- Fast WAV export

## Current status

This repository contains the initial application scaffold for the native Orion desktop app built with CMake and JUCE.

## Planned architecture

- `Source/Core`: application state, commands, serialization
- `Source/Audio`: transport, render graph, tempo, clip playback
- `Source/Midi`: note model, scales, quantization, piano roll logic
- `Source/Plugins`: VST3 scan/load/state handling
- `Source/UI`: arrangement, transport, piano roll, browser, export UI

## Local setup

1. Add a JUCE checkout to `third_party/JUCE`.
2. Configure with CMake.
3. Build the `Orion` target in Xcode or from the command line.

```bash
cmake -S . -B build
cmake --build build
```

If JUCE is missing, CMake will stop with a message that tells you where to place it.
