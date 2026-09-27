#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_system.h"
#include "esp_log.h"
#include "software_jpeg_encoder.h"

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
static const uint8_t FRAME_MAGIC[4]    = {'M','J','P','G'};
static const char TX_CONTEXT[] = "moody-tx-proof-v1";
static const char RX_CONTEXT[] = "moody-rx-proof-v1";

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

  uint8_t header[16];
  memcpy(header, FRAME_MAGIC, sizeof(FRAME_MAGIC));
  writeU32BE(header + 4, frameId);
  writeU32BE(header + 8, static_cast<uint32_t>(jpegLength));
  writeU32BE(header + 12, millis());

  const uint32_t sendStarted = micros();
  if (frameId == 0) Serial.println("first frame: header send start");
  const bool headerSent = writeAll(client, header, sizeof(header));
  if (frameId == 0) Serial.println(headerSent
    ? "first frame: header send complete"
    : "first frame: header send failed");
  const bool sent = headerSent && writeAll(client, jpegData, jpegLength);
  if (frameId == 0) Serial.println(sent
    ? "first frame: payload send complete"
    : "first frame: payload send failed");
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
  client.setNoDelay(true);
  client.setTimeout(IO_TIMEOUT_MS);

  Serial.printf("Client connected from %s\n", client.remoteIP().toString().c_str());

  if (!authenticateReceiver(client)) {
    Serial.println("Receiver authentication failed");
    client.stop();
    return;
  }

  Serial.println("moody-rx authenticated; starting video");
  uint32_t frameId = 0;
  uint32_t nextFrameAt = millis();
  resetTxTiming(nextFrameAt);

  while (client.connected()) {
    const int32_t waitMs = static_cast<int32_t>(nextFrameAt - millis());
    if (waitMs > 0) delay(waitMs);

    if (!sendFrame(client, frameId++)) break;
    reportTxTiming();
    nextFrameAt += FRAME_INTERVAL_MS;

    // If capture/transmission falls behind, resume from the current time rather
    // than creating an ever-growing latency queue.
    if (static_cast<int32_t>(millis() - nextFrameAt) >
        static_cast<int32_t>(FRAME_INTERVAL_MS)) {
      nextFrameAt = millis();
    }
  }

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
