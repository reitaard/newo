#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX_BIN="${CXX:-c++}"
OUT="${TMPDIR:-/tmp}/newo-display-animation-host-test"

"$CXX_BIN" -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/Newo" \
  "$ROOT/tools/display-animation-host-test.cpp" \
  "$ROOT/Newo/newo_eye_pose.cpp" \
  "$ROOT/Newo/newo_gaze_motion.cpp" \
  -o "$OUT"

"$OUT"
rm -f "$OUT"
