# VW CAN Sniffer

Hardware-agnostic CAN bus sniffer focused on MK7.5 VW GTI, starting with Teensy 4.0 and a UART stream protocol suitable for an ESP32 Wi‑Fi dashboard.

## Scope
- Teensy 4.0 CAN capture
- UART framing protocol for streaming frames to a Wi‑Fi module
- Future STM32H7 custom board

## Current Contents
- `firmware/teensy/teensy_uart_streamer.ino`
- `firmware/teensy/teensy_can_simulator.ino`
- `docs/uart_protocol.md`
- `firmware/esp32/esp32_uart_gateway.ino`

## Hardware (Initial Setup)
- Teensy 4.0
- CAN transceiver (TJA1051T/3 recommended)
- ESP32 module (UART bridge for Wi‑Fi)

### Basic Wiring (Teensy ↔ ESP32)
- `Teensy TX` → `ESP32 RX`
- `Teensy RX` ← `ESP32 TX`
- `GND` ↔ `GND`
- Optional: `Teensy pin 4` → `ESP32 EN` (reset control)
- Optional: `Teensy pin 5` → `ESP32 RST` (soft reset)

### CAN Transceiver (Optional Control)
- Optional: `Teensy pin 20` → TJA1051 `S` (silent/listen‑only control)

### Output Mode + LEDs
- Mode button: `pin 8` (toggle UART/Wi‑Fi vs USB)
- Select button: `pin 9` (toggle CAN silent)
- LEDs: CAN heartbeat `pin 14`, Wi‑Fi active `pin 15`, USB active `pin 16`, silent indicator `pin 21`

## Firmware Notes
- Default UART: `Serial1` @ `921600`
- Default CAN speed: `500000`
- CAN IDs encode extended frames with bit31 set

## System Block Diagram
```
                 CANH/CANL
   ┌──────────────────────────────────────────┐
   │              Bench / Vehicle             │
   └──────────────────────────────────────────┘
                    │
                    │
              ┌──────────┐        UART (3.3V)
              │ TJA1051  │<-------------------------┐
              └──────────┘                          │
                    │ TXD/RXD                       │
                    │                               │
            ┌────────────────┐                      │
            │ Teensy 4.0     │                      │
            │ CAN1 (pins 22/23)                     │
            │ UART -> ESP32  │----------------------┘
            └────────────────┘
                       │ Wi-Fi
                       │
                 ┌──────────┐
                 │  ESP32   │
                 │ Web UI   │
                 └──────────┘
                       │
                       │ HTTP/WS
                       ▼
                 ┌──────────┐
                 │ Browser  │
                 └──────────┘
```

## Wiring Overview (ASCII)
```
Teensy 4.0                          TJA1051T/3                 CAN Bus
-----------                          ----------                 -------
CTX1 (pin 22) ---------------------> TXD
CRX1 (pin 23) <--------------------- RXD
pin 20 ----------------------------> S (silent)
3V3 -------------------------------> VIO
5V  -------------------------------> VCC
GND -------------------------------> GND ---------------------- GND
                                   CANH ---------------------- CANH
                                   CANL ---------------------- CANL

Teensy 4.0                          ESP32 (UART)               Wi-Fi
-----------                          -----------               -----
TX1 (pin 1) ----------------------> RX
RX1 (pin 0) <---------------------- TX
GND ------------------------------> GND                         ~~~

LEDs / Buttons (Teensy 4.0)
LED CAN (pin 14), LED Wi-Fi (pin 15), LED USB (pin 16), LED Silent (pin 21)
Mode button (pin 8), Select button (pin 9)
```

### Test Mode
Enable synthetic frames for bench testing:

```
#define TEST_MODE 1
#define TEST_RATE_HZ 50
```

## Protocol
See `docs/uart_protocol.md` for the UART framing spec.

## USB Capture
Use `tools/usb_dump.py` to read the framed stream from USB and emit CSV.
Requires `pyserial`.

## PCB Docs
See `pcb-teensy-esp32/teensy-esp32-can-sniffer/docs/README.md` for schematic, layout, and 3D captures.

## Next Steps
- ESP32 firmware to host a WebSocket dashboard
- Basic browser UI for live frame display
- STM32H7 hardware plan

## PlatformIO (ESP32)
If you prefer automation, PlatformIO can build and upload the ESP32 firmware and SPIFFS assets.
See `platformio.ini` and `tools/upload_spiffs.sh`.
