#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <TCA9554.h>
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

#include "frame_fifo.h"
#include "moody_keys.h"
#include "timing_stats.h"
#include "video_layout.h"

// Dedicated moody-tx network. These must exactly match moody-tx.ino.
static const char *TX_AP_SSID = "moody-camera";
static const char *TX_AP_PASSWORD = "CHANGE-ME-9274";
static const IPAddress TX_ADDRESS(192, 168, 4, 1);
static constexpr uint16_t TX_PORT = 3333;

// Classic Waveshare ESP32-Touch-LCD-3.5 pin map.
#define LCD_BACKLIGHT 25
#define LCD_DC        27
#define LCD_CS         5
#define LCD_SCK       18
#define LCD_MOSI      23
#define LCD_MISO      19
#define I2C_SDA       21
#define I2C_SCL       22

static constexpr uint32_t IO_TIMEOUT_MS = 5000;
static constexpr uint32_t WIFI_ATTEMPT_MS = 12000;
static constexpr uint32_t RETRY_DELAY_MS = 2000;
static constexpr size_t MAX_JPEG_SIZE = 128 * 1024;
static constexpr size_t MAX_SIGNATURE_SIZE = 96;
static constexpr uint8_t JPEG_SLOT_COUNT = 2;
static constexpr uint16_t DISPLAY_WIDTH = 480;
static constexpr uint16_t DISPLAY_HEIGHT = 320;
// The classic ESP32's DMA driver tops out below 26.7 MHz, but this display's
// non-DMA bus is stable at 80 MHz and halves full-frame transfer time.
static constexpr uint32_t LCD_SPI_HZ = 80000000;

static const uint8_t TX_HELLO_MAGIC[8] = {'M','O','O','D','Y','T','X','1'};
static const uint8_t RX_AUTH_MAGIC[8]  = {'M','O','O','D','Y','R','X','1'};
static const uint8_t AUTH_OK_MAGIC[8]  = {'A','U','T','H','O','K','0','1'};
static const uint8_t FRAME_MAGIC[4]    = {'M','J','P','G'};
static const char TX_CONTEXT[] = "moody-tx-proof-v1";
static const char RX_CONTEXT[] = "moody-rx-proof-v1";

TCA9554 TCA(0x20);

Arduino_DataBus *displayBus = new Arduino_ESP32SPI(
  LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO, VSPI
);

Arduino_GFX *gfx = new Arduino_ST7796(
  displayBus,
  GFX_NOT_DEFINED,
  0,
  true
);

WiFiClient streamClient;
JPEGDEC jpeg;
uint8_t *jpegBuffers[JPEG_SLOT_COUNT] = {nullptr, nullptr};

struct FrameDescriptor {
  uint8_t slot;
  uint32_t frameId;
  uint32_t txMillis;
  uint32_t txClockOffsetMs;
  uint32_t jpegLength;
  uint32_t headerWaitUs;
  uint32_t payloadUs;
  uint32_t enqueuedAtUs;
  bool terminal;
};

QueueHandle_t freeSlots = nullptr;
QueueHandle_t readyFrames = nullptr;
SemaphoreHandle_t fifoStateMutex = nullptr;
SemaphoreHandle_t readerFinished = nullptr;
FrameSlotFifo<JPEG_SLOT_COUNT> frameSlotFifo;
bool streamReaderRunning = false;

mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctrDrbg;
mbedtls_pk_context rxPrivateKey;
mbedtls_pk_context txPublicKey;

TimingStats rxFrameWaitUs;
TimingStats rxPayloadUs;
TimingStats rxFifoUs;
TimingStats rxAgeMs;
TimingStats rxDecodeUs;
TimingStats rxLcdUs;
TimingStats rxJpegBytes;
FrameOutcomeStats rxFrames;
uint32_t rxTimingWindowStart = 0;
uint32_t currentQueuedDepth = 0;
uint32_t activeLcdUs = 0;
bool activeCoverScale = false;
uint16_t coverSourceX[DISPLAY_WIDTH];
uint16_t coverSourceY[DISPLAY_HEIGHT];
// JPEGDEC may return a complete source-width MCU row. At the QCIF-to-display
// cover scale, a 16-row source block becomes at most 44 destination rows.
alignas(4) uint8_t scaledJpegBlock[DISPLAY_WIDTH * 48 * 2];

