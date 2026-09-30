#!/bin/bash
# Flash the robot. Over WiFi by default; over USB when asked, which is needed
# once to install the partition layout that makes over-the-air updates possible.
#
#   tools/flash.sh                 # over WiFi, finding the robot by name
#   tools/flash.sh 172.20.10.2     # over WiFi, at an address
#   tools/flash.sh usb             # over USB
set -e
cd "$(dirname "$0")/.."
SKETCH=robo_wifi
FQBN="esp32:esp32:esp32cam"
# min_spiffs keeps two app slots, which is what an over-the-air update writes into
OPTS="PartitionScheme=min_spiffs"

arduino-cli compile --fqbn "$FQBN" --board-options "$OPTS" --output-dir build "$SKETCH"

if [ "$1" = "usb" ]; then
  PORT=$(ls /dev/cu.usbserial* 2>/dev/null | head -1)
  [ -z "$PORT" ] && { echo "No board on USB."; exit 1; }
  arduino-cli upload --fqbn "$FQBN" --board-options "$OPTS" -p "$PORT" --input-dir build "$SKETCH"
else
  HOST=${1:-robo.local}
  ESPOTA=$(find ~/Library/Arduino15/packages/esp32/hardware/esp32 -name espota.py | head -1)
  [ -z "$ESPOTA" ] && { echo "espota.py not found in the ESP32 core."; exit 1; }
  echo "Flashing $HOST over WiFi..."
  python3 "$ESPOTA" -i "$HOST" -p 3232 -f build/"$SKETCH".ino.bin -r
fi
