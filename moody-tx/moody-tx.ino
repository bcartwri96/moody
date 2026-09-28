#include <Arduino.h>
#include <WiFi.h>
#include "driver/i2s.h"
#include "esp_camera.h"
#include "esp_system.h"
#include "esp_log.h"
#include "software_jpeg_encoder.h"
#include "audio_pins.h"
#include "audio_stream.h"

// The attached USB bridge is wired to UART0; native USB CDC is not exposed.
#define Serial Serial0

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

#include "moody_keys.h"
#include "timing_stats.h"

// Freenove ESP32-S3-WROOM CAM / OV2640 pin map.
#define CAM_PWDN  -1
#define CAM_RESET -1
#define CAM_XCLK  15
#define CAM_SIOD   4
#define CAM_SIOC   5
#define CAM_D0    11
#define CAM_D1     9
#define CAM_D2     8
#define CAM_D3    10
#define CAM_D4    12
#define CAM_D5    18
#define CAM_D6    17
#define CAM_D7    16
#define CAM_VSYNC  6
#define CAM_HREF   7
#define CAM_PCLK  13

// This is a dedicated device-to-device network, not your home Wi-Fi.
// Change both values before deployment. Password must be at least 8 characters.
static const char *AP_SSID = "moody-camera";
static const char *AP_PASSWORD = "CHANGE-ME-9274";
static constexpr uint8_t AP_CHANNEL = 6;
static constexpr uint16_t STREAM_PORT = 3333;
static constexpr uint8_t TARGET_FPS = 20;
static constexpr uint32_t FRAME_INTERVAL_MS = 1000 / TARGET_FPS;
static constexpr uint32_t IO_TIMEOUT_MS = 5000;

static const uint8_t TX_HELLO_MAGIC[8] = {'M','O','O','D','Y','T','X','1'};
static const uint8_t RX_AUTH_MAGIC[8]  = {'M','O','O','D','Y','R','X','1'};
static const uint8_t AUTH_OK_MAGIC[8]  = {'A','U','T','H','O','K','0','1'};
static const uint8_t AUTH_FAIL_MAGIC[8]= {'A','U','T','H','N','O','0','1'};
static const char TX_CONTEXT[] = "moody-tx-proof-v1";
static const char RX_CONTEXT[] = "moody-rx-proof-v1";

static constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
static constexpr size_t AUDIO_SAMPLES_PER_BLOCK = 320;
static constexpr size_t AUDIO_BLOCK_QUEUE_DEPTH = 4;

struct AudioBlock {
  uint32_t sampleTimeMs;
  int16_t samples[320];
};

static_assert(sizeof(AudioBlock::samples) == moody_audio::kAudioPayloadLength,
              "Audio blocks must contain 20 ms of 16-bit PCM");

WiFiServer streamServer(STREAM_PORT);

mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctrDrbg;
mbedtls_pk_context txPrivateKey;
mbedtls_pk_context rxPublicKey;

TimingStats txCaptureUs;
TimingStats txEncodeUs;
TimingStats txSendUs;
TimingStats txJpegBytes;
uint32_t txTimingWindowStart = 0;
bool txUsesSoftwareJpeg = false;
SoftwareJpegEncoder softwareJpegEncoder;

QueueHandle_t audioBlockQueue = nullptr;
SemaphoreHandle_t audioQueueMutex = nullptr;
SemaphoreHandle_t audioCaptureStopped = nullptr;
TaskHandle_t audioCaptureTask = nullptr;
volatile bool audioCaptureRunning = false;
bool audioI2sInstalled = false;
uint32_t txRecordSequence = 0;

static void writeU16BE(uint8_t out[2], uint16_t value) {
  out[0] = static_cast<uint8_t>(value >> 8);
  out[1] = static_cast<uint8_t>(value);
}

static uint16_t readU16BE(const uint8_t in[2]) {
  return (static_cast<uint16_t>(in[0]) << 8) | in[1];
}

static void writeU32BE(uint8_t out[4], uint32_t value) {
  out[0] = static_cast<uint8_t>(value >> 24);
  out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8);
  out[3] = static_cast<uint8_t>(value);
}