static uint16_t readU16BE(const uint8_t in[2]) {
  return (static_cast<uint16_t>(in[0]) << 8) | in[1];
}

static uint32_t readU32BE(const uint8_t in[4]) {
  return (static_cast<uint32_t>(in[0]) << 24) |
         (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) |
          static_cast<uint32_t>(in[3]);
}

static void writeU16BE(uint8_t out[2], uint16_t value) {
  out[0] = static_cast<uint8_t>(value >> 8);
  out[1] = static_cast<uint8_t>(value);
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
  mbedtls_pk_init(&rxPrivateKey);
  mbedtls_pk_init(&txPublicKey);

  const char personalization[] = "moody-rx-v1";
  if (mbedtls_ctr_drbg_seed(&ctrDrbg,
                           mbedtls_entropy_func,
                           &entropy,
                           reinterpret_cast<const uint8_t *>(personalization),
                           sizeof(personalization) - 1) != 0) {
    return false;
  }

  if (mbedtls_pk_parse_key(
        &rxPrivateKey,
        reinterpret_cast<const uint8_t *>(MOODY_RX_PRIVATE_KEY_PEM),
        strlen(MOODY_RX_PRIVATE_KEY_PEM) + 1,
        nullptr,
        0,
        mbedtls_ctr_drbg_random,
        &ctrDrbg) != 0) {
    return false;
  }

  return mbedtls_pk_parse_public_key(
           &txPublicKey,
           reinterpret_cast<const uint8_t *>(MOODY_TX_PUBLIC_KEY_PEM),
           strlen(MOODY_TX_PUBLIC_KEY_PEM) + 1) == 0;
}

static void resetLCD() {
  TCA.pinMode1(0, OUTPUT);
  TCA.write1(0, 1);
  delay(10);
  TCA.write1(0, 0);
  delay(10);
  TCA.write1(0, 1);
  delay(200);
}

static void showStatus(const char *heading,
                       const String &detail,
                       uint16_t colour) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextWrap(true);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(28, 40);
  gfx->println("Moody RX");
  gfx->drawFastHLine(28, 82, 424, RGB565_DARKGREY);

  gfx->setTextColor(colour);
  gfx->setTextSize(2);
  gfx->setCursor(28, 115);
  gfx->println(heading);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(28, 165);
  gfx->println(detail);
}

static bool joinTransmitterNetwork() {
  ESP_LOGE("moody-rx", "joining transmitter network");
  showStatus("Waiting for moody-tx", TX_AP_SSID, RGB565_YELLOW);
  Serial.printf("Looking for %s\n", TX_AP_SSID);

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(TX_AP_SSID, TX_AP_PASSWORD);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < WIFI_ATTEMPT_MS) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    ESP_LOGE("moody-rx", "transmitter network unavailable");
    showStatus("moody-tx not found", "Retrying...", RGB565_RED);
    return false;
  }

  showStatus("Network found", "Opening secure link...", RGB565_CYAN);
  ESP_LOGE("moody-rx", "joined transmitter network");
  Serial.printf("Joined; RX address %s\n", WiFi.localIP().toString().c_str());
  return true;
}

