# Task 3 review — FIFO helper

## Verdict

**Accepted.** The helper meets the Task 3 interface and lifecycle requirements.

## Evidence

The prescribed strict host command was run on 2026-08-30:

```bash
c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/frame_fifo_test.cpp -o /tmp/moody-frame-fifo-test && /tmp/moody-frame-fifo-test
```

It exited with status 0 and produced no diagnostics.

## Lifecycle and invariants review

- Initial value-initialization makes every `SlotState` `free`; `freeCount()` is
  therefore `SlotCount` and `readyCount()` is zero.
- `acquireFree()` selects the lowest free slot and makes it
  `held_by_reader`; it cannot allocate a held or ready slot.
- `enqueueReady()` accepts only an in-range held slot, appends it at the tail,
  advances the circular index, increments the count, and marks the slot ready.
  Duplicate enqueue, enqueue of a free/ready slot, invalid IDs, and a full
  queue leave state unchanged.
- `dequeueReady()` reads only the head, returns the oldest ready slot, advances
  the head, decrements the count, and returns the slot to `held_by_reader`.
  It rejects an empty queue without mutation.
- `release()` accepts only an in-range held slot, preventing release of a
  ready/free slot and duplicate release. A dequeued slot cannot be reacquired
  until released.
- Queue contents, head/tail, and ready count are mutually consistent across
  all public legal transitions. The defensive state check in `dequeueReady()`
  also avoids consuming a malformed internal queue entry.

## Invalid cases and portability

The test covers invalid slot IDs, duplicate enqueue, release of a ready slot,
duplicate release, exhaustion of free slots, and empty dequeue. The header is
FreeRTOS-independent and compiles under strict C++17 using only standard
headers. `SlotCount` is asserted nonzero and bounded at 256 so every valid
slot ID fits in `uint8_t`; the modulo operation is consequently defined.

## Non-blocking coverage note

The current test proves the specified two-frame FIFO sequence but does not
exercise a wrap-around sequence (dequeue/release one slot, enqueue it again,
then verify order). The circular-index implementation itself is correct by
inspection; adding that regression case would strengthen future maintenance
coverage but is not required to accept Task 3.
