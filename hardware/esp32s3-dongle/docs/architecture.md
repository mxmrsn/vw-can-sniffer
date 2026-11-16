# OBD dongle (v0.4.0): mezzanine architecture

The plan for the low-profile version: the interface plugs straight into the OBD port, two boards stacked behind
the plug, in an enclosure you design. This document fixes the electrical architecture and the interface between
the boards and the enclosure. **Both schematics are drawn** (see "Schematics" below); nothing is laid out or
built yet.

The v0.3.0 XIAO board stays the bench and first-car-session board. This one is designed in parallel and ordered
once the first car session has answered the questions it depends on (see "Decisions waiting on the car").

## What changes from v0.3.0, and why
| | v0.3.0 bench board | v0.4.0 dongle | Why |
|---|---|---|---|
| OBD connection | Pigtail to a 4-pin header | J1962 plug on the bottom board | Low profile, nothing to snag |
| Processor | XIAO ESP32-S3 module on sockets | ESP32-S3-MINI-1 module soldered to the top board | The XIAO's 11 pins are all used; the MINI-1 has about 30 free, is smaller (15.4 x 20.5 mm), and has its antenna on the module |
| CAN transceiver | TJA1051T/3 (silent pin) | **TJA1043T** (sleep, wake, listen-only, INH) | A dongle stays plugged in; it has to switch itself off and wake again |
| Off switch | Slide switch on the buck enable | None: the transceiver's INH output switches the buck | No mechanical part to reach behind the dash |
| 3.3 V supply | The XIAO's own regulator | AP2112K-3.3 on the top board | No module regulator any more |
| USB | On the XIAO | USB-C on the top board, straight to the chip's own USB | Console, firmware and bench power, as now |
| Indicators | Four addressable LEDs, top-emitting | Same chain, at the top board's edge, right-angle or under light pipes | Visible from the driver's seat |
| Expansion | One spare pin | Connector for an external GPS and camera sync; optional IMU | Drive replay and sensor fusion |

## The stack
```
                 driver's side (LEDs, USB-C, card slot face out)
   +--------------------------------------------------------------+
   | TOP BOARD (logic)                                            |
   |  ESP32-S3-MINI-1 (antenna at the board edge, away from metal)|
   |  USB-C . microSD . LED chain + buffer . button               |
   |  AP2112K-3.3 . IMU (optional) . GPS / sync connector         |
   +----------------------- board-to-board ----------------------+
   | BOTTOM BOARD (power and CAN)                                 |
   |  J1962 male plug . fuse . TVS . reverse-blocking diode       |
   |  TJA1043T transceiver . AP63301 buck 12 V -> 5 V             |
   |  engine-running detector . OBD voltage divider               |
   +--------------------------------------------------------------+
                 car's side (plugs into the OBD socket)
```

The split follows what each board is tied to: the bottom board belongs to the plug and to the car's 12 V and
CAN wiring; the top board belongs to the user and holds everything you see, press or plug into.

## Power and sleep
The OBD port's pin 16 is always live, so a dongle that never sleeps slowly drains the battery. The architecture
makes "off" the normal parked state, enforced in hardware:

```
pin 16 --fuse--TVS--diode-- VBAT ---+--> TJA1043 VBAT (always powered, about 20 uA asleep*)
                                    |
                                    +--> AP63301 buck VIN
                                              ^ EN
                         TJA1043 INH ---------+     (INH floats in sleep: buck off, everything else off)
```
\* NXP's datasheet: 18 uA typical, 30 uA maximum in sleep.

- **Sleep:** firmware puts the transceiver into go-to-sleep mode; its INH output floats, a pull-down on the buck's
  EN pin turns the buck off, and
  the module, card and LEDs lose power. Only the transceiver and a few leakage paths remain.
- **Wake sources**, each turning INH back on:
  1. **CAN activity** on the OBD bus (the transceiver recognises the standard wake-up pattern).
  2. **Engine running**: a TPS3700 voltage detector on VBAT (13.4 V rising, 13.2 V falling) switches a small
     MOSFET that pulls the WAKE node low. The transceiver wakes on any change of level on WAKE.
  3. **The button** on the top board pulls the same node low.
- **Staying awake:** the firmware keeps the system on while a phone is connected, while logging, or while the
  engine-running voltage is present, and goes back to sleep after a quiet period. It also sleeps, and refuses to
  wake for anything but the button, if VBAT falls below about 12.0 V, to protect the battery.
- **Asleep, the phone cannot see the dongle.** Bluetooth needs the processor running. That is the trade for not
  draining the battery; waking it is starting the engine, any CAN activity, or the button. A "stay connectable"
  option (processor in light sleep with slow Bluetooth advertising, a few mA) can be a firmware setting later.

**Sleep current budget**, from the datasheets now that the parts are chosen:

