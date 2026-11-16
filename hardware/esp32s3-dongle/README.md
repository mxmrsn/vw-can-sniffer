# OBD dongle (v0.4.0, architecture only)

| Path | What it is |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | The two-board stack behind a J1962 plug: partitioning, power and sleep, the board-to-board connector, the mechanical interface with the enclosure, and what is still to verify |

The low-profile version of the [XIAO board](../xiao-esp32s3/README.md): same firmware core, protocol and tools,
plugged straight into the OBD port, and switching itself off when the car is parked. **Nothing is drawn yet**;
the schematics follow once the architecture is agreed, and the layout once the enclosure and the J1962 plug are
chosen.
