#include <assert.h>
#include <stdint.h>
#include "../moody-rx/audio_playback_format.h"

int main() {
  // Silence, positive/negative full scale, -1 and distinct byte order.
  const uint8_t pcm[] = {0, 0, 0xff, 0x7f, 0, 0x80, 0xff, 0xff, 0x34, 0x12};
  const uint32_t expected[] = {0, 0x7fff0000U, 0x80000000U, 0xffff0000U, 0x12340000U};
  uint32_t slots[12] = {};
  slots[10] = slots[11] = 0xdeadbeefU;
  moody_audio::expandMono16LeToStereo32(pcm, 5, slots);
  for (unsigned i = 0; i < 5; ++i) {
    assert(slots[i * 2] == expected[i]);
    assert(slots[i * 2 + 1] == expected[i]);
  }
  assert(slots[10] == 0xdeadbeefU && slots[11] == 0xdeadbeefU);
  moody_audio::expandMono16LeToStereo32(nullptr, 0, nullptr);
}
