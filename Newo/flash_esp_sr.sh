#!/usr/bin/env bash
# Flash the sketch and the official ESP_SR model partition. Do not omit the
# second step: Arduino-ESP32 copies srmodels.bin during build but its normal
# upload recipe writes only bootloader/partition table/application.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <serial-port>" >&2
  exit 2
fi

PORT=$1
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-/tmp/newo-esp-sr-build}
FQBN='esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,UploadSpeed=921600'
ARDUINO_DATA_DIR=${ARDUINO_DATA_DIR:-"$HOME/.arduino15"}

rm -rf "$BUILD_DIR"
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD_DIR" "$SCRIPT_DIR"
arduino-cli upload --fqbn "$FQBN" --input-dir "$BUILD_DIR" --port "$PORT"

ESPTOOL=$(find "$ARDUINO_DATA_DIR/packages/esp32/tools/esptool_py" -type f -name esptool -perm -u+x \
  | sort -V | tail -n 1)
if [[ -z "$ESPTOOL" || ! -f "$BUILD_DIR/srmodels.bin" ]]; then
  echo "ESP_SR model upload prerequisites were not found." >&2
  exit 1
fi
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 write-flash -z \
  --flash-mode keep --flash-freq keep --flash-size keep 0xC10000 "$BUILD_DIR/srmodels.bin"

echo "Newo application and ESP_SR model partition flashed."
