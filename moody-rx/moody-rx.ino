#include <Arduino.h>

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "moody-rx requires classic ESP32 (Waveshare ESP32-Touch-LCD-3.5)"
#endif
#include <WiFi.h>
#include <Preferences.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <TCA9554.h>
#include <driver/i2s_std.h>
#include "src/es8311/es8311.h"
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
#include "audio_stream.h"
#include "audio_playback_format.h"
#include "board_pins.h"
#include "moody_keys.h"
#include "timing_stats.h"
#include "touch_ft6336.h"
#include "video_layout.h"
#include "volume_control.h"

// Dedicated moody-tx network. These must exactly match moody-tx.ino.
static const char *TX_AP_SSID = "moody-camera";
static const char *TX_AP_PASSWORD = "CHANGE-ME-9274";
static const IPAddress TX_ADDRESS(192, 168, 4, 1);
static constexpr uint16_t TX_PORT = 3333;

static constexpr uint32_t IO_TIMEOUT_MS = 5000;
static constexpr uint32_t WIFI_ATTEMPT_MS = 12000;
static constexpr uint32_t RETRY_DELAY_MS = 2000;
static constexpr size_t MAX_JPEG_SIZE = 128 * 1024;
static constexpr size_t MAX_SIGNATURE_SIZE = 96;
static constexpr uint8_t JPEG_SLOT_COUNT = 2;
static constexpr uint8_t AUDIO_QUEUE_LENGTH = 4;
static constexpr uint32_t STREAM_READER_STACK_BYTES = 8192;
static constexpr uint16_t DISPLAY_WIDTH = 480;
static constexpr uint16_t DISPLAY_HEIGHT = 320;
// Preserve the classic board's working non-DMA 80 MHz display bus.
static constexpr uint32_t LCD_SPI_HZ = 80000000;
static constexpr uint32_t AUDIO_SAMPLE_RATE_HZ = 16000;
static constexpr uint32_t AUDIO_MCLK_MULTIPLE = 256;
static constexpr uint32_t AUDIO_MCLK_FREQUENCY_HZ =
  AUDIO_SAMPLE_RATE_HZ * AUDIO_MCLK_MULTIPLE;
static constexpr uint32_t AUDIO_BLOCK_SAMPLES = 320;
static constexpr uint32_t AUDIO_BLOCK_BYTES = AUDIO_BLOCK_SAMPLES * sizeof(int16_t);
static constexpr uint32_t AUDIO_WRITE_DEADLINE_MS = 30;
static constexpr uint8_t DEFAULT_UI_VOLUME = 70;
static constexpr uint32_t TOUCH_POLL_MS = 20;
static constexpr uint32_t TOUCH_TASK_STACK_BYTES = 3072;
static constexpr uint8_t TOUCH_TAP_QUEUE_LENGTH = 4;
// Bounded so volume taps are serviced while waiting for the next frame.
static constexpr uint32_t FRAME_WAIT_MS = 30;
static_assert(AUDIO_BLOCK_BYTES == moody_audio::kAudioPayloadLength,
              "I2S playback block must consume one complete PCM record");

static const uint8_t TX_HELLO_MAGIC[8] = {'M','O','O','D','Y','T','X','1'};
static const uint8_t RX_AUTH_MAGIC[8]  = {'M','O','O','D','Y','R','X','1'};
static const uint8_t AUTH_OK_MAGIC[8]  = {'A','U','T','H','O','K','0','1'};
static const char TX_CONTEXT[] = "moody-tx-proof-v1";
static const char RX_CONTEXT[] = "moody-rx-proof-v1";

TCA9554 TCA(moody_rx::board::kTca9554Address);

Arduino_DataBus *displayBus = new Arduino_ESP32SPI(
  moody_rx::board::kDisplayDcPin,
  moody_rx::board::kDisplayCsPin,
  moody_rx::board::kDisplaySclkPin,
  moody_rx::board::kDisplayMosiPin,
  moody_rx::board::kDisplayMisoPin,
  VSPI
);

