# vwdiag: host tools for the diagnostic interface

Python tools that drive the Pico 2 W firmware (or its simulator) over the serial console. Standard library only;
Python 3.9 or newer.

```bash
cd tools
python3 -m vwdiag --sim --transmit scan                 # against the simulator
python3 -m vwdiag --sim-ble status                      # through its Bluetooth stand-in
python3 -m vwdiag --port /dev/cu.usbmodem1101 status    # against a board
```

With one board plugged in, `--port` can be omitted.

| Command | What it does | Transmits? |
|---|---|---|
| `status` | Interface state and frame counters | no |
| `monitor [--seconds N] [--out FILE] [--decode]` | Capture raw CAN frames, optionally decoded | no |
| `decode FILE [--summary]` | Decode a saved capture; needs no board | no |
| `log start [--poll VALUE ...] [--interval S]` | Start recording to the board's microSD card, optionally polling live values | only with `--poll` |
| `log stop`, `log status`, `log mark TEXT` | Stop, check, or add a note to the log | no |
| `power [--stay on\|off] [--idle S] [--cutoff MV] [--sleep]` | The dongle's power state: why it woke, the battery, what keeps it awake; set how and when it sleeps | no |
| `gps [--sync HZ\|off] [--watch S]` | The dongle's GPS receiver: fix, position, pulse lock, how far its clock drifts from GPS; set the camera sync output | no |
| `experiment NAME [--seconds S] [--yes]` | A scripted logging experiment: listen, identify, timing, cold-start, warm-idle, rev-holds, drive (`docs/logging_experiments.md`) | after listen |
| `calibrate FOLDER ... [--verify]` | Simulator settings that match the car, from experiment results; with --verify, the simulator rerun side by side | no |
| `files [NAME ... \| all] [--out DIR]` | List the card's files, or copy them here over USB or Bluetooth | no |
| `replay FILE [--out OUT]` | A log as one HTML page: each engine value and GPS speed on its own chart over a shared timeline, the GPS track, the marks; a cursor and time slider move through all of them. Needs no network (the track is drawn without map tiles) | no |
| `logread FILE [--csv OUT] [--candump OUT] [--gpx OUT] [--sync-csv OUT] [--decode] [--ids] [--frames]` | Read a log copied off the card; needs no board | no |
| `scan` | Find which control modules answer | yes |
| `info [module ...]` | VIN, part number, software and hardware versions, system name | yes |
| `dtc [module ...] [--mask HEX]` | Read and decode fault codes | yes |
| `pids` | List the OBD live values the engine supports | yes |
| `live [value ...] [--interval S] [--count N] [--csv FILE]` | Poll live values, optionally logging to CSV | yes |
| `clear module ... --yes` | Erase fault codes. Changes the car | yes |
| `raw module HEX ...` | Send any UDS request. Can change the car | yes |
| `misfire [--watch S] [--csv FILE]` | Misfire counts per cylinder (service 06), trims, freeze frame, emission codes | yes |
| `measure [value ...] [--list] [--count N]` | Engine measuring values from the Simos 18 list (knock retard per cylinder, misfires, cams, rail pressure...) | yes |
| `dtc ... --details` | Also each code's snapshot (freeze frame) and counters | yes |
| `session [--listen S] [--idle S] [--drive S] [--sweep] [--yes]` | The scripted first car session; everything saved to a folder | yes, after listening |

`--sim-profile SPEC` picks the simulator's scenario, e.g. `--sim --sim-profile gti` for this car's misfire
(see `hardware/pico2w/firmware/README.md`).

The board powers up listen-only. Commands that transmit refuse to run without `--transmit`.

## What is implemented
- **Three ways to talk to the interface**: the text console (`link.py`, the default), the binary packet protocol
  an app uses (`binlink.py`, pass `--binary`), and that same protocol over the simulator's Bluetooth stand-in
  (`BleSimLink`, pass `--sim-ble`). All described in `hardware/pico2w/docs/link_protocol.md`.
- **UDS** (`uds.py`): requests over either link, negative responses, timeouts.
- **OBD-II** (`obd.py`): 20 standard service-01 values (now with fuel trims and lambda), supported-PID
  discovery, VIN, warning lamp and code count, stored and pending emission codes (services 03 and 07), freeze
  frame (service 02), and the misfire monitor per cylinder (service 06, MIDs A1 to A5).
- **Engine measuring values** (`simos18.py`): 48 service-0x22 values for the Simos 18 engine module, from the
  list the open-source VW_Flash logger uses (`data/simos18_parameters_22.csv`, BSD licence).
- **The first car session** (`session.py`): listen-only capture with ignition off and on, scan and
  identification, fault codes with snapshots, an idle capture of the misfire set, and a drive logged to the card.
  Requests are retried; a step that fails is noted and the session carries on.
- **VW specifics** (`vw.py`): control module addresses and the identification DIDs.
- **Fault codes** (`dtc.py`): SAE code and status-bit decoding, and a plain-language description of every code:
  about 470 generic codes from the dtcdb project (MIT licence), 37 more that matter on this car written from the
  SAE J2012 titles, and otherwise what the code's SAE group is for. VW's own codes (P1xxx, most B, C and U1/U2)
  say so: their meaning is in VW's data, which is not public.
- **Log files** (`logfile.py`): reader for the card's log format, with OBD live-value extraction and can-utils
  export. Format in `hardware/pico2w/docs/logging.md`.
- **Broadcast frames** (`dbc.py`): decoding with the public VW MQB message database (113 messages) from
  comma.ai's opendbc, in `data/vw_mqb.dbc`.

## What is verified and what is not
Everything is tested end to end against the simulator (`python3 -m unittest discover -s tools/tests`), which runs
the real firmware core against invented control modules. That proves the tools, the console protocol and the
transport work together. It does not prove anything about the car:

- Addresses for airbag, steering, radar and camera, and the part-number, software-version and coding identifiers,
  match what opendbc uses on Golf MK7 cars. ABS, cluster and gateway addresses and the other identifiers are
  unconfirmed. `docs/vw_can_data_sources.md` has the full breakdown.
- The broadcast frames in the database may not be visible at the OBD port at all.
- The Simos 18 measuring values come from VW_Flash's list, used on Simos 18 cars by that project; whether this
  car's software answers each of them in the default session is unconfirmed. VW's own fault-code numbers are not
  implemented.
- Coding, adaptations and security access are not implemented.

`hardware/pico2w/docs/bringup_plan.md` lists the on-car tests that settle the unconfirmed items.
