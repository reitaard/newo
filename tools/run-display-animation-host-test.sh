#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX_BIN="${CXX:-c++}"
EYE_OUT="${TMPDIR:-/tmp}/newo-display-animation-host-test"
STATE_OUT="${TMPDIR:-/tmp}/newo-autonomy-state-host-test"
PRESENTATION_OUT="${TMPDIR:-/tmp}/newo-presentation-host-test"

"$CXX_BIN" -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/Newo" \
  "$ROOT/tools/display-animation-host-test.cpp" \
  "$ROOT/Newo/newo_eye_pose.cpp" \
  "$ROOT/Newo/newo_gaze_motion.cpp" \
  -o "$EYE_OUT"

"$CXX_BIN" -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/Newo" \
  "$ROOT/tools/autonomy-state-host-test.cpp" \
  "$ROOT/Newo/newo_autonomy_state.cpp" \
  -o "$STATE_OUT"

"$CXX_BIN" -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/Newo" \
  "$ROOT/tools/presentation-host-test.cpp" \
  "$ROOT/Newo/newo_presentation.cpp" \
  -o "$PRESENTATION_OUT"

"$EYE_OUT"
"$STATE_OUT"
"$PRESENTATION_OUT"
rm -f "$EYE_OUT" "$STATE_OUT" "$PRESENTATION_OUT"
