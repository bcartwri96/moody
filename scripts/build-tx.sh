#!/bin/sh
set -eu

ARDUINO_CLI='/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli'
FQBN='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi'

"$ARDUINO_CLI" compile \
  --fqbn "$FQBN" \
  --libraries moody-tx/libraries \
  --output-dir /tmp/moody-tx-build \
  moody-tx
