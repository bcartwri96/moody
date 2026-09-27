# Task 4 — RX two-slot FIFO integration report

## Result

Implemented the approved two-slot receiver pipeline in `moody-rx`.

- Setup allocates two `MAX_JPEG_SIZE` buffers with `ps_malloc` and fails on
  the existing LCD status screen if PSRAM or either allocation is unavailable.
- Each authenticated connection resets and seeds a two-entry free-slot queue
  with slots 0 then 1, then starts a dedicated FreeRTOS reader task.
- After authentication the reader task is the sole `WiFiClient` owner. It
  blocks on a free slot, reads one complete 16-byte `MJPG` header and JPEG
  payload, and appends its descriptor to the two-entry ready queue. With both
  slots occupied it blocks rather than discarding a frame, propagating TCP
  backpressure.
- The Arduino loop is the sole `JPEGDEC`, Arduino_GFX, and LCD owner. It
  consumes descriptors FIFO, records every complete frame as attempted, and
  returns the slot after both successful and failed JPEG decodes.
- Header, payload, protocol magic, frame ID, length, TX timestamp,
  authentication, timeout, and reconnect behaviour remain in place.
- Reader failures return an incomplete acquired slot, append one terminal
  descriptor after earlier ready frames, stop the client, signal completion,
  and self-delete. The display loop consumes the terminal only after earlier
  frames and waits for the completion signal before reusing the client or
  resetting queues. No deleted task handle is retained or dereferenced.
- RX telemetry retains the existing fields and adds
  `fifo_us=avg/max age_ms=avg/max queued=current_depth`. Failed complete JPEG
  decodes retain their header, payload, FIFO, age, JPEG-size, decode, and LCD
  samples.

## Clock-domain handling

The wire timestamp is TX `millis()`, which cannot be subtracted directly from
the independent RX `millis()` counter. On the first complete header of each
authenticated connection, the reader derives a wrap-safe remote-to-local
counter offset from the local header-arrival time. Each descriptor carries the
raw TX timestamp and that connection offset; the display loop translates the
timestamp into the RX counter domain before calculating `age_ms`.

This makes `age_ms` a connection-relative TX-header-to-display measurement.
Its first-frame baseline begins at the first header's RX arrival, so the
unobservable transit time of that calibration header is not included. Later
growth includes relative network delay, payload receipt, FIFO wait, decode
scheduling, and display backlog without being dominated by different device
boot times.

## Files changed

- `moody-rx/moody-rx.ino`
- `moody-rx/frame_fifo.h`
- `moody-rx/timing_stats.h`
- `tests/frame_fifo_test.cpp`
- `tests/timing_stats_test.cpp`
- `.superpowers/sdd/2026-08-30-video-pipeline/task-4-report.md`

No transmitter files or project documentation outside this report were
changed.

`frame_fifo.h` gained the narrow `acquireSpecific(slot)` transition required
when the FreeRTOS free queue returns a valid slot in an order other than the
helper's original lowest-numbered scan. The original Task 3 API and semantics
remain intact.

## Test-driven evidence

### Initial RED

Before adding wrap-safe counter helpers, the strict timing test failed with:

```text
tests/timing_stats_test.cpp:6:10: error: use of undeclared identifier 'elapsedCounter'
```

### Review regressions RED

Code review identified two integration defects. Tests added before their fixes
failed with the expected missing interfaces:

```text
error: no member named 'acquireSpecific' in 'FrameSlotFifo<2>'
error: use of undeclared identifier 'remoteToLocalCounterOffset'
error: use of undeclared identifier 'localizeRemoteCounter'
```

The slot test now covers a queue-selected higher-numbered free slot, duplicate
and invalid specific acquisition, and release. The timing test covers ordinary
and wrapping elapsed counters plus remote-to-local timestamp mapping.

### GREEN commands

```bash
c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/frame_fifo_test.cpp -o /tmp/moody-frame-fifo-test && /tmp/moody-frame-fifo-test
c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/timing_stats_test.cpp -o /tmp/moody-timing-stats-test && /tmp/moody-timing-stats-test
c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/video_layout_test.cpp -o /tmp/moody-video-layout-test && /tmp/moody-video-layout-test
/Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir /tmp/moody-rx-build moody-rx
```

All commands exited 0. The firmware compile reported:

```text
Sketch uses 1017411 bytes (77%) of program storage space. Maximum is 1310720 bytes.
Global variables use 65512 bytes (19%) of dynamic memory, leaving 262168 bytes for local variables. Maximum is 327680 bytes.
```

An early compile also exposed an Arduino sketch-preprocessor conflict with a
`[[noreturn]]` definition whose generated prototype omitted the attribute.
Removing only that attribute resolved the compiler error; the function retains
an explicit non-returning tail after self-deletion.

## Review

The first review found two Important issues: queue/helper slot-order divergence
under dual-core scheduling and subtraction of unrelated TX/RX uptime counters.
Both received failing regression tests and the fixes described above. The
follow-up review verdict was **Accepted**, with no remaining Critical or
Important findings.

## Caveats and follow-up

- No hardware was flashed in Task 4. Runtime FPS, FIFO saturation,
  `age_ms` growth, damaged-JPEG recovery, PSRAM availability, and reconnect
  behaviour still require the Task 5 hardware acceptance run.
- The two devices have no synchronized wall clock. `age_ms` therefore uses the
  connection-relative first-header calibration described above; it is useful
  for backlog growth and comparative latency, not an absolute one-way network
  measurement.
- This workspace has no Git metadata, as recorded in the execution ledger, so
  no commit, branch integration, or Git diff was produced.
