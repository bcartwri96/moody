#pragma once

#include <stdint.h>

namespace moody_rx {

// Turns touch samples into taps: a held finger counts once.
struct TouchEdge {
  bool wasTouched = false;

  bool pressed(bool touched) {
    const bool newPress = touched && !wasTouched;
    wasTouched = touched;
    return newPress;
  }
};

// Tap-halves volume logic, kept hardware-free so it can be host-tested.
// Right half of the screen steps up, left half steps down. The caller draws
// the overlay and applies codecVolumeFor(volume()) to the ES8311.
class VolumeControl {
 public:
  static constexpr uint8_t kStep = 10;
  static constexpr uint8_t kMax = 100;
  static constexpr uint32_t kOverlayMs = 1500;

  explicit VolumeControl(uint8_t initial)
    : volume_(static_cast<uint8_t>((initial > kMax ? kMax : initial) / kStep * kStep)) {}

  void onTap(uint16_t x, uint16_t screenWidth, uint32_t nowMs) {
    if (x >= screenWidth / 2) {
      volume_ = volume_ + kStep > kMax ? kMax : static_cast<uint8_t>(volume_ + kStep);
    } else {
      volume_ = volume_ < kStep ? 0 : static_cast<uint8_t>(volume_ - kStep);
    }
    overlayVisible_ = true;
    overlayShownAtMs_ = nowMs;
  }

  // Returns true exactly once when the overlay's display time has elapsed.
  bool overlayExpired(uint32_t nowMs) {
    if (!overlayVisible_ || nowMs - overlayShownAtMs_ < kOverlayMs) return false;
    overlayVisible_ = false;
    return true;
  }

  uint8_t volume() const { return volume_; }
  bool overlayVisible() const { return overlayVisible_; }

 private:
  uint8_t volume_;
  bool overlayVisible_ = false;
  uint32_t overlayShownAtMs_ = 0;
};

// es8311_voice_volume_set() is linear in 0.5 dB register steps (100 = +32 dB),
// so map the UI percentage onto a usable window: 0 mutes, 70 matches the
// original fixed level of 70 (about -6.5 dB), and 100 is about +8.5 dB.
inline uint8_t codecVolumeFor(uint8_t uiVolume) {
  if (uiVolume == 0) return 0;
  if (uiVolume > VolumeControl::kMax) uiVolume = VolumeControl::kMax;
  return static_cast<uint8_t>(42 + uiVolume * 2 / 5);
}

}  // namespace moody_rx
