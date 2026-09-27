# Task 2 Review — RGB565 software encoder wrapper

## Verdict: accepted with one minor verification-report discrepancy

### Critical

None.

### Important

None.

### Minor

1. The Task 2 report overstates its full-tree mirror check. The Task 2 files,
   sketches, README, and vendored encoder artefacts are byte-identical, but
   `diff -qr moody-tx 'moody-tx 2'` currently reports four pre-existing
   differences under `provisioning/*.pem`; therefore the report's statement
   that the full-tree diff returned zero is not reproducible. This does not
   affect the compiled sketches (`moody_keys.h` is identical) or Task 2's
   named deliverables, but the report should not cite a clean full-tree diff
   unless those unrelated provisioning files are reconciled.

## Review evidence

- The wrapper matches the vendored API signatures and uses the required fixed
  profile: 480 x 320, RGB565 big-endian, 4:2:0, quality 75, zero rotation, and
  mono-task encoding. `RGB565_BE` is consistent with Espressif's documented
  default byte order for camera/frame-buffer RGB565 input.
- Input validation covers handle/buffer readiness, exact pixel format,
  dimensions, non-null source, exact `480 * 320 * 2` length, and 16-byte source
  alignment. The encoder receives the true input and output capacities, and a
  non-positive or over-capacity result is rejected before publication.
- Allocation ownership is paired correctly: `jpeg_calloc_align` with
  `jpeg_free_align`, and `jpeg_enc_open` with `jpeg_enc_close`. `begin()` safely
  cleans partial state, `end()` is idempotent, and the destructor calls it.
  The returned JPEG pointer remains wrapper-owned and stable through the
  synchronous send, before the next encode can overwrite it.
- Every acquired camera frame is returned on profile failure, encode failure,
  and after transmission. Encoding failure is visible and terminates the
  stream; there is no `frame2jpg`/`fmt2jpg` fallback.
- `txEncodeUs` brackets only `encode()`. Header construction, `writeAll`, JPEG
  byte accounting, send timing, and camera-buffer return ordering remain
  outside that boundary. Telemetry reports `software-new` for the configured
  software path and `sensor` for the camera-JPEG path.
- The persistent encoder is begun once after successful camera setup. Explicit
  `end()` exists for teardown; firmware's normal never-exit lifecycle does not
  otherwise require per-client teardown.

## Fresh verification

- `sh scripts/compile-tx.sh`: exit 0; both ESP32-S3 sketches compiled with the
  vendored precompiled library. Each build reports 1,013,723 bytes of flash and
  57,044 bytes of global RAM.
- Both ELFs contain `SoftwareJpegEncoder::{begin,encode,end}` and
  `jpeg_enc_{open,process,close}` plus the aligned allocation/free symbols.
- `cmp -s` passes for both sketches, both wrapper files,
  `library.properties`, the vendored headers/archive, and the README.
- Static search finds no legacy encoder call or Task 1 link-probe identifier.

Hardware image capture was outside this review, so byte order and runtime
throughput remain compile/source validated rather than camera-tested.
