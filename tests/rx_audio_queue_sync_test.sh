#!/bin/sh
set -eu

source_file="$(dirname "$0")/../moody-rx/moody-rx.ino"

# Queue-full replacement and playback dequeue share a mutex. The producer
# stays nonblocking and reports when lock contention drops an incoming block;
# timed playback polling happens outside the lock.
python3 - "$source_file" <<'PYCODE'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
start = source.index('class AudioPcmQueue {')
end = source.index('struct PartialVideoFrame', start)
queue = source[start:end]
enqueue_start = queue.index('bool enqueue(')
dequeue_start = queue.index('bool dequeue(')
private_start = queue.index(' private:', dequeue_start)
enqueue = queue[enqueue_start:dequeue_start]
dequeue = queue[dequeue_start:private_start]
private = queue[private_start:]
assert 'SemaphoreHandle_t mutex_ = nullptr;' in queue
assert 'mutex_ = xSemaphoreCreateMutex();' in queue
assert 'if (xSemaphoreTake(mutex_, 0) != pdPASS) {' in enqueue
assert 'return false;' in enqueue
assert enqueue.index('xSemaphoreTake(mutex_, 0)') < enqueue.index('xQueueSend(queue_, &record, 0)')
assert enqueue.index('xQueueReceive(queue_, &discarded, 0)') < enqueue.index('xQueueSend(queue_, &record, 0)', enqueue.index('xQueueReceive(queue_, &discarded, 0)'))
assert 'xSemaphoreGive(mutex_);' in enqueue
assert 'return tryDequeue(record);' in dequeue
assert 'xSemaphoreTake(mutex_, wait)' not in dequeue
assert 'if (xSemaphoreTake(mutex_, 0) != pdPASS) return false;' in private
assert 'xSemaphoreGive(mutex_);' in private
assert 'if (mutex_ == nullptr)' in queue

# Audio status is an explicit, machine-readable connection state. Queue-full
# and mutex-contention drops are separately counted, and the periodic playback
# report carries a safe depth/capacity snapshot without conflating it with
# transport record-sequence gaps.
assert 'struct AudioQueueTelemetry' in source
assert 'uint32_t fullDrops;' in source
assert 'uint32_t contentionDrops;' in source
assert 'uint32_t depth;' in source
assert '++fullDrops_;' in enqueue
assert '__atomic_fetch_add(&contentionDrops_' in enqueue
assert 'bool snapshot(AudioQueueTelemetry &telemetry)' in queue
assert 'uxQueueMessagesWaiting(queue_)' in queue
assert '__atomic_load_n(&contentionDrops_' in queue
assert 'bool takeTelemetry(AudioQueueTelemetry &telemetry)' in queue
assert '__atomic_exchange_n(&contentionDrops_' in queue
assert 'audio=ready rate=%lu Hz queue=%lu/%u' in source
assert 'audio=disabled rate=%lu Hz queue=%lu/%u' in source
assert 'queue=%lu/%u drops_full=%lu drops_contended=%lu' in source
assert 'Media record sequence gaps: %lu' in source
PYCODE