Arduino_GFX *gfx = new Arduino_ST7796(
  displayBus,
  moody_rx::board::kDisplayResetPin,
  0,
  true,
  moody_rx::board::kDisplayNativeWidth,
  moody_rx::board::kDisplayNativeHeight
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

struct AudioPcmRecord {
  uint32_t timestamp;
  uint8_t pcm[moody_audio::kAudioPayloadLength];
};

struct AudioQueueTelemetry {
  uint32_t depth;
  uint32_t fullDrops;
  uint32_t contentionDrops;
};

class AudioPcmQueue {
 public:
  bool initialise() {
    if (queue_ != nullptr || mutex_ != nullptr) return false;
    queue_ = xQueueCreateStatic(AUDIO_QUEUE_LENGTH,
                                sizeof(AudioPcmRecord),
                                reinterpret_cast<uint8_t *>(records_),
                                &queueStorage_);
    if (queue_ == nullptr) return false;

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
      queue_ = nullptr;
      return false;
    }
    return true;
  }

  void clear() {
    if (queue_ == nullptr || mutex_ == nullptr ||
        xSemaphoreTake(mutex_, portMAX_DELAY) != pdPASS) {
      return;
    }
    xQueueReset(queue_);
    fullDrops_ = 0;
    __atomic_store_n(&contentionDrops_, 0U, __ATOMIC_RELAXED);
    xSemaphoreGive(mutex_);
  }

  bool enqueue(uint32_t timestamp,
               const uint8_t pcm[moody_audio::kAudioPayloadLength]) {
    if (queue_ == nullptr || mutex_ == nullptr) return false;
    if (xSemaphoreTake(mutex_, 0) != pdPASS) {
      __atomic_fetch_add(&contentionDrops_, 1U, __ATOMIC_RELAXED);
      return false;
    }

    AudioPcmRecord record = {};
    record.timestamp = timestamp;
    memcpy(record.pcm, pcm, sizeof(record.pcm));
    bool queued = xQueueSend(queue_, &record, 0) == pdTRUE;
    if (!queued) {
      AudioPcmRecord discarded = {};
      if (xQueueReceive(queue_, &discarded, 0) == pdTRUE) {
        ++fullDrops_;
        queued = xQueueSend(queue_, &record, 0) == pdTRUE;
      }
    }
    xSemaphoreGive(mutex_);
    return queued;
  }

  bool dequeue(AudioPcmRecord &record, TickType_t wait) {
    if (queue_ == nullptr || mutex_ == nullptr) return false;
    if (wait == 0) return tryDequeue(record);

    const TickType_t started = xTaskGetTickCount();
    while (true) {
      if (tryDequeue(record)) return true;
      if (wait != portMAX_DELAY && xTaskGetTickCount() - started >= wait) {
        return false;
      }
      vTaskDelay(1);
    }
  }

  bool snapshot(AudioQueueTelemetry &telemetry) {
    if (queue_ == nullptr || mutex_ == nullptr ||
        xSemaphoreTake(mutex_, portMAX_DELAY) != pdPASS) {
      return false;
    }
    telemetry.depth = static_cast<uint32_t>(uxQueueMessagesWaiting(queue_));
    telemetry.fullDrops = fullDrops_;
    xSemaphoreGive(mutex_);
    telemetry.contentionDrops =
      __atomic_load_n(&contentionDrops_, __ATOMIC_RELAXED);
    return true;
  }

  bool takeTelemetry(AudioQueueTelemetry &telemetry) {
    if (queue_ == nullptr || mutex_ == nullptr ||
        xSemaphoreTake(mutex_, portMAX_DELAY) != pdPASS) {
      return false;
    }
    telemetry.depth = static_cast<uint32_t>(uxQueueMessagesWaiting(queue_));
    telemetry.fullDrops = fullDrops_;
    fullDrops_ = 0;
    xSemaphoreGive(mutex_);
    telemetry.contentionDrops =
      __atomic_exchange_n(&contentionDrops_, 0U, __ATOMIC_RELAXED);
    return true;
  }

 private:
  bool tryDequeue(AudioPcmRecord &record) {
    if (xSemaphoreTake(mutex_, 0) != pdPASS) return false;
    const bool dequeued = xQueueReceive(queue_, &record, 0) == pdTRUE;
    xSemaphoreGive(mutex_);
    return dequeued;
  }

  AudioPcmRecord records_[AUDIO_QUEUE_LENGTH];
  StaticQueue_t queueStorage_ = {};
  QueueHandle_t queue_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  uint32_t fullDrops_ = 0;
  uint32_t contentionDrops_ = 0;
};

struct PartialVideoFrame {
  bool active;
  uint8_t slot;
  uint32_t frameId;
  uint32_t txMillis;
  uint32_t txClockOffsetMs;
  uint32_t jpegLength;
  uint32_t nextOffset;
  uint32_t headerWaitUs;
  uint32_t payloadStartedUs;
};

QueueHandle_t freeSlots = nullptr;
QueueHandle_t readyFrames = nullptr;
SemaphoreHandle_t fifoStateMutex = nullptr;
SemaphoreHandle_t readerFinished = nullptr;
FrameSlotFifo<JPEG_SLOT_COUNT> frameSlotFifo;
bool streamReaderRunning = false;
AudioPcmQueue audioPcmQueue;
static i2s_chan_handle_t audioTxChannel = nullptr;
static es8311_handle_t audioCodec = nullptr;
static SemaphoreHandle_t audioPlaybackFinished = nullptr;
static volatile bool audioPlaybackRunning = false;
static bool audioI2cReady = false;
static bool audioAmplifierReady = false;
static bool audioQueueReady = false;
moody_rx::Ft6336 touchPanel(Wire);
moody_rx::VolumeControl volumeControl(DEFAULT_UI_VOLUME);
// Landscape X of each new tap, produced by the touch task for loop().
static QueueHandle_t touchTaps = nullptr;
static uint8_t savedUiVolume = DEFAULT_UI_VOLUME;
Preferences settings;
PartialVideoFrame partialVideoFrame = {};
bool hasExpectedRecordSequence = false;
uint32_t expectedRecordSequence = 0;
uint32_t droppedRecordCount = 0;
bool hasTxClockOffset = false;
uint32_t txClockOffsetMs = 0;

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

