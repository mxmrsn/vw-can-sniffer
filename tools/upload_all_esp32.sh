#!/usr/bin/env bash
set -euo pipefail

# Upload ESP32 firmware and SPIFFS assets
# Requires: platformio

pio run -e esp32dev -t upload
pio run -e esp32dev -t uploadfs