static bool writeAll(WiFiClient &client, const uint8_t *data, size_t length) {
  const uint32_t deadline = millis() + IO_TIMEOUT_MS;
  size_t sent = 0;

  while (sent < length && client.connected()) {
    const size_t written = client.write(data + sent, length - sent);
    if (written > 0) {
      sent += written;
      continue;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
    delay(1);
  }
  return sent == length;
}

static bool readExact(WiFiClient &client, uint8_t *data, size_t length) {
  const uint32_t deadline = millis() + IO_TIMEOUT_MS;
  size_t received = 0;

  while (received < length && client.connected()) {
    const int available = client.available();
    if (available > 0) {
      const int count = client.read(data + received, length - received);
      if (count > 0) received += static_cast<size_t>(count);
      continue;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
    delay(1);
  }
  return received == length;
}

static void audioCaptureTaskMain(void *) {
  int32_t microphoneSlots[AUDIO_SAMPLES_PER_BLOCK];
  uint32_t pcmReportAt = millis();
  uint32_t pcmPeak = 0;
  uint64_t pcmAbsoluteSum = 0;
  uint32_t pcmSampleCount = 0;
  uint32_t pcmClippedCount = 0;
  uint32_t pcmNonzeroCount = 0;
  uint32_t rawNonzeroCount = 0;
  uint32_t rawUpperNonzeroCount = 0;
  uint32_t rawLowerNonzeroCount = 0;
  uint32_t rawExampleCount = 0;
  uint32_t rawExamples[4] = {};

  while (audioCaptureRunning) {
    size_t samplesRead = 0;
    while (audioCaptureRunning && samplesRead < AUDIO_SAMPLES_PER_BLOCK) {
      size_t bytesRead = 0;
      const esp_err_t result = i2s_read(
        I2S_NUM_0,
        microphoneSlots + samplesRead,
        (AUDIO_SAMPLES_PER_BLOCK - samplesRead) * sizeof(microphoneSlots[0]),
        &bytesRead,
        pdMS_TO_TICKS(100));
      if (result != ESP_OK) {
        if (audioCaptureRunning) {
          ESP_LOGE("moody-tx", "I2S microphone read failed: 0x%x", result);
          Serial.printf("I2S microphone read failed: 0x%x\n", result);
        }
        samplesRead = 0;
        break;
      }
      if (bytesRead % sizeof(microphoneSlots[0]) != 0) {
        ESP_LOGE("moody-tx", "I2S microphone read returned %u partial bytes",
                 static_cast<unsigned>(bytesRead));
        samplesRead = 0;
        break;
      }
      samplesRead += bytesRead / sizeof(microphoneSlots[0]);
    }

    if (!audioCaptureRunning) break;
    if (samplesRead != AUDIO_SAMPLES_PER_BLOCK) continue;

    AudioBlock block = {};
    block.sampleTimeMs = millis();
    for (size_t sample = 0; sample < AUDIO_SAMPLES_PER_BLOCK; ++sample) {
      const uint32_t raw = static_cast<uint32_t>(microphoneSlots[sample]);
      if (raw != 0) ++rawNonzeroCount;
      if ((raw >> 16U) != 0) ++rawUpperNonzeroCount;
      if ((raw & 0xffffU) != 0) ++rawLowerNonzeroCount;
      // Keep at most four distinct nonzero words; never log the audio buffer.
      if (raw != 0 && rawExampleCount < 4) {
        bool seen = false;
        for (uint32_t i = 0; i < rawExampleCount; ++i) {
          if (rawExamples[i] == raw) seen = true;
        }
        if (!seen) rawExamples[rawExampleCount++] = raw;
      }
      // INMP441's signed 24-bit word is left-justified in the 32-bit slot.
      block.samples[sample] = static_cast<int16_t>(microphoneSlots[sample] >> 16);
      const int32_t value = block.samples[sample];
      const uint32_t magnitude = static_cast<uint32_t>(value < 0 ? -value : value);
      if (magnitude > pcmPeak) pcmPeak = magnitude;
      pcmAbsoluteSum += magnitude;
      ++pcmSampleCount;
      if (value != 0) ++pcmNonzeroCount;
      if (value == INT16_MIN || value == INT16_MAX) ++pcmClippedCount;
    }

    // Keep the full-queue replacement atomic with the stream owner's dequeue.
    // A missed immediate lock simply drops this fresh block; it never disturbs
    // already queued audio or delays media/socket work.
    if (xSemaphoreTake(audioQueueMutex, 0) == pdPASS) {
      if (xQueueSend(audioBlockQueue, &block, 0) != pdPASS) {
        AudioBlock discarded;
        xQueueReceive(audioBlockQueue, &discarded, 0);
        xQueueSend(audioBlockQueue, &block, 0);
      }
      xSemaphoreGive(audioQueueMutex);
    }
    const uint32_t pcmNow = millis();
    if (pcmNow - pcmReportAt >= 5000) {
      Serial.printf("mic PCM: samples=%lu peak=%lu mean_abs=%lu clipped=%lu nonzero=%lu window_ms=%lu\n",
                    static_cast<unsigned long>(pcmSampleCount),
                    static_cast<unsigned long>(pcmPeak),
                    static_cast<unsigned long>(pcmSampleCount ? pcmAbsoluteSum / pcmSampleCount : 0),
                    static_cast<unsigned long>(pcmClippedCount),
                    static_cast<unsigned long>(pcmNonzeroCount),
                    static_cast<unsigned long>(pcmNow - pcmReportAt));
      Serial.printf("mic raw32: nonzero=%lu upper16_nonzero=%lu lower16_nonzero=%lu examples=%lu %08lx %08lx %08lx %08lx\n",
                    static_cast<unsigned long>(rawNonzeroCount),
                    static_cast<unsigned long>(rawUpperNonzeroCount),
                    static_cast<unsigned long>(rawLowerNonzeroCount),
                    static_cast<unsigned long>(rawExampleCount),
                    static_cast<unsigned long>(rawExamples[0]),
                    static_cast<unsigned long>(rawExamples[1]),
                    static_cast<unsigned long>(rawExamples[2]),
                    static_cast<unsigned long>(rawExamples[3]));
      rawNonzeroCount = rawUpperNonzeroCount = rawLowerNonzeroCount = 0;
      rawExampleCount = 0;
      memset(rawExamples, 0, sizeof(rawExamples));
      pcmReportAt = pcmNow;
      pcmPeak = pcmSampleCount = pcmClippedCount = pcmNonzeroCount = 0;
      pcmAbsoluteSum = 0;
    }
  }

  // Teardown remains the only owner of the task handle and deletion. Keeping
  // this task suspended after it reports completion prevents a dual-core race
  // between task completion and endAudioCapture().
  xSemaphoreGive(audioCaptureStopped);
  for (;;) vTaskSuspend(nullptr);
}

static void endAudioCapture() {
  audioCaptureRunning = false;
  if (audioI2sInstalled) {
    const esp_err_t stopResult = i2s_stop(I2S_NUM_0);
    if (stopResult != ESP_OK) {
      ESP_LOGW("moody-tx", "I2S microphone stop returned: 0x%x", stopResult);
    }
  }
  if (audioCaptureTask != nullptr) {
    // The completion semaphore is created before the task, so a live task
    // always has a valid join signal. Its read loop has a finite I2S timeout;
    // wait for that loop to finish before deleting the task or its resources.
    configASSERT(audioCaptureStopped != nullptr);
    xSemaphoreTake(audioCaptureStopped, portMAX_DELAY);
    vTaskDelete(audioCaptureTask);
    audioCaptureTask = nullptr;
  }
  if (audioI2sInstalled) {
    i2s_driver_uninstall(I2S_NUM_0);
    audioI2sInstalled = false;
  }
  if (audioCaptureStopped != nullptr) {
    vSemaphoreDelete(audioCaptureStopped);
    audioCaptureStopped = nullptr;
  }
  if (audioQueueMutex != nullptr) {
    vSemaphoreDelete(audioQueueMutex);
    audioQueueMutex = nullptr;
  }
  if (audioBlockQueue != nullptr) {
    xQueueReset(audioBlockQueue);
    vQueueDelete(audioBlockQueue);
    audioBlockQueue = nullptr;
  }
}

static bool beginAudioCapture() {
  endAudioCapture();

  audioBlockQueue = xQueueCreate(AUDIO_BLOCK_QUEUE_DEPTH, sizeof(AudioBlock));
  if (audioBlockQueue == nullptr) {
    ESP_LOGE("moody-tx", "could not allocate microphone block queue");
    return false;
  }
  audioQueueMutex = xSemaphoreCreateMutex();
  if (audioQueueMutex == nullptr) {
    ESP_LOGE("moody-tx", "could not allocate microphone queue mutex");
    endAudioCapture();
    return false;
  }
  audioCaptureStopped = xSemaphoreCreateBinary();
  if (audioCaptureStopped == nullptr) {
    ESP_LOGE("moody-tx", "could not allocate microphone task completion semaphore");
    endAudioCapture();
    return false;
  }

  const i2s_config_t config = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
  };
  const i2s_pin_config_t pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = moody_audio::kMicBclkGpio,
    .ws_io_num = moody_audio::kMicLrclkGpio,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = moody_audio::kMicDataGpio,
  };

  esp_err_t result = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);
  if (result != ESP_OK) {
    ESP_LOGE("moody-tx", "I2S microphone install failed: 0x%x", result);
    Serial.printf("I2S microphone install failed: 0x%x\n", result);
    endAudioCapture();
    return false;
  }
  audioI2sInstalled = true;

  result = i2s_set_pin(I2S_NUM_0, &pins);
  if (result != ESP_OK) {
    ESP_LOGE("moody-tx", "I2S microphone pin setup failed: 0x%x", result);
    Serial.printf("I2S microphone pin setup failed: 0x%x\n", result);
    endAudioCapture();
    return false;
  }

  audioCaptureRunning = true;
  if (xTaskCreatePinnedToCore(audioCaptureTaskMain,
                              "moody-mic",
                              8192,
                              nullptr,
                              1,
                              &audioCaptureTask,
                              tskNO_AFFINITY) != pdPASS) {
    ESP_LOGE("moody-tx", "could not start microphone capture task");
    Serial.println("Could not start microphone capture task");
    endAudioCapture();
    return false;
  }
  return true;
}

