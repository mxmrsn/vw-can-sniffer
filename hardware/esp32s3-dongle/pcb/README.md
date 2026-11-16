# OBD dongle boards (v0.4.0)

| Path | What it is |
|---|---|
| `bottom/dongle-bottom.kicad_sch` | J1962 plug, protection, TJA1043T CAN with sleep and wake, INH-switched 5 V buck, wake detector |
| `top/dongle-top.kicad_sch` | ESP32-S3-MINI-1, USB-C, 3.3 V, microSD, LED chain, button, IMU (optional), GPS / sync and expansion |
| `lib/dongle.kicad_sym` | Symbols KiCad does not ship (TPS3700) |
| `bottom/dongle-bottom.kicad_pcb`, `top/dongle-top.kicad_pcb` | Both boards, placed and routed on a temporary 56 x 44 mm outline, 0 DRC errors |
| `step/` | Both boards as STEP, every part modelled, for the enclosure |
| `3dmodels/` | Placeholder boxes for parts KiCad has no model for (`make_models.py`) |
| `scripts/` | Generators for both schematics (`sheetgen.py` turns a parts list into a label-connected sheet), placement (`layout.py`), and the whole pipeline (`route.sh`) |

The outline is a stand-in until the enclosure exists; the plug is held by the enclosure and wired to pads on the
bottom board. See [`../docs/architecture.md`](../docs/architecture.md).

```bash
hardware/esp32s3-dongle/pcb/scripts/route.sh    # schematics, netlists, placement, autorouting, DRC, STEP, renders
```
