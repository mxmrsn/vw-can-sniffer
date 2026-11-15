#!/usr/bin/env bash
set -euo pipefail

# Upload SPIFFS image for ESP32 web assets
# Requires: platformio

cd "$(dirname "$0")/.."   # platformio.ini lives in hardware/teensy-esp32
pio run -e esp32dev -t uploadfs
