#!/bin/sh
set -eu

source_file="$(dirname "$0")/../CameraWebServer/app_httpd.cpp"

grep -F 'application/octet-stream' "$source_file" >/dev/null
grep -F 'AudioContext' "$source_file" >/dev/null
grep -F 'Serial.printf("I2S audio:' "$source_file" >/dev/null
grep -F 'id="meter"' "$source_file" >/dev/null
grep -F 'Pause live audio' "$source_file" >/dev/null
grep -F 'Replay last 10 seconds' "$source_file" >/dev/null
grep -F 'I2S_CHANNEL_FMT_ONLY_RIGHT' "$source_file" >/dev/null
