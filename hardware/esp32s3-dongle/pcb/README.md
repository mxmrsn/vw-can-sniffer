# OBD dongle boards (v0.4.0)

| Path | What it is |
|---|---|
| `bottom/dongle-bottom.kicad_sch` | J1962 plug, protection, TJA1043T CAN with sleep and wake, INH-switched 5 V buck, wake detector |
| `top/dongle-top.kicad_sch` | ESP32-S3-MINI-1, USB-C, 3.3 V, microSD, LED chain, button, IMU (optional), GPS / sync and expansion |
| `lib/dongle.kicad_sym` | Symbols KiCad does not ship (TPS3700) |
| `scripts/` | Generators for both schematics; `sheetgen.py` turns a parts list into a label-connected sheet |

Schematics only: no board layouts yet. The layout waits on the enclosure envelope and the J1962 plug choice;
see [`../docs/architecture.md`](../docs/architecture.md).
