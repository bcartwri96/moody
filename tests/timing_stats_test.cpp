#include <cassert>

#include "../moody-rx/timing_stats.h"

int main() {
  assert(elapsedCounter(250, 100) == 150);
  assert(elapsedCounter(5, UINT32_MAX - 4) == 10);
  const uint32_t clockOffset = remoteToLocalCounterOffset(100, 1000);
  assert(localizeRemoteCounter(100, clockOffset) == 1000);
  assert(elapsedCounter(1120,
                        localizeRemoteCounter(110, clockOffset)) == 110);

  TimingStats stats;
  assert(stats.count() == 0);
  assert(stats.average() == 0);
  assert(stats.maximum() == 0);

  stats.add(100);
  stats.add(250);
  stats.add(150);
  assert(stats.count() == 3);
  assert(stats.average() == 166);
  assert(stats.maximum() == 250);

  stats.reset();
  assert(stats.count() == 0);
  assert(stats.average() == 0);
  assert(stats.maximum() == 0);

  stats.add(UINT32_MAX);
  stats.add(UINT32_MAX);
  assert(stats.average() == UINT32_MAX);
  assert(stats.maximum() == UINT32_MAX);
  stats.reset();

  FrameOutcomeStats frames;
  frames.record(10, true);
  frames.record(11, false);
  frames.record(12, true);
  assert(frames.attempted() == 3);
  assert(frames.displayed() == 2);
  assert(frames.failed() == 1);
  assert(frames.lastFrameId() == 12);
  assert(frames.displayedFps(1000) == 2.0f);

  frames.reset();
  assert(frames.attempted() == 0);
  assert(frames.displayed() == 0);
  assert(frames.failed() == 0);
  assert(frames.displayedFps(1000) == 0.0f);
}
