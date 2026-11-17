# OBD dongle firmware (v0.4.0)

The same diagnostic interface as the XIAO board, for the dongle (`../pcb/`): ESP32-S3-MINI-1 on the top board,
TJA1043T and the switched supply on the bottom board. Everything above the platform is shared: the application
core, protocol, transport, logger and the **power manager** come from `../../pico2w/firmware/src/`, and the
ESP32-S3 drivers (microSD, Bluetooth, the LED chain) from `../../esp32s3-common/`. The repository holds one copy of
each; only the files here are the dongle's own.

| Path | What it is |
|---|---|
| `main/main.c` | Entry point and platform hooks (clock, console, LEDs) |
| `main/can_port_tja1043.c`, `main/tja1043.h` | can_port.h on the TWAI controller and the TJA1043T's modes |
| `main/power_esp32.c` | Battery voltage, USB host detection, why it woke, and switching itself off |
| `main/gnss_esp32.c` | GPS / sync connector: the receiver's UART, the pulse interrupt, the sync output timer |
| `main/pins.h` | Pin map of the top board |

## What is different from the XIAO board
- **Transceiver modes, not a silent pin.** Listen-only is the TJA1043's own Listen-only mode (STB_N high, EN low)
  plus the controller in listen-only: neither can drive the bus. `silent off` goes to Normal mode only once the
  controller is transmit-capable. Before the firmware runs, the pins' pull-downs hold the transceiver in Standby,
  which cannot transmit either.
- **It switches itself off.** The core is built with `APP_POWER`: the power manager (`src/power.c`, host-tested)
  decides, and `plat_power_sleep()` puts the transceiver into Go-to-sleep. Its INH output then floats, the buck's
  enable is pulled low, and the top board loses power. What holds it awake:

  | Reason | Meaning |
  |---|---|
  | usb | A computer on the USB port (never sleeps then) |
  | phone | A phone connected over Bluetooth |
  | activity | A request from a person (console or app) within the idle time (default 5 minutes); waking counts as one |
  | engine | Battery above 13.2 V: the engine is running |
  | bus | CAN frames within the idle time |
  | logging | Recording while there is something to record (engine running or bus traffic) |
  | stay | The stay-awake setting |

  The dongle's own poll-list requests do not count, or it would never sleep. On this car the gateway probably
  keeps the OBD port silent, so a quiet bus alone cannot mean "parked": hence the other reasons.
  **Battery protection** overrides all of them except USB: below 12.0 V with the engine off for a minute, it
  sleeps. Before sleeping it stops the poll list, closes the log properly and goes listen-only.
- **Why it woke** is read from the transceiver before CAN starts: the Wake flag in Standby, the Pwon flag in
  Listen-only (the battery was just connected), then the wake-up source flag in Normal mode with TXD held
  recessive (local = the WAKE pin: engine-running detector or button, told apart by the battery voltage;
  otherwise the bus). Running from USB with no battery reads as `usb`.
- **The button** reads low when pressed (internal pull-up through a diode), and also wakes a sleeping dongle.
  The engine-running detector pulls the same line, so with the engine running the button cannot be read: press
  it with the engine off to open Bluetooth pairing.
- **If the power does not go** after Go-to-sleep (USB is powering the top board, or a wake request arrived during
  the transition, which makes the transceiver refuse Sleep), it deep-sleeps for a minute and starts again.

## GPS and camera sync
The core is also built with `APP_GNSS`: `src/gnss.c` (host-tested) reads an external GPS receiver on the GPS /
sync connector (J3: UART on GPIO 17/18, pulse-per-second on 34, sync output on 35) and keeps the dongle's clock
lined up with GPS time.
- **Receiver:** NMEA RMC (and GGA for satellites, HDOP and altitude) at 9600 baud by default; `gps baud <n>` to
  change it (put it in `AUTORUN.TXT` to keep it). A u-blox M8 or M10 module with its pulse output wired to J3 is
  the expected part; set its rate (10 Hz for drive replay) once with the maker's tool.
