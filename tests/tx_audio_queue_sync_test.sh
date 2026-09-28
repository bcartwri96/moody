#!/bin/sh
set -eu

source_file="$(dirname "$0")/../moody-tx/moody-tx.ino"

# The producer's send/drop/retry and the serving task's nonblocking dequeue
# must share one immediate mutex. Socket writes stay outside these operations.
grep -F 'SemaphoreHandle_t audioQueueMutex = nullptr;' "$source_file" >/dev/null
grep -F 'audioQueueMutex = xSemaphoreCreateMutex();' "$source_file" >/dev/null
grep -F 'if (xSemaphoreTake(audioQueueMutex, 0) == pdPASS) {' "$source_file" >/dev/null
grep -F 'if (audioQueueMutex == nullptr ||' "$source_file" >/dev/null
grep -F 'xSemaphoreTake(audioQueueMutex, 0) != pdPASS' "$source_file" >/dev/null
grep -F 'vSemaphoreDelete(audioQueueMutex);' "$source_file" >/dev/null

# Teardown owns the task handle. The capture task signals completion and stays
# suspended until teardown deletes it, so it cannot invalidate that handle.
grep -F 'SemaphoreHandle_t audioCaptureStopped = nullptr;' "$source_file" >/dev/null
grep -F 'audioCaptureStopped = xSemaphoreCreateBinary();' "$source_file" >/dev/null
grep -F 'xSemaphoreGive(audioCaptureStopped);' "$source_file" >/dev/null
grep -F 'vTaskSuspend(nullptr);' "$source_file" >/dev/null
grep -F 'xSemaphoreTake(audioCaptureStopped, portMAX_DELAY)' "$source_file" >/dev/null
grep -F 'vSemaphoreDelete(audioCaptureStopped);' "$source_file" >/dev/null

# A completed capture task must be joined and deleted before shared I2S and
# queue synchronization resources are dismantled. The task handle is owned
# only by teardown and the task never self-deletes.
python3 - "$source_file" <<'PYCODE'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
task_start = source.index('static void audioCaptureTaskMain(')
task_end = source.index('static void endAudioCapture()', task_start)
task = source[task_start:task_end]
end_start = source.index('static void endAudioCapture()')
end_end = source.index('static bool beginAudioCapture()', end_start)
teardown = source[end_start:end_end]

assert 'xSemaphoreGive(audioCaptureStopped);' in task
assert 'for (;;) vTaskSuspend(nullptr);' in task
assert 'vTaskDelete(' not in task
assert 'audioCaptureTask =' not in task
handle_assignments = [
    (index, line.strip())
    for index, line in enumerate(source.splitlines())
    if line.strip().startswith('audioCaptureTask =')
]
assert len(handle_assignments) == 1
assert handle_assignments[0][1] == 'audioCaptureTask = nullptr;'
assert sum(line.strip().startswith('audioCaptureTask =') for line in teardown.splitlines()) == 1
assert 'xSemaphoreTake(audioCaptureStopped, portMAX_DELAY)' in teardown
assert 'vTaskDelete(audioCaptureTask);' in teardown
assert teardown.index('xSemaphoreTake(audioCaptureStopped, portMAX_DELAY)') < teardown.index('vTaskDelete(audioCaptureTask);')
assert teardown.index('vTaskDelete(audioCaptureTask);') < teardown.index('i2s_driver_uninstall(I2S_NUM_0);')
assert teardown.index('vTaskDelete(audioCaptureTask);') < teardown.index('vSemaphoreDelete(audioQueueMutex);')
assert teardown.index('vTaskDelete(audioCaptureTask);') < teardown.index('vQueueDelete(audioBlockQueue);')
PYCODE
