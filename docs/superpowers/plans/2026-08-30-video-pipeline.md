# Video Encoder and FIFO Receive Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the transmitter's legacy RGB565 JPEG fallback with Espressif `esp_new_jpeg` and receive frames through a two-slot FIFO that preserves order and exposes latency.

**Architecture:** TX keeps the authenticated, length-prefixed JPEG protocol but obtains software JPEG bytes from one persistent `SoftwareJpegEncoder` instance. RX splits its post-authentication work into a network-reader task that owns `WiFiClient` and a display-loop consumer that owns `JPEGDEC` and the LCD; two PSRAM JPEG slots plus free/ready queues enforce FIFO preservation and TCP backpressure.

**Tech Stack:** Arduino-ESP32 3.3.5, ESP32-S3 transmitter, classic ESP32 receiver, FreeRTOS queues/tasks, Espressif `esp_new_jpeg`, JPEGDEC, Arduino_GFX.

**Spec:** `docs/superpowers/specs/2026-08-30-video-pipeline-design.md`

## Global Constraints

- Preserve the existing 16-byte `MJPG` header, authenticated TCP connection, JPEG payloads, 480 x 320 video profile, and preserve-and-crop presentation.
- Pin and vendor the official Espressif `esp_new_jpeg` package, including its licence and ESP32-S3 archive; do not require a user-installed library.
- Configure the RGB565 software path for JPEG 4:2:0, quality 75, no rotation, and mono-task encoding.
- Preserve RX frame order; when both slots are occupied, block the reader and let TCP backpressure grow latency rather than dropping frames.
- Keep `moody-tx/moody-tx.ino`, `moody-tx 2/moody-tx.ino`, their encoder support files, and their READMEs byte-identical.
- The RX reader task exclusively owns `WiFiClient` after authentication; the main loop exclusively owns `JPEGDEC`, Arduino_GFX, and LCD writes.
- Report `source=software-new`, FIFO wait, frame age, and queued depth alongside the existing timing fields.
- This workspace has no Git metadata; do not attempt commits.

---

## File structure

- `moody-tx/software_jpeg_encoder.h` / `.cpp`: narrow wrapper over `esp_new_jpeg`; manages an aligned output buffer and one encoder handle.
- `moody-tx/libraries/esp_new_jpeg/`: pinned official headers, ESP32-S3 archive, licence, and Arduino library metadata.
- `moody-tx/moody-tx.ino`: calls the wrapper only for RGB565 camera frames and reports `software-new`.
- `moody-tx 2/...`: exact mirror of every TX change.
- `moody-rx/frame_fifo.h`: platform-independent two-slot FIFO state helper used by host tests and RX queue lifecycle code.
- `moody-rx/moody-rx.ino`: allocates two PSRAM JPEG buffers, starts/stops the reader task, consumes frame descriptors in FIFO order, and prints latency telemetry.
- `tests/frame_fifo_test.cpp`: host coverage for slot ownership/order/backpressure/release.
- `tests/timing_stats_test.cpp`: extended only if new timing statistics require host-level arithmetic coverage.
- `moody-tx/README.md`, `moody-tx 2/README.md`, `moody-rx/README.md`, `docs/video-performance-analysis.md`: document new telemetry, dependency provenance, FIFO semantics, and expected interpretation.

## Task 1: Vendor and prove the Espressif encoder dependency

**Files:**
- Create: `moody-tx/libraries/esp_new_jpeg/library.properties`
- Create: `moody-tx/libraries/esp_new_jpeg/LICENSE`
- Create: `moody-tx/libraries/esp_new_jpeg/src/esp_jpeg_common.h`
- Create: `moody-tx/libraries/esp_new_jpeg/src/esp_jpeg_enc.h`
- Create: `moody-tx/libraries/esp_new_jpeg/src/esp32s3/libesp_new_jpeg.a`
- Create: mirrored files below `moody-tx 2/libraries/esp_new_jpeg/`
- Modify: `moody-tx/moody-tx.ino` and `moody-tx 2/moody-tx.ino` only temporarily for a compile-only include/link probe

**Interfaces:**
- Consumes: Espressif `esp_new_jpeg` pinned source release and the project ESP32-S3 FQBN.
- Produces: Arduino can resolve `#include <esp_jpeg_enc.h>` and link `jpeg_enc_open` / `jpeg_enc_process` for ESP32-S3.

- [ ] **Step 1: Add a compile-probe declaration that references the target API**

  Add this guarded declaration near the TX includes in both TX sketches:

  ```cpp
  #include <esp_jpeg_enc.h>

  static jpeg_enc_handle_t encoderLinkProbe = nullptr;
  ```