static bool initialiseBoardIo() {
  Wire.begin(moody_rx::board::kI2cSdaPin, moody_rx::board::kI2cSclPin);
  pinMode(moody_rx::board::kDisplayBacklightPin, OUTPUT);
  digitalWrite(moody_rx::board::kDisplayBacklightPin, LOW);
  audioAmplifierReady = false;
  if (!TCA.begin()) return false;

  // P0 controls LCD reset, so its setup is display-critical. P2 is optional:
  // keep its TCA default input state and disable audio if safe setup fails.
  if (!TCA.pinMode1(moody_rx::board::kTcaLcdResetPin, OUTPUT)) return false;
  audioI2cReady = true;

  if (!TCA.write1(moody_rx::board::kTcaAmplifierEnablePin, LOW)) {
    Serial.println("audio disabled: could not preload TCA9554 PA_CTRL LOW");
    return true;
  }
  if (!TCA.pinMode1(moody_rx::board::kTcaAmplifierEnablePin, OUTPUT)) {
    Serial.println("audio disabled: could not configure TCA9554 PA_CTRL output");
    return true;
  }
  audioAmplifierReady = true;
  return true;
}

static void stopAudioI2sDriver() {
  if (audioTxChannel != nullptr) {
    i2s_channel_disable(audioTxChannel);
    i2s_del_channel(audioTxChannel);
    audioTxChannel = nullptr;
  }
}

static bool initialiseAudioCodec() {
  if (!audioAmplifierReady) {
    Serial.println("audio init failed at TCA9554 PA_CTRL: safe output unavailable");
    return false;
  }
  if (!audioI2cReady) {
    Serial.println("audio init failed at shared I2C: board bus unavailable");
    return false;
  }

  audioCodec = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (audioCodec == nullptr) {
    Serial.println("audio init failed at ES8311 create/I2C");
    return false;
  }

  const es8311_clock_config_t codecClock = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = false,
    .mclk_frequency = static_cast<int>(AUDIO_MCLK_FREQUENCY_HZ),
    .sample_frequency = static_cast<int>(AUDIO_SAMPLE_RATE_HZ),
  };
  esp_err_t err = es8311_init(audioCodec,
                              &codecClock,
                              ES8311_RESOLUTION_32,
                              ES8311_RESOLUTION_32);
  if (err != ESP_OK) {
    Serial.printf("audio init failed at ES8311 init: %s\n", esp_err_to_name(err));
    es8311_delete(audioCodec);
    audioCodec = nullptr;
    return false;
  }
  err = es8311_voice_volume_set(
    audioCodec, moody_rx::codecVolumeFor(volumeControl.volume()), nullptr);
  if (err == ESP_OK) err = es8311_microphone_config(audioCodec, false);
  if (err != ESP_OK) {
    Serial.printf("audio init failed at ES8311 DAC setup: %s\n", esp_err_to_name(err));
    es8311_delete(audioCodec);
    audioCodec = nullptr;
    return false;
  }

  i2s_chan_config_t channelConfig =
    I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Send silence instead of replaying old DMA buffers when PCM runs out.
  channelConfig.auto_clear_after_cb = true;
  err = i2s_new_channel(&channelConfig, &audioTxChannel, nullptr);
  if (err != ESP_OK) {
    Serial.printf("audio init failed at I2S channel allocation: %s\n", esp_err_to_name(err));
    stopAudioI2sDriver();
    es8311_delete(audioCodec);
    audioCodec = nullptr;
    return false;
  }

  i2s_std_config_t standardConfig = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = static_cast<gpio_num_t>(moody_rx::board::kAudioMclkPin),
      .bclk = static_cast<gpio_num_t>(moody_rx::board::kAudioBclkPin),
      .ws = static_cast<gpio_num_t>(moody_rx::board::kAudioLrckPin),
      .dout = static_cast<gpio_num_t>(moody_rx::board::kAudioDataOutPin),
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };
  standardConfig.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  standardConfig.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

  err = i2s_channel_init_std_mode(audioTxChannel, &standardConfig);
  if (err == ESP_OK) err = i2s_channel_enable(audioTxChannel);
  if (err != ESP_OK) {
    Serial.printf("audio init failed at I2S BCLK/TX setup: %s\n", esp_err_to_name(err));
    stopAudioI2sDriver();
    es8311_delete(audioCodec);
    audioCodec = nullptr;
    return false;
  }

  // PA_CTRL is active HIGH. Keep it LOW until codec and all I2S clocks work.
  if (!TCA.write1(moody_rx::board::kTcaAmplifierEnablePin, HIGH)) {
    Serial.println("audio init failed at TCA9554 PA_CTRL enable");
    TCA.write1(moody_rx::board::kTcaAmplifierEnablePin, LOW);
    stopAudioI2sDriver();
    es8311_delete(audioCodec);
    audioCodec = nullptr;
    return false;
  }

  Serial.printf("audio codec ready: rate=%lu Hz internal MCLK=%lu Hz (from BCLK) stereo 32-bit\n",
                static_cast<unsigned long>(AUDIO_SAMPLE_RATE_HZ),
                static_cast<unsigned long>(AUDIO_MCLK_FREQUENCY_HZ));
  return true;
}

