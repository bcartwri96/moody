#pragma once

#include <Wire.h>
#include <stdint.h>

namespace moody_rx {

struct TouchPoint {
  bool touched = false;
  uint16_t rawX = 0;  // Panel-native portrait coordinates (0..319).
  uint16_t rawY = 0;  // (0..479)
};

// Minimal FT6336 reader on the shared board I2C bus. Reset is driven by the
// TCA9554 expander, so the caller owns it; this class only reads registers.
class Ft6336 {
 public:
  static constexpr uint8_t kAddress = 0x38;
  static constexpr uint8_t kRegTouchCount = 0x02;
  static constexpr uint8_t kRegChipId = 0xA3;
  static constexpr uint8_t kRegVendorId = 0xA8;
  static constexpr uint8_t kFocaltechVendorId = 0x11;

  explicit Ft6336(TwoWire &wire) : wire_(wire) {}

  bool readIds(uint8_t &chipId, uint8_t &vendorId) {
    return readRegisters(kRegChipId, &chipId, 1) &&
           readRegisters(kRegVendorId, &vendorId, 1);
  }

  bool read(TouchPoint &point) {
    // TD_STATUS, P1_XH, P1_XL, P1_YH, P1_YL.
    uint8_t bytes[5];
    if (!readRegisters(kRegTouchCount, bytes, sizeof(bytes))) return false;
    const uint8_t count = bytes[0] & 0x0f;
    const uint8_t event = bytes[1] >> 6;  // 0 down, 1 up, 2 contact, 3 none.
    point.touched = count >= 1 && count <= 2 && (event == 0 || event == 2);
    point.rawX = static_cast<uint16_t>(((bytes[1] & 0x0f) << 8) | bytes[2]);
    point.rawY = static_cast<uint16_t>(((bytes[3] & 0x0f) << 8) | bytes[4]);
    return true;
  }

 private:
  bool readRegisters(uint8_t reg, uint8_t *data, uint8_t length) {
    wire_.beginTransmission(kAddress);
    wire_.write(reg);
    if (wire_.endTransmission(true) != 0) return false;
    if (wire_.requestFrom(kAddress, length) != length) return false;
    for (uint8_t i = 0; i < length; ++i) data[i] = static_cast<uint8_t>(wire_.read());
    return true;
  }

  TwoWire &wire_;
};

}  // namespace moody_rx
