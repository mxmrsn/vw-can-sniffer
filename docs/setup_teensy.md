# Teensy Setup

This project uses the Arduino IDE + Teensyduino and the FlexCAN_T4 library.

## Install Tools
1. Install the Arduino IDE.
2. Install Teensyduino (the Teensy installer). It integrates with the Arduino IDE.

## Install FlexCAN_T4
1. Open Arduino IDE.
2. Go to `Sketch` → `Include Library` → `Manage Libraries…`
3. Search for `FlexCAN_T4` and install the latest version.

## Build/Upload
1. Open `firmware/teensy/teensy_uart_streamer.ino`.
2. Select `Tools` → `Board` → `Teensy 4.1`.
3. Select the Teensy serial port under `Tools` → `Port`.
4. Click Upload.

## Notes
- Default CAN speed in the sketch is `500000`.
- Default UART is `Serial1` at `921600`.
- Enable synthetic frames for bench testing by setting `TEST_MODE` to `1`.

## Recommended CAN Transceiver
- `TJA1051T/3` (good availability and 3.3V MCU I/O support via VIO pin)

## TJA1051 Wiring (Typical)
- `VCC` → 5V supply
- `VIO` → 3.3V (Teensy logic)
- `GND` → Ground
- `TXD` → Teensy CAN TX (see explicit pins below)
- `RXD` ← Teensy CAN RX (see explicit pins below)
- `CANH` / `CANL` → CAN bus
- `S` → Teensy `pin 21` (optional silent mode control)

Bench bus termination:
- 120Ω at each end of the CAN bus (not at every node)

## Teensy 4.1 CAN Pins (Explicit)
These are the default CAN pin pairs on Teensy 4.1:
- `CAN1` TX/RX: `CTX1 = pin 22`, `CRX1 = pin 23`
- `CAN2` TX/RX: `CTX2 = pin 1`, `CRX2 = pin 0`
- `CAN3` TX/RX: `CTX3 = pin 31`, `CRX3 = pin 30`

This project uses `CAN1` in the sketches by default, so wire the transceiver to pins 22/23.

## Wiring Table (Teensy 4.1 CAN1 ↔ TJA1051)
Use this as a quick reference for CAN1:

| TJA1051 Pin | Connects To |
| --- | --- |
| `TXD` | Teensy `CTX1` (pin 22) |
| `RXD` | Teensy `CRX1` (pin 23) |
| `VCC` | 5V |
| `VIO` | 3.3V |
| `GND` | GND |
| `CANH` | CANH |
| `CANL` | CANL |

## Optional ESP32 Reset (Teensy GPIO)
If you want the Teensy to reset the ESP32:
- Teensy `pin 4` → ESP32 `EN`
- Teensy `pin 5` → ESP32 `RST`
- Keep `EN` high for normal operation

Note: `EN` is active‑low; pulling it low resets the ESP32.
Note: `RST` is also active‑low; pulling it low reboots the ESP32 without cutting power.

## Optional TJA1051 Silent Mode
- Teensy `pin 21` → TJA1051 `S`
- `S = LOW` → normal mode
- `S = HIGH` → silent (listen‑only)