- **Pulse:** a GPIO interrupt stamps each rising edge with `esp_timer_get_time()`, the clock every log stamp comes
  from. The firmware pairs each pulse with the time sentence that follows it, so each is a (local microseconds,
  UTC second) point; from one to the next it measures the crystal's drift. The first paired pulse sets the log's
  wall-clock time.
- **Sync output:** a general-purpose timer at 1 MHz toggles GPIO 35 at 1 to 100 Hz (`sync <hz>`, `sync off`).
  Each GPS pulse restarts it with a rising edge, so the edges fall on the GPS second and equal divisions of it.
  Every rising edge is logged with its number and microsecond time, for a camera or IMU rig to line up with.
- **Log records** (format version 2): pulse, fix, and sync edge; `vwdiag logread --gpx` and `--sync-csv` export
  them, and `logread` converts every stamp to UTC through the pulses (`../../pico2w/docs/logging.md`, "GPS time").

## Console
The XIAO board's commands, plus:

| Command | What it does |
|---|---|
| `power` | Why it woke, the battery voltage, what keeps it awake, when it would sleep, the settings |
| `power stay on\|off` | Never sleep for idleness (battery protection still applies) |
| `power idle <s>` | Sleep after this long with nothing holding it (30 to 86400 s) |
| `power cutoff <mV>` | Battery cutoff with the engine off (10500 to 13000 mV) |
| `power sleep` | Sleep now (not while a computer is on USB) |
| `gps` | Receiver, fix, position, pulse lock, clock drift against GPS, counters |
| `gps baud <n>` | The receiver's serial speed (4800 to 921600; default 9600) |
| `sync off\|<hz>` | The camera sync output, 1 to 100 Hz, restarted by each GPS pulse |

The packet protocol has `gps` and `sync` as command 0x11, and the power commands as command 0x10 (`../../pico2w/docs/link_protocol.md`); `vwdiag power` and the
app's Interface tab use it. The settings (idle time, cutoff, stay awake) are saved in flash (NVS) whenever they
change and loaded at every wake; values outside the allowed ranges are ignored, so a damaged record falls back to
the defaults.

## Build
```bash
. ~/esp/esp-idf/export.sh
cd hardware/esp32s3-dongle/firmware
idf.py set-target esp32s3 && idf.py build && idf.py -p /dev/cu.usbmodem<N> flash
```

## Status
Builds with ESP-IDF v5.5, no warnings in project code. **Never run on hardware: the boards do not exist yet.**
The power manager's decisions are covered by host tests (`tests/test_power.c`), the simulator sleeps the same way
(the process ends, so a connected app sees the link drop: try `VWDIAG_USB=0` and `power idle 30`), and the tools
and app are tested against that. Untested in every sense: the ADC reading of the battery, the ERR_N flag
sequence, Go-to-sleep actually removing power, USB host detection, and the current drawn asleep (the
architecture's budget is 46 µA typical). The GPS parsing, pulse pairing and drift are host-tested (`tests/test_gnss.c`)
and run against a simulated receiver (`VWDIAG_GPS=1`); the UART, the pulse interrupt's latency and the sync
timer are not.

## To check first on hardware
- `vwdiag selftest`, then on a bench supply with nothing else on the bus and a 120 Ω across CAN-H and CAN-L,
  `vwdiag --transmit selftest --active --force`: every line `pass` (or `skip` for a GPS not fitted), with the
  probe points for anything that fails.
- With 12 V on the bottom board and nothing else: `power` reads the battery within 0.2 V of a meter (the reading
  is the node after the input diode, about 0.3 V under the OBD pin).
- Wake reasons: plug in (power-on), button with the engine off (button), engine start (engine), bus traffic from
  the bench car (can).
- `power sleep` on 12 V only: the top board's 3.3 V falls to 0 within a few seconds, and the draw from 12 V falls
  under 100 µA.
- The current asleep, against the 46 µA typical and 73 µA worst-case budget in `../docs/architecture.md`.
- GPS: with a receiver on J3 and a sky view, `gps` shows a fix and "pulse locked" within a minute of a cold start,
  and a clock drift within ±30 ppm (the module's crystal).
- Sync: `sync 10` with a scope on J3 pin 6 and the receiver's pulse: a rising edge every 100 ms, one of them on
  each pulse edge, a few microseconds behind it.
