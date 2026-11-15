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
| `monitor [--seconds N] [--out FILE]` | Capture raw CAN frames | no |
| `scan` | Find which control modules answer | yes |
| `info [module ...]` | VIN, part number, software and hardware versions, system name | yes |
| `dtc [module ...] [--mask HEX]` | Read and decode fault codes | yes |
| `pids` | List the OBD live values the engine supports | yes |
| `live [value ...] [--interval S] [--count N] [--csv FILE]` | Poll live values, optionally logging to CSV | yes |
| `clear module ... --yes` | Erase fault codes. Changes the car | yes |
| `raw module HEX ...` | Send any UDS request. Can change the car | yes |

The board powers up listen-only. Commands that transmit refuse to run without `--transmit`.

## What is implemented
- **UDS** (`uds.py`): requests through the firmware's `x` command, negative responses, timeouts.
- **OBD-II live data** (`obd.py`): 15 standard service-01 values with their standard formulas, supported-PID
  discovery, VIN.
- **VW specifics** (`vw.py`): control module addresses and the identification DIDs.
- **Fault codes** (`dtc.py`): SAE code and status-bit decoding.

## What is verified and what is not
Everything is tested end to end against the simulator (`python3 -m unittest discover -s tools/tests`), which runs
the real firmware core against invented control modules. That proves the tools, the console protocol and the
transport work together. It does not prove anything about the car:

- The control module addresses other than engine and transmission are commonly published VW values, unconfirmed.
- The identification DIDs are commonly published VW values, unconfirmed.
- VW-specific measuring values (beyond standard OBD) and VW's own fault-code numbers are not implemented.
- Coding, adaptations and security access are not implemented.

`docs/bringup_plan.md` lists the on-car tests that settle the unconfirmed items.
