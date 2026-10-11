#!/usr/bin/env bash
# Offline x86 host test under ASan/UBSan: no device, no ALSA (STEPRUNNER_TEST captures the MIDI). See host_test.cpp.
# Uses the local g++ if there is one, else Docker (gcc:12). MPC_VST = the mpc-vst-plugins checkout (default ../..).
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-../..}"
mkdir -p build
python3 gen_params.py
python3 -c "
import sys, json; sys.path.insert(0, '$MPC_VST/tools')
import gen_vst, params, shadow_skin
cfg = gen_vst.load('vst.json'); pl, _ = params.load('params.json')
gen_vst.gen_params(cfg, pl + shadow_skin.popup_params('layout.conf', pl), 'build/params.h')"
cp "$MPC_VST/wrapper/popup.h" build/
CMD='g++ -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=c++17 -Wall -Wextra -Wno-unused-parameter -DSTEPRUNNER_TEST -I. -Ibuild steprunner8.cpp host_test.cpp -o build/host_test -lpthread -lm -ldl && build/host_test'
if command -v g++ >/dev/null; then bash -euc "$CMD"; else docker run --rm -v "$PWD":/w -w /w gcc:12 bash -euc "$CMD"; fi
echo "PASSED"
