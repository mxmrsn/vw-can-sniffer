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
- `TJA1051` (good availability and 3.3V MCU I/O support via VIO pin)

## TJA1051 Wiring (Typical)
- `VCC` → 5V supply
- `VIO` → 3.3V (Teensy logic)
- `GND` → Ground
- `TXD` → Teensy CAN TX (use the Teensy 4.1 CAN1 TX pin from the official pinout)
- `RXD` ← Teensy CAN RX (use the Teensy 4.1 CAN1 RX pin from the official pinout)
- `CANH` / `CANL` → CAN bus

Bench bus termination:
- 120Ω at each end of the CAN bus (not at every node)