static bool authenticateTransmitter() {
  showStatus("Authenticating", "Verifying moody-tx...", RGB565_CYAN);

  uint8_t receivedMagic[8];
  uint8_t nonce[32];
  uint8_t txLengthBytes[2];

  if (!readExact(streamClient, receivedMagic, sizeof(receivedMagic)) ||
      memcmp(receivedMagic, TX_HELLO_MAGIC, sizeof(TX_HELLO_MAGIC)) != 0 ||
      !readExact(streamClient, nonce, sizeof(nonce)) ||
      !readExact(streamClient, txLengthBytes, sizeof(txLengthBytes))) {
    return false;
  }

  const uint16_t txSignatureLength = readU16BE(txLengthBytes);
  if (txSignatureLength == 0 || txSignatureLength > MAX_SIGNATURE_SIZE) {
    return false;
  }

  uint8_t txSignature[MAX_SIGNATURE_SIZE];
  if (!readExact(streamClient, txSignature, txSignatureLength)) return false;

  uint8_t txDigest[32];
  if (!sha256Parts(TX_CONTEXT, nonce, txDigest) ||
      mbedtls_pk_verify(&txPublicKey,
                        MBEDTLS_MD_SHA256,
                        txDigest,
                        sizeof(txDigest),
                        txSignature,
                        txSignatureLength) != 0) {
    Serial.println("moody-tx signature rejected");
    return false;
  }

  uint8_t rxDigest[32];
  if (!sha256Parts(RX_CONTEXT, nonce, rxDigest)) return false;

  uint8_t rxSignature[MAX_SIGNATURE_SIZE];
  size_t rxSignatureLength = 0;
  if (mbedtls_pk_sign(&rxPrivateKey,
                      MBEDTLS_MD_SHA256,
                      rxDigest,
                      sizeof(rxDigest),
                      rxSignature,
                      sizeof(rxSignature),
                      &rxSignatureLength,
                      mbedtls_ctr_drbg_random,
                      &ctrDrbg) != 0 ||
      rxSignatureLength > UINT16_MAX) {
    return false;
  }

  uint8_t rxLengthBytes[2];
  writeU16BE(rxLengthBytes, static_cast<uint16_t>(rxSignatureLength));
  if (!writeAll(streamClient, RX_AUTH_MAGIC, sizeof(RX_AUTH_MAGIC)) ||
      !writeAll(streamClient, rxLengthBytes, sizeof(rxLengthBytes)) ||
      !writeAll(streamClient, rxSignature, rxSignatureLength)) {
    return false;
  }

  uint8_t authResult[8];
  if (!readExact(streamClient, authResult, sizeof(authResult)) ||
      memcmp(authResult, AUTH_OK_MAGIC, sizeof(AUTH_OK_MAGIC)) != 0) {
    Serial.println("moody-rx identity rejected by transmitter");
    return false;
  }

  showStatus("Authenticated", "Waiting for video...", RGB565_GREEN);
  Serial.println("Mutual authentication complete");
  return true;
}

static int drawJpegBlock(JPEGDRAW *draw) {
  const uint32_t lcdStarted = micros();
  if (!activeCoverScale) {
    gfx->draw16bitBeRGBBitmap(draw->x, draw->y, draw->pPixels,
                               draw->iWidth, draw->iHeight);
  } else {
    int destinationLeft = 0;
    while (destinationLeft < DISPLAY_WIDTH &&
           coverSourceX[destinationLeft] < draw->x) {
      ++destinationLeft;
    }
    int destinationRight = destinationLeft;
    while (destinationRight < DISPLAY_WIDTH &&
           coverSourceX[destinationRight] < draw->x + draw->iWidth) {
      ++destinationRight;
    }

    int destinationTop = 0;
    while (destinationTop < DISPLAY_HEIGHT &&
           coverSourceY[destinationTop] < draw->y) {
      ++destinationTop;
    }
    int destinationBottom = destinationTop;
    while (destinationBottom < DISPLAY_HEIGHT &&
           coverSourceY[destinationBottom] < draw->y + draw->iHeight) {
      ++destinationBottom;
    }

    const int width = destinationRight - destinationLeft;
    const int height = destinationBottom - destinationTop;
    const size_t outputBytes = static_cast<size_t>(width) * height * 2;
    if (width > 0 && height > 0 && outputBytes <= sizeof(scaledJpegBlock)) {
      const uint8_t *source =
        reinterpret_cast<const uint8_t *>(draw->pPixels);
      int previousSourceY = -1;
      for (int destinationY = destinationTop;
           destinationY < destinationBottom;
           ++destinationY) {
        const int sourceY = coverSourceY[destinationY] - draw->y;
        uint8_t *outputRow = scaledJpegBlock +
          static_cast<size_t>(destinationY - destinationTop) * width * 2;
        if (sourceY == previousSourceY) {
          memcpy(outputRow, outputRow - width * 2, width * 2);
        } else {
          const uint8_t *sourceRow = source +
            static_cast<size_t>(sourceY) * draw->iWidth * 2;
          uint8_t *output = outputRow;
          for (int destinationX = destinationLeft;
               destinationX < destinationRight;
               ++destinationX) {
            const uint8_t *sourcePixel = sourceRow +
              static_cast<size_t>(coverSourceX[destinationX] - draw->x) * 2;
            *output++ = sourcePixel[0];
            *output++ = sourcePixel[1];
          }
        }
        previousSourceY = sourceY;
      }
      gfx->draw16bitBeRGBBitmap(
        destinationLeft,
        destinationTop,
        reinterpret_cast<uint16_t *>(scaledJpegBlock),
        width,
        height
      );
    }
  }
  activeLcdUs += elapsedCounter(micros(), lcdStarted);
  return 1;
}

