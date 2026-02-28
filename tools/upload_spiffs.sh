#!/usr/bin/env bash
set -euo pipefail

# Upload SPIFFS image for ESP32 web assets
# Requires: platformio

pio run -e esp32dev -t uploadfs
