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
| `src/gnss.c`, `sim/gps_host.c` | GPS input and time sync for the dongle: NMEA, pulse pairing, clock drift; a simulated receiver |
| `src/storage_pico.c`, `sim/storage_host.c` | microSD card on the board; a folder in the simulator |
| `src/ble_link.c`, `src/ble_pico.c` | Bluetooth LE: buffers, and BTstack on the CYW43439 |
| `src/vw_diag.gatt`, `src/btstack_config.h` | Bluetooth service definition and stack configuration |
| `src/frame.c`, `src/proto.c` | Binary packet protocol for apps (USB now, BLE later); see `../docs/link_protocol.md` |
| `src/can_port.c`, `src/main.c` | Pico-only: can2040 glue and platform hooks |
| `sim/` | Host build of the same core against simulated VW control modules |
| `tests/` | Host tests for the transport, diagnostic, packet framing and logging layers |

## Status
- Transport and diagnostic layers: 59 host checks against a simulated ECU (`make -C tests`), sanitizers on.
- Packet framing: 73 host checks, logger: 21, Bluetooth buffers: 886, in the same run.
- Console, commands and both tool protocols (text and binary): tested end to end through the simulator by the
  Python suite in `tools/tests` (repo root), 105 tests. Everything runs over the text console, the binary
  protocol and a TCP stand-in for Bluetooth that imitates an iPhone's packet size and speed.
- Bluetooth LE: compiles and is covered by the stand-in, but **has never run on a radio or talked to a phone.** The binary protocol is also run with the byte stream
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
are deliberately missing) answer identification, fault-code, OBD and engine measuring-value requests. Part numbers
and software versions are real Golf MK7 values from opendbc; the engine code is this car's (DKFA).

**It is pessimistic by default**, in the ways a real car is: modules take 5 to 50 ms to answer and pace long
requests (block size 8, 5 ms apart); an extended session drops back after 5 s without a request; the engine and
the transmission both answer a functional (7DF) OBD request; the gateway keeps broadcast traffic off the OBD port;
with the ignition off only the gateway answers, and while cranking the engine says "busy, ask again".

Live values come from a small engine model (`sim/sim_engine.c`): rpm, load, airflow, manifold pressure, timing,
fuel trims, lambda and temperatures move together along a throttle trace, a cold start idles fast with retarded
spark, and a cylinder with low compression misfires most at idle and when cold, pushing the trims rich and setting
P030x with a freeze frame. The misfire shows up in OBD service 06 (per-cylinder counts), service 02 (freeze frame),
services 03/07 (codes), UDS 19 01/02/04/06 (count, codes, snapshot, counters) and the Simos 18 measuring values.

The scenario comes from the `VWSIM` environment variable (all settings in `sim/sim_config.c`):

| `VWSIM=` | Scenario |
|---|---|
| (unset), `realistic` | As above, engine warm and idling |
| `ideal` | Instant answers, broadcast visible, sessions never expire: the old behaviour |
| `gti` | Realistic, plus this car: cylinder 3 low on compression, P0303 stored from earlier drives, cold start |
| `lossy` | Realistic, plus frames the interface misses, error frames and arbitration delays |
| e.g. `realistic,gateway=open,drive=1,ignition=off:0/on:2000/crank:5000/run:5600,busoff=8000:500` | Any mix of settings |

The engine's numbers are settings too (`idle`, `cold_idle`, `warmup`, misfire rates, trims, and `speed` for a
faster engine clock); `vwdiag calibrate` sets them from experiments on the car (`docs/logging_experiments.md`).

The simulator is built with the dongle's power manager and sleeps like it: with `VWDIAG_USB=0` (no computer on
USB) it ends the process when nothing keeps it awake, which an app sees as the link dropping. The battery voltage
is the engine model's; `VWDIAG_WAKE=can|engine|button|power-on` sets the wake reason it reports, and
`VWDIAG_SETTINGS=<file>` keeps the power settings across runs, as the dongle keeps them in flash.

It also has the dongle's GPS / sync connector. `VWDIAG_GPS=1` connects a simulated receiver: NMEA at 10 Hz, a
pulse every GPS second, the car driving east from a fixed point at the engine model's road speed. Its time runs
against the computer's clock with a known error, `VWDIAG_GPS_DRIFT` ppm (default 25), which `gps` should report
as the clock drift. The sync output (`sync <hz>`) works with or without it.

The car itself (`sim/car.c`, `sim_engine.c`, `sim_config.c`) does not depend on the host: `sim/sim_bus.c` puts it
on a pretend bus with fault injection, and the XIAO bench car (`hardware/xiao-esp32s3/bench-car`) puts the same
code on a real CAN bus.

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
microSD driver no-OS-FatFS-SD-SDIO-SPI-RPi-Pico v3.6.2 (Apache-2.0) during configure. Bluetooth needs the
`btstack` and `cyw43-driver` submodules of the Pico SDK:

```bash
git -C $PICO_SDK_PATH submodule update --init --depth 1 lib/btstack lib/cyw43-driver
```

The carrier takes any of the four official Pico boards; pass `-DPICO_BOARD=` to build for another:

| Board | Builds | Bluetooth |
|---|---|---|
| `pico2_w` (default) | yes | yes |
| `pico_w` | yes | yes |
| `pico2`, `pico` | yes | no: compiled out, `ble` reports "not available" |

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
| `log ls`, `log get <file> <offset> <length>` | List the card's files; read up to 1024 bytes of one, as hex |
| `power`, `power stay on\|off`, `power idle <s>`, `power cutoff <mV>`, `power sleep` | Power state and settings, on boards that switch themselves off (the dongle, and the simulator); others say so |
| `time <unix seconds>` | Tell the board the time, for log timestamps |
| `poll <req> <resp> <ms> <hex>` | Repeat a short request on a timer (up to 8); `poll list`, `poll clear` |
| `mode m` / `mode h` | Machine mode for programs (no echo, each reply ends with `$`), or human mode |
| `ble` | Bluetooth state, name and pairing passkey |
| `ble pair` / `ble forget` | Allow a new phone to pair for 2 minutes, or forget every paired phone |
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
