# VW CAN Sniffer

Hardware-agnostic CAN bus sniffer focused on MK7.5 VW GTI, starting with Teensy 4.1 and a UART stream protocol suitable for an ESP32 Wi‑Fi dashboard.

## Scope
- Teensy 4.1 CAN capture
- UART framing protocol for streaming frames to a Wi‑Fi module
- Future STM32H7 custom board

## Current Contents
- `firmware/teensy/teensy_uart_streamer.ino`
- `firmware/teensy/teensy_can_simulator.ino`
- `docs/uart_protocol.md`
- `firmware/esp32/esp32_uart_gateway.ino`

## Hardware (Initial Setup)
- Teensy 4.1
- CAN transceiver (TJA1051 recommended)
- ESP32 module (UART bridge for Wi‑Fi)

### Basic Wiring (Teensy ↔ ESP32)
- `Teensy TX` → `ESP32 RX`
- `Teensy RX` ← `ESP32 TX`
- `GND` ↔ `GND`

## Firmware Notes
- Default UART: `Serial1` @ `921600`
- Default CAN speed: `500000`
- CAN IDs encode extended frames with bit31 set

### Test Mode
Enable synthetic frames for bench testing:

```
#define TEST_MODE 1
#define TEST_RATE_HZ 50
```

## Protocol
See `docs/uart_protocol.md` for the UART framing spec.

## Next Steps
- ESP32 firmware to host a WebSocket dashboard
- Basic browser UI for live frame display
- STM32H7 hardware plan

## PlatformIO (ESP32)
If you prefer automation, PlatformIO can build and upload the ESP32 firmware and SPIFFS assets.
See `platformio.ini` and `tools/upload_spiffs.sh`.
