#ifndef MOODY_AUDIO_STREAM_H
#define MOODY_AUDIO_STREAM_H

#include <stddef.h>
#include <stdint.h>

namespace moody_audio {

constexpr uint8_t kPreamble[8] = {'M', 'O', 'O', 'D', 'Y', 'A', 'V', '1'};

constexpr uint8_t VIDEO_BEGIN = 1U;
constexpr uint8_t VIDEO_CHUNK = 2U;
constexpr uint8_t AUDIO_PCM = 3U;

constexpr size_t kRecordHeaderSize = 12U;
constexpr uint16_t kAudioPayloadLength = 640U;
constexpr uint32_t kMaxVideoChunkLength = 1024U;

struct RecordHeader {
  uint8_t type;
  uint8_t flags;
  uint16_t payloadLength;
  uint32_t sequence;
  uint32_t timestamp;
};

inline void writeU16BE(uint16_t value, uint8_t out[2]) {
  out[0] = static_cast<uint8_t>(value >> 8U);
  out[1] = static_cast<uint8_t>(value);
}

inline uint16_t readU16BE(const uint8_t in[2]) {
  return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) << 8U) |
         static_cast<uint16_t>(in[1]);
}

inline void writeU32BE(uint32_t value, uint8_t out[4]) {
  out[0] = static_cast<uint8_t>(value >> 24U);
  out[1] = static_cast<uint8_t>(value >> 16U);
  out[2] = static_cast<uint8_t>(value >> 8U);
  out[3] = static_cast<uint8_t>(value);
}

inline uint32_t readU32BE(const uint8_t in[4]) {
  return (static_cast<uint32_t>(in[0]) << 24U) |
         (static_cast<uint32_t>(in[1]) << 16U) |
         (static_cast<uint32_t>(in[2]) << 8U) |
         static_cast<uint32_t>(in[3]);
}

inline bool isKnownRecordType(uint8_t type) {
  return type == VIDEO_BEGIN || type == VIDEO_CHUNK || type == AUDIO_PCM;
}

inline bool validRecordPayload(uint8_t type, uint16_t payloadLength) {
  switch (type) {
    case VIDEO_BEGIN:
      return payloadLength == 8U;
    case VIDEO_CHUNK:
      return payloadLength >= 5U && payloadLength <= 1028U;
    case AUDIO_PCM:
      return payloadLength == kAudioPayloadLength;
    default:
      return false;
  }
}

inline bool encodeRecordHeader(const RecordHeader &header,
                               uint8_t out[kRecordHeaderSize]) {
  if (!isKnownRecordType(header.type) ||
      !validRecordPayload(header.type, header.payloadLength)) {
    return false;
  }

  out[0] = header.type;
  out[1] = header.flags;
  writeU16BE(header.payloadLength, out + 2U);
  writeU32BE(header.sequence, out + 4U);
  writeU32BE(header.timestamp, out + 8U);
  return true;
}

inline bool decodeRecordHeader(const uint8_t *in, size_t length,
                               RecordHeader &header) {
  if (in == NULL || length < kRecordHeaderSize) {
    return false;
  }

  const uint8_t type = in[0];
  const uint16_t payloadLength = readU16BE(in + 2U);
  if (!isKnownRecordType(type) || !validRecordPayload(type, payloadLength)) {
    return false;
  }

  header.type = type;
  header.flags = in[1];
  header.payloadLength = payloadLength;
  header.sequence = readU32BE(in + 4U);
  header.timestamp = readU32BE(in + 8U);
  return true;
}

inline bool validVideoChunk(uint32_t total, uint32_t expectedOffset,
                            uint32_t offset, uint32_t chunkLength) {
  if (chunkLength == 0U || chunkLength > kMaxVideoChunkLength ||
      offset != expectedOffset || offset > total ||
      chunkLength > UINT32_MAX - offset || offset + chunkLength > total) {
    return false;
  }
  return true;
}

inline bool validVideoFrameLength(uint32_t total, uint32_t maximumLength) {
  return total != 0U && total <= maximumLength;
}

}  // namespace moody_audio

#endif
