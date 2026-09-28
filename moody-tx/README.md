# moody-tx

Sender firmware for the Freenove ESP32-S3-WROOM camera board with an OV2640.
It creates a dedicated WPA2 Wi-Fi access point, mutually authenticates the
permanently paired `moody-rx`, then sends a low-latency stream of JPEG frames.

## Security model

- The Wi-Fi password encrypts the local radio link.
- `moody-tx` signs a fresh 32-byte nonce with its P-256 private key.
- `moody-rx` will verify that signature with the embedded transmitter public key.
- `moody-rx` signs the same nonce with its own P-256 private key.
- `moody-tx` accepts the connection only if that signature verifies against its
  embedded receiver public key.
- A recorded authentication exchange cannot be replayed because every connection
  uses a new random nonce.

The identities are permanently paired. Replacing either unit requires generating
or provisioning a new identity and reflashing the peer with its new public key.

## Permanent identities

This delivered project already contains a generated permanent pair. Do not run
the generator before building `moody-rx`, or its saved receiver private key will
no longer match the public key compiled into `moody-tx`.

Only run the following when you intentionally want to replace both identities
and reflash both units:

```bash
cd /path/to/moody-tx
chmod +x generate_keys.sh
./generate_keys.sh
```

This creates:

- `moody_keys.h`: transmitter private key plus receiver public key; compiled into
  this project.
- `provisioning/moody-tx-public.pem`: needed by the future receiver firmware.
- `provisioning/moody-rx-private.pem`: needed by the future receiver firmware.
- The remaining PEM files are the complete identity backup.

Do not publish the private PEM files or commit them to a public repository.

## Configure the dedicated network

Edit these values near the top of `moody-tx.ino`:

```cpp
static const char *AP_SSID = "moody-camera";
static const char *AP_PASSWORD = "CHANGE-ME-9274";
```

Use a new password made specifically for this device link, not a home Wi-Fi
password. The receiver will later use the same values.

## Arduino IDE settings

- Board: `ESP32S3 Dev Module`
- esp32 by Espressif Systems: `3.3.5`
- USB CDC On Boot: `Enabled`
- Flash Size: select the board's actual flash size (commonly 16 MB)
- PSRAM: `OPI PSRAM`
- Partition Scheme: `Huge APP`

No third-party Arduino library is required. The ESP32 camera, Wi-Fi and mbedTLS
components come with the Espressif board package.

## INMP441 microphone wiring

Turn off all power before connecting the microphone. The verified assignments
below keep the microphone off the camera, OPI PSRAM, USB, UART0, SD-card, and
ESP32-S3 strapping pins. The three GPIO constants are in `audio_pins.h`.

| INMP441 module pin | Connect to the Freenove ESP32-S3-WROOM CAM | Purpose |
| --- | --- | --- |
| VDD | 3V3 | 3.3 V supply |
| GND | GND | Common ground |
| SCK / BCLK | GPIO41 | I2S bit clock |
| WS / LRCLK | GPIO21 | I2S word-select clock |
| SD | GPIO47 | I2S microphone data into the ESP32-S3 |
| L/R | GND | Select the left I2S channel |

The INMP441 has no MCLK pin; do not add one. The audio stream profile is mono
signed 16-bit PCM at 16 kHz, sent in 20 ms blocks (640 PCM bytes).

