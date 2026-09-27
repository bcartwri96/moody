# moody-rx

Receiver firmware for the classic Waveshare ESP32-Touch-LCD-3.5. It joins the
private network created by `moody-tx`, mutually authenticates the permanently
paired transmitter, receives JPEG video frames, and draws them to the LCD.

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

## Arduino IDE settings

- Board: `ESP32 Dev Module`
- esp32 by Espressif Systems: `3.3.5`
- PSRAM: use the board's actual setting; the sketch also works without PSRAM
- GFX Library for Arduino: `1.5.5`
- TCA9554: Waveshare-provided version or compatible `0.1.2`
- JPEGDEC by Larry Bank: install through Library Manager

The transmitter supplies a 3:2 HVGA (480 x 320) camera window that fills the
landscape display without stretching. Live video has no on-screen overlay, so
the LCD spends its time drawing camera pixels; FPS and frame IDs remain visible
in Serial Monitor.

## Expected serial output

```text
moody-rx ready
Looking for moody-camera
Joined; RX address 192.168.4.2
Mutual authentication complete
rx fps=... attempted=... displayed=... failed=... last_frame=... jpeg_B=.../... wait_us=.../... receive_us=.../... decode_us=.../... lcd_us=.../...
```

Live timing is reported once per second to avoid Serial output affecting every
frame. Each `value/value` pair is the average and maximum for that window.
`attempted`, `displayed`, and `failed` distinguish recoverable JPEG failures;
`wait_us` is time waiting for the next frame header, `receive_us` is JPEG payload
read time, `decode_us` excludes LCD writes, and `lcd_us` is time spent sending
decoded pixels to the display.