static void reportAudioStatus(bool ready) {
  AudioQueueTelemetry telemetry = {};
  if (audioPcmQueue.snapshot(telemetry)) {
    if (ready) {
      Serial.printf("audio=ready rate=%lu Hz queue=%lu/%u\n",
                    static_cast<unsigned long>(AUDIO_SAMPLE_RATE_HZ),
                    static_cast<unsigned long>(telemetry.depth),
                    static_cast<unsigned>(AUDIO_QUEUE_LENGTH));
    } else {
      Serial.printf("audio=disabled rate=%lu Hz queue=%lu/%u\n",
                    static_cast<unsigned long>(AUDIO_SAMPLE_RATE_HZ),
                    static_cast<unsigned long>(telemetry.depth),
                    static_cast<unsigned>(AUDIO_QUEUE_LENGTH));
    }
    return;
  }
  if (ready) {
    Serial.printf("audio=ready rate=%lu Hz queue=unavailable/%u\n",
                  static_cast<unsigned long>(AUDIO_SAMPLE_RATE_HZ),
                  static_cast<unsigned>(AUDIO_QUEUE_LENGTH));
  } else {
    Serial.printf("audio=disabled rate=%lu Hz queue=unavailable/%u\n",
                  static_cast<unsigned long>(AUDIO_SAMPLE_RATE_HZ),
                  static_cast<unsigned>(AUDIO_QUEUE_LENGTH));
  }
}

static void audioPlaybackTask(void *) {
  AudioPcmRecord record = {};
  uint32_t playbackSlots[AUDIO_BLOCK_SAMPLES * 2];
  TickType_t lastReport = xTaskGetTickCount();
  uint32_t blocks = 0;
  uint32_t underruns = 0;
  uint32_t shortWrites = 0;

  while (audioPlaybackRunning) {
    if (!audioPcmQueue.dequeue(record, pdMS_TO_TICKS(20))) {
      ++underruns;
    } else {
      moody_audio::expandMono16LeToStereo32(record.pcm, AUDIO_BLOCK_SAMPLES, playbackSlots);
      const uint8_t *playbackBytes = reinterpret_cast<const uint8_t *>(playbackSlots);
      size_t writtenTotal = 0;
      const uint32_t deadline = millis() + AUDIO_WRITE_DEADLINE_MS;
      while (writtenTotal < sizeof(playbackSlots) && audioPlaybackRunning) {
        if (static_cast<int32_t>(millis() - deadline) >= 0) break;
        size_t written = 0;
        const esp_err_t err = i2s_channel_write(
          audioTxChannel,
          playbackBytes + writtenTotal,
          sizeof(playbackSlots) - writtenTotal,
          &written,
          0);
        writtenTotal += written;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) break;
        // A DMA buffer takes 15 ms to drain. Retry partial writes within the
        // block's deadline without restarting a blocking driver timeout.
        if (writtenTotal < sizeof(playbackSlots)) vTaskDelay(1);
      }
      if (writtenTotal != sizeof(playbackSlots)) {
        ++shortWrites;
      } else {
        ++blocks;
      }
    }

    if (xTaskGetTickCount() - lastReport >= pdMS_TO_TICKS(5000)) {
      AudioQueueTelemetry telemetry = {};
      if (audioPcmQueue.takeTelemetry(telemetry)) {
        Serial.printf(
          "audio playback: blocks=%lu underruns=%lu short_writes=%lu "
          "queue=%lu/%u drops_full=%lu drops_contended=%lu rssi_dbm=%ld\n",
          static_cast<unsigned long>(blocks),
          static_cast<unsigned long>(underruns),
          static_cast<unsigned long>(shortWrites),
          static_cast<unsigned long>(telemetry.depth),
          static_cast<unsigned>(AUDIO_QUEUE_LENGTH),
          static_cast<unsigned long>(telemetry.fullDrops),
          static_cast<unsigned long>(telemetry.contentionDrops),
          static_cast<long>(WiFi.RSSI()));
      }
      blocks = underruns = shortWrites = 0;
      lastReport = xTaskGetTickCount();
    }
  }

  xSemaphoreGive(audioPlaybackFinished);
  vTaskDelete(nullptr);
}