static bool tryDequeueAudioBlock(AudioBlock &block) {
  if (audioQueueMutex == nullptr ||
      audioBlockQueue == nullptr ||
      xSemaphoreTake(audioQueueMutex, 0) != pdPASS) {
    return false;
  }
  const bool dequeued = xQueueReceive(audioBlockQueue, &block, 0) == pdPASS;
  xSemaphoreGive(audioQueueMutex);
  return dequeued;
}

static bool sendRecord(WiFiClient &client,
                       uint8_t type,
                       uint32_t timestamp,
                       const uint8_t *payload,
                       uint16_t payloadLength) {
  // Submit one complete bounded record per socket write, avoiding a separate
  // tiny TCP send for every 12-byte header when TCP_NODELAY is enabled.
  uint8_t recordBytes[moody_audio::kRecordHeaderSize + 4 +
                      moody_audio::kMaxVideoChunkLength];
  const moody_audio::RecordHeader header = {
    type,
    0,
    payloadLength,
    txRecordSequence,
    timestamp,
  };
  if (payloadLength > sizeof(recordBytes) - moody_audio::kRecordHeaderSize ||
      !moody_audio::encodeRecordHeader(header, recordBytes)) {
    return false;
  }
  memcpy(recordBytes + moody_audio::kRecordHeaderSize, payload, payloadLength);
  if (!writeAll(client, recordBytes, moody_audio::kRecordHeaderSize + payloadLength)) {
    return false;
  }
  ++txRecordSequence;
  return true;
}

