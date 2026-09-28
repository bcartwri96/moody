#pragma once

#include <stdint.h>

namespace moody_rx {
namespace board {

// Waveshare ESP32-Touch-LCD-3.5 (classic ESP32) display wiring.
constexpr int8_t kDisplayMisoPin = 19;
constexpr int8_t kDisplayMosiPin = 23;
constexpr int8_t kDisplaySclkPin = 18;
constexpr int8_t kDisplayCsPin = 5;
constexpr int8_t kDisplayDcPin = 27;
constexpr int8_t kDisplayResetPin = -1;
constexpr int8_t kDisplayBacklightPin = 25;
constexpr uint16_t kDisplayNativeWidth = 320;
constexpr uint16_t kDisplayNativeHeight = 480;

// This bus is shared by the TCA9554 board-control expander and ES8311 codec.
constexpr int8_t kI2cSdaPin = 21;
constexpr int8_t kI2cSclPin = 22;
constexpr uint8_t kTca9554Address = 0x20;
constexpr uint8_t kTcaLcdResetPin = 0;
constexpr uint8_t kTcaTouchResetPin = 1;
constexpr uint8_t kTcaAmplifierEnablePin = 2;

// FT6336 touch reports portrait coordinates; with display rotation 1 the
// landscape X axis follows the panel's raw Y. Flip if taps land mirrored.
constexpr bool kTouchLandscapeXFlipped = false;

// ES8311 derives its master clock from BCLK. Never drive GPIO3 MCLK:
// it is also UART0 RX, driven by the onboard CH343P USB bridge.
constexpr int8_t kAudioMclkPin = -1;
constexpr int8_t kAudioBclkPin = 2;
constexpr int8_t kAudioLrckPin = 4;
constexpr int8_t kAudioDataOutPin = 12;
constexpr int8_t kAudioDataInPin = 34;

}  // namespace board
}  // namespace moody_rx
