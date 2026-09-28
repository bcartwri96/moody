# moody-rx

Receiver firmware for the Waveshare ESP32-Touch-LCD-3.5 (classic ESP32). It
joins the private network created by `moody-tx`, mutually authenticates the
permanently paired transmitter, receives JPEG video frames, and draws them to
the LCD.

## Displayed startup states

1. Starting / loading paired identity
2. Waiting for `moody-tx`
3. Network found / opening secure link
4. Authenticating / verifying `moody-tx`
5. Authenticated / waiting for video
6. Full-screen live video; measured FPS and frame IDs are reported over Serial

Network, authentication, and fatal stream failures are shown on the LCD before
the receiver retries automatically. Recoverable JPEG decode failures are counted
in the once-per-second Serial summary while streaming continues.

## Dedicated Wi-Fi credentials

The following values in `moody-rx.ino` must exactly match `moody-tx.ino`:

```cpp
static const char *TX_AP_SSID = "moody-camera";
static const char *TX_AP_PASSWORD = "CHANGE-ME-9274";
```

Use a password created specifically for this device-to-device network.

## Permanent pairing

`moody_keys.h` contains the receiver private key and transmitter public key from
the permanent pair generated with the delivered `moody-tx` project. Do not
regenerate either project's keys independently.

## Wire protocol v1

Authentication is unchanged. TX sends `MOODYTX1`, a 32-byte nonce, a 16-bit
big-endian signature length, and its DER ECDSA signature over
`SHA-256("moody-tx-proof-v1" || nonce)`. RX verifies it, replies with `MOODYRX1`,
a 16-bit big-endian signature length, and its DER ECDSA signature over
`SHA-256("moody-rx-proof-v1" || nonce)`. TX then sends `AUTHOK01` or
`AUTHNO01`.

After `AUTHOK01`, RX requires the eight-byte `MOODYAV1` preamble once. It then
accepts typed media records with this 12-byte header:

| Bytes | Field | Encoding |
| --- | --- | --- |
| 0 | type | `uint8` |
| 1 | flags | `uint8`; current TX writes `0` |
| 2--3 | payload length | unsigned 16-bit big-endian |
| 4--7 | sequence | unsigned 32-bit big-endian; current TX starts at 0 and increments for every record |
| 8--11 | timestamp | unsigned 32-bit big-endian sender `millis()` value |

| Type | Payload and RX validation |
| --- | --- |
| `VIDEO_BEGIN` (`1`) | Exactly 8 bytes: 32-bit big-endian frame ID and 32-bit big-endian JPEG length. RX accepts 1 through 131072 bytes. |
| `VIDEO_CHUNK` (`2`) | 5 through 1028 bytes: 32-bit big-endian offset followed by 1 through 1024 JPEG bytes. It must extend the active frame at the expected contiguous offset and complete at the declared JPEG length. |
| `AUDIO_PCM` (`3`) | Exactly 640 bytes: 320 mono signed 16-bit PCM samples, little-endian, at 16 kHz. Each record is 20 ms. |

Audio may be interleaved between a frame's `VIDEO_BEGIN` and `VIDEO_CHUNK`
records. RX keeps the partial JPEG while it queues PCM. Both projects must run
compatible `MOODYAV1` firmware: RX reports `Incompatible transmitter firmware:
expected MOODYAV1` for an older post-auth stream, while an older RX cannot read
the current preamble.

## Arduino IDE settings

- Board: `ESP32 Dev Module`
- esp32 by Espressif Systems: `3.3.5`
- Flash Size: `16 MB`
- PSRAM: `Enabled` (2 MB on the confirmed board)
- Partition Scheme: `Huge APP`
- GFX Library for Arduino: `1.5.5`
- TCA9554: Waveshare-provided version or compatible `0.1.2`
- JPEGDEC by Larry Bank: install through Library Manager
- ES8311: vendored Waveshare driver at revision
  `283ec84c566c096f8c30493b93dcd4b0bb608de7` in `src/es8311/`

Build the confirmed ESP32-D0WDR2-V3 board with 16 MB flash and 2 MB PSRAM:

```sh
sh scripts/build-rx.sh
```

The helper builds without uploading and writes build files to
`/private/tmp/moody-rx-classic/build` and firmware to
`/private/tmp/moody-rx-classic/firmware`. Its FQBN is
`esp32:esp32:esp32:FlashSize=16M,PartitionScheme=huge_app,PSRAM=enabled`.
Do not select an ESP32-S3 target; the sketch rejects that build.

Live video fills the existing 480 x 320 landscape surface without an on-screen
overlay. FPS and frame IDs remain visible in Serial Monitor.

## Display and board control

The display uses VSPI: GPIO23 MOSI, GPIO19 MISO, GPIO18 SCLK, GPIO5 CS,
and GPIO27 DC. Its reset line is TCA9554 P0, pulsed high/low/high before the
ST7796 starts. GPIO25 enables the backlight after display initialization.
The native 320 x 480 panel uses rotation 1 for landscape video.

