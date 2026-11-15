# VW CAN sniffer, XIAO ESP32-S3 carrier (v0.3.0)

Carrier board for a **Seeed XIAO ESP32-S3**: CAN through the chip's own TWAI controller and a TJA1051T/3
transceiver, power from the OBD port or the module's USB-C, and a microSD socket for logging. It does the same
job as the [Pico 2 W board](../../../pico2w/README.md) (v0.2.0) with a module that is far easier to buy, and both
run the same firmware core, protocol and tools.

**This board has not been fabricated, and the layout is not finished.** See "State" below.

## Why this board exists
The Pico 2 W was down to two in stock at DigiKey when the v0.2.0 BOM was built. The XIAO ESP32-S3 is widely
stocked, and it suits the project better in two ways: CAN is a hardware peripheral rather than software
bit-banging in PIO, and Bluetooth LE is part of the main chip. See [`../../docs/design_notes.md`](../../docs/design_notes.md)
for the decisions and the pin map.

## What is on the board
- **U1** XIAO ESP32-S3 on 2.54 mm headers (sockets, or soldered flat).
- **U4** TJA1051T/3 CAN transceiver, 5 V supply with a 3.3 V VIO pin, with **R2** holding it in listen-only
  until firmware says otherwise.
- **J1** 4-pin Molex Pico-SPOX for the OBD harness: +12 V, CAN high, CAN low, ground.
- **Power**: OBD +12 V through a PTC fuse, a TVS clamp and a blocking Schottky into an AP63301 buck at 5 V,
  then a second Schottky into the module's 5V pad. **S3** switches the buck off so the board can stay plugged in.
- **J2** microSD socket on SPI for logging.
- **D1/D2** green and blue indicators, **S1** a button, and the OBD voltage divider on **D10**.

Everything except the module, R2 and the indicators is unchanged from v0.2.0, including the simulated and
sourced power section.

## State
| Piece | State |
|---|---|
| Schematic | Complete, ERC clean (0 violations) |
| Netlist | Checked against the pin map by hand |
| Symbol and footprint | Written for this project; **dimensions from the datasheet, not measured** |
| PCB placement | Done |
| PCB routing | Power section and the local cluster done; **15 module connections still to route** |
| BOM | Carried over from v0.2.0 plus the module and R2; **the module's distributor line is unverified** |
| Fabrication files | Not generated: the board is not routed |

Nothing here has been built or tested.

## Finishing the layout
Open the board in KiCad and route the fifteen remaining connections (DRC lists them; they are the module's CAN,
microSD, OBD-sense and power connections to the right-hand side of the board). The interactive router does this
far better than a script can. Then shrink the outline: the module is a third of the Pico's length, so most of the
left-hand half is now empty.
