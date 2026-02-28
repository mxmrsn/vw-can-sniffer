#!/usr/bin/env bash
set -euo pipefail

# Upload ESP32 firmware
# Requires: platformio

pio run -e esp32dev -t upload