static bool decodeAndDisplayJpeg(const uint8_t *data, size_t length) {
  if (!jpeg.openRAM(const_cast<uint8_t *>(data),
                    static_cast<int>(length),
                    drawJpegBlock)) {
    return false;
  }

  jpeg.setPixelType(RGB565_BIG_ENDIAN);

  const int imageWidth = jpeg.getWidth();
  const int imageHeight = jpeg.getHeight();
  activeCoverScale = imageWidth != DISPLAY_WIDTH || imageHeight != DISPLAY_HEIGHT;
  if (activeCoverScale) {
    for (int x = 0; x < DISPLAY_WIDTH; ++x) {
      coverSourceX[x] = static_cast<uint16_t>(coverCropSourceX(
        x, imageWidth, imageHeight, DISPLAY_WIDTH, DISPLAY_HEIGHT));
    }
    for (int y = 0; y < DISPLAY_HEIGHT; ++y) {
      coverSourceY[y] = static_cast<uint16_t>(coverCropSourceY(
        y, imageWidth, imageHeight, DISPLAY_WIDTH, DISPLAY_HEIGHT));
    }
  }
  const int x = activeCoverScale ? 0 : centeredCoordinate(gfx->width(), imageWidth);
  const int y = activeCoverScale ? 0 : centeredCoordinate(gfx->height(), imageHeight);
  const bool decoded = jpeg.decode(x, y, 0) != 0;
  jpeg.close();
  activeCoverScale = false;
  return decoded;
}

static bool resetFrameQueues() {
  if (xQueueReset(freeSlots) != pdPASS ||
      xQueueReset(readyFrames) != pdPASS) {
    return false;
  }

  xSemaphoreTake(fifoStateMutex, portMAX_DELAY);
  frameSlotFifo = FrameSlotFifo<JPEG_SLOT_COUNT>();
  xSemaphoreGive(fifoStateMutex);

  for (uint8_t slot = 0; slot < JPEG_SLOT_COUNT; ++slot) {
    if (xQueueSend(freeSlots, &slot, 0) != pdTRUE) return false;
  }
  return true;
}

static bool markSlotAcquired(uint8_t slot) {
  xSemaphoreTake(fifoStateMutex, portMAX_DELAY);
  const bool acquired = frameSlotFifo.acquireSpecific(slot);
  xSemaphoreGive(fifoStateMutex);
  return acquired;
}

static bool enqueueFrameDescriptor(const FrameDescriptor &descriptor) {
  xSemaphoreTake(fifoStateMutex, portMAX_DELAY);
  const bool markedReady = frameSlotFifo.enqueueReady(descriptor.slot);
  const bool queued = markedReady &&
    xQueueSend(readyFrames, &descriptor, 0) == pdTRUE;
  xSemaphoreGive(fifoStateMutex);
  return queued;
}

static bool markFrameDequeued(uint8_t slot, uint32_t &queuedDepth) {
  xSemaphoreTake(fifoStateMutex, portMAX_DELAY);
  uint8_t expectedSlot = 0;
  const bool dequeued = frameSlotFifo.dequeueReady(expectedSlot);
  const bool matches = dequeued && expectedSlot == slot;
  queuedDepth = static_cast<uint32_t>(frameSlotFifo.readyCount());
  xSemaphoreGive(fifoStateMutex);
  return matches;
}