static bool startAudioPlayback() {
  if (audioPlaybackRunning) return true;
  if (!audioQueueReady) {
    Serial.println("audio playback disabled: PCM queue unavailable");
    reportAudioStatus(false);
    return false;
  }
  if (audioPlaybackFinished == nullptr || !initialiseAudioCodec()) {
    reportAudioStatus(false);
    return false;
  }

  audioPcmQueue.clear();
  xSemaphoreTake(audioPlaybackFinished, 0);
  audioPlaybackRunning = true;
  const BaseType_t created = xTaskCreatePinnedToCore(
    audioPlaybackTask, "audio-playback", 8192, nullptr, 3, nullptr, 0);
  if (created != pdPASS) {
    audioPlaybackRunning = false;
    TCA.write1(moody_rx::board::kTcaAmplifierEnablePin, LOW);
    stopAudioI2sDriver();
    es8311_delete(audioCodec);
    audioCodec = nullptr;
    Serial.println("audio playback disabled: task creation failed");
    reportAudioStatus(false);
    return false;
  }
  Serial.println("audio playback enabled");
  reportAudioStatus(true);
  return true;
}

static void stopAudioPlayback() {
  if (audioPlaybackRunning) {
    audioPlaybackRunning = false;
    xSemaphoreTake(audioPlaybackFinished, portMAX_DELAY);
  }
  audioPcmQueue.clear();
  if (audioCodec != nullptr || audioTxChannel != nullptr) {
    TCA.write1(moody_rx::board::kTcaAmplifierEnablePin, LOW);
    stopAudioI2sDriver();
    if (audioCodec != nullptr) {
      es8311_delete(audioCodec);
      audioCodec = nullptr;
    }
    Serial.println("audio playback stopped; PA_CTRL LOW");
  }
}

static bool resetDisplay() {
  if (!TCA.write1(moody_rx::board::kTcaLcdResetPin, HIGH)) return false;
  delay(10);
  if (!TCA.write1(moody_rx::board::kTcaLcdResetPin, LOW)) return false;
  delay(10);
  if (!TCA.write1(moody_rx::board::kTcaLcdResetPin, HIGH)) return false;
  delay(200);
  return true;
}

