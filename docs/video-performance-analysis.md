# Video performance analysis

## Scope

This note records the measurements from the current full-screen 480 x 320
Moody video stream and the evidence-based next optimisation. The video wire
format remains a persistent, length-prefixed JPEG stream over the authenticated
TCP connection.

## Measured pipeline

The transmitter reports `source=software` on every frame. It captures RGB565
from the camera and uses `frame2jpg` to create the JPEG payload before sending
it.

| Transmitter stage | Observed average | Observed range / maximum |
| --- | ---: | ---: |
| Camera capture | about 69 ms | 64–78 ms |
| Software JPEG encode | about 133 ms | 130–135 ms |
| TCP send | about 33–172 ms | up to 275 ms |
| JPEG payload | about 7.6–13.1 KB | up to 14.5 KB |
| Delivered rate | 2.7–4.3 fps | — |

The capture and encode stages alone cost about 202 ms per frame. That produces
a best-case serial TX ceiling of roughly 4.95 fps before socket time is added:

```text
1000 / (69 ms capture + 133 ms encode) = 4.95 fps
```

The receiver is successfully displaying every received frame (`failed=0`).

| Receiver stage | Observed average | Observed range / maximum |
| --- | ---: | ---: |
| Wait for frame header | about 26–74 ms | up to 187 ms |
| Receive JPEG payload | about 38–90 ms | up to 161 ms |
| JPEG decode CPU time | about 67–70 ms | — |
| LCD writes | about 69.5 ms | — |
| Displayed rate | 3.5–4.3 fps | — |

The receiver's JPEG decode plus LCD update cost about 139 ms. Even with an
instant transmitter and network, the current serial receive/decode/draw path
therefore tops out at about 7.2 fps:

```text
1000 / (69 ms decode + 69.5 ms LCD) = 7.2 fps
```

## Root cause

The immediate 4 fps limit is not the transport protocol. It is the TX software
JPEG fallback.

`moody-tx` first requests `PIXFORMAT_JPEG`. The installed Espressif camera
driver returns `ESP_ERR_NOT_SUPPORTED`, after which the sketch intentionally
reinitialises the camera as RGB565 and invokes `frame2jpg` for each frame. In
this driver revision, that error is returned for a recognised camera sensor
whose `support_jpeg` capability is false. The supplied sensor therefore does
not provide camera-side JPEG output.

The variation in `send_us` and `receive_us` is downstream backpressure: while
the receiver is decoding and writing the LCD, it is not progressing the next
frame read. This can make TCP writes wait, but it does not explain the fixed
133 ms encode cost. Switching to UDP, RTP, or HTTP multipart would not remove
that cost or the receiver's 139 ms display path.

## Recommended next change

Replace the legacy `frame2jpg` fallback encoder with Espressif's
`esp_new_jpeg`, while keeping all of the following unchanged:

- 480 x 320 camera frame and full-screen display;
- JPEG payloads and the current authenticated TCP framing;
- preserve-and-crop display behaviour;
- latest-frame behaviour rather than building a latency queue.

`esp_new_jpeg` is Espressif's newer software JPEG library. It accepts RGB565
input, supports 4:2:0 subsampling, and supports mono- and dual-task encoding.
Its ESP32-S3 benchmark reports 15.84 fps for 480 x 320 RGB888 mono-task
encoding and 23.57 fps dual-task encoding. Those figures are not a promise for
this RGB565 camera path, but they show a materially faster canonical encoder
than the current 7.5 fps (`~133 ms`) fallback. The replacement must be measured
on the board using the existing timing lines.

If the faster encoder lowers `encode_us` substantially, RX becomes the next
constraint at about 7 fps. Further gains then require a receiver pipeline that
receives into a second JPEG buffer while the previous frame is decoded/drawn,
or a lower-resolution source that is upscaled with preserve-and-crop. The
latter trades image detail for a higher frame rate.

## References

- [Espressif esp32-camera README: sensor formats, RGB/Wi-Fi warning, and frame-buffer guidance](https://github.com/espressif/esp32-camera#important-to-remember)
- [Espressif camera driver: JPEG capability check in `esp_camera_init`](https://github.com/espressif/esp32-camera/blob/ddd00c5649ad72a007133e33ea8f34e9d7636078/driver/esp_camera.c)
- [Espressif camera sensor capability table](https://github.com/espressif/esp32-camera/blob/ddd00c5649ad72a007133e33ea8f34e9d7636078/driver/sensor.c)
- [Espressif `esp_new_jpeg`: features and ESP32-S3 encoder benchmarks](https://github.com/espressif/esp-adf-libs/tree/master/esp_new_jpeg)
