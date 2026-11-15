# Pico 2 W diagnostic firmware

Bench firmware for PCB v0.2.0 (`../pcb/`). It talks to VW control modules over the OBD port using UDS
(ISO 14229) on ISO-TP (ISO 15765-2), driven from a serial console on the Pico's USB port.

This is the first step toward the iOS-driven interface: Bluetooth, logging and sleep are not here yet.

Paths in this file are relative to this folder (`hardware/pico2w/firmware/`) unless noted.

## Layout
| Path | What it is |
|---|---|
| `src/isotp.c`, `src/uds.c` | Transport and diagnostic layers, platform independent |
| `src/app.c` | Console and commands, platform independent |
| `src/logger.c` | Logging to a file, platform independent; see `../docs/logging.md` |
| `src/storage_pico.c`, `sim/storage_host.c` | microSD card on the board; a folder in the simulator |
| `src/frame.c`, `src/proto.c` | Binary packet protocol for apps (USB now, BLE later); see `../docs/link_protocol.md` |
| `src/can_port.c`, `src/main.c` | Pico-only: can2040 glue and platform hooks |
| `sim/` | Host build of the same core against simulated VW control modules |
| `tests/` | Host tests for the transport, diagnostic, packet framing and logging layers |

## Status
- Transport and diagnostic layers: 54 host checks against a simulated ECU (`make -C tests`), sanitizers on.
- Packet framing: 73 host checks, logger: 21 host checks with a fake card, in the same run.
- Console, commands and both tool protocols (text and binary): tested end to end through the simulator by the
  Python suite in `tools/tests` (repo root), 88 tests, including logging to a folder that stands in for the
  card. The binary protocol is also run with the byte stream
  delivered 7 bytes at a time, as a BLE connection would.
- All of the above and the Pico build run on every push (`.github/workflows/ci.yml`).
- Pico build: compiles cleanly (Pico SDK 2.2.0, Arm GNU toolchain 13.3). **Not yet run on hardware.**
- Nothing has been tried on a car. `../docs/bringup_plan.md` is the ordered test plan for when hardware arrives.

## Simulator
```bash
make -C sim
./sim/vw_diag_sim        # same console as the board, on stdin/stdout
```
Seven simulated control modules (engine, transmission, ABS, cluster, airbag, gateway, steering; radar and camera
are deliberately missing) answer identification, fault-code and OBD live-data requests, including multi-frame and
"response pending" replies, and the bus carries a few broadcast frames laid out as in the public VW MQB database.
Part numbers and software versions are real Golf MK7 values from opendbc; the rest is made up for testing.

## Build
Needs the Pico SDK (2.1 or newer, with the `lib/tinyusb` submodule) and the Arm GNU toolchain.

```bash
export PICO_SDK_PATH=~/pico/pico-sdk
export PICO_TOOLCHAIN_PATH=~/pico/arm-gnu-toolchain-13.3.rel1-darwin-arm64-arm-none-eabi
cmake -B build -G "Unix Makefiles"
cmake --build build
```

The first configure also downloads and builds `picotool`.

Copy `build/vw_diag.uf2` to the Pico in BOOTSEL mode. CMake downloads can2040 v1.7.0 (GPL-3.0) and the
microSD driver no-OS-FatFS-SD-SDIO-SPI-RPi-Pico v3.6.2 (Apache-2.0) during configure.

## Console
At power-up the board first runs any commands in `AUTORUN.TXT` on the microSD card (see `../docs/logging.md`).

Open the Pico's USB serial port (any baud rate) and type `help`.

| Command | What it does |
|---|---|
| `status` | Bus mode, frame counters, current target |
| `silent on` / `silent off` | Listen-only (default at power-up) or allow transmitting |
| `mon on` / `mon off` | Print every received CAN frame |
| `ecus`, `ecu <name>`, `ecu <req> <resp>` | List or select the target control module |
| `scan` | TesterPresent to every known module |
| `tp` | TesterPresent: quickest check that a module answers |
| `keepalive on` / `off` | Send TesterPresent every 2 s to hold a diagnostic session |
| `timeout <ms>` | Response timeout |
| `session <hex>` | DiagnosticSessionControl (`01` default, `03` extended) |
| `vin` | Read the VIN (DID F190) |
| `did <hex>` | ReadDataByIdentifier |
| `dtc [mask]` | Read fault codes as raw 3-byte codes with status |
| `raw <hex bytes>` | Send any UDS request and print the response |
| `x <req> <resp> <hex bytes>` | One request to any address; replies `+ <hex>` or `- <reason>` |
| `log on` / `log off` / `log` | Record every CAN frame to the microSD card; `log` alone shows the state |
| `log mark <text>` | Put a note in the log |
| `time <unix seconds>` | Tell the board the time, for log timestamps |
| `poll <req> <resp> <ms> <hex>` | Repeat a short request on a timer (up to 8); `poll list`, `poll clear` |
| `mode m` / `mode h` | Machine mode for programs (no echo, each reply ends with `$`), or human mode |
| `mode b` | Binary packet protocol for apps; see `../docs/link_protocol.md` |

The Python tools in `tools/vwdiag` (repo root) use machine mode and the `x` command, or the binary protocol
with `--binary`.

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