- [ ] **Step 2: Compile before vendoring to verify the dependency is absent**

  Run:

  ```bash
  /Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli compile \
    --fqbn 'esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi' \
    --output-dir /tmp/moody-tx-build moody-tx
  ```

  Expected: FAIL with `esp_jpeg_enc.h: No such file or directory`.

- [ ] **Step 3: Vendor a pinned official package and write Arduino library metadata**

  Download one specific Espressif `esp_new_jpeg` release/commit. Copy its
  `LICENSE`, `include/esp_jpeg_common.h`, `include/esp_jpeg_enc.h`, and
  `lib/esp32s3/libesp_new_jpeg.a`. Create `library.properties` with:

  ```properties
  name=moody-esp-new-jpeg
  version=1.0.2
  architectures=esp32
  precompiled=true
  dot_a_linkage=false
  ```

  Place the archive in the architecture folder required by the Arduino ESP32
  builder after checking the compile verbose output. Record the exact upstream
  release `v1.0.2` in a comment in `library.properties` and mirror all files to
  `moody-tx 2`. `dot_a_linkage=false` is required because the upstream archive
  has the canonical filename `libesp_new_jpeg.a`; dot linkage instead asks the
  Arduino builder for `esp_new_jpeg.a`.

- [ ] **Step 4: Compile the link probe**

  Run the Task 1 compile command again.

  Expected: PASS; the sketch links against the vendored ESP32-S3 archive.

- [ ] **Step 5: Remove the temporary probe only after the wrapper in Task 2 consumes the API**

  Do not leave `encoderLinkProbe` in either sketch. Confirm TX files remain identical:

  ```bash
  cmp -s moody-tx/moody-tx.ino 'moody-tx 2/moody-tx.ino'
  ```

## Task 2: Add a testable RGB565 software encoder wrapper

**Files:**
- Create: `moody-tx/software_jpeg_encoder.h`
- Create: `moody-tx/software_jpeg_encoder.cpp`
- Create: matching files in `moody-tx 2/`
- Modify: `moody-tx/moody-tx.ino`
- Modify: `moody-tx 2/moody-tx.ino`

**Interfaces:**
- Consumes: `camera_fb_t` with `PIXFORMAT_RGB565`, frame width 480, frame height 320, and a byte-aligned source buffer.
- Produces: `class SoftwareJpegEncoder` with `bool begin()`, `bool encode(const camera_fb_t &, const uint8_t *&, size_t &)`, and `void end()`.

- [ ] **Step 1: Add compile-time profile invariants before implementation**

  In `software_jpeg_encoder.h`, declare the fixed profile constants and assert
  they match the camera configuration:

  ```cpp
  static constexpr uint16_t kSoftwareJpegWidth = 480;
  static constexpr uint16_t kSoftwareJpegHeight = 320;
  static constexpr uint8_t kSoftwareJpegQuality = 75;
  static constexpr size_t kSoftwareJpegOutputBytes = 128 * 1024;
  ```

  Add a runtime guard rejecting a non-480 x 320 RGB565 frame before invoking
  the wrapper; the encoder must never process an unexpected camera profile.

- [ ] **Step 2: Compile to verify the wrapper interface is missing**

  Change the RGB565 branch in `sendFrame` to construct `SoftwareJpegEncoder`
  and call `encode`, then compile.

  Expected: FAIL because `software_jpeg_encoder.h` does not yet exist.

- [ ] **Step 3: Implement the minimal persistent wrapper**

  Implement `begin()` to allocate a 16-byte-aligned output buffer and open one
  `jpeg_enc_handle_t` using:

  ```cpp
  jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
  config.width = kSoftwareJpegWidth;
  config.height = kSoftwareJpegHeight;
  config.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
  config.subsampling = JPEG_SUBSAMPLE_420;
  config.quality = kSoftwareJpegQuality;
  config.rotate = JPEG_ROTATE_0D;
  config.task_enable = false;
  ```

  Implement `encode()` to reject a non-RGB565 or non-HVGA frame, reject a
  source buffer that is not 16-byte aligned, call `jpeg_enc_process`, reject a
  non-positive or oversized output length, and return the stable output pointer
  plus exact byte count. Implement `end()` to close the handle and free the
  output buffer. Keep `frame2jpg` out of the RGB565 path.

