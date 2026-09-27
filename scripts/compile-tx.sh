#!/bin/sh
set -eu

ARDUINO_CLI='/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli'
FQBN='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi'

"$ARDUINO_CLI" compile \
  --fqbn "$FQBN" \
  --libraries moody-tx/libraries \
  --output-dir /tmp/moody-tx-build \
  moody-tx

MIRROR_BUILD_ROOT=$(mktemp -d /tmp/moody-tx-2-build.XXXXXX)
trap 'rm -rf "$MIRROR_BUILD_ROOT"' EXIT
cp -R 'moody-tx 2' "$MIRROR_BUILD_ROOT/moody-tx"

"$ARDUINO_CLI" compile \
  --fqbn "$FQBN" \
  --libraries "$MIRROR_BUILD_ROOT/moody-tx/libraries" \
  --output-dir /tmp/moody-tx-2-build \
  "$MIRROR_BUILD_ROOT/moody-tx"