static bool releaseFrameSlot(uint8_t slot) {
  xSemaphoreTake(fifoStateMutex, portMAX_DELAY);
  const bool released = frameSlotFifo.release(slot);
  const bool queued = released && xQueueSend(freeSlots, &slot, 0) == pdTRUE;
  xSemaphoreGive(fifoStateMutex);
  return queued;
}

static void finishStreamReader(bool slotHeld, uint8_t slot) {
  if (slotHeld && !releaseFrameSlot(slot)) {
    Serial.println("Reader failed to return incomplete JPEG slot");
  }

  FrameDescriptor terminal = {};
  terminal.terminal = true;
  xQueueSend(readyFrames, &terminal, portMAX_DELAY);
  streamClient.stop();
  xSemaphoreGive(readerFinished);
  vTaskDelete(nullptr);
  while (true) delay(1000);
}

static void streamReaderTask(void *) {
  bool hasTxClockOffset = false;
  uint32_t txClockOffsetMs = 0;
  bool loggedHeader = false;
  bool loggedPayload = false;

  while (true) {
    uint8_t slot = 0;
    if (xQueueReceive(freeSlots, &slot, portMAX_DELAY) != pdTRUE ||
        !markSlotAcquired(slot)) {
      finishStreamReader(false, slot);
    }

    uint8_t header[16];
    const uint32_t waitStarted = micros();
    if (!readExact(streamClient, header, sizeof(header))) {
      finishStreamReader(true, slot);
    }
    const uint32_t headerReceivedAtMs = millis();
    const uint32_t headerWaitUs = elapsedCounter(micros(), waitStarted);

    if (!loggedHeader) {
      Serial.println("reader received first frame header");
      loggedHeader = true;
    }

    if (memcmp(header, FRAME_MAGIC, sizeof(FRAME_MAGIC)) != 0) {
      finishStreamReader(true, slot);
    }

    const uint32_t frameId = readU32BE(header + 4);
    const uint32_t jpegLength = readU32BE(header + 8);
    const uint32_t txMillis = readU32BE(header + 12);
    if (!hasTxClockOffset) {
      txClockOffsetMs =
        remoteToLocalCounterOffset(txMillis, headerReceivedAtMs);
      hasTxClockOffset = true;
    }
    if (jpegLength == 0 || jpegLength > MAX_JPEG_SIZE) {
      Serial.printf("Invalid JPEG length: %lu\n",
                    static_cast<unsigned long>(jpegLength));
      finishStreamReader(true, slot);
    }

    const uint32_t payloadStarted = micros();
    if (!readExact(streamClient, jpegBuffers[slot], jpegLength)) {
      finishStreamReader(true, slot);
    }
    if (!loggedPayload) {
      Serial.println("reader received first frame payload");
      loggedPayload = true;
    }

    FrameDescriptor descriptor = {};
    descriptor.slot = slot;
    descriptor.frameId = frameId;
    descriptor.txMillis = txMillis;
    descriptor.txClockOffsetMs = txClockOffsetMs;
    descriptor.jpegLength = jpegLength;
    descriptor.headerWaitUs = headerWaitUs;
    descriptor.payloadUs = elapsedCounter(micros(), payloadStarted);
    descriptor.enqueuedAtUs = micros();
    descriptor.terminal = false;
    if (!enqueueFrameDescriptor(descriptor)) {
      finishStreamReader(true, slot);
    }
  }
}

static bool startStreamReader() {
  if (streamReaderRunning) return true;

  xSemaphoreTake(readerFinished, 0);
  if (!resetFrameQueues()) return false;

  const BaseType_t created = xTaskCreatePinnedToCore(
    streamReaderTask,
    "stream-reader",
    4096,
    nullptr,
    2,
    nullptr,
    0
  );
  if (created != pdPASS) return false;

  streamReaderRunning = true;
  return true;
}

static void stopStreamReader() {
  if (!streamReaderRunning) return;

  // A terminal descriptor is enqueued before this completion signal. Waiting
  // here ensures the old reader has stopped using streamClient before reconnect.
  xSemaphoreTake(readerFinished, portMAX_DELAY);
  streamReaderRunning = false;
}

