#pragma once

// Freenove ESP32-S3-WROOM CAM / INMP441 I2S input wiring.
// These GPIOs are exposed on the board header and are not used by the camera,
// OPI PSRAM, USB, UART0, SD socket, or an ESP32-S3 boot-strap function.
namespace moody_audio {

constexpr int kMicBclkGpio = 41;
constexpr int kMicLrclkGpio = 21;  // GPIO42 header pin failed continuity on this board.
constexpr int kMicDataGpio = 47;

}  // namespace moody_audio
