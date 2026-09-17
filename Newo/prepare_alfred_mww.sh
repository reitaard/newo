#!/usr/bin/env bash
set -euo pipefail

# Prepare the exact TensorFlow Lite Micro frontend used by Tater's native
# microWakeWord runtime. The source is pinned so local and CI builds are
# reproducible. Generated files live under Newo/src and are intentionally
# ignored by git.
TATER_REF="fafe9e3c91c0fae8e95fef05a54edf9765e3714e"
BASE="https://raw.githubusercontent.com/TaterTotterson/Tater-Native-Firmware/${TATER_REF}/components/tflm_microfrontend"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${SCRIPT_DIR}/src/tflm_microfrontend"

mkdir -p "$DEST"

src_files=(
  _kiss_fft_guts.h bits.h
  fft.c fft.h fft_util.c fft_util.h
  filterbank.c filterbank.h filterbank_util.c filterbank_util.h
  frontend.c frontend_util.c
  kiss_fft.c kiss_fft.h kiss_fftr.c kiss_fftr.h
  log_lut.c log_lut.h log_scale.c log_scale.h log_scale_util.c log_scale_util.h
  noise_reduction.c noise_reduction.h noise_reduction_util.c noise_reduction_util.h
  pcan_gain_control.c pcan_gain_control.h pcan_gain_control_util.c pcan_gain_control_util.h
  window.c window.h window_util.c window_util.h
)

for file in "${src_files[@]}"; do
  echo "[mww] $file"
  curl --fail --location --silent --show-error \
    "${BASE}/src/${file}" -o "${DEST}/${file}"
done

curl --fail --location --silent --show-error \
  "${BASE}/include/frontend.h" -o "${DEST}/frontend.h"
curl --fail --location --silent --show-error \
  "${BASE}/include/frontend_util.h" -o "${DEST}/frontend_util.h"
curl --fail --location --silent --show-error \
  "${BASE}/LICENSE" -o "${DEST}/LICENSE"

# Tater keeps public headers in include/ and implementation headers in src/.
# Arduino sketches do not automatically add both nested directories to every C
# compile command, so flatten the two public headers into this generated source
# directory and make their includes local. No DSP code or constants are changed.
sed -i 's#"../src/#"#g' "${DEST}/frontend.h" "${DEST}/frontend_util.h"

# Basic integrity checks. These sizes are stable at the pinned Tater commit.
test -s "${DEST}/frontend.c"
test -s "${DEST}/frontend_util.c"
test -s "${DEST}/kiss_fft.c"
grep -q 'FrontendProcessSamples' "${DEST}/frontend.h"
grep -q 'FrontendPopulateState' "${DEST}/frontend_util.h"

echo "[mww] frontend ready: ${DEST}"