static void resetRxTiming(uint32_t now) {
  rxFrameWaitUs.reset();
  rxPayloadUs.reset();
  rxFifoUs.reset();
  rxAgeMs.reset();
  rxDecodeUs.reset();
  rxLcdUs.reset();
  rxJpegBytes.reset();
  rxFrames.reset();
  currentQueuedDepth = 0;
  rxTimingWindowStart = now;
}

static void reportRxTiming() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - rxTimingWindowStart;
  if (elapsed < 1000 || rxFrames.attempted() == 0) return;

  Serial.printf(
    "rx fps=%.1f attempted=%lu displayed=%lu failed=%lu last_frame=%lu "
    "jpeg_B=%lu/%lu wait_us=%lu/%lu receive_us=%lu/%lu "
    "fifo_us=%lu/%lu age_ms=%lu/%lu queued=%lu "
    "decode_us=%lu/%lu lcd_us=%lu/%lu\n",
    rxFrames.displayedFps(elapsed),
    static_cast<unsigned long>(rxFrames.attempted()),
    static_cast<unsigned long>(rxFrames.displayed()),
    static_cast<unsigned long>(rxFrames.failed()),
    static_cast<unsigned long>(rxFrames.lastFrameId()),
    static_cast<unsigned long>(rxJpegBytes.average()),
    static_cast<unsigned long>(rxJpegBytes.maximum()),
    static_cast<unsigned long>(rxFrameWaitUs.average()),
    static_cast<unsigned long>(rxFrameWaitUs.maximum()),
    static_cast<unsigned long>(rxPayloadUs.average()),
    static_cast<unsigned long>(rxPayloadUs.maximum()),
    static_cast<unsigned long>(rxFifoUs.average()),
    static_cast<unsigned long>(rxFifoUs.maximum()),
    static_cast<unsigned long>(rxAgeMs.average()),
    static_cast<unsigned long>(rxAgeMs.maximum()),
    static_cast<unsigned long>(currentQueuedDepth),
    static_cast<unsigned long>(rxDecodeUs.average()),
    static_cast<unsigned long>(rxDecodeUs.maximum()),
    static_cast<unsigned long>(rxLcdUs.average()),
    static_cast<unsigned long>(rxLcdUs.maximum())
  );
  resetRxTiming(now);
}

