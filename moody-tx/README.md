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

## Wire protocol v1

All multi-byte integers are unsigned and big-endian.

1. TX sends `MOODYTX1`, a 32-byte nonce, a 16-bit signature length, and its DER
   ECDSA signature over `SHA-256("moody-tx-proof-v1" || nonce)`.
2. RX verifies the TX signature and sends `MOODYRX1`, a 16-bit signature length,
   and its DER ECDSA signature over `SHA-256("moody-rx-proof-v1" || nonce)`.
3. TX sends `AUTHOK01` or `AUTHNO01`.
4. After success, each frame is a 16-byte header followed by JPEG bytes:
   `MJPG`, 32-bit frame ID, 32-bit JPEG length, 32-bit sender uptime in ms.

The current profile uses a 3:2 HVGA (480 x 320) camera window, JPEG quality 16,
a 20 MHz camera clock, and a 20 fps target. The matching aspect ratio fills the
receiver without stretching the image. Frames are captured with
`CAMERA_GRAB_LATEST`; if transmission falls behind, the schedule is reset rather
than deliberately accumulating latency.

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
moody-rx authenticated; starting video
tx fps=... frames=... source=sensor jpeg_B=.../... capture_us=.../... encode_us=.../... send_us=.../...
```

The transmitter will not stream to a browser or an unpaired client. It waits for
the matching receiver signature before releasing any camera frames.

Live timing is reported once per second. Each `value/value` pair is the average
and maximum for that window. `source=sensor` with zero encode time means the
camera supplied JPEG directly; `source=software-new` means RGB565 frames are
being encoded by the vendored Espressif encoder on the ESP32-S3.
