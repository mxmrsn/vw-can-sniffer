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
