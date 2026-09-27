# Task 2 — Software JPEG Encoder Wrapper Report

## Scope

Implemented Task 2 only. The matching transmitter files are:

- `moody-tx/software_jpeg_encoder.h`
- `moody-tx/software_jpeg_encoder.cpp`
- `moody-tx/moody-tx.ino`
- their byte-identical counterparts under `moody-tx 2/`

No RX, FIFO, documentation, or vendored dependency files were changed.

## Test-first evidence

The transmitter sketch was first changed to include and construct the planned
`SoftwareJpegEncoder` before that header existed. The project-local build
helper then failed as intended:

```text
moody-tx/moody-tx.ino:6:10: fatal error: software_jpeg_encoder.h:
No such file or directory
```

The initial sandboxed invocation could not access Arduino's external sketch
cache. The same command was then run with the required cache permission and
reached the intended compiler failure above.

## Implementation

`SoftwareJpegEncoder` owns exactly one `jpeg_enc_handle_t` and one persistent,
16-byte-aligned 128 KiB output buffer. `begin()` allocates the buffer with
`jpeg_calloc_align(..., 16)` and opens the encoder once with the fixed profile:

- RGB565 big-endian input;
- 480 x 320 (HVGA);
- 4:2:0 subsampling;
- quality 75;
- no rotation;
- mono-task encoding (`task_enable = false`).

The public fixed-profile constants are compile-time asserted as 480 x 320 and
MCU-aligned. `encode()` additionally rejects an unavailable encoder/output
buffer, non-RGB565 input, non-HVGA dimensions, null input, a frame length other
than exactly `480 * 320 * 2`, a source address not aligned to 16 bytes, and
non-positive or oversized encoder output. It resets its result pointer and
length on every failure. `end()` closes the handle and frees the persistent
buffer safely.

`sendFrame()` now performs a visible frame-profile guard before the wrapper and
times only `SoftwareJpegEncoder::encode()` in `txEncodeUs`. It retains the
existing header construction, `writeAll()` sequence, payload-size timing,
send timing, and camera-buffer return ordering. The RGB565 path no longer uses
`frame2jpg` or per-frame JPEG allocation/free. The wrapper is initialized once
after successful RGB565 camera initialization; a setup failure stops firmware
visibly. Telemetry is `source=software-new` precisely when the camera fallback
selected the wrapper, and remains `sensor` for camera-provided JPEG.

The Task 1 temporary direct-link probe and its `esp_jpeg_enc.h` sketch include
were removed after the wrapper began making the real API calls.

## Fresh validation

Final command:

```sh
sh scripts/compile-tx.sh && \
  cmp -s moody-tx/moody-tx.ino 'moody-tx 2/moody-tx.ino' && \
  cmp -s moody-tx/software_jpeg_encoder.h 'moody-tx 2/software_jpeg_encoder.h' && \
  cmp -s moody-tx/software_jpeg_encoder.cpp 'moody-tx 2/software_jpeg_encoder.cpp' && \
  cmp -s moody-tx/libraries/esp_new_jpeg/library.properties 'moody-tx 2/libraries/esp_new_jpeg/library.properties' && \
  diff -qr moody-tx 'moody-tx 2' && \
  ! rg -n 'frame2jpg|encoderLinkProbe|compileOnlyEncoderLinkProbe' moody-tx 'moody-tx 2'
```

Result: exit 0. The helper compiled both mirrors using the repository-local
vendored dependency and the required ESP32-S3 FQBN. Its final primary-build
output reported:

```text
Library moody-esp-new-jpeg has been declared precompiled:
Using precompiled library in .../moody-tx/libraries/esp_new_jpeg/src/esp32s3
Sketch uses 1013723 bytes (32%) of program storage space.
Global variables use 57044 bytes (17%) of dynamic memory.
```

All specified `cmp` checks and the full TX-tree `diff -qr` returned zero, and
the final search found neither the legacy `frame2jpg` path nor Task 1 probe
identifiers. Hardware flashing/capture was not part of Task 2 and was not run.
