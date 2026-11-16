# OBD dongle (v0.4.0, laid out on a temporary outline)

| Path | What it is |
|---|---|
| [`pcb/`](pcb/README.md) | Schematics for both boards and the scripts that generate them |
| [`docs/architecture.md`](docs/architecture.md) | The two-board stack behind a J1962 plug: partitioning, power and sleep, the board-to-board connector, the mechanical interface with the enclosure, and what is still to verify |

The low-profile version of the [XIAO board](../xiao-esp32s3/README.md): same firmware core, protocol and tools,
plugged straight into the OBD port, and switching itself off when the car is parked. **The schematics are drawn**
and ERC clean, and both boards are routed (0 DRC errors) on a temporary outline, with STEP files for the
enclosure. The real outline follows the enclosure.
