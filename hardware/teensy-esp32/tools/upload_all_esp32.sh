#!/usr/bin/env bash
set -euo pipefail

# Upload ESP32 firmware and SPIFFS assets
# Requires: platformio

cd "$(dirname "$0")/.."   # platformio.ini lives in hardware/teensy-esp32
pio run -e esp32dev -t upload
pio run -e esp32dev -t uploadfs