The GPIO selection was checked against the [Freenove ESP32-S3-WROOM board
documentation](https://docs.freenove.com/projects/fnk0083/en/latest/fnk0083/codes/Python/Preface.html),
its [GPIO interface diagram](https://docs.freenove.com/projects/fnk0083/en/latest/_images/Preface54.png),
and this sketch's `CAM_*` pin map. GPIO41, GPIO21, and GPIO47 are physical
header pins on this exact board revision; the camera uses GPIO4--GPIO18.
WS was moved from GPIO42 after its header pin failed a continuity check on
this unit.

## Wire protocol v1

Authentication is unchanged. All multi-byte integers in this authentication
exchange are unsigned and big-endian.

1. TX sends `MOODYTX1`, a 32-byte nonce, a 16-bit signature length, and its DER
   ECDSA signature over `SHA-256("moody-tx-proof-v1" || nonce)`.
2. RX verifies the TX signature and sends `MOODYRX1`, a 16-bit signature length,
   and its DER ECDSA signature over `SHA-256("moody-rx-proof-v1" || nonce)`.
3. TX sends `AUTHOK01` or `AUTHNO01`.

After `AUTHOK01`, TX writes the eight-byte `MOODYAV1` preamble once, then a
sequence of typed media records. Every record has this 12-byte header:

| Bytes | Field | Encoding |
| --- | --- | --- |
| 0 | type | `uint8` |
| 1 | flags | `uint8`; TX currently writes `0` |
| 2--3 | payload length | unsigned 16-bit big-endian |
| 4--7 | sequence | unsigned 32-bit big-endian; starts at 0 and increments for every record |
| 8--11 | timestamp | unsigned 32-bit big-endian sender `millis()` value |

The record types and payload bounds are:

| Type | Payload |
| --- | --- |
| `VIDEO_BEGIN` (`1`) | Exactly 8 bytes: 32-bit big-endian frame ID, then 32-bit big-endian JPEG length. RX accepts JPEG lengths from 1 through 131072 bytes. |
| `VIDEO_CHUNK` (`2`) | 5 through 1028 bytes: 32-bit big-endian offset followed by 1 through 1024 JPEG bytes. Chunks for an active frame must have contiguous offsets and end exactly at the declared JPEG length. |
| `AUDIO_PCM` (`3`) | Exactly 640 bytes: 320 mono signed 16-bit PCM samples, little-endian, at 16 kHz. One record is 20 ms of audio. |

TX sends `VIDEO_BEGIN` before its frame's chunks. Audio records can appear
between video chunks, so an implementation must retain the active frame while
dispatching audio. TX uses a bounded four-block audio queue. When that queue is
full, capture replaces its oldest block; if capture cannot immediately acquire
the queue mutex, it drops the fresh block instead. TX does not print an
audio-drop counter.

`MOODYAV1` is a paired-firmware protocol change. Flash compatible current TX
and RX firmware together: an older RX rejects the preamble and a current RX
rejects the older post-auth stream.

The current profile uses a 3:2 HVGA (480 x 320) camera window, JPEG quality 16,
a 20 MHz camera clock, and a 20 fps target. The matching aspect ratio fills the
receiver without stretching the image. Frames are captured with
`CAMERA_GRAB_WHEN_EMPTY`, so capture does not deliberately accumulate a backlog
while the previous frame is still in use.

If the attached sensor rejects hardware JPEG mode, the firmware automatically
reinitialises it in RGB565 mode and software-encodes each frame as JPEG quality
75. The receiver protocol does not change. Software encoding may reduce the
achieved FPS, which is reported by `moody-rx`.

## First test

For a reproducible command-line upload, connect the transmitter and run:

```sh
sh scripts/upload-tx.sh /dev/cu.usbmodemXXXXXXXX
```

The helper creates the project-local `uv` environment if needed, runs `uv add
esptool`, builds the primary transmitter sketch, then flashes the bootloader,
partition table, and firmware. The separate `scripts/compile-tx.sh` remains the
slower two-sketch mirror-validation build.

Open Serial Monitor at 115200 baud after uploading. Expected output:

```text
moody-tx starting
AP: moody-camera
Address: 192.168.4.1:3333
Waiting for permanently paired moody-rx
audio=enabled
moody-rx authenticated; starting media stream
tx fps=... frames=... source=sensor jpeg_B=.../... capture_us=.../... encode_us=.../... send_us=.../...
```

If microphone I2S or its capture task cannot start, the line is
`audio=disabled`; video still starts. These are the only TX audio state lines.

The transmitter will not stream to a browser or an unpaired client. It waits for
the matching receiver signature before releasing any camera frames.

Live timing is reported once per second. Each `value/value` pair is the average
and maximum for that window. `source=sensor` with zero encode time means the
camera supplied JPEG directly; `source=software-new` means RGB565 frames are
being encoded by the vendored Espressif encoder on the ESP32-S3.
