#pragma once

#include <stddef.h>
#include <stdint.h>

namespace moody_audio {

// Preserve the signed PCM bit pattern in the upper half of both I2S slots.
// Unsigned shifts also handle negative samples without signed-shift UB.
inline void expandMono16LeToStereo32(const uint8_t *pcm, size_t sampleCount,
                                     uint32_t *slots) {
  for (size_t sample = 0; sample < sampleCount; ++sample) {
    const uint32_t bits = static_cast<uint32_t>(pcm[sample * 2]) |
                          (static_cast<uint32_t>(pcm[sample * 2 + 1]) << 8U);
    slots[sample * 2] = bits << 16U;
    slots[sample * 2 + 1] = bits << 16U;
  }
}

}  // namespace moody_audio
