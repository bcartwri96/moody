#!/bin/sh
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEMP_ROOT=$(mktemp -d /tmp/moody-upload-tx-test.XXXXXX)
trap 'rm -rf "$TEMP_ROOT"' EXIT

FAKE_UV="$TEMP_ROOT/uv"
FAKE_BUILD="$TEMP_ROOT/build-tx.sh"
BUILD_DIR="$TEMP_ROOT/build"
LOG="$TEMP_ROOT/commands.log"

mkdir -p "$BUILD_DIR"
touch "$BUILD_DIR/moody-tx.ino.bootloader.bin"
touch "$BUILD_DIR/moody-tx.ino.partitions.bin"
touch "$BUILD_DIR/moody-tx.ino.bin"

cat > "$FAKE_UV" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$UPLOAD_TX_LOG"
EOF
chmod +x "$FAKE_UV"

cat > "$FAKE_BUILD" <<'EOF'
#!/bin/sh
printf 'compile\n' >> "$UPLOAD_TX_LOG"
EOF
chmod +x "$FAKE_BUILD"

UPLOAD_TX_LOG="$LOG" \
UV_BIN="$FAKE_UV" \
BUILD_TX_SCRIPT="$FAKE_BUILD" \
BUILD_DIR="$BUILD_DIR" \
sh "$PROJECT_ROOT/scripts/upload-tx.sh" /dev/cu.usbmodem-test

EXPECTED="$TEMP_ROOT/expected.log"
cat > "$EXPECTED" <<'EOF'
add esptool
compile
run esptool --chip esp32s3 --port /dev/cu.usbmodem-test --baud 921600 --before default_reset --after hard_reset write_flash -z 0x0 BUILD/moody-tx.ino.bootloader.bin 0x8000 BUILD/moody-tx.ino.partitions.bin 0x10000 BUILD/moody-tx.ino.bin
EOF
sed "s|BUILD|$BUILD_DIR|g" "$EXPECTED" > "$TEMP_ROOT/expected-expanded.log"

diff -u "$TEMP_ROOT/expected-expanded.log" "$LOG"