static bool sendAudioRecord(WiFiClient &client, const AudioBlock &block) {
  uint8_t pcm[moody_audio::kAudioPayloadLength];
  for (size_t sample = 0; sample < AUDIO_SAMPLES_PER_BLOCK; ++sample) {
    const uint16_t value = static_cast<uint16_t>(block.samples[sample]);
    pcm[sample * 2] = static_cast<uint8_t>(value);
    pcm[sample * 2 + 1] = static_cast<uint8_t>(value >> 8);
  }
  return sendRecord(client,
                    moody_audio::AUDIO_PCM,
                    block.sampleTimeMs,
                    pcm,
                    sizeof(pcm));
}

static bool sha256Parts(const char *context,
                        const uint8_t nonce[32],
                        uint8_t digest[32]) {
  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_setup(&md, info, 0) != 0 ||
      mbedtls_md_starts(&md) != 0 ||
      mbedtls_md_update(&md,
                        reinterpret_cast<const uint8_t *>(context),
                        strlen(context)) != 0 ||
      mbedtls_md_update(&md, nonce, 32) != 0 ||
      mbedtls_md_finish(&md, digest) != 0) {
    mbedtls_md_free(&md);
    return false;
  }
  mbedtls_md_free(&md);
  return true;
}

static bool initialiseCrypto() {
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctrDrbg);
  mbedtls_pk_init(&txPrivateKey);
  mbedtls_pk_init(&rxPublicKey);

  const char personalization[] = "moody-tx-v1";
  if (mbedtls_ctr_drbg_seed(&ctrDrbg,
                           mbedtls_entropy_func,
                           &entropy,
                           reinterpret_cast<const uint8_t *>(personalization),
                           sizeof(personalization) - 1) != 0) {
    return false;
  }

  if (mbedtls_pk_parse_key(
        &txPrivateKey,
        reinterpret_cast<const uint8_t *>(MOODY_TX_PRIVATE_KEY_PEM),
        strlen(MOODY_TX_PRIVATE_KEY_PEM) + 1,
        nullptr,
        0,
        mbedtls_ctr_drbg_random,
        &ctrDrbg) != 0) {
    return false;
  }

  return mbedtls_pk_parse_public_key(
           &rxPublicKey,
           reinterpret_cast<const uint8_t *>(MOODY_RX_PUBLIC_KEY_PEM),
           strlen(MOODY_RX_PUBLIC_KEY_PEM) + 1) == 0;
}

