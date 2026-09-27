#pragma once

#include <stdint.h>

constexpr uint32_t elapsedCounter(uint32_t now, uint32_t started) {
  return now - started;
}

constexpr uint32_t remoteToLocalCounterOffset(uint32_t remoteCounter,
                                              uint32_t localCounter) {
  return localCounter - remoteCounter;
}

constexpr uint32_t localizeRemoteCounter(uint32_t remoteCounter,
                                         uint32_t offset) {
  return remoteCounter + offset;
}

class TimingStats {
public:
  void add(uint32_t value) {
    sampleCount++;
    total += value;
    if (value > peak) peak = value;
  }

  uint32_t count() const { return sampleCount; }

  uint32_t average() const {
    return sampleCount == 0
      ? 0
      : static_cast<uint32_t>(total / sampleCount);
  }

  uint32_t maximum() const { return peak; }

  void reset() {
    sampleCount = 0;
    total = 0;
    peak = 0;
  }

private:
  uint32_t sampleCount = 0;
  uint64_t total = 0;
  uint32_t peak = 0;
};

class FrameOutcomeStats {
public:
  void record(uint32_t frameId, bool wasDisplayed) {
    attempts++;
    lastId = frameId;
    if (wasDisplayed) {
      displayedFrames++;
    } else {
      failedFrames++;
    }
  }

  uint32_t attempted() const { return attempts; }
  uint32_t displayed() const { return displayedFrames; }
  uint32_t failed() const { return failedFrames; }
  uint32_t lastFrameId() const { return lastId; }

  float displayedFps(uint32_t elapsedMs) const {
    return elapsedMs == 0
      ? 0.0f
      : (1000.0f * displayedFrames) / elapsedMs;
  }

  void reset() {
    attempts = 0;
    displayedFrames = 0;
    failedFrames = 0;
    lastId = 0;
  }

private:
  uint32_t attempts = 0;
  uint32_t displayedFrames = 0;
  uint32_t failedFrames = 0;
  uint32_t lastId = 0;
};
