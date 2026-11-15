# XIAO ESP32-S3 board (v0.3.0)

| Path | What it is |
|---|---|
| [`pcb/`](pcb/docs/README.md) | KiCad project, with the board write-up in `pcb/docs/README.md` |
| [`firmware/`](firmware/README.md) | ESP-IDF firmware: the shared core from the Pico board plus this chip's drivers |
| [`docs/design_notes.md`](docs/design_notes.md) | Why this board exists, the pin map, and what is decided |

Same job as [`../pico2w`](../pico2w/README.md) with a module that is actually in stock, CAN as a hardware
peripheral instead of software bit-banging, and Bluetooth in the main chip. The application core, the link
protocol, the logging format and the Python tools are shared with that board.

**Nothing here has been built or run.** The schematic is complete and checked, the firmware compiles, and the
PCB still needs fifteen connections routed.