static bool initialiseCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_D0;
  config.pin_d1 = CAM_D1;
  config.pin_d2 = CAM_D2;
  config.pin_d3 = CAM_D3;
  config.pin_d4 = CAM_D4;
  config.pin_d5 = CAM_D5;
  config.pin_d6 = CAM_D6;
  config.pin_d7 = CAM_D7;
  config.pin_xclk = CAM_XCLK;
  config.pin_pclk = CAM_PCLK;
  config.pin_vsync = CAM_VSYNC;
  config.pin_href = CAM_HREF;
  config.pin_sccb_sda = CAM_SIOD;
  config.pin_sccb_scl = CAM_SIOC;
  config.pin_pwdn = CAM_PWDN;
  config.pin_reset = CAM_RESET;
  config.xclk_freq_hz = 24000000;
  // This Freenove sensor rejects JPEG mode during camera initialisation.
  // Start directly in RGB565 instead of probing a mode that never returns.
  config.pixel_format = PIXFORMAT_RGB565;
  // The 3:2 camera window fills moody-rx without stretching the image.
  config.frame_size = FRAMESIZE_QCIF;
  config.jpeg_quality = 16;
  // Keep FIFO semantics while allowing the sensor to fill the next buffer
  // during JPEG encoding. GRAB_LATEST stalled this RGB565 sensor.
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  txUsesSoftwareJpeg = true;
  esp_err_t result = esp_camera_init(&config);

  if (result != ESP_OK) {
    ESP_LOGE("moody-tx", "camera initialisation failed: 0x%x", result);
    Serial.printf("Camera init failed: 0x%x\n", result);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    Serial.printf("Camera sensor PID: 0x%04x\n", sensor->id.PID);
    if (sensor->id.PID == OV2640_PID) {
      sensor->set_hmirror(sensor, 1);
      sensor->set_vflip(sensor, 1);
    }
  }
  return true;
}

