# Bench car (XIAO ESP32-S3 board)

A second XIAO board flashed with this firmware answers like the car: the simulator's control modules, engine model
and broadcast traffic, on a real CAN bus. Wire it to the interface board and the interface, the tools and the app
can be tested on the desk, with real bit timing, a real transceiver and real termination, before the car.

It is the host simulator's own car (`../../pico2w/firmware/sim/car.c`, `sim_engine.c`, `sim_config.c`), compiled
for the ESP32-S3: what the simulator does, the bench car does on the wire. Only the fault layer stays behind (missed
frames and the like happen on a real bus by themselves; use `scenario lossy` for slower answers and errors in the
modules).

## Wiring
| Interface board J1 | Bench car J1 |
|---|---|
| pin 3 CAN-H | pin 3 CAN-H |
| pin 2 CAN-L | pin 2 CAN-L |
| pin 4 GND | pin 4 GND |

Bridge JP1 (120 Ω) on **both** boards for the bench; with power off the bus then measures 60 Ω between CAN-H and
CAN-L. Power both from USB: the transceivers run from VSYS, which USB supplies. **Open JP1 again on the board that
goes into the car**: the car terminates its own bus.

## Build and flash
```bash
. ~/esp/esp-idf/export.sh
cd hardware/xiao-esp32s3/bench-car
idf.py set-target esp32s3 && idf.py build && idf.py -p /dev/cu.usbmodem<N> flash
```

## Console
Open the bench car's USB serial port (any baud rate).

| Command | What it does |
|---|---|
| `status` | Scenario, ignition, engine (rpm, speed, coolant, trims, misfires), frame counters, CAN controller state and error counters |
| `scenario <spec>` | Restart as another car, e.g. `gti`, `lossy`, `realistic,gateway=open,drive=1` (same settings as the simulator's `VWSIM`) |
| `ign off\|on\|crank\|run` | Hold the ignition in a state: off leaves only the gateway awake, crank makes the engine answer "busy" |
| `mon on\|off` | Print every frame received from the interface |
| `burst <n> [id]` | Send n filler frames back to back, for the interface's receive-overrun test |

It starts as `realistic,gateway=open` so the interface sees broadcast traffic straight away.

LEDs: status green while the engine runs, BLE LED on with the ignition on, log LED on while cranking, CAN LED with
traffic.

## What it is for
Stages 4 to 6 of `../docs/bringup_plan.md`: listen-only really not acknowledging (the bench car's transmit errors
rise while the interface listens), bit timing on a scope, the TWAI reinstall on `silent off`, scan, misfire, a full
`vwdiag session`, the app over Bluetooth, and an hour of logging.

## Status
Builds with ESP-IDF v5.5, no warnings. **Never run on hardware.** The car logic is the simulator's, which the host
tests cover; the TWAI glue and the console here are untested.
