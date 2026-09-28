#!/bin/sh
set -eu

ARDUINO_CLI='/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli'
FQBN='esp32:esp32:esp32:FlashSize=16M,PartitionScheme=huge_app,PSRAM=enabled'
PROJECT_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

"$ARDUINO_CLI" compile \
  --fqbn "$FQBN" \
  --build-path /private/tmp/moody-rx-classic/build \
  --output-dir /private/tmp/moody-rx-classic/firmware \
  "$PROJECT_ROOT/moody-rx"
