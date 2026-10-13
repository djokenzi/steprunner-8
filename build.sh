#!/usr/bin/env bash
# Build StepRunner-8 as a VST2 plugin for the MPC OS plugin host (armhf). Needs Docker.
#   build/steprunner8.so, build/skin/          together they are ONE plugin folder in /sdcard/Synths
#   build/pluginlist-entry.xml             the <PLUGIN> line for MPC.settings' pluginList-arm
# vst.json/params.json only feed mpc-vst-plugins' tools/gen_vst.py for params.h + the skin; the plugin itself is
# steprunner8.cpp (own VST2 wrapper + ALSA seq MIDI out). Nothing in mpc-vst-plugins is modified.
# MPC_VST = the mpc-vst-plugins checkout (default ../.., i.e. this folder is poc/trigseq inside it).
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../..}" && pwd)"
U="$(id -u):$(id -g)"
if grep -q '^#EDIT-HIDDEN# ' layout.conf; then echo "layout.conf has lines hidden for Skin Studio: run ./skin_edit.sh show first" >&2; exit 1; fi
mkdir -p build
python3 gen_params.py

# 1. skin: the browser renderer (vst.json "art": "html", needed for the TRIGS cell pictures) makes params.h, the skin
#    and pluginlist-entry.xml. Same image as mpc-vst-maze's sequencer build.
docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null
docker run --rm -u "$U" -e HOME=/tmp -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w mpc-vst-html-art \
  python3 /mv/tools/gen_vst.py vst.json
for d in build/skin/*/"Plugin Skins"; do python3 skin_dedupe.py "$d"; done   # v1.1.1: identical pictures share one file (less RAM on the MPC)
cp "$MPC_VST/wrapper/popup.h" build/

# 2. the plugin (armhf, glibc 2.31 (bullseye) so it loads on MPC OS 2.x (2.32) and 3.x (2.39))
docker run --rm --platform linux/arm/v7 -v "$PWD":/w -w /w arm32v7/gcc:11-bullseye bash -euxc '
  apt-get update -qq && apt-get install -y -qq -t bullseye libasound2-dev >/dev/null
  g++ -O2 -D_FILE_OFFSET_BITS=64 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter -I. -Ibuild \
      -shared -o build/steprunner8.so steprunner8.cpp \
      -static-libstdc++ -static-libgcc -Wl,--no-undefined -lasound -lpthread -lm -ldl
  strip build/steprunner8.so
  echo "-- exported --"; readelf --dyn-syms -W build/steprunner8.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/steprunner8.so | grep NEEDED
  echo "-- highest glibc (MPC OS 2.x has 2.32) --"; readelf -V build/steprunner8.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/steprunner8.so
