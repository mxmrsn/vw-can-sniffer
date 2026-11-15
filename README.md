# VW CAN Sniffer

A home-built diagnostic interface for an MK7.5 VW GTI: hardware that plugs into the OBD-II port, firmware that
speaks the car's diagnostic protocol, and host tools for scanning modules, reading fault codes and logging live
data. The longer-term aim is an iOS app with custom dashboards talking to a low-profile dongle.

## Layout
| Path | What it is |
|---|---|
| [`hardware/pico2w/`](hardware/pico2w/README.md) | **Current board (v0.2.0):** Raspberry Pi Pico 2 W carrier, its firmware and bring-up plan |
| [`hardware/teensy-esp32/`](hardware/teensy-esp32/README.md) | Original board (v0.1.0, tag `v0.1.0`): Teensy 4.0 + ESP32, its firmware and tools |
| [`tools/vwdiag/`](tools/vwdiag/README.md) | Host tools: scan, identification, fault codes, live data, logging, frame decoding |
| [`tools/tests/`](tools/tests) | End-to-end tests for the tools, run against the firmware simulator |
| [`docs/`](docs/vw_can_data_sources.md) | Project-wide notes; currently where the VW-specific data came from |

Each board folder holds everything specific to that board: `pcb/` (KiCad project, BOM, fab files), `firmware/`
and `docs/`.

## Status
- **v0.2.0 board:** designed, checked by ERC/DRC and ready to order. Not yet built.
- **Firmware:** CAN, ISO-TP and UDS with a USB console. Builds for the Pico 2 W; not yet run on hardware.
- **Tools:** working and tested against a simulated car. Nothing has been tried on the real car.

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

## Other
- `tools/md_to_pdf.sh` regenerates the PDF copy that sits next to each Markdown write-up.
