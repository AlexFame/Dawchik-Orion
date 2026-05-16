#!/bin/zsh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-local"
APP_PATH="$BUILD_DIR/Orion_artefacts/Orion.app"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR"

osascript -e 'tell application "Orion" to quit' >/dev/null 2>&1 || true
sleep 0.4
pkill -TERM -x Orion >/dev/null 2>&1 || true

for _ in {1..20}; do
  if ! pgrep -x Orion >/dev/null 2>&1; then
    break
  fi
  sleep 0.15
done

pkill -KILL -x Orion >/dev/null 2>&1 || true
sleep 0.2
open -n "$APP_PATH"