- [ ] **Step 4: Integrate the wrapper and preserve timing boundaries**

  Create the wrapper after camera initialisation if `txUsesSoftwareJpeg` is
  true. In `sendFrame`, measure only `encode()` with `txEncodeUs`; preserve the
  existing header, `writeAll`, `txJpegBytes`, and camera-buffer return order.
  Set the reported source string to `software-new` only when the wrapper is
  active; report `sensor` for camera-provided JPEG.

- [ ] **Step 5: Compile both TX sketches and verify exact mirroring**

  Run the Task 1 compile command for both TX directories, then:

  ```bash
  cmp -s moody-tx/moody-tx.ino 'moody-tx 2/moody-tx.ino'
  cmp -s moody-tx/software_jpeg_encoder.h 'moody-tx 2/software_jpeg_encoder.h'
  cmp -s moody-tx/software_jpeg_encoder.cpp 'moody-tx 2/software_jpeg_encoder.cpp'
  cmp -s moody-tx/libraries/esp_new_jpeg/library.properties 'moody-tx 2/libraries/esp_new_jpeg/library.properties'
  ```

  Expected: both compiles PASS and every comparison exits 0.

## Task 3: Define and test FIFO slot lifecycle independently of FreeRTOS

**Files:**
- Create: `moody-rx/frame_fifo.h`
- Create: `tests/frame_fifo_test.cpp`

**Interfaces:**
- Produces: `FrameSlotFifo<2>` with `acquireFree(uint8_t &)`, `enqueueReady(uint8_t)`, `dequeueReady(uint8_t &)`, `release(uint8_t)`, `freeCount()`, and `readyCount()`.
- Consumes: slot IDs in `[0, 1]`.

- [ ] **Step 1: Write failing host tests for order, saturation, and release**

  Create `tests/frame_fifo_test.cpp` with assertions equivalent to:

  ```cpp
  FrameSlotFifo<2> fifo;
  uint8_t first = 0, second = 0, out = 0;
  assert(fifo.acquireFree(first) && first == 0);
  assert(fifo.acquireFree(second) && second == 1);
  assert(!fifo.acquireFree(out));
  assert(fifo.enqueueReady(first));
  assert(fifo.enqueueReady(second));
  assert(fifo.dequeueReady(out) && out == first);
  assert(fifo.release(out));
  assert(fifo.dequeueReady(out) && out == second);
  assert(fifo.release(out));
  assert(fifo.freeCount() == 2 && fifo.readyCount() == 0);
  ```

  Add assertions that reject releasing or enqueueing an invalid/duplicate slot.

- [ ] **Step 2: Run the test to verify it fails**

  Run:

  ```bash
  c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/frame_fifo_test.cpp -o /tmp/moody-frame-fifo-test && /tmp/moody-frame-fifo-test
  ```

  Expected: FAIL because `frame_fifo.h` does not exist.

- [ ] **Step 3: Implement the minimal header-only slot state machine**

  Implement a fixed-size array-based state machine with states `free`,
  `held_by_reader`, and `ready`, plus a two-entry circular ready queue. Do not
  include FreeRTOS headers. Return `false` for duplicate, invalid, or
  impossible transitions.

- [ ] **Step 4: Run the host test to verify it passes**

  Re-run the Task 3 command.

  Expected: PASS.

## Task 4: Integrate the two-slot FIFO reader and display consumer

**Files:**
- Modify: `moody-rx/moody-rx.ino`
- Modify: `moody-rx/timing_stats.h`
- Modify: `tests/timing_stats_test.cpp`

**Interfaces:**
- Consumes: `FrameSlotFifo<2>`, two PSRAM buffers of `MAX_JPEG_SIZE`, and frame descriptors from the reader.
- Produces: `startStreamReader()`, `stopStreamReader()`, `receiveAndDisplayQueuedFrame()`, and a serial RX report including `fifo_us`, `age_ms`, and `queued`.

- [ ] **Step 1: Add buffers, descriptors, queues, and reader task lifecycle**

  Define a `FrameDescriptor` containing:

  ```cpp
  struct FrameDescriptor {
    uint8_t slot;
    uint32_t frameId;
    uint32_t txMillis;
    uint32_t jpegLength;
    uint32_t headerWaitUs;
    uint32_t payloadUs;
    uint32_t enqueuedAtUs;
    bool terminal;
  };
  ```

  Allocate `uint8_t *jpegBuffers[2]` with `ps_malloc(MAX_JPEG_SIZE)` during
  setup and fail on the existing LCD status screen if either allocation fails.
  Create `QueueHandle_t freeSlots` and `readyFrames`, both length 2. Seed
  `freeSlots` with slot IDs 0 then 1 on every authenticated connection.

  Start `streamReaderTask(void *)` only after authentication. The task blocks
  on `freeSlots`, reads exactly one header and one payload into the acquired
  slot, and sends a descriptor to `readyFrames`. If the reader encounters an
  invalid header/length or `readExact` fails, it sends one `terminal=true`
  descriptor after previously queued frames, stops the client, marks itself
  finished, and deletes itself. It must return an acquired slot before sending
  a terminal event if the frame was incomplete.

