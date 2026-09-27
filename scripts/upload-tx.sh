#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 SERIAL_PORT" >&2
  exit 64
fi

SERIAL_PORT=$1
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
UV_BIN=${UV_BIN:-uv}
BUILD_TX_SCRIPT=${BUILD_TX_SCRIPT:-"$PROJECT_ROOT/scripts/build-tx.sh"}
BUILD_DIR=${BUILD_DIR:-/tmp/moody-tx-build}
BAUD_RATE=${BAUD_RATE:-921600}

BOOTLOADER="$BUILD_DIR/moody-tx.ino.bootloader.bin"
PARTITIONS="$BUILD_DIR/moody-tx.ino.partitions.bin"
FIRMWARE="$BUILD_DIR/moody-tx.ino.bin"

cd "$PROJECT_ROOT"
if [ ! -f pyproject.toml ]; then
  "$UV_BIN" init --bare
fi
"$UV_BIN" add esptool
sh "$BUILD_TX_SCRIPT"

for artifact in "$BOOTLOADER" "$PARTITIONS" "$FIRMWARE"; do
  if [ ! -f "$artifact" ]; then
    echo "Missing compiled firmware artifact: $artifact" >&2
    exit 1
  fi
done

exec "$UV_BIN" run esptool \
  --chip esp32s3 \
  --port "$SERIAL_PORT" \
  --baud "$BAUD_RATE" \
  --before default_reset \
  --after hard_reset \
  write_flash -z \
  0x0 "$BOOTLOADER" \
  0x8000 "$PARTITIONS" \
  0x10000 "$FIRMWARE"
