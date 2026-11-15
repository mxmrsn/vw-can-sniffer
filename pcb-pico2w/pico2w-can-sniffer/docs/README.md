# Pico 2 W CAN Sniffer PCB — v0.2.0 (bench / development board)

Goal: a home-built VW diagnostic interface (UDS over the OBD port) driven from an iOS app over Bluetooth LE, with
logging. This board is the development unit; a low-profile OBD dongle on RP2350 is planned as v0.3.0.

Single-MCU carrier for a **Raspberry Pi Pico 2 W** (RP2350A + CYW43439 Wi-Fi/BLE). It replaces the
Teensy 4.0 + XIAO ESP32S3 pair used on [v0.1.0](../../../pcb-teensy-esp32/teensy-esp32-can-sniffer/docs/README.md)
(git tag `v0.1.0`).

> The RP2350 chip itself has no radio. Wi-Fi/BT comes from the CYW43439 on the Pico 2 W module, so this
> board carries the module (socketed on two 1×20 headers) rather than a bare RP2350.

![Schematic](./schematic.png)
![Layout, both copper layers](./pcb-layout.png)
![Back copper](./pcb-layout-back.png)
![3D top](./3d-render.png)
![3D angle](./3d-render-angle.png)

## What changed from v0.1.0
- U1 Teensy 4.0 and U2 XIAO ESP32S3 removed; U1 is now the Pico 2 W. The Teensy↔ESP32 UART link is gone.
- CAN controller: RP2350 has no CAN peripheral, so CAN runs in PIO ([can2040](https://github.com/KevinOConnor/can2040))
  straight into the existing TJA1051T/3.
- **D5** (MBR120VLSFT1G Schottky) between the 5 V buck and VSYS, so USB and the 12 V buck can be plugged in
  together without back-feeding the buck. TJA1051 VCC moved to VSYS so the transceiver also works from USB alone.
- **JP1** solder jumper in series with the 120 Ω terminator R7, **open by default**. Leave it open on the car
  (the bus is already terminated); bridge it only on the bench.
- Power comes from a protected OBD 12 V feed or the Pico's own micro-USB (see Power below). **S3** is the on/off
  switch for the OBD-powered buck.
- **microSD socket (J2)** on SPI1 for standalone logging.
- OBD supply voltage is measured on GP28 so firmware can tell engine-running from parked.
- Board is now 93.98 × 40.64 mm (v0.1.0 was 68.58 mm wide): +7.62 mm to keep the CAN/power section out from
  under the Pico antenna and +17.78 mm for the power-input and microSD section. Left mounting holes are unchanged;
  the right pair moved +25.4 mm. The CAN transceiver and buck layout are carried over from v0.1.0.
- Antenna end of the Pico has a copper/track keep-out on both layers.

## Pin map
| Pico pin | GPIO | Net | Function |
|---|---|---|---|
| 5  | GP3  | `MODE_SELECT`        | S1 mode button (active-high, 10 k pull-down) |
| 9  | GP6  | `SILENT_MODE_SELECT` | S2 silent-mode button (active-high, 10 k pull-down) |
| 10 | GP7  | `CAN_STATUS`         | D1 CAN activity LED |
| 11 | GP8  | `SD_MISO`            | microSD DAT0 (SPI1 RX) |
| 14 | GP10 | `SD_SCK`             | microSD CLK (SPI1 SCK) |
| 15 | GP11 | `SD_MOSI`            | microSD CMD (SPI1 TX) |
| 16 | GP12 | `SD_CS`              | microSD DAT3/CS |
| 17 | GP13 | `WIFI_STATUS`        | D2 Wi-Fi LED |
| 19 | GP14 | `USB_STATUS`         | D3 USB LED |
| 20 | GP15 | `CAN_SILENT_STATUS`  | D4 silent-mode LED |
| 21 | GP16 | `CAN_SILENT`         | TJA1051 `S` (high = listen-only) |
| 22 | GP17 | `CTX`                | TJA1051 `TXD` |
| 24 | GP18 | `CRX`                | TJA1051 `RXD` |
| 34 | GP28 | `OBD_SENSE`          | OBD supply voltage, ADC2. V_obd = V_adc × 5.545 |
| 36 | —    | `+3V3`               | 3V3 out → TJA1051 VIO, buttons, microSD |
| 39 | —    | `VSYS`               | 5 V in via D5 (buck); also TJA1051 VCC |

All other GPIO are unconnected. Matching firmware header: `firmware/pico2w/pins.h`.

## Power
```
OBD +12V ─ F1 (PTC) ─┬─ D7 ─ VIN ─ AP63205 buck ─ +5V ─ D5 ─┬─ VSYS ─ Pico 3V3 reg ─ +3V3
                     D6 (TVS)            Pico micro-USB ────┘ (Pico on-board diode)
```
- **OBD:** J1 pin 1 → 0.75 A PTC → SMBJ18A clamp → Schottky blocking diode. Harness: OBD pin 16 → +12V,
  pins 4/5 → GND, pin 6 → CAN_H, pin 14 → CAN_L.
- **Power switch (S3):** shorts the buck enable pin to ground. OBD pin 16 is always live and the board draws
  roughly 50–100 mA while running, so switch off or unplug when parked until firmware sleep exists.
- **OBD sense:** fused OBD voltage → 100 k / 22 k → GP28. About 12.0–12.8 V parked, 13.5–14.5 V with the engine running.
- **USB:** the Pico's micro-USB powers the board and carries data and firmware loading. It is not switched by S3.
  The separate USB-C power socket from an earlier pass was removed as redundant.
- VSYS is about 4.7 V on any source (TJA1051 VCC minimum is 4.5 V).
- There is no battery input; the earlier 2S LiPo circuit was removed.

## microSD
Hirose DM3AT push-push socket on the right edge, wired for SPI mode on SPI1 with a 4.7 µF supply capacitor.
DAT1/DAT2 and the card-detect switch are not connected, and there is no pull-up on CS (firmware should drive CS
high before touching the bus). No battery-backed clock: timestamps come from the phone at connect, or count
from power-on.

## Verification status
- ERC: 0 errors (remaining warnings are missing third-party libraries, same as v0.1.0).
- DRC: 0 unconnected, 0 clearance/short/courtyard errors. Remaining: Pico USB-connector silkscreen overhanging the
  board edge (intentional) and the J1 reference over its own pads (cosmetic).
- `kicad-cli` schematic-parity reports 34 "No corresponding pin found" warnings on auto-named nets
  (`Net-(…)`, `unconnected-(…)`). Pad nets were assigned directly from the schematic netlist; run
  *Tools → Update PCB from Schematic* once in the GUI to refresh pad metadata.
- The power-input and microSD sections are new and unbuilt.
- The SD fan-out uses 0.6 mm / 0.3 mm vias; check your fab's minimum before ordering.
- F1 is specified by rating only (0.75 A hold, ≥30 V, 1812); pick an in-stock part before ordering.
- 3D models: J1 (Molex Pico-SPOX) and S3 (slide switch) have no model in the KiCad library. They look for
  manufacturer STEP files in [`../3dmodels/`](../3dmodels/README.md) and stay blank until those are added. F1 uses a same-size 1812 capacitor model as a stand-in.
- Not yet done: fab outputs (gerbers), bring-up on hardware.

## BOM
See [`bom.csv`](./bom.csv). Removed vs v0.1.0: Teensy 4.0, XIAO ESP32S3. Added: Pico 2 W (or 2 WH) on two 1×20
socket strips, the OBD protection parts, microSD socket, power switch, D5 Schottky and JP1.