static bool receiveAndDisplayQueuedFrame() {
  static bool loggedDisplay = false;
  FrameDescriptor descriptor = {};
  if (xQueueReceive(readyFrames, &descriptor, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (descriptor.terminal) return false;

  uint32_t queuedDepth = 0;
  if (!markFrameDequeued(descriptor.slot, queuedDepth)) {
    Serial.println("Invalid FIFO JPEG descriptor");
    return false;
  }

  if (!loggedDisplay) {
    Serial.println("display decoding first frame directly");
    loggedDisplay = true;
  }

  const uint32_t decodeStarted = micros();
  const uint32_t displayStartedMs = millis();
  rxFrameWaitUs.add(descriptor.headerWaitUs);
  rxPayloadUs.add(descriptor.payloadUs);
  rxFifoUs.add(elapsedCounter(decodeStarted, descriptor.enqueuedAtUs));
  rxAgeMs.add(elapsedCounter(
    displayStartedMs,
    localizeRemoteCounter(descriptor.txMillis, descriptor.txClockOffsetMs)
  ));
  rxJpegBytes.add(descriptor.jpegLength);
  currentQueuedDepth = queuedDepth;

  activeLcdUs = 0;
  const bool displayed = decodeAndDisplayJpeg(jpegBuffers[descriptor.slot],
                                               descriptor.jpegLength);
  const uint32_t totalUs = elapsedCounter(micros(), decodeStarted);
  rxLcdUs.add(activeLcdUs);
  rxDecodeUs.add(totalUs >= activeLcdUs ? totalUs - activeLcdUs : totalUs);
  rxFrames.record(descriptor.frameId, displayed);

  if (!releaseFrameSlot(descriptor.slot)) return false;

  reportRxTiming();
  return true; // A damaged complete JPEG is recoverable; its slot is released.
}

static bool openAuthenticatedStream() {
  ESP_LOGE("moody-rx", "opening authenticated stream");
  showStatus("Connecting", "Contacting moody-tx...", RGB565_CYAN);
  streamClient.stop();
  streamClient.setNoDelay(true);
  streamClient.setTimeout(IO_TIMEOUT_MS);

  if (!streamClient.connect(TX_ADDRESS, TX_PORT, IO_TIMEOUT_MS)) {
    ESP_LOGE("moody-rx", "stream connection failed");
    showStatus("Connection failed", "Retrying...", RGB565_RED);
    return false;
  }

  if (!authenticateTransmitter()) {
    ESP_LOGE("moody-rx", "stream authentication failed");
    showStatus("Authentication failed", "Connection rejected", RGB565_RED);
    streamClient.stop();
    return false;
  }

  gfx->fillScreen(RGB565_BLACK);
  resetRxTiming(millis());
  ESP_LOGE("moody-rx", "stream authenticated");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("rx setup: serial ready");
  ESP_LOGE("moody-rx", "setup started");

  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  resetLCD();
  Serial.println("rx setup: display reset complete");

  if (!gfx->begin(LCD_SPI_HZ)) {
    ESP_LOGE("moody-rx", "display initialisation failed");
    Serial.println("Fatal: display initialisation failed");
    while (true) delay(1000);
  }

  gfx->setRotation(1);
  pinMode(LCD_BACKLIGHT, OUTPUT);
  digitalWrite(LCD_BACKLIGHT, HIGH);
  Serial.println("rx setup: display ready");

  showStatus("Starting", "Loading paired identity...", RGB565_YELLOW);

  if (!initialiseCrypto()) {
    ESP_LOGE("moody-rx", "identity initialisation failed");
    showStatus("Fatal error", "Identity key invalid", RGB565_RED);
    while (true) delay(1000);
  }
  Serial.println("rx setup: crypto ready");

  Serial.printf("rx setup: psram=%s free=%u\n",
                psramFound() ? "yes" : "no",
                static_cast<unsigned>(ESP.getFreePsram()));
  if (psramFound()) {
    for (uint8_t slot = 0; slot < JPEG_SLOT_COUNT; ++slot) {
      jpegBuffers[slot] = static_cast<uint8_t *>(ps_malloc(MAX_JPEG_SIZE));
      Serial.printf("rx setup: JPEG slot %u=%p free_psram=%u\n",
                    slot,
                    jpegBuffers[slot],
                    static_cast<unsigned>(ESP.getFreePsram()));
    }
  }

  if (!jpegBuffers[0] || !jpegBuffers[1]) {
    ESP_LOGE("moody-rx", "frame buffer allocation failed");
    showStatus("Fatal error", "No JPEG buffer memory", RGB565_RED);
    while (true) delay(1000);
  }
  Serial.println("rx setup: JPEG buffers ready");

  freeSlots = xQueueCreate(JPEG_SLOT_COUNT, sizeof(uint8_t));
  readyFrames = xQueueCreate(JPEG_SLOT_COUNT, sizeof(FrameDescriptor));
  fifoStateMutex = xSemaphoreCreateMutex();
  readerFinished = xSemaphoreCreateBinary();
  if (!freeSlots || !readyFrames || !fifoStateMutex || !readerFinished) {
    ESP_LOGE("moody-rx", "FIFO allocation failed");
    showStatus("Fatal error", "No FIFO control memory", RGB565_RED);
    while (true) delay(1000);
  }
  Serial.println("rx setup: queues ready");

  Serial.println("moody-rx ready");
  ESP_LOGE("moody-rx", "setup complete");
}

void loop() {
  if (!streamReaderRunning) {
    if (WiFi.status() != WL_CONNECTED) {
      streamClient.stop();
      if (!joinTransmitterNetwork()) {
        delay(RETRY_DELAY_MS);
        return;
      }
    }

    if (!openAuthenticatedStream()) {
      delay(RETRY_DELAY_MS);
      return;
    }

    if (!startStreamReader()) {
      streamClient.stop();
      showStatus("Stream unavailable", "Retrying...", RGB565_RED);
      delay(RETRY_DELAY_MS);
      return;
    }
  }

  if (!receiveAndDisplayQueuedFrame()) {
    stopStreamReader();
    showStatus("Stream interrupted", "Reconnecting...", RGB565_RED);
    delay(RETRY_DELAY_MS);
  }
}
