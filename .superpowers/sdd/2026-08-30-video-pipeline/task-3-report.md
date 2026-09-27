# Task 3 — FIFO helper report

## Files created

- `moody-rx/frame_fifo.h`
- `tests/frame_fifo_test.cpp`

## Red phase

Before the helper existed, the prescribed command failed as expected:

```text
tests/frame_fifo_test.cpp:4:10: fatal error: '../moody-rx/frame_fifo.h' file not found
```

Command:

```bash
c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/frame_fifo_test.cpp -o /tmp/moody-frame-fifo-test && /tmp/moody-frame-fifo-test
```

## Green phase

The same strict host command passed with exit status 0 and no output:

```bash
c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/frame_fifo_test.cpp -o /tmp/moody-frame-fifo-test && /tmp/moody-frame-fifo-test
```

## State-machine reasoning

Each slot starts `free`. `acquireFree` selects the lowest-numbered free slot
and transitions it to `held_by_reader`. Only a held slot can transition to
`ready` through `enqueueReady`, which appends its ID to the fixed-size circular
queue. `dequeueReady` removes the oldest ready ID and makes it held again until
`release` returns it to `free`. Invalid IDs, duplicate enqueue/release, release
of a ready slot, empty dequeue, and a full ready queue return `false` without
changing ownership. The helper uses only fixed-size arrays and standard C++
headers; it has no FreeRTOS dependency.
