#include "software_jpeg_encoder.h"

#include <stdint.h>

namespace {

constexpr size_t kSoftwareJpegInputBytes =
    static_cast<size_t>(kSoftwareJpegWidth) * kSoftwareJpegHeight * 2;
constexpr size_t kSoftwareJpegAlignment = 16;

static_assert(kSoftwareJpegInputBytes <= INT32_MAX,
              "Encoder input size must fit the Espressif API");
static_assert(kSoftwareJpegOutputBytes <= INT32_MAX,
              "Encoder output size must fit the Espressif API");

}  // namespace

SoftwareJpegEncoder::~SoftwareJpegEncoder() {
  end();
}

bool SoftwareJpegEncoder::begin() {
  if (handle_ && output_) return true;
  end();

  output_ = static_cast<uint8_t *>(
      jpeg_calloc_align(kSoftwareJpegOutputBytes, kSoftwareJpegAlignment));
  if (!output_) return false;

  jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
  config.width = kSoftwareJpegWidth;
  config.height = kSoftwareJpegHeight;
  config.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
  config.subsampling = JPEG_SUBSAMPLE_444;
  config.quality = kSoftwareJpegQuality;
  config.rotate = JPEG_ROTATE_0D;
  // Required on this S3: the mono-task encoder stalls after the first frame.
  config.task_enable = true;
  config.hfm_task_priority = 13;
  config.hfm_task_core = 0;

  if (jpeg_enc_open(&config, &handle_) != JPEG_ERR_OK || !handle_) {
    end();
    return false;
  }
  return true;
}

bool SoftwareJpegEncoder::encode(const camera_fb_t &frame,
                                 const uint8_t *&jpegData,
                                 size_t &jpegLength) {
  jpegData = nullptr;
  jpegLength = 0;

  if (!handle_ || !output_ || frame.format != PIXFORMAT_RGB565 ||
      frame.width != kSoftwareJpegWidth || frame.height != kSoftwareJpegHeight ||
      !frame.buf || frame.len != kSoftwareJpegInputBytes ||
      (reinterpret_cast<uintptr_t>(frame.buf) % kSoftwareJpegAlignment) != 0) {
    return false;
  }

  int outputSize = 0;
  if (jpeg_enc_process(handle_, frame.buf, static_cast<int>(frame.len), output_,
                       static_cast<int>(kSoftwareJpegOutputBytes),
                       &outputSize) != JPEG_ERR_OK ||
      outputSize <= 0 ||
      static_cast<size_t>(outputSize) > kSoftwareJpegOutputBytes) {
    return false;
  }

  jpegData = output_;
  jpegLength = static_cast<size_t>(outputSize);
  return true;
}

void SoftwareJpegEncoder::end() {
  if (handle_) {
    jpeg_enc_close(handle_);
    handle_ = nullptr;
  }
  if (output_) {
    jpeg_free_align(output_);
    output_ = nullptr;
  }
}
