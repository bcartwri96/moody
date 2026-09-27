#include <cassert>
#include <cstdint>

#include "../moody-rx/frame_fifo.h"

int main() {
  FrameSlotFifo<2> fifo;
  uint8_t first = 0;
  uint8_t second = 0;
  uint8_t out = 0;

  assert(fifo.freeCount() == 2);
  assert(fifo.readyCount() == 0);
  assert(fifo.acquireFree(first) && first == 0);
  assert(fifo.acquireFree(second) && second == 1);
  assert(!fifo.acquireFree(out));

  assert(!fifo.enqueueReady(2));
  assert(!fifo.release(2));
  assert(fifo.enqueueReady(first));
  assert(!fifo.enqueueReady(first));
  assert(!fifo.release(first));
  assert(fifo.enqueueReady(second));

  assert(fifo.dequeueReady(out) && out == first);
  assert(fifo.release(out));
  assert(!fifo.release(out));
  assert(fifo.dequeueReady(out) && out == second);
  assert(fifo.release(out));
  assert(!fifo.dequeueReady(out));
  assert(fifo.freeCount() == 2);
  assert(fifo.readyCount() == 0);

  // The FreeRTOS free-slot queue can validly return a higher-numbered slot
  // first after the consumer releases slots on another core.
  assert(fifo.acquireSpecific(1));
  assert(fifo.acquireSpecific(0));
  assert(!fifo.acquireSpecific(0));
  assert(!fifo.acquireSpecific(2));
  assert(fifo.release(1));
  assert(fifo.release(0));
}