static bool authenticateReceiver(WiFiClient &client) {
  uint8_t nonce[32];
  esp_fill_random(nonce, sizeof(nonce));

  uint8_t txDigest[32];
  if (!sha256Parts(TX_CONTEXT, nonce, txDigest)) return false;

  uint8_t txSignature[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
  size_t txSignatureLength = 0;
  if (mbedtls_pk_sign(&txPrivateKey,
                      MBEDTLS_MD_SHA256,
                      txDigest,
                      sizeof(txDigest),
                      txSignature,
                      sizeof(txSignature),
                      &txSignatureLength,
                      mbedtls_ctr_drbg_random,
                      &ctrDrbg) != 0 ||
      txSignatureLength > UINT16_MAX) {
    return false;
  }

  uint8_t signatureLengthBytes[2];
  writeU16BE(signatureLengthBytes, static_cast<uint16_t>(txSignatureLength));

  if (!writeAll(client, TX_HELLO_MAGIC, sizeof(TX_HELLO_MAGIC)) ||
      !writeAll(client, nonce, sizeof(nonce)) ||
      !writeAll(client, signatureLengthBytes, sizeof(signatureLengthBytes)) ||
      !writeAll(client, txSignature, txSignatureLength)) {
    return false;
  }

  uint8_t receivedMagic[8];
  uint8_t rxLengthBytes[2];
  if (!readExact(client, receivedMagic, sizeof(receivedMagic)) ||
      memcmp(receivedMagic, RX_AUTH_MAGIC, sizeof(RX_AUTH_MAGIC)) != 0 ||
      !readExact(client, rxLengthBytes, sizeof(rxLengthBytes))) {
    return false;
  }

  const uint16_t rxSignatureLength = readU16BE(rxLengthBytes);
  if (rxSignatureLength == 0 || rxSignatureLength > MBEDTLS_PK_SIGNATURE_MAX_SIZE) {
    return false;
  }

  uint8_t rxSignature[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
  if (!readExact(client, rxSignature, rxSignatureLength)) return false;

  uint8_t rxDigest[32];
  if (!sha256Parts(RX_CONTEXT, nonce, rxDigest)) return false;

  const bool verified = mbedtls_pk_verify(&rxPublicKey,
                                          MBEDTLS_MD_SHA256,
                                          rxDigest,
                                          sizeof(rxDigest),
                                          rxSignature,
                                          rxSignatureLength) == 0;

  writeAll(client,
           verified ? AUTH_OK_MAGIC : AUTH_FAIL_MAGIC,
           sizeof(AUTH_OK_MAGIC));
  return verified;
}

static bool sendFrame(WiFiClient &client, uint32_t frameId) {
  if (frameId == 0) Serial.println("first frame: capture start");
  const uint32_t captureStarted = micros();
  camera_fb_t *frame = esp_camera_fb_get();
  txCaptureUs.add(micros() - captureStarted);
  if (!frame) return false;
  if (frameId == 0) {
    Serial.printf("first frame: captured %ux%u len=%u\n", frame->width,
                  frame->height, static_cast<unsigned>(frame->len));
  }

  const uint8_t *jpegData = frame->buf;
  size_t jpegLength = frame->len;

  if (frame->format != PIXFORMAT_JPEG) {
    if (frame->format != PIXFORMAT_RGB565 ||
        frame->width != kSoftwareJpegWidth ||
        frame->height != kSoftwareJpegHeight) {
      esp_camera_fb_return(frame);
      Serial.println("Unexpected software JPEG camera frame profile");
      return false;
    }
    const uint32_t encodeStarted = micros();
    if (frameId == 0) Serial.println("first frame: encode start");
    if (!softwareJpegEncoder.encode(*frame, jpegData, jpegLength)) {
      esp_camera_fb_return(frame);
      Serial.println("Software JPEG encoding failed");
      return false;
    }
    txEncodeUs.add(micros() - encodeStarted);
    if (frameId == 0) Serial.println("first frame: encode complete");
  }

  txJpegBytes.add(static_cast<uint32_t>(jpegLength));
  const uint32_t sendStarted = micros();
  uint8_t beginPayload[8];
  moody_audio::writeU32BE(frameId, beginPayload);
  moody_audio::writeU32BE(static_cast<uint32_t>(jpegLength), beginPayload + 4);

  if (frameId == 0) Serial.println("first frame: record send start");
  bool sent = sendRecord(client,
                         moody_audio::VIDEO_BEGIN,
                         millis(),
                         beginPayload,
                         sizeof(beginPayload));

  size_t offset = 0;
  while (sent && offset < jpegLength) {
    const size_t chunkLength = min(
      jpegLength - offset,
      static_cast<size_t>(moody_audio::kMaxVideoChunkLength));
    uint8_t chunkPayload[4 + moody_audio::kMaxVideoChunkLength];
    moody_audio::writeU32BE(static_cast<uint32_t>(offset), chunkPayload);
    memcpy(chunkPayload + 4, jpegData + offset, chunkLength);
    sent = sendRecord(client,
                      moody_audio::VIDEO_CHUNK,
                      millis(),
                      chunkPayload,
                      static_cast<uint16_t>(4 + chunkLength));
    offset += chunkLength;

    // A single audio record after each video chunk bounds time spent sending
    // one JPEG while preserving the record order owned by this task.
    AudioBlock block;
    if (sent && tryDequeueAudioBlock(block)) {
      sent = sendAudioRecord(client, block);
    }
  }
  if (frameId == 0) Serial.println(sent
    ? "first frame: record send complete"
    : "first frame: record send failed");
  txSendUs.add(micros() - sendStarted);

  esp_camera_fb_return(frame);
  return sent;
}

static void resetTxTiming(uint32_t now) {
  txCaptureUs.reset();
  txEncodeUs.reset();
  txSendUs.reset();
  txJpegBytes.reset();
  txTimingWindowStart = now;
}

static void reportTxTiming() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - txTimingWindowStart;
  if (elapsed < 1000 || txCaptureUs.count() == 0) return;

  const float fps = (1000.0f * txCaptureUs.count()) / elapsed;
  Serial.printf(
    "tx fps=%.1f frames=%lu source=%s jpeg_B=%lu/%lu "
    "capture_us=%lu/%lu encode_us=%lu/%lu send_us=%lu/%lu\n",
    fps,
    static_cast<unsigned long>(txCaptureUs.count()),
    txUsesSoftwareJpeg ? "software-new" : "sensor",
    static_cast<unsigned long>(txJpegBytes.average()),
    static_cast<unsigned long>(txJpegBytes.maximum()),
    static_cast<unsigned long>(txCaptureUs.average()),
    static_cast<unsigned long>(txCaptureUs.maximum()),
    static_cast<unsigned long>(txEncodeUs.average()),
    static_cast<unsigned long>(txEncodeUs.maximum()),
    static_cast<unsigned long>(txSendUs.average()),
    static_cast<unsigned long>(txSendUs.maximum())
  );
  resetTxTiming(now);
}

static void serveClient(WiFiClient client) {
  const int noDelayResult = client.setNoDelay(true);
  client.setTimeout(IO_TIMEOUT_MS);
  Serial.printf("media TCP: no_delay_set=%d no_delay=%u record_write_max=1040 bytes\n",
                noDelayResult, static_cast<unsigned>(client.getNoDelay()));

  Serial.printf("Client connected from %s\n", client.remoteIP().toString().c_str());

  if (!authenticateReceiver(client)) {
    Serial.println("Receiver authentication failed");
    endAudioCapture();
    client.stop();
    return;
  }

  if (!writeAll(client, moody_audio::kPreamble, sizeof(moody_audio::kPreamble))) {
    Serial.println("Could not send MOODYAV1 preamble");
    endAudioCapture();
    client.stop();
    return;
  }

  txRecordSequence = 0;
  if (beginAudioCapture()) {
    Serial.println("audio=enabled");
  } else {
    Serial.println("audio=disabled");
  }

  Serial.println("moody-rx authenticated; starting media stream");
  uint32_t frameId = 0;
  uint32_t nextFrameAt = millis();
  resetTxTiming(nextFrameAt);
  bool streaming = true;

  while (streaming && client.connected()) {
    while (client.connected() &&
           static_cast<int32_t>(nextFrameAt - millis()) > 0) {
      AudioBlock block;
      if (tryDequeueAudioBlock(block)) {
        if (!sendAudioRecord(client, block)) {
          streaming = false;
          break;
        }
        continue;
      }
      delay(1);
    }
    if (!streaming || !client.connected()) break;

    if (!sendFrame(client, frameId++)) {
      streaming = false;
      break;
    }
    reportTxTiming();
    nextFrameAt += FRAME_INTERVAL_MS;

    // If capture/transmission falls behind, resume from the current time rather
    // than creating an ever-growing latency queue.
    if (static_cast<int32_t>(millis() - nextFrameAt) >
        static_cast<int32_t>(FRAME_INTERVAL_MS)) {
      nextFrameAt = millis();
    }
  }

  endAudioCapture();
  client.stop();
  Serial.println("Receiver disconnected");
}

static bool benchmarkTxPipeline() {
  static constexpr uint8_t SAMPLE_COUNT = 8;
  TimingStats captureUs;
  TimingStats encodeUs;
  TimingStats jpegBytes;
  const uint32_t benchmarkStarted = micros();

  Serial.println("tx benchmark: start");
  for (uint8_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
    const uint32_t captureStarted = micros();
    camera_fb_t *frame = esp_camera_fb_get();
    captureUs.add(micros() - captureStarted);
    if (!frame) {
      Serial.printf("tx benchmark: capture failed sample=%u\n", sample);
      return false;
    }

    Serial.printf("tx benchmark: sample=%u frame=%ux%u len=%u\n", sample,
                  frame->width, frame->height,
                  static_cast<unsigned>(frame->len));
    const uint8_t *jpegData = nullptr;
    size_t jpegLength = 0;
    const uint32_t encodeStarted = micros();
    const bool encoded = softwareJpegEncoder.encode(*frame, jpegData, jpegLength);
    encodeUs.add(micros() - encodeStarted);
    esp_camera_fb_return(frame);
    if (!encoded) {
      Serial.printf("tx benchmark: encode failed sample=%u\n", sample);
      return false;
    }
    jpegBytes.add(static_cast<uint32_t>(jpegLength));
  }

  const uint32_t elapsedUs = micros() - benchmarkStarted;
  Serial.printf(
    "tx benchmark: fps=%.1f capture_us=%lu/%lu encode_us=%lu/%lu jpeg_B=%lu/%lu\n",
    elapsedUs ? (1000000.0f * SAMPLE_COUNT) / elapsedUs : 0.0f,
    static_cast<unsigned long>(captureUs.average()),
    static_cast<unsigned long>(captureUs.maximum()),
    static_cast<unsigned long>(encodeUs.average()),
    static_cast<unsigned long>(encodeUs.maximum()),
    static_cast<unsigned long>(jpegBytes.average()),
    static_cast<unsigned long>(jpegBytes.maximum())
  );
  return true;
}

void setup() {
  Serial0.begin(115200);
  delay(500);
  Serial.println("moody-tx UART0 console ready");
  Serial.println("\nmoody-tx starting");

  if (!psramFound()) {
    Serial.println("Warning: PSRAM not detected; check Tools > PSRAM > OPI PSRAM");
  }

  if (!initialiseCrypto()) {
    Serial.println("Fatal: cryptographic key initialisation failed");
    while (true) delay(1000);
  }

  if (!initialiseCamera()) {
    Serial.println("Fatal: OV2640 initialisation failed");
    while (true) delay(1000);
  }

  if (txUsesSoftwareJpeg && !softwareJpegEncoder.begin()) {
    Serial.println("Fatal: software JPEG encoder initialisation failed");
    while (true) delay(1000);
  }

  if (txUsesSoftwareJpeg && !benchmarkTxPipeline()) {
    Serial.println("Fatal: TX capture/encode benchmark failed");
    while (true) delay(1000);
  }

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, 1)) {
    ESP_LOGE("moody-tx", "access point creation failed");
    Serial.println("Fatal: could not create access point");
    while (true) delay(1000);
  }

  streamServer.begin();
  streamServer.setNoDelay(true);

  Serial.printf("AP: %s\n", AP_SSID);
  Serial.printf("Address: %s:%u\n",
                WiFi.softAPIP().toString().c_str(),
                STREAM_PORT);
  Serial.println("Waiting for permanently paired moody-rx");
  ESP_LOGE("moody-tx", "access point ready; waiting for receiver");
}

void loop() {
  WiFiClient client = streamServer.accept();
  if (client) {
    ESP_LOGE("moody-tx", "receiver TCP connection accepted");
    serveClient(client);
  }
  delay(10);
}
