# Pico 2 W diagnostic interface (v0.2.0)

Everything specific to the current board.

| Path | What it is |
|---|---|
| [`pcb/`](pcb/docs/README.md) | KiCad project, with the board write-up in `pcb/docs/README.md` |
| `pcb/bom/` | Purchase BOM with DigiKey part numbers, and a cart-upload file |
| `pcb/fab/` | Gerbers, drill files and placement file |
| [`pcb/spice/`](pcb/spice/README.md) | Regulator power-stage simulation |
| [`pcb/3dmodels/`](pcb/3dmodels/README.md) | 3D models not in the stock KiCad library |
| [`firmware/`](firmware/README.md) | Pico 2 W firmware, host simulator (`firmware/sim/`) and protocol tests (`firmware/tests/`) |
| [`docs/bringup_plan.md`](docs/bringup_plan.md) | What to test, in order, when the hardware arrives |

The host tools that drive this board are board-independent and live in [`../../tools/vwdiag`](../../tools/vwdiag/README.md).
