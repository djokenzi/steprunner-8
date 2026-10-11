#!/usr/bin/env bash
# Package the built plugin as one installable zip: dist/StepRunner-8-<version>-mpc-armv7.zip
# (install.sh / uninstall.sh, the plugin folder, INSTALL.md). Run ./build.sh first.
#   ./release.sh [version]        default version: 1.0.0
# Uses mpc-vst-plugins' tools/release.py unchanged. MPC_VST = that checkout (default ../..).
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../..}" && pwd)"
VERSION="${1:-1.0.0}"
SKIN="build/skin/djokenzi - VST - StepRunner-8"
for f in build/steprunner8.so build/pluginlist-entry.xml "$SKIN/Plugin Skins/TUI.json"; do
  [ -e "$f" ] || { echo "missing $f: run ./build.sh first" >&2; exit 1; }
done
python3 "$MPC_VST/tools/release.py" --so build/steprunner8.so --skin "$SKIN" --entry build/pluginlist-entry.xml \
  --version "$VERSION" -o dist \
  --about "StepRunner-8: an 8-track, 64-step sequencer for MPC standalone with step conditions, step and CC locks, patterns, states and performance effects, driving a drum program over MIDI (ALSA port StepRunner-8)."
ls -l dist/*.zip
