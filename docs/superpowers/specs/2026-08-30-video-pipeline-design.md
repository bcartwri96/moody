# Video encoder and FIFO receive pipeline

## Goal

Increase the live video rate on the existing 480 x 320 Moody stream while
preserving JPEG-over-TCP framing, mutual authentication, image geometry, and
every received frame. The test deliberately combines a faster TX software
encoder with a two-slot RX FIFO.

## Constraints

- The transmitter's detected camera does not provide sensor JPEG; it captures
  RGB565.
- The transmitter and receiver continue using their existing authenticated TCP
  connection and 16-byte `MJPG` frame header.
- The display remains full-screen 480 x 320 with preserve-and-crop behaviour.
- RX preserves FIFO order. It does not discard frames to reduce latency.
- Both `moody-tx` directories remain byte-identical.

## Options considered

1. Keep the current encoder and add only the RX FIFO. This improves TCP
   backpressure but leaves the measured 133 ms TX encoder bottleneck in place.
2. Replace only the encoder. This isolates encoder performance, but RX still
   stops reading while it displays a frame, causing TCP write stalls.
3. Replace the encoder and add a bounded FIFO receiver. This is the chosen
   option: it tests both known constraints simultaneously without changing the
   on-wire protocol. A full FIFO backs up through TCP when full, preserving
   frames and allowing latency to grow as requested.

## Transmitter design

### Dependency

Vendor a pinned release of Espressif `esp_new_jpeg`, including its licence,
headers, and ESP32-S3 precompiled archive, under each transmitter sketch. Add
the minimal Arduino library metadata required to link the archive for the
ESP32-S3 FQBN used by this project. The dependency is local to the sketch, so
the user does not need to install an additional library.

### Encoder boundary

Create a small `SoftwareJpegEncoder` wrapper that owns:

- one `esp_new_jpeg` encoder handle, configured once for 480 x 320 RGB565,
  JPEG 4:2:0, quality 75, no rotation, and mono-task operation;
- an aligned output buffer sized to the existing maximum JPEG payload;
- validation that the camera input buffer meets the encoder alignment
  requirement; and
- explicit open, encode, and close operations.

`sendFrame` continues to send the existing header followed by one JPEG payload.
When the camera frame is already JPEG it remains zero-copy. When it is RGB565,
the wrapper replaces `frame2jpg`; an encoder setup or encode error fails the
stream visibly rather than silently reverting to the legacy encoder.

Telemetry changes `source=software` to `source=software-new` for this path,
so the experiment cannot be mistaken for the old fallback. Existing capture,
encode, send, payload-size, and FPS measurements remain intact.

## Receiver design

### Ownership and queues

Allocate two `MAX_JPEG_SIZE` PSRAM buffers. Maintain two FreeRTOS queues:

- `freeSlots`, initially containing slot IDs 0 and 1;
- `readyFrames`, containing filled frame descriptors in arrival order.

Each descriptor contains slot ID, frame ID, TX timestamp, JPEG length, header
wait duration, payload receive duration, and the local enqueue time.

After mutual authentication, a dedicated reader task becomes the sole owner of
the `WiFiClient`. It waits for a free slot, reads one complete framed JPEG into
that slot, then appends its descriptor to `readyFrames`. The main loop becomes
the sole owner of `JPEGDEC`, Arduino_GFX, and LCD writes: it removes the oldest
descriptor, displays it, and returns the slot to `freeSlots`.

When both slots are occupied, the reader blocks waiting for a returned slot.
It therefore stops reading TCP, propagating backpressure to TX and preserving
all frames in FIFO order. This is bounded local memory with growing end-to-end
latency, not frame dropping.

### Failure and lifecycle rules

- A malformed header, invalid JPEG length, socket failure, or read timeout is
  emitted as a terminal reader event after any already queued frames.
- The display loop drains earlier FIFO frames, then stops the connection,
  presents the existing reconnect status, and retries as it does today.
- A damaged but complete JPEG remains recoverable: it is recorded as a failed
  display, its slot is returned, and later frames continue.
- Only the reader task accesses the stream after authentication; only the main
  loop accesses the display. This avoids concurrent use of non-thread-safe
  objects.

### Telemetry

The main loop remains the sole writer of the existing timing statistics. It
adds reader-provided header/payload timings when it dequeues each descriptor,
and reports:

- FIFO wait from enqueue to display start;
- frame age from TX header timestamp to display start; and
- queued frame count at dequeue.

The existing `attempted`, `displayed`, `failed`, JPEG size, receive, decode,
LCD, and FPS fields remain available. This distinguishes an improved frame
rate from an improvement obtained only by concealing latency.

## Validation

1. Add host tests for FIFO slot lifecycle: initial free order, FIFO delivery,
   no slot reuse until returned, full-queue blocking state, and recoverable
   decode-failure release.
2. Compile the host tests and both Arduino sketches. Confirm both transmitter
   directories are identical.
3. Flash both devices and capture at least five consecutive TX and RX timing
   lines. Success means `source=software-new`, no failed frames, a lower
   `encode_us` than the 130–135 ms baseline, and the FIFO telemetry confirms
   ordered frame preservation. The report must include frame age so the
   throughput/latency trade-off is explicit.

## Expected result and limit

The encoder change is expected to reduce the TX software-encode cost. The RX
FIFO should remove much of the observed TCP send/receive stall by allowing
network reading to proceed while the previous image is displayed. It does not
remove the measured serial JPEG decode plus LCD-write cost of about 139 ms;
therefore a sustained rate substantially above about 7 fps will require a
separate RX decode/display or resolution optimisation.
