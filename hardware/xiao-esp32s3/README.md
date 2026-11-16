# XIAO ESP32-S3 board (v0.3.0)

| Path | What it is |
|---|---|
| [`pcb/`](pcb/docs/README.md) | KiCad project, with the board write-up in `pcb/docs/README.md` |
| [`firmware/`](firmware/README.md) | ESP-IDF firmware: the shared core from the Pico board plus this chip's drivers |
| [`docs/design_notes.md`](docs/design_notes.md) | Why this board exists, the pin map, and what is decided |
| [`docs/bringup_plan.md`](docs/bringup_plan.md) | What to test, in order, when the boards arrive: bench, phone, car |
| [`bench-car/`](bench-car/README.md) | Firmware for a second board that answers like the car, for testing on the desk |

Same job as [`../pico2w`](../pico2w/README.md) with a module that is actually in stock, CAN as a hardware
peripheral instead of software bit-banging, and Bluetooth in the main chip. The application core, the link
protocol, the logging format and the Python tools are shared with that board.

**Nothing here has been built or run.** The schematic is complete and checked, the board is routed and DRC
clean, and the firmware compiles.
