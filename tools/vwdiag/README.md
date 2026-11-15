# vwdiag: host tools for the diagnostic interface

Python tools that drive the Pico 2 W firmware (or its simulator) over the serial console. Standard library only;
Python 3.9 or newer.

```bash
cd tools
python3 -m vwdiag --sim --transmit scan                 # against the simulator
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
| `logread FILE [--csv OUT] [--candump OUT] [--decode] [--ids] [--frames]` | Read a log copied off the card; needs no board | no |
| `scan` | Find which control modules answer | yes |
| `info [module ...]` | VIN, part number, software and hardware versions, system name | yes |
| `dtc [module ...] [--mask HEX]` | Read and decode fault codes | yes |
| `pids` | List the OBD live values the engine supports | yes |
| `live [value ...] [--interval S] [--count N] [--csv FILE]` | Poll live values, optionally logging to CSV | yes |
| `clear module ... --yes` | Erase fault codes. Changes the car | yes |
| `raw module HEX ...` | Send any UDS request. Can change the car | yes |

The board powers up listen-only. Commands that transmit refuse to run without `--transmit`.

## What is implemented
- **Two ways to talk to the interface**: the text console (`link.py`, the default) and the binary packet
  protocol an app will use (`binlink.py`, pass `--binary`), described in `hardware/pico2w/docs/link_protocol.md`.
- **UDS** (`uds.py`): requests over either link, negative responses, timeouts.
- **OBD-II live data** (`obd.py`): 15 standard service-01 values with their standard formulas, supported-PID
  discovery, VIN.
- **VW specifics** (`vw.py`): control module addresses and the identification DIDs.
- **Fault codes** (`dtc.py`): SAE code and status-bit decoding.
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
- VW-specific measuring values (beyond standard OBD) and VW's own fault-code numbers are not implemented.
- Coding, adaptations and security access are not implemented.

`hardware/pico2w/docs/bringup_plan.md` lists the on-car tests that settle the unconfirmed items.
