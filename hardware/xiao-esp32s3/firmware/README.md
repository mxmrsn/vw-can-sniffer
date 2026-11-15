# XIAO ESP32-S3 diagnostic firmware

The same diagnostic interface as the Pico 2 W firmware, on the Seeed XIAO ESP32-S3 board (PCB v0.3.0,
`../pcb/`, pin map in `../docs/design_notes.md`). It talks to VW control modules over the OBD port using UDS
(ISO 14229) on ISO-TP (ISO 15765-2), driven from a serial console on the module's USB-C port, from Bluetooth
LE, or on its own from a script on the microSD card.

This is a port, not a rewrite. The application core, transport, diagnostic, logging, packet-protocol and
Bluetooth buffer layers are the files in `../../pico2w/firmware/src/`, compiled from where they live: the
repository holds exactly one copy of them and both boards behave identically. Only the platform layer is new.

Paths in this file are relative to this folder (`hardware/xiao-esp32s3/firmware/`) unless noted.

## Layout
| Path | What it is |
|---|---|
| `main/main.c` | Entry point and platform.h hooks: clock, console, LEDs |
| `main/can_port_twai.c` | can_port.h on the ESP32-S3's TWAI controller and a TJA1051T/3 |
| `main/storage_esp32.c` | storage.h on a microSD card over SPI (sdmmc + FATFS) |
| `main/ble_esp32.c` | Nordic UART Service on NimBLE, with the Pico board's pairing rules |
| `main/pins.h` | Pin map for this board, from `../docs/design_notes.md` |
| `sdkconfig.defaults` | Console on USB Serial/JTAG, NimBLE, 1 kHz tick, 8 MB flash |
| `../../pico2w/firmware/src/*.c` | Everything else, shared and unchanged; see that board's README |

## What is different from the Pico board
- **CAN is hardware.** TWAI replaces can2040 in PIO, so CAN no longer competes with anything for timing.
  TWAI fixes its mode when the driver is installed, so `silent on` / `silent off` reinstalls the driver;
  that costs a few milliseconds and clears the controller's queues. The transceiver's S pin is driven in
  the safe direction before and after, so there is no moment where the board could drive the bus
  unintentionally. `can_port_init()` only accepts 500 kbit/s, which is all this project uses.
- **Bluetooth is NimBLE**, on the main chip rather than a separate radio. Same service, same security:
  bonded, LE Secure Connections only (legacy pairing is compiled out), MITM protection with a fixed
  six-digit passkey. The passkey is derived from the chip's eFuse MAC instead of the Pico's flash ID, so it
  is a different number on every board; `ble` prints it. New phones may pair for two minutes after power-up,
  after pressing the MODE button, or after `ble pair`; bonded phones reconnect at any time. Bonds live in
  NVS and `ble forget` clears them.
- **Two LEDs instead of four.** Only 11 pads are usable, so the green LED is CAN activity as before, the blue
  one lights while logging *or* while a phone is connected, and the module's own LED is the listen-only
  indicator. There is no USB LED.
- **One button instead of two.** MODE is on D2, wired exactly as on v0.2.0: to +3V3 with a 10k pull-down on the
  board, so pressed reads high. It only opens the Bluetooth pairing window.
- `PROTO_CMD_HELLO` still answers `vw-diag pico2w 0.2.0`: the name is in the shared `proto.h` and changing it
  would change the Pico board's protocol too. Tools do not key off it.

Everything else is the same code, so the console commands, the text and machine modes, the binary protocol
(`../../pico2w/docs/link_protocol.md`), the log format (`../../pico2w/docs/logging.md`) and `AUTORUN.TXT` all
behave exactly as the Pico board's README describes.

## Build
Needs ESP-IDF v5.x (built and checked against v5.5) with the esp32s3 toolchain installed.

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

Flashing needs no button press: the ESP32-S3's USB-C port is the chip's own USB, and `idf.py` resets it into
the bootloader.

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

If a previous sketch has left the module unable to enumerate, hold BOOT, tap RESET, release BOOT, and flash
again.

## Console
Open the module's USB serial port (any baud rate) and type `help`. The command list is the Pico board's; see
`../../pico2w/firmware/README.md`. `AUTORUN.TXT` on the card still runs at power-up.

`idf.py monitor` works, but it is a terminal of its own: for the binary protocol and for the Python tools in
`tools/vwdiag` (repo root), open the port directly instead.

## Status
**Nothing in this folder has ever run on hardware.** The board does not exist yet. What is actually true:

- It compiles cleanly for esp32s3 with ESP-IDF v5.5, warnings on (`-Wall -Wextra`), no warnings.
- The shared code it compiles is the code the host tests and the Python suite cover (1034 host checks and 105
  end-to-end tests on the Pico side), so the layers above the platform are as tested as they were there.
- The platform layer itself (TWAI, SD over SPI, NimBLE, the USB console) is **untested in every sense**: no
  bus, no card, no phone, no serial port. None of it has been on a bench, let alone a car.
- `../../pico2w/docs/bringup_plan.md` is the ordered test plan; most of it applies here unchanged.

## Things to check first when hardware arrives
- The console is the USB Serial/JTAG peripheral, and `plat_write()` bypasses the console VFS so the binary
  protocol is not mangled by newline translation. Confirm with `mode b` that no `\r` appears.
- TWAI bit timing comes from `TWAI_TIMING_CONFIG_500KBITS()` against the default 80 MHz APB clock. Check the
  bit rate with a scope before trusting a sample point on a car.
- Reinstalling the TWAI driver on `silent off` is the one piece of behaviour with no counterpart on the Pico.
  Check that a request sent immediately afterwards is not lost.
- GPIO43 and GPIO44 are UART0 by default, and on this board they carry the microSD chip select and clock. The
  ROM boot log therefore wiggles both before `app_main()` takes them over. The card is only mounted afterwards,
  so this should be harmless, but it has not been seen on hardware.
- The microSD card is mounted on demand, at `/sd`. A card inserted after boot should work; a card pulled
  while logging should stop the log rather than hang.

## Safety on the car
Unchanged: the board powers up listen-only, the transceiver's S pin is pulled high on the PCB so the bus is
safe even before the firmware runs, and nothing transmits until you type `silent off`. `raw` sends whatever
you give it, including services that write or reprogram a module.