static bool initialiseDisplay() {
  if (!resetDisplay() || !gfx->begin(LCD_SPI_HZ)) return false;

  gfx->setRotation(1);
  digitalWrite(moody_rx::board::kDisplayBacklightPin, HIGH);
  return true;
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

// Polls the FT6336 independently of JPEG decode so short taps are not missed.
// Only I2C reads happen here; loop() owns drawing and codec writes.
static void touchPollTask(void *) {
  moody_rx::TouchEdge edge;
  moody_rx::TouchPoint point;
  for (;;) {
    if (touchPanel.read(point) && edge.pressed(point.touched)) {
      const uint16_t rawY = point.rawY >= DISPLAY_WIDTH ? DISPLAY_WIDTH - 1 : point.rawY;
      const uint16_t x = moody_rx::board::kTouchLandscapeXFlipped
        ? static_cast<uint16_t>(DISPLAY_WIDTH - 1 - rawY)
        : rawY;
      Serial.printf("touch tap raw=%u,%u screen_x=%u\n",
                    point.rawX, point.rawY, x);
      xQueueSend(touchTaps, &x, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
  }
}

// Touch is optional: any failure leaves video and audio at the saved volume.
static void initialiseTouchVolume() {
  settings.begin("moody-rx", false);
  savedUiVolume = settings.getUChar("volume", DEFAULT_UI_VOLUME);
  volumeControl = moody_rx::VolumeControl(savedUiVolume);
  savedUiVolume = volumeControl.volume();
  Serial.printf("volume: restored %u%%\n", savedUiVolume);

  if (!TCA.write1(moody_rx::board::kTcaTouchResetPin, LOW) ||
      !TCA.pinMode1(moody_rx::board::kTcaTouchResetPin, OUTPUT)) {
    Serial.println("touch disabled: could not drive TCA9554 TP_RST");
    return;
  }
  delay(10);
  TCA.write1(moody_rx::board::kTcaTouchResetPin, HIGH);
  delay(300);  // FT6336 needs ~300 ms after reset before I2C.

  uint8_t chipId = 0;
  uint8_t vendorId = 0;
  if (!touchPanel.readIds(chipId, vendorId)) {
    Serial.println("touch disabled: FT6336 did not answer at 0x38");
    return;
  }
  Serial.printf("touch: FT6336 chip=0x%02x vendor=0x%02x\n", chipId, vendorId);

  touchTaps = xQueueCreate(TOUCH_TAP_QUEUE_LENGTH, sizeof(uint16_t));
  if (touchTaps == nullptr ||
      xTaskCreatePinnedToCore(touchPollTask, "moody-touch",
                              TOUCH_TASK_STACK_BYTES, nullptr, 2,
                              nullptr, 1) != pdPASS) {
    Serial.println("touch disabled: could not start touch task");
  }
}

static void drawVolumeOverlay() {
  static constexpr int16_t kLeft = 90;
  static constexpr int16_t kTop = 268;
  static constexpr int16_t kWidth = 300;
  static constexpr int16_t kHeight = 36;
  static constexpr int16_t kBarLeft = kLeft + 60;
  static constexpr int16_t kBarWidth = 150;
  const uint8_t volume = volumeControl.volume();

  gfx->fillRect(kLeft, kTop, kWidth, kHeight, RGB565_BLACK);
  gfx->drawRect(kLeft, kTop, kWidth, kHeight, RGB565_DARKGREY);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(kLeft + 10, kTop + 11);
  gfx->print("VOL");
  const int16_t segment = kBarWidth / (moody_rx::VolumeControl::kMax /
                                       moody_rx::VolumeControl::kStep);
  for (uint8_t level = moody_rx::VolumeControl::kStep, i = 0;
       level <= moody_rx::VolumeControl::kMax;
       level += moody_rx::VolumeControl::kStep, ++i) {
    const int16_t x = kBarLeft + i * segment;
    const uint16_t colour = level <= volume ? RGB565_GREEN : RGB565_DARKGREY;
    gfx->fillRect(x, kTop + 9, segment - 3, kHeight - 18, colour);
  }
  gfx->setCursor(kBarLeft + kBarWidth + 12, kTop + 11);
  gfx->printf("%3u%%", volume);
}

// Applies queued taps and hides/saves the overlay. Called from loop() only,
// so codec writes never race startAudioPlayback()/stopAudioPlayback().
static void serviceVolumeTouch() {
  if (touchTaps == nullptr) return;
  uint16_t x = 0;
  bool tapped = false;
  while (xQueueReceive(touchTaps, &x, 0) == pdTRUE) {
    volumeControl.onTap(x, DISPLAY_WIDTH, millis());
    tapped = true;
  }
  if (tapped) {
    if (audioCodec != nullptr) {
      es8311_voice_volume_set(
        audioCodec, moody_rx::codecVolumeFor(volumeControl.volume()), nullptr);
    }
    drawVolumeOverlay();
  }
  // The next video frame paints over the expired overlay; save only then so
  // a burst of taps costs one flash write.
  if (volumeControl.overlayExpired(millis()) &&
      volumeControl.volume() != savedUiVolume) {
    savedUiVolume = volumeControl.volume();
    settings.putUChar("volume", savedUiVolume);
    Serial.printf("volume: saved %u%%\n", savedUiVolume);
  }
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

static void resetConnectionReceiveState() {
  if (partialVideoFrame.active && !releaseFrameSlot(partialVideoFrame.slot)) {
    Serial.println("Reader failed to return incomplete JPEG slot");
  }
  partialVideoFrame = {};
  audioPcmQueue.clear();
  hasExpectedRecordSequence = false;
  expectedRecordSequence = 0;
  droppedRecordCount = 0;
  hasTxClockOffset = false;
  txClockOffsetMs = 0;
}

static bool beginVideoFrame(const moody_audio::RecordHeader &header,
                            const uint8_t payload[8],
                            uint32_t headerReceivedAtMs,
                            uint32_t headerWaitUs) {
  const uint32_t jpegLength = moody_audio::readU32BE(payload + 4);
  if (partialVideoFrame.active ||
      !moody_audio::validVideoFrameLength(
        jpegLength, static_cast<uint32_t>(MAX_JPEG_SIZE))) {
    Serial.printf("Invalid VIDEO_BEGIN JPEG length: %lu\n",
                  static_cast<unsigned long>(jpegLength));
    return false;
  }

  uint8_t slot = 0;
  if (xQueueReceive(freeSlots, &slot, portMAX_DELAY) != pdTRUE ||
      !markSlotAcquired(slot)) {
    return false;
  }

  if (!hasTxClockOffset) {
    txClockOffsetMs =
      remoteToLocalCounterOffset(header.timestamp, headerReceivedAtMs);
    hasTxClockOffset = true;
  }

  partialVideoFrame.active = true;
  partialVideoFrame.slot = slot;
  partialVideoFrame.frameId = moody_audio::readU32BE(payload);
  partialVideoFrame.txMillis = header.timestamp;
  partialVideoFrame.txClockOffsetMs = txClockOffsetMs;
  partialVideoFrame.jpegLength = jpegLength;
  partialVideoFrame.nextOffset = 0;
  partialVideoFrame.headerWaitUs = headerWaitUs;
  partialVideoFrame.payloadStartedUs = micros();
  return true;
}

static bool finishVideoFrame() {
  if (!partialVideoFrame.active ||
      partialVideoFrame.nextOffset != partialVideoFrame.jpegLength) {
    return false;
  }

  FrameDescriptor descriptor = {};
  descriptor.slot = partialVideoFrame.slot;
  descriptor.frameId = partialVideoFrame.frameId;
  descriptor.txMillis = partialVideoFrame.txMillis;
  descriptor.txClockOffsetMs = partialVideoFrame.txClockOffsetMs;
  descriptor.jpegLength = partialVideoFrame.jpegLength;
  descriptor.headerWaitUs = partialVideoFrame.headerWaitUs;
  descriptor.payloadUs = elapsedCounter(micros(), partialVideoFrame.payloadStartedUs);
  descriptor.enqueuedAtUs = micros();
  descriptor.terminal = false;
  if (!enqueueFrameDescriptor(descriptor)) return false;

  partialVideoFrame = {};
  return true;
}

static bool appendVideoChunk(const uint8_t *payload, uint16_t payloadLength) {
  if (!partialVideoFrame.active || payloadLength < 5U) return false;

  const uint32_t offset = moody_audio::readU32BE(payload);
  const uint32_t chunkLength = static_cast<uint32_t>(payloadLength - 4U);
  if (!moody_audio::validVideoChunk(partialVideoFrame.jpegLength,
                                    partialVideoFrame.nextOffset,
                                    offset,
                                    chunkLength)) {
    Serial.println("Invalid VIDEO_CHUNK offset or length");
    return false;
  }

  memcpy(jpegBuffers[partialVideoFrame.slot] + offset, payload + 4U, chunkLength);
  partialVideoFrame.nextOffset += chunkLength;
  return partialVideoFrame.nextOffset == partialVideoFrame.jpegLength
    ? finishVideoFrame()
    : true;
}

static void recordSequence(const moody_audio::RecordHeader &header) {
  if (hasExpectedRecordSequence && header.sequence != expectedRecordSequence) {
    droppedRecordCount += header.sequence - expectedRecordSequence;
  }
  expectedRecordSequence = header.sequence + 1U;
  hasExpectedRecordSequence = true;
}

static bool readAndDispatchRecord() {
  uint8_t headerBytes[moody_audio::kRecordHeaderSize];
  const uint32_t waitStarted = micros();
  if (!readExact(streamClient, headerBytes, sizeof(headerBytes))) return false;

  const uint32_t headerReceivedAtMs = millis();
  const uint32_t headerWaitUs = elapsedCounter(micros(), waitStarted);
  moody_audio::RecordHeader header = {};
  if (!moody_audio::decodeRecordHeader(headerBytes, sizeof(headerBytes), header)) {
    Serial.println("Invalid media record header");
    return false;
  }
  recordSequence(header);

  switch (header.type) {
    case moody_audio::VIDEO_BEGIN: {
      uint8_t payload[8];
      return readExact(streamClient, payload, sizeof(payload)) &&
        beginVideoFrame(header, payload, headerReceivedAtMs, headerWaitUs);
    }
    case moody_audio::VIDEO_CHUNK: {
      uint8_t payload[4U + moody_audio::kMaxVideoChunkLength];
      return readExact(streamClient, payload, header.payloadLength) &&
        appendVideoChunk(payload, header.payloadLength);
    }
    case moody_audio::AUDIO_PCM: {
      uint8_t payload[moody_audio::kAudioPayloadLength];
      if (!readExact(streamClient, payload, sizeof(payload))) return false;
      // A missing queue or contended producer lock drops audio without ending
      // the video stream. enqueue() reports the dropped block to its caller.
      audioPcmQueue.enqueue(header.timestamp, payload);
      return true;
    }
    default:
      return false;
  }
}

static void finishStreamReader() {
  if (droppedRecordCount != 0U) {
    Serial.printf("Media record sequence gaps: %lu\n",
                  static_cast<unsigned long>(droppedRecordCount));
  }
  resetConnectionReceiveState();

  FrameDescriptor terminal = {};
  terminal.terminal = true;
  xQueueSend(readyFrames, &terminal, portMAX_DELAY);
  streamClient.stop();
  xSemaphoreGive(readerFinished);
  vTaskDelete(nullptr);
  while (true) delay(1000);
}

static void streamReaderTask(void *) {
  uint32_t records = 0;
  uint32_t lastStackReportAt = millis();
  while (readAndDispatchRecord()) {
    ++records;
    const uint32_t now = millis();
    if (records == 1 || records == 10 || records == 50 ||
        now - lastStackReportAt >= 5000) {
      // ESP-IDF reports this watermark in bytes (minimum unused stack).
      const uint32_t unusedBytes = uxTaskGetStackHighWaterMark(nullptr);
      Serial.printf("reader stack: size=%lu unused_min=%lu used_peak=%lu bytes records=%lu\n",
                    static_cast<unsigned long>(STREAM_READER_STACK_BYTES),
                    static_cast<unsigned long>(unusedBytes),
                    static_cast<unsigned long>(STREAM_READER_STACK_BYTES - unusedBytes),
                    static_cast<unsigned long>(records));
      lastStackReportAt = now;
    }
  }
  Serial.println("Media stream ended or contained an invalid record");
  finishStreamReader();
}

static bool startStreamReader() {
  if (streamReaderRunning) return true;

  xSemaphoreTake(readerFinished, 0);
  resetConnectionReceiveState();
  if (!resetFrameQueues()) return false;

  const BaseType_t created = xTaskCreatePinnedToCore(
    streamReaderTask,
    "stream-reader",
    STREAM_READER_STACK_BYTES,
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
  if (streamReaderRunning) {
    // A terminal descriptor is enqueued before this completion signal. Waiting
    // here ensures the old reader has stopped using streamClient before reconnect.
    xSemaphoreTake(readerFinished, portMAX_DELAY);
    streamReaderRunning = false;
  }
  stopAudioPlayback();
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
  if (xQueueReceive(readyFrames, &descriptor, pdMS_TO_TICKS(FRAME_WAIT_MS)) != pdTRUE) {
    return true;  // No frame yet; loop() services touch and waits again.
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
  if (volumeControl.overlayVisible()) drawVolumeOverlay();

  if (!releaseFrameSlot(descriptor.slot)) return false;

  reportRxTiming();
  return true; // A damaged complete JPEG is recoverable; its slot is released.
}

static bool openAuthenticatedStream() {
  ESP_LOGE("moody-rx", "opening authenticated stream");
  showStatus("Connecting", "Contacting moody-tx...", RGB565_CYAN);
  streamClient.stop();
  resetConnectionReceiveState();
  if (!resetFrameQueues()) return false;
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

  uint8_t preamble[sizeof(moody_audio::kPreamble)];
  if (!readExact(streamClient, preamble, sizeof(preamble))) {
    Serial.println("Could not read transmitter media preamble");
    streamClient.stop();
    return false;
  }
  if (memcmp(preamble, moody_audio::kPreamble, sizeof(preamble)) != 0) {
    ESP_LOGE("moody-rx", "incompatible transmitter media protocol");
    Serial.println("Incompatible transmitter firmware: expected MOODYAV1");
    showStatus("Incompatible moody-tx", "Update transmitter firmware", RGB565_RED);
    resetConnectionReceiveState();
    resetFrameQueues();
    streamClient.stop();
    return false;
  }

  gfx->fillScreen(RGB565_BLACK);
  resetRxTiming(millis());
  ESP_LOGE("moody-rx", "stream authenticated");
  // Audio is optional: codec, I2S, or expander failures leave video active.
  startAudioPlayback();
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("rx setup: serial ready");
  ESP_LOGE("moody-rx", "setup started");

  if (!initialiseBoardIo()) {
    ESP_LOGE("moody-rx", "board I/O initialisation failed");
    Serial.println("Fatal: board I/O initialisation failed");
    while (true) delay(1000);
  }
  Serial.println("rx setup: display reset complete");

  if (!initialiseDisplay()) {
    ESP_LOGE("moody-rx", "display initialisation failed");
    Serial.println("Fatal: display initialisation failed");
    while (true) delay(1000);
  }

  Serial.println("rx setup: display ready");
  initialiseTouchVolume();

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
  audioPlaybackFinished = xSemaphoreCreateBinary();
  audioQueueReady = audioPcmQueue.initialise();
  if (!freeSlots || !readyFrames || !fifoStateMutex || !readerFinished) {
    ESP_LOGE("moody-rx", "FIFO allocation failed");
    showStatus("Fatal error", "No FIFO control memory", RGB565_RED);
    while (true) delay(1000);
  }
  if (!audioQueueReady || audioPlaybackFinished == nullptr) {
    Serial.println("Audio queue unavailable; continuing with video only");
    audioQueueReady = false;
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
      stopAudioPlayback();
      streamClient.stop();
      showStatus("Stream unavailable", "Retrying...", RGB565_RED);
      delay(RETRY_DELAY_MS);
      return;
    }
  }

  serviceVolumeTouch();
  if (!receiveAndDisplayQueuedFrame()) {
    stopStreamReader();
    showStatus("Stream interrupted", "Reconnecting...", RGB565_RED);
    delay(RETRY_DELAY_MS);
  }
}
