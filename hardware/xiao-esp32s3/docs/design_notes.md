# XIAO ESP32-S3 board (v0.3.0): design decisions

Why a second board: the Raspberry Pi Pico 2 W was hard to buy (2 in stock at DigiKey when the v0.2.0 BOM was
built), while the Seeed XIAO ESP32-S3 is widely stocked. The ESP32-S3 also suits this project better in two ways:
it has a **hardware CAN controller** (TWAI), so CAN no longer rides on software bit-banging in PIO, and its
Bluetooth LE is part of the main chip rather than a separate radio module.

The v0.2.0 Pico board stays as it is: both boards run the same application core, protocol and tools.

## Module
Seeed XIAO ESP32-S3 (the plain one, not Sense; Sense adds a camera and microphone that this project does not use).
21 x 17.8 mm, 14 castellated pads on 2.54 mm pitch, rows 17.78 mm apart. USB-C on the module carries the console
and firmware loading, so the board needs no USB connector of its own, exactly as on v0.2.0.

Only 11 of the 14 pads are I/O (D0 to D10); the other three are 5V, GND and 3V3. That is three fewer than the
Pico board used. The indicators therefore share one pin as a chain of addressable LEDs (below), the two buttons
become one, and one pad is left spare on a header. The module's Boot button (GPIO0) is still there for flashing.

## Pin map
TWAI can be routed to any GPIO through the ESP32-S3's GPIO matrix, and the microSD runs on the general-purpose
SPI peripheral, so almost nothing here is fixed by the chip. The assignment therefore follows the board layout:
the two indicators and the button, which stay next to the module, sit on the pads at one end, and everything
that has to reach the transceiver, the card socket or the divider sits on the pads nearest them.

| Pad | GPIO | Signal | Notes |
|---|---|---|---|
| D0 | 1 | `LED_DATA` | Addressable LED chain, through the U5 buffer |
| D1 | 2 | `SPARE_IO` | Header J3, for a GPS pulse-per-second input or a camera sync line |
| D2 | 3 | `MODE_SELECT` | Button to +3V3 with a 10k pull-down, as on v0.2.0, so pressed = high |
| D3 | 4 | `CAN_SILENT` | High = listen-only. R2 holds it high until firmware drives it low |
| D4 | 5 | `CTX` | TWAI transmit |
| D5 | 6 | `CRX` | TWAI receive |
| D6 | 43 | `SD_CS` | Also UART0 TX, so the ROM boot log wiggles it before the firmware starts |
| D7 | 44 | `SD_SCK` | Also UART0 RX |
| D8 | 7 | `SD_MOSI` | |
| D9 | 8 | `SD_MISO` | |
| D10 | 9 | `OBD_SENSE` | ADC1_CH8. Fused OBD voltage through 100k/22k, as on v0.2.0 |

**GPIO3 is a strapping pin.** It selects the JTAG source, but only when the `STRAP_JTAG_SEL` eFuse is burned,
which it is not on a stock module. The button's 10k pull-down also gives it a defined level at reset rather
than leaving it floating, which Espressif's hardware guidelines ask for.

The button keeps v0.2.0's arrangement (switch to +3V3, 10k pull-down) rather than switching to ground, so the
two boards behave the same way and the firmware difference is one line.

**R2, new on this board:** a 100k pull-up holds the transceiver's S pin high, so the board is listen-only from
the moment it is powered rather than from the moment firmware runs. The ESP32-S3's pins float during reset, and
the v0.2.0 board has the same gap; this closes it.

## Indicators
Four SK6812MINI-class addressable RGB LEDs in one chain on D0, along the board's bottom edge:

| LED | Shows |
|---|---|
| D1 | Status: amber while listen-only, green once transmitting is allowed |
| D2 | CAN activity |
| D3 | Bluetooth: a phone is connected |
| D4 | Logging to the card |

The LEDs run from VSYS (about 4.7 V), where their data input needs about 3.3 V, the very top of what the
ESP32-S3 drives. U5, a 74AHCT1G125 buffer powered from VSYS, takes the 3.3 V signal (TTL threshold) and drives the
chain at the supply level. R3 damps ringing on the line. One chain on one pin is what freed D1.

**Right-angle LEDs are left for the dongle.** Side-emitting addressable LEDs exist, but KiCad has no footprint for
them and no datasheet drawing was verified, so this board uses top-emitting ones. For the dongle, either a
verified side-view part or light pipes over these, chosen together with the enclosure.

## Power
Unchanged from v0.2.0, which was simulated and has a sourced BOM:

```
OBD +12V - F1 (PTC) -+- D7 (Schottky) - VIN - AP63301 buck - +5V - D5 (Schottky) -+- XIAO 5V pad
                     D6 (TVS)                                    XIAO USB-C ------+
```

The XIAO's own 3.3 V regulator (700 mA) then feeds the CAN transceiver's VIO and the microSD card. Seeed's
documentation requires a series diode when feeding the 5V pad from outside, which D5 already provides.

S3 still switches the buck's enable pin, so the board can be left plugged in without draining the car.

## CAN
TJA1051T/3 again: same part as v0.2.0, already sourced and in stock, 5 V supply with a 3.3 V VIO pin so it talks
to the ESP32-S3 directly. 120 ohm termination is **not** fitted (the car terminates its own bus); the footprint
is there for bench use.

## What is deliberately left out
- **Wi-Fi**: the ESP32-S3 has it, and the firmware does not use it. Bluetooth LE is enough for a phone, and
  Wi-Fi on a device plugged into a car's diagnostic port is a liability, not a feature.
- **Battery**: the XIAO has charging circuitry and BAT pads, but as decided for v0.2.0, the board runs from the
  OBD port or USB only.

## State of the design
- **Schematic**: complete and ERC clean; netlist checked against the pin map.
- **Symbol and footprint**: written for this project, in `pcb/lib/`. Through-hole on 2.54 mm pitch, so the module
  can sit in sockets or be soldered flat. **Checked on 2026-11-15 against Seeed's official KiCad footprint** (OPL_Kicad_Library, XIAO-ESP32-S3-DIP): the pin
  rows are 15.24 mm (0.6 in) apart. The first footprint had them 17.78 mm apart, which would not have fitted the
  module or its headers; fixed, and the board rerouted.
- **PCB**: compact 68 x 38 mm layout (the v0.2.0 board was 94 x 41), routed with the open-source Freerouting
  autorouter, ground poured on both layers, DRC clean (no errors, nothing unconnected). Gerbers and drill files
  are in `pcb/fab/`. The buck converter was reviewed on 2026-11-15 and re-placed: input capacitor straight across
  VIN and GND, boot capacitor beside BST and SW (2.3 mm, no vias), inductor tight to SW (switch node 5.3 mm),
  feedback divider beside the FB pin (6.5 mm, no vias; it was 15.6 mm with two vias).
- **STEP model** (`pcb/fab/xiao-esp32s3-can-sniffer.step`) for enclosure work, with the module and LEDs as
  placeholder boxes whose heights are assumed.
- **BOM**: every line stock-checked on 2026-11-15. The LEDs are Opsco SK6812mini-012 (LCSC C2886570), the one
  variant whose pads match the footprint corner for corner; the -HS and -E variants have different pinouts and
  must not be substituted. Headers and sockets are generic.
- **Firmware**: ported and building (`../firmware`), including the LED chain. Never run.
