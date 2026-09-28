#include <cassert>
#include <cstdint>

#include "../moody-rx/audio_stream.h"

using moody_audio::AUDIO_PCM;
using moody_audio::VIDEO_BEGIN;
using moody_audio::VIDEO_CHUNK;
using moody_audio::RecordHeader;

static void testEndianHelpers() {
  uint8_t bytes[4] = {};
  moody_audio::writeU16BE(0xabcdU, bytes);
  assert(bytes[0] == 0xabU && bytes[1] == 0xcdU);
  assert(moody_audio::readU16BE(bytes) == 0xabcdU);

  moody_audio::writeU32BE(0x12345678UL, bytes);
  assert(bytes[0] == 0x12U && bytes[1] == 0x34U && bytes[2] == 0x56U &&
         bytes[3] == 0x78U);
  assert(moody_audio::readU32BE(bytes) == 0x12345678UL);
}

static void testRecordRoundTrips() {
  const RecordHeader records[] = {
      {VIDEO_BEGIN, 0x01U, 8U, 0xffffffffUL, 0x01020304UL},
      {VIDEO_CHUNK, 0x02U, 1028U, 0U, 0xffffffffUL},
      {AUDIO_PCM, 0U, 640U, 0x80000000UL, 0x00000001UL},
  };

  for (const RecordHeader &expected : records) {
    uint8_t wire[12] = {};
    RecordHeader actual{};
    assert(moody_audio::encodeRecordHeader(expected, wire));
    assert(moody_audio::decodeRecordHeader(wire, sizeof(wire), actual));
    assert(actual.type == expected.type);
    assert(actual.flags == expected.flags);
    assert(actual.payloadLength == expected.payloadLength);
    assert(actual.sequence == expected.sequence);
    assert(actual.timestamp == expected.timestamp);
  }
}

static void testHeaderValidation() {
  uint8_t wire[12] = {};
  RecordHeader decoded{};

  assert(!moody_audio::encodeRecordHeader({AUDIO_PCM, 0U, 0U, 0U, 0U}, wire));
  assert(!moody_audio::encodeRecordHeader({AUDIO_PCM, 0U, 641U, 0U, 0U}, wire));
  assert(!moody_audio::encodeRecordHeader({VIDEO_CHUNK, 0U, 4U, 0U, 0U}, wire));
  assert(!moody_audio::encodeRecordHeader({VIDEO_CHUNK, 0U, 1029U, 0U, 0U}, wire));
  assert(!moody_audio::encodeRecordHeader({VIDEO_BEGIN, 0U, 7U, 0U, 0U}, wire));
  assert(!moody_audio::encodeRecordHeader({99U, 0U, 8U, 0U, 0U}, wire));

  assert(moody_audio::encodeRecordHeader(
      {VIDEO_BEGIN, 0U, 8U, 0xffffffffUL, 0xffffffffUL}, wire));
  assert(moody_audio::decodeRecordHeader(wire, sizeof(wire), decoded));
  assert(decoded.sequence == 0xffffffffUL);
  assert(!moody_audio::decodeRecordHeader(wire, sizeof(wire) - 1U, decoded));

  wire[0] = 99U;
  assert(!moody_audio::decodeRecordHeader(wire, sizeof(wire), decoded));
}

static void testVideoChunkBounds() {
  assert(moody_audio::validVideoChunk(2048U, 0U, 0U, 1U));
  assert(moody_audio::validVideoChunk(2048U, 1024U, 1024U, 1024U));
  assert(!moody_audio::validVideoChunk(2048U, 0U, 0U, 0U));
  assert(!moody_audio::validVideoChunk(2048U, 0U, 0U, 1025U));
  assert(!moody_audio::validVideoChunk(2048U, 0U, 1U, 1U));
  assert(!moody_audio::validVideoChunk(2048U, 0U, 2048U, 1U));
  assert(!moody_audio::validVideoChunk(2048U, 2048U, 2048U, 1U));
  assert(!moody_audio::validVideoChunk(0xffffffffUL, 0xffffffffUL,
                                       0xffffffffUL, 1U));
}

static void testVideoFrameLengthBounds() {
  constexpr uint32_t kMaxJpegSize = 128U * 1024U;

  assert(moody_audio::validVideoFrameLength(1U, kMaxJpegSize));
  assert(moody_audio::validVideoFrameLength(kMaxJpegSize, kMaxJpegSize));
  assert(!moody_audio::validVideoFrameLength(0U, kMaxJpegSize));
  assert(!moody_audio::validVideoFrameLength(kMaxJpegSize + 1U,
                                              kMaxJpegSize));
}

int main() {
  testEndianHelpers();
  testRecordRoundTrips();
  testHeaderValidation();
  testVideoChunkBounds();
  testVideoFrameLengthBounds();
}