GPIO21/GPIO22 are the shared I2C SDA/SCL bus for the TCA9554 and ES8311.
Board setup preloads TCA9554 P2 (`PA_CTRL`) LOW before making it an output.
The amplifier remains disabled until codec and I2S initialization succeed.
LCD reset P0 and the expander remain required for display startup. A failed
amplifier setup disables audio while allowing video to continue.

## Audio hardware and verified pin map

The classic board has an ES8311 codec and onboard speaker amplifier. Connect
a compatible speaker to its onboard speaker connector for audible playback.

| Signal | GPIO or controller pin | Firmware configuration |
| --- | --- | --- |
| Audio I2C SDA | GPIO21 | Shared I2C SDA |
| Audio I2C SCL | GPIO22 | Shared I2C SCL |
| Codec MCLK / UART RX | GPIO3 | **Never output a clock on this pin:** the onboard CH343P USB bridge also drives it |
| I2S BCLK | GPIO2 | 1.024 MHz at 16 kHz, two 32-bit slots |
| I2S LRCLK | GPIO4 | 16 kHz word-select clock |
| Playback data out | GPIO12 | ESP32 to ES8311, standard I2S stereo 32-bit |
| Onboard microphone data in | GPIO34 | Unused for remote microphone playback |
| Amplifier enable | TCA9554 P2 | LOW until codec and I2S initialization succeed |

The codec derives its internal 4.096 MHz master clock from BCLK. Firmware sets
`mclk_from_mclk_pin=false` and leaves I2S MCLK unassigned (`-1`), avoiding
output contention with the CH343P on GPIO3. Both codec resolution and I2S slot
width are 32 bits. Each transport sample remains signed 16-bit mono PCM;
playback duplicates it into the upper 16 bits of the left and right 32-bit
slots. Thus each 640-byte, 20 ms network record becomes 2560 bytes of I2S data,
with no change to its duration or network format.

Pin assignments come from Waveshare's [classic board page](https://www.waveshare.com/wiki/ESP32-Touch-LCD-3.5)
and [Rev1.1 schematic](https://files.waveshare.com/wiki/ESP32-Touch-LCD-3.5/ESP32-Touch-LCD-3.5_Rev1.1.pdf).

The ES8311 driver is pinned at Waveshare revision
`283ec84c566c096f8c30493b93dcd4b0bb608de7` (source:
[`Arduino/libraries/es8311`](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.5/tree/283ec84c566c096f8c30493b93dcd4b0bb608de7/Arduino/libraries/es8311)).
The three source files and repository Apache-2.0 license are in
`src/es8311/`; Arduino compiles them automatically, without a separate library
installation or special `--libraries` argument.

At each authenticated connection, RX initializes codec and I2S, then enables
TCA9554 P2. Any codec/I2S/amplifier error disables audio for that connection
while video continues. Disconnect clears queued PCM, disables P2, stops I2S,
and retries initialization on the next authenticated stream.

The playback task is separate from the stream reader and LCD loop. It consumes
a four-block audio queue and gives each I2S write a bounded timeout. I2S clears
transmitted DMA buffers automatically, so a PCM underrun outputs silence once
already buffered audio finishes. When the
queue is full, RX replaces its oldest PCM block; if the reader cannot
immediately acquire the queue mutex, it drops the fresh block instead. The
five-second playback report includes a safe queue-depth snapshot and separate
window counters for full-queue and mutex-contention drops. Media-record
sequence gaps remain a separate transport diagnostic.

## Expected serial output

```text
moody-rx ready
Looking for moody-camera
Joined; RX address 192.168.4.2
Mutual authentication complete
audio codec ready: rate=16000 Hz internal MCLK=4096000 Hz (from BCLK) stereo 32-bit
audio playback enabled
audio=ready rate=16000 Hz queue=0/4
rx fps=... attempted=... displayed=... failed=... last_frame=... jpeg_B=.../... wait_us=.../... receive_us=.../... fifo_us=.../... age_ms=.../... queued=... decode_us=.../... lcd_us=.../...
audio playback: blocks=... underruns=... short_writes=... queue=.../4 drops_full=... drops_contended=...
```

The codec-ready and playback-enabled lines only appear after successful ES8311,
I2S, and amplifier setup. A failed setup prints its actual cause, such as
`audio init failed at ES8311 init: ...` or
`audio playback disabled: PCM queue unavailable`, then prints
`audio=disabled rate=16000 Hz queue=.../4`; the unavailable queue case prints
`queue=unavailable/4`. Video continues in either disabled case.
`Media record sequence gaps: ...` is printed when record sequence numbers skip;
it is distinct from `drops_full` and `drops_contended`, which are audio queue
drop counts for the same five-second playback-report window.

Live timing is reported once per second to avoid Serial output affecting every
frame. Each `value/value` pair is the average and maximum for that window.
`attempted`, `displayed`, and `failed` distinguish recoverable JPEG failures;
`wait_us` is time waiting for the next frame header, `receive_us` is JPEG payload
read time, `decode_us` excludes LCD writes, and `lcd_us` is time spent sending
decoded pixels to the display.
