#!/usr/bin/env bash
set -euo pipefail

# Serve ESP32 UI locally for preview
# Open: http://localhost:8000

cd firmware/esp32/data
python3 -m http.server 8000
