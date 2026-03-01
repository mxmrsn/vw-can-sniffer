# ESP32 UART Gateway

This firmware bridges Teensy UART CAN frames to a browser dashboard via WebSocket.

## Dependencies
Install these in Arduino IDE:
- `WebSockets` by Markus Sattler

## Build & Upload
1. Open `firmware/esp32/esp32_uart_gateway.ino`.
2. Select an ESP32 board (e.g., `ESP32 Dev Module`).
3. Set Wi-Fi credentials in the sketch:

```
static const char *WIFI_SSID = "YOUR_SSID";
static const char *WIFI_PASS = "YOUR_PASSWORD";
```

4. Upload to the ESP32.

## Upload Web Assets (SPIFFS)
The HTML/CSS/JS live in `firmware/esp32/data/` and must be uploaded to SPIFFS.

### PlatformIO (Recommended)
1. Install PlatformIO.
2. Run:

```
./tools/upload_spiffs.sh
```

To upload firmware:

```
./tools/upload_esp32.sh
```

To upload both firmware and SPIFFS:

```
./tools/upload_all_esp32.sh
```

### Arduino IDE 2.x
1. Tools → `ESP32 Sketch Data Upload`.
2. This creates and uploads a SPIFFS image.

If the menu item is missing, install the ESP32 filesystem uploader plugin
for your IDE version.

## Wiring
Default UART pins:
- `RX` = GPIO16
- `TX` = GPIO17

Connect:
- `Teensy TX` → `ESP32 RX` (GPIO16)
- `Teensy RX` ← `ESP32 TX` (GPIO17)
- `GND` ↔ `GND`

## Dashboard
Open a browser to the ESP32’s IP address.
- Web UI: `http://<esp32-ip>/`
- WebSocket: `ws://<esp32-ip>:81`

## Stats
The dashboard shows two kinds of stats:
- `Rx ok / CRC bad / Bytes` from the ESP32 UART parser
- `T rx_ok / T drop` from Teensy status frames (type `0x02`)

## Controls
The UI can send commands to the Teensy:
- Toggle output mode (UART/Wi‑Fi vs USB)
- Toggle CAN silent mode

## Notes
- If STA Wi-Fi connection fails, the ESP32 starts an AP:
  - SSID: `vw-can-sniffer`
  - Password: `canlogger`