| Item | Typical | Worst case |
|---|---|---|
| TJA1043 in sleep | 18 uA | 30 uA |
| TPS3700 wake detector | 6 uA | 13 uA |
| Its supply clamp and output pull-up (R5/D4, R8) | 6 uA | 7 uA |
| Its 13.4 V sense divider (R6/R7) | 4 uA | 4 uA |
| OBD voltage divider, now 1 M / 220 k | 10 uA | 11 uA |
| Buck with EN low | 1 uA | 3 uA |
| TVS and diode leakage | 1 uA | 5 uA |
| **Total** | **about 46 uA** | **about 73 uA** |

Under the 100 uA target: a 60 Ah battery would take decades to notice.

**3.3 V:** the buck makes 5 V on the bottom board (the transceiver needs 5 V); USB 5 V and that rail are
diode-OR'd on the top board into an AP2112K-3.3. With Bluetooth and no Wi-Fi the module averages well under
100 mA, so a linear regulator dissipates under 0.2 W. Wi-Fi peaks would make it warm in a closed box, which is one
more reason the firmware leaves Wi-Fi off.

## CAN
TJA1043T: 5 V supply, 3.3 V I/O pin, and the modes this needs: normal, **listen-only** (keeps the v0.3.0 board's
hardware safety: the board cannot drive the bus until firmware asks), standby, sleep, plus INH, WAKE and an
error output.

Modes, from NXP's mode table: normal is STB_N high and EN high, **listen-only is STB_N high and EN low**, standby
is both low, and go-to-sleep is STB_N low with EN high. Both pins have internal pull-downs, so before firmware
runs the transceiver sits in standby: it cannot drive the bus, and INH stays on so the board stays powered.

