#pragma once

#include <stddef.h>
#include <stdint.h>

static constexpr uint16_t kSoftwareJpegWidth = 176;
static constexpr uint16_t kSoftwareJpegHeight = 144;
static constexpr uint8_t kSoftwareJpegQuality = 100;
static constexpr size_t kSoftwareJpegOutputBytes = 128 * 1024;

static_assert(kSoftwareJpegWidth == 176, "Software JPEG width must match QCIF");
static_assert(kSoftwareJpegHeight == 144, "Software JPEG height must match QCIF");
static_assert((kSoftwareJpegWidth % 16) == 0,
              "RGB565 encoder width must be MCU aligned");
static_assert((kSoftwareJpegHeight % 16) == 0,
              "RGB565 encoder height must be MCU aligned");
