# VW CAN Sniffer

A home-built diagnostic interface for an MK7.5 VW GTI: hardware that plugs into the OBD-II port, firmware that
speaks the car's diagnostic protocol, and host tools for scanning modules, reading fault codes and logging live
data. The longer-term aim is an iOS app with custom dashboards talking to a low-profile dongle.

## Layout
| Path | What it is |
|---|---|
| [`hardware/pico2w/`](hardware/pico2w/README.md) | **Board v0.2.0 (tag `v0.2.0`):** Raspberry Pi Pico 2 W carrier, its firmware and bring-up plan |
| [`hardware/xiao-esp32s3/`](hardware/xiao-esp32s3/README.md) | **Current board (v0.3.0):** Seeed XIAO ESP32-S3 carrier, because the Pico 2 W is hard to buy |
| [`hardware/esp32s3-dongle/`](hardware/esp32s3-dongle/README.md) | **Next (v0.4.0), schematics drawn:** OBD dongle, two boards stacked behind a J1962 plug |
| [`hardware/teensy-esp32/`](hardware/teensy-esp32/README.md) | Original board (v0.1.0, tag `v0.1.0`): Teensy 4.0 + ESP32, its firmware and tools |
| [`tools/vwdiag/`](tools/vwdiag/README.md) | Host tools: scan, identification, fault codes, live data, logging, frame decoding |
| [`tools/tests/`](tools/tests) | End-to-end tests for the tools, run against the firmware simulator |
| [`docs/roadmap.md`](docs/roadmap.md) | **What comes next, in order**: hardware, simulator, iOS app, data, tuning |
| [`docs/`](docs/vw_can_data_sources.md) | Project-wide notes, including where the VW-specific data came from |

Each board folder holds everything specific to that board: `pcb/` (KiCad project, BOM, fab files), `firmware/`
and `docs/`.

## Status
- **v0.2.0 board (Pico 2 W):** designed, checked by ERC/DRC and ready to order. Not yet built.
- **v0.3.0 board (XIAO ESP32-S3):** routed and DRC clean, 68 x 38 mm. A few BOM lines still need a stock
  check before ordering.
- **Firmware:** CAN, ISO-TP, UDS, a USB console, a framed binary protocol over USB and Bluetooth LE, and logging
  to a microSD card. The application core is shared by both boards. Builds for the Pico 2 W and the XIAO
  ESP32-S3; **never run on hardware.**
- **Tools:** working and tested against a simulated car, over the text console, the binary protocol and a
  Bluetooth stand-in. Nothing has been tried on the real car.

## Quick start (no hardware needed)
```bash
make -C hardware/pico2w/firmware/sim
cd tools
python3 -m vwdiag --sim --transmit scan
```

## Tests
```bash
make -C hardware/pico2w/firmware/tests
python3 -m unittest discover -s tools/tests
```

Both run on every push, along with firmware builds for every supported board ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)).

## Other
- `tools/md_to_pdf.sh` regenerates the PDF copy that sits next to each Markdown write-up.