Termination is not fitted, as on v0.3.0 (the car's bus is already terminated).

**A second bus, later:** a gateway tap for high-fidelity data needs two or three CAN buses. The ESP32-S3 has one
controller. That is a separate harness and probably a third board in the stack (MCP2518FD SPI controllers plus
transceivers), so the top board's expansion connector carries SPI, two chip selects and interrupt lines for it.

## Board-to-board connector
A 2 x 7, 1.27 mm pin header and socket pair (14 positions), hand-solderable. The stack height follows the
tallest part on the bottom board under the top board (the TVS diode and the inductor, about 2.5 mm), so a stack
of 5 to 6 mm works. Exact parts to be chosen with the enclosure.

| Pin | Signal | Direction | Note |
|---|---|---|---|
| 1, 2 | GND | | |
| 3, 4 | +5V | up | Buck output, about 300 mA peak |
| 5 | +3V3 | down | For the transceiver's I/O pin |
| 6 | GND | | |
| 7 | CAN_TXD | down | |
| 8 | CAN_RXD | up | |
| 9 | CAN_STB_N | down | Mode control |
| 10 | CAN_EN | down | Mode control |
| 11 | CAN_ERR_N | up | Errors, wake source after waking |
| 12 | OBD_SENSE | up | Analog, divided VBAT |
| 13 | WAKE_BTN | down | The button, to the transceiver's WAKE pin |
| 14 | GND | | Spare position kept as ground |

## Top board: pin plan
ESP32-S3-MINI-1 (quad flash, no PSRAM, so GPIO 33 to 37 are free). Avoid the strapping pins 0, 3, 45, 46 for
anything driven at reset; GPIO 19 and 20 are the chip's USB.

| Function | Pins | Notes |
|---|---|---|
| CAN | TXD, RXD, STB_N, EN, ERR_N | 5 |
| OBD sense | 1 ADC1 pin | |
| microSD (SPI) | SCK, MOSI, MISO, CS | 4 |
| LED chain | 1 | Through the buffer, as on v0.3.0 |
| Button | 1 | Also to WAKE through the connector |
| IMU (I2C) | SDA, SCL, INT | 3, optional part |
| GPS / sync connector | UART TX, RX, PPS in, SYNC out | 4 |
| Expansion (third board) | SPI shared with the card, 2 CS, 2 INT | 4 extra |
| USB | 19, 20 | Native USB |

About 27 pins, inside what the module has free.

## Expansion
- **GPS / sync connector** (a 6-pin JST-GH or similar): ground, 3.3 V, UART to an external GPS module, its
  pulse-per-second input, and a sync output for your camera and IMU rig. External because a dongle under the
  dashboard has a poor view of the sky.
- **IMU on the top board, optional.** Cheap, and useful for drive replay, but a dongle hanging from the OBD
  socket is not rigidly fixed to the car, so for sensor fusion your own rigidly mounted IMU is the better source.
  The footprint goes on; whether it is fitted is your call.
- **Time sync:** CAN frames, IMU samples and GPS fixes are all timestamped by the same microsecond timer, with the
  pulse-per-second edge as the absolute reference. The log format gains microsecond stamps and sync records.

## Mechanical interface: what I need from you, what you get from me
**From you, before layout:**
1. The enclosure envelope: maximum length, width and height behind the plug, and where the boards sit.
2. The J1962 plug: which part (see "Decisions waiting on the car"), so the bottom board's outline and holes
   follow it.
3. How the stack is held: screws through both boards, a clip, or the enclosure halves clamping them.
4. Where the user-facing parts go: LEDs (right-angle at the edge, or top-emitting under light pipes), USB-C,
   card slot (reachable, or behind a cover), button.
5. Keep-outs: metal near the antenna, ribs, bosses.

**From me, each revision:** both boards as STEP with every component, an interface drawing (outlines, holes,
connector and LED positions, heights), and board outlines that follow a DXF or STEP of your enclosure.

## Decisions waiting on the car (first session with v0.3.0)
- **Is there CAN traffic at the OBD port with the car merely unlocked or the ignition on?** If yes, bus wake alone
  wakes the dongle as you get in; if not, waking relies on the engine-running detector and the button.
- **Is any broadcast data visible at the OBD port?** If not, the high-fidelity data plan needs the gateway tap,
  which settles the third board.
- **Real current draw** of the v0.3.0 board, to size the power path.
- **Physical fit** of a J1962 plug in the car's socket, with samples in hand.

## The J1962 plug
PCB-mount male J1962 connectors exist (right-angle and straight, solder pins), sold mostly by Asian connector
houses, without published drawings. The plan: buy samples of two or three types, measure them, and design the
bottom board around the best. Fallbacks: a short molded pigtail inside the enclosure, or a printed housing around
stamped pins. This is the item most likely to need your mechanical judgement.

## Firmware impact
The application core, protocol, logging and tools do not change. New platform code:
- A pin map for the MINI-1.
- Transceiver mode control (listen-only, normal, sleep) replacing the silent pin.
- A sleep manager: idle timeout, battery-voltage cutoff, stay-awake reasons, wake-reason reporting.
- 3.3 V I/O for the GPS connector and the IMU driver.

The simulator gains sleep and wake states so the app can be tested against "the dongle went to sleep".

## Schematics
Drawn, ERC clean (0 errors), and generated by scripts so they can be rebuilt:

| Board | Schematic | Parts |
|---|---|---|
| Bottom | [`../pcb/bottom/dongle-bottom.kicad_sch`](../pcb/bottom/dongle-bottom.kicad_sch) | 36 |
| Top | [`../pcb/top/dongle-top.kicad_sch`](../pcb/top/dongle-top.kicad_sch) | 41 (the IMU and the expansion header are not fitted by default) |

```bash
python3 hardware/esp32s3-dongle/pcb/scripts/bottom.py      # python 3.10 or newer
python3 hardware/esp32s3-dongle/pcb/scripts/top.py
```

Every pin ends in a short stub and a net label, so a sheet reads as a reference drawing rather than a
hand-routed one, and connectivity is exactly what the script lists. The generator refuses to write a sheet in
which two different nets would touch. Both netlists were checked: the two halves of the board-to-board connector
agree pin for pin, and the transceiver, regulator and detector pins match their datasheets.

Two ERC warnings remain on the top board, both intended: the IMU's address and auxiliary pins are tied to
ground, which KiCad flags against a power flag on the same net.

Things the schematics settle that the architecture left open:
- **The MODE button** pulls the WAKE node low (waking a sleeping dongle) and reads on a GPIO through a diode,
  which keeps the 12 V-level node off the 3.3 V pin.
- **CAN bus ESD**: a NUP2105L across CAN-H and CAN-L, since the bus now goes straight to a connector.
- **microSD**: 47 k pull-ups on chip select, data in and data out.
- **Processor pins**: CAN on 4 to 7 and 15, the card on the SPI pins 10 to 13, LEDs on 48, button on 2, OBD
  sense on 1 (ADC), IMU on 8, 9 and 16, GPS and sync on 17, 18, 34 and 35, expansion on 14, 21, 47 and 33.

## Effort
| Step | Estimate |
|---|---|
| Confirm the transceiver modes, the INH-to-buck connection and the sleep currents from datasheets | Part of the schematic session |
| Schematics for both boards | **Done** |
| Layout of both boards, once outlines and the plug are known (the v0.3.0 scripts and autorouter carry over) | 1 to 2 sessions |
| Firmware: transceiver modes, sleep manager, new pin map | 1 session |
| Mechanical iterations with your enclosure | As many as it takes |

## Parts to verify before the schematic is final
| Part | Status |
|---|---|
| TJA1043T | Modes, INH, WAKE and sleep current confirmed in NXP's datasheet. Orderable code and stock to check |
| ESP32-S3-MINI-1 (N8) | KiCad has the symbol and footprint. Stock to check |
| AP2112K-3.3 | KiCad has the symbol. Stock to check |
| AP63301WU-7 | As v0.3.0. Confirmed: EN is rated to 35 V, so INH can drive it directly; EN has an internal 1.5 uA pull-up, so a 100 k pull-down (R4) holds it off when INH floats |
| TPS3700 (wake detector) | Chosen; pinout, 18 V supply limit and 6 uA from TI's datasheet. Symbol drawn for this project. Stock to check |
| IMU | Candidates in KiCad: LSM6DSM, LSM6DSL. To choose |
| 2 x 7 1.27 mm header and socket | To choose with the enclosure |
| J1962 PCB plug | Samples needed |
