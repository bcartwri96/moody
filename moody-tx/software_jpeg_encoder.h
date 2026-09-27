#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_camera.h"
#include <esp_jpeg_enc.h>
#include "video_profile.h"

class SoftwareJpegEncoder {
 public:
  SoftwareJpegEncoder() = default;
  ~SoftwareJpegEncoder();

  SoftwareJpegEncoder(const SoftwareJpegEncoder &) = delete;
  SoftwareJpegEncoder &operator=(const SoftwareJpegEncoder &) = delete;

  bool begin();
  bool encode(const camera_fb_t &frame, const uint8_t *&jpegData,
              size_t &jpegLength);
  void end();

 private:
  jpeg_enc_handle_t handle_ = nullptr;
  uint8_t *output_ = nullptr;
};