- [ ] **Step 2: Replace direct receive/display with FIFO consumption**

  Replace `receiveAndDisplayFrame()` in `loop()` with a consumer that blocks on
  `readyFrames`. For a normal descriptor, calculate `fifo_us` as
  `micros() - enqueuedAtUs` and `age_ms` as `millis() - txMillis`; add the
  descriptor's wait/payload metrics to the existing statistics, decode from
  `jpegBuffers[slot]`, record the display outcome, then return `slot` to
  `freeSlots` even after a recoverable JPEG decode failure. For a terminal
  descriptor, drain no later frames, show the existing reconnect state, and
  reconnect after the existing delay.

  Do not access `streamClient` from the display loop after the reader starts.
  Do not access `JPEGDEC`, `gfx`, or the LCD from the reader task.

- [ ] **Step 3: Add report fields and reset behavior**

  Add `TimingStats rxFifoUs` and `TimingStats rxAgeMs`. Change the RX line to
  include:

  ```text
  fifo_us=avg/max age_ms=avg/max queued=current_depth
  ```

  Reset these statistics with the existing timing window. Ensure report
  population remains one sample per complete frame descriptor; failed JPEG
  decodes count as attempted/failed but retain their timing samples.

- [ ] **Step 4: Run host tests and compile RX**

  Run:

  ```bash
  c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/frame_fifo_test.cpp -o /tmp/moody-frame-fifo-test && /tmp/moody-frame-fifo-test
  c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/timing_stats_test.cpp -o /tmp/moody-timing-stats-test && /tmp/moody-timing-stats-test
  /Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli compile \
    --fqbn 'esp32:esp32:esp32' --output-dir /tmp/moody-rx-build moody-rx
  ```

  Expected: all commands PASS.

## Task 5: Document and verify the combined experiment

**Files:**
- Modify: `moody-tx/README.md`
- Modify: `moody-tx 2/README.md`
- Modify: `moody-rx/README.md`
- Modify: `docs/video-performance-analysis.md`

**Interfaces:**
- Consumes: final TX/RX serial fields and dependency provenance.
- Produces: flashing instructions and an unambiguous interpretation of encoder, FIFO delay, and frame-age data.

- [ ] **Step 1: Update expected serial output and provenance**

  Document `source=software-new` as the vendored Espressif encoder path and
  retain `source=sensor` for camera JPEG. Document that RX `fifo_us` measures
  local queue delay, `age_ms` measures TX-header-to-display-start age, and
  `queued` is FIFO depth when display begins. State explicitly that FIFO order
  is preserved and latency may grow under sustained overload.

- [ ] **Step 2: Run complete static verification**

  Run:

  ```bash
  c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/video_layout_test.cpp -o /tmp/moody-video-layout-test && /tmp/moody-video-layout-test
  c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/timing_stats_test.cpp -o /tmp/moody-timing-stats-test && /tmp/moody-timing-stats-test
  c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/frame_fifo_test.cpp -o /tmp/moody-frame-fifo-test && /tmp/moody-frame-fifo-test
  cmp -s moody-tx/moody-tx.ino 'moody-tx 2/moody-tx.ino'
  cmp -s moody-tx/software_jpeg_encoder.h 'moody-tx 2/software_jpeg_encoder.h'
  cmp -s moody-tx/software_jpeg_encoder.cpp 'moody-tx 2/software_jpeg_encoder.cpp'
  cmp -s moody-tx/README.md 'moody-tx 2/README.md'
  ```

  Expected: every command exits 0.

- [ ] **Step 3: Compile all firmware deliverables**

  Run both TX compilations with the ESP32-S3 FQBN and the RX compilation with
  `esp32:esp32:esp32` as shown in earlier tasks.

  Expected: all three builds PASS.

- [ ] **Step 4: Perform hardware acceptance test**

  Flash both sketches and collect five consecutive `tx` and `rx` timing lines.
  Confirm `source=software-new`, `failed=0`, a lower `encode_us` than the
  130–135 ms baseline, FIFO descriptor order without skipped frame IDs, and
  visible `age_ms` growth if the FIFO is persistently full. Record the observed
  FPS and latency in `docs/video-performance-analysis.md` as a follow-up
  measurement.
