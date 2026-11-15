# Pico 2 W diagnostic firmware

Bench firmware for PCB v0.2.0 (`pcb-pico2w/`). It talks to VW control modules over the OBD port using UDS
(ISO 14229) on ISO-TP (ISO 15765-2), driven from a serial console on the Pico's USB port.

This is the first step toward the iOS-driven interface: Bluetooth, logging and sleep are not here yet.

## Status
- `src/isotp.c`, `src/uds.c`: transport and diagnostic layers. Covered by host tests against a simulated ECU
  (`make -C tests`, 54 checks, built with address and undefined-behaviour sanitizers).
- `src/can_port.c`, `src/main.c`: can2040 glue and the console. They build cleanly for the Pico 2 W
  (Pico SDK 2.2.0, Arm GNU toolchain 13.3) but have **not been run on hardware**.
- Nothing has been tried on a car.

## Build
Needs the Pico SDK (2.1 or newer, with the `lib/tinyusb` submodule) and the Arm GNU toolchain.

```bash
export PICO_SDK_PATH=~/pico/pico-sdk
export PICO_TOOLCHAIN_PATH=~/pico/arm-gnu-toolchain-13.3.rel1-darwin-arm64-arm-none-eabi
cmake -B build -G "Unix Makefiles"
cmake --build build
```

The first configure also downloads and builds `picotool`.

Copy `build/vw_diag.uf2` to the Pico in BOOTSEL mode. CMake downloads can2040 v1.7.0 during configure.

## Console
Open the Pico's USB serial port (any baud rate) and type `help`.

| Command | What it does |
|---|---|
| `status` | Bus mode, frame counters, current target |
| `silent on` / `silent off` | Listen-only (default at power-up) or allow transmitting |
| `mon on` / `mon off` | Print every received CAN frame |
| `ecus`, `ecu <name>`, `ecu <req> <resp>` | List or select the target control module |
| `tp` | TesterPresent: quickest check that a module answers |
| `session <hex>` | DiagnosticSessionControl (`01` default, `03` extended) |
| `vin` | Read the VIN (DID F190) |
| `did <hex>` | ReadDataByIdentifier |
| `dtc [mask]` | Read fault codes as raw 3-byte codes with status |
| `raw <hex bytes>` | Send any UDS request and print the response |

## Safety on the car
- The board powers up listen-only and refuses to transmit until you type `silent off`.
- The named commands only read. `raw` sends whatever you type, including services that write, reset or
  reprogram a module. Know what a request does before sending it.
- Leave JP1 (termination) open on the car.

## Things to verify on the car
- The control-module addresses in `src/vw_ecus.h` other than engine and transmission are the commonly
  published VW values and are unconfirmed for this car.
- Frames are padded to 8 bytes with 0x55; change `pad_byte` in `isotp_init()` if a module ignores them.
- The UDS response timeout is 1 s, deliberately longer than the standard 50 ms to allow for the gateway.

## Licence note
can2040 is GPL-3.0. Firmware that links it must be distributed under GPL-3.0 as well. The ISO-TP and UDS
code here does not depend on can2040.
