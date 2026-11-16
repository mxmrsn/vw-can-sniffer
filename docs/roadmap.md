# Roadmap

Where the project is, where it is going, and in what order. Written 2026-11-15, after the v0.2.0 tag and the
first cut of the XIAO ESP32-S3 board.

The guiding rule: **the first time the hardware is plugged into the car, it should do something useful, read
only, and capture everything.** Every phase before the car exists to make that session count; every phase after
it feeds what the car told us back into the simulator.

## Where we are
| Piece | State |
|---|---|
| Pico 2 W board (v0.2.0) | Designed, DRC clean, sourced. Not ordered. Pico 2 W hard to buy |
| XIAO ESP32-S3 board (v0.3.0) | Routed, DRC clean, BOM verified, buck reviewed, module footprint corrected. **Ready to order** |
| Firmware | CAN, ISO-TP, UDS, text console, packet protocol over USB and BLE, logging to SD. Builds for both boards, never run |
| Tools (`vwdiag`) | Scan, identification, faults, OBD live data, logging, DBC decoding. Tested against the simulator only |
| Simulator | 7 control modules, real part numbers, invented live data, idealised timing (audit below) |
| iOS app | Not started. The simulator's BLE stand-in is ready for it |

## Priorities at a glance
| # | Phase | Why now | Effort |
|---|---|---|---|
| 0 | ~~Verify the remaining BOM lines and the module footprint, review the buck layout~~, order | Hardware lead time is the long pole | Done; ordering is yours |
| 1 | ~~Make the simulator pessimistic, add misfire diagnostics, script the first car session~~ | Has to be ready before the boards arrive | Done (2026-11-15) |
| 2 | ~~iOS app skeleton: scan, faults, live data, logging~~ | Fills the 1 to 3 weeks of fabrication and shipping | Done (2026-11-15); Bluetooth untested on hardware |
| 3 | Bring-up, hardware-in-the-loop bench, first car session | When the boards arrive | 2 sessions plus car time |
| 4 | Misfire analysis; stock and stage 1 baseline logs | First real data | Ongoing |
| 5 | v0.4.0: integrated OBD connector, mezzanine, sensor and sync expansion | Needs what the car taught us | 3 to 5 sessions, plus your enclosure |
| 6 | App parity with OBDeleven / VCDS reads, then careful writes | Builds on real module data | Ongoing |
| 7 | Tuning toolchain | After the engine is healthy and logging is trusted | Large |
| 8 | Dashboards, lap timing, custom ECU | Long term | Large |

A "session" here is one working session like the ones so far.

---

## Phase 0: finish the XIAO board and order (now)
**Goal:** an orderable v0.3.0 bench board, connected to the car through a pigtail.

- Indicators become one addressable LED chain on a single GPIO: one RGB status LED plus one per function (CAN,
  Bluetooth, logging). That frees a pin, which goes to a small header for a future GPS pulse-per-second input or
  a camera sync line. Right-angle versions of the per-function LEDs are left for the dongle (Phase 5), where a
  verified side-view part or a light pipe from your enclosure can be chosen together.
- **Done:** outline shrunk to 68 x 38 mm, routed with the Freerouting autorouter, DRC clean, gerbers and a STEP
  model generated.
- **Done (2026-11-15):** every BOM line stock-checked (LEDs: Opsco SK6812mini-012 from LCSC, the variant that
  matches the footprint); the module footprint checked against Seeed's own, which caught pin rows 17.78 mm apart
  instead of 15.24 mm (fixed, rerouted); the buck converter re-placed for a tight input loop and short switch
  node and feedback traces.
- **Left:** place the order (yours): boards from `hardware/xiao-esp32s3/pcb/fab/`, parts from the BOM.
- Order alongside: OBD pigtail, a USB-CAN adapter for the bench, a second XIAO (see Phase 3).

**Hold the mezzanine and integrated-connector versions** until after the first car session. What the car shows
us (is broadcast data visible at the OBD port? do we need a gateway tap? GPS?) changes what goes in the stack.

## Phase 1: sim-to-real hardening and the first-session script (before hardware arrives)
**Done (2026-11-15).** The simulator is pessimistic by default (every row of the table below except replay and
measuring values from real captures), with an engine model for this car (DKFA, EA888 Gen 3) and a `gti` scenario
for its misfire. `vwdiag misfire`, `measure` and `session` are in; see `tools/vwdiag/README.md`. Still open: replay
of real captures, which needs the captures (the first car session, and the public data listed in
`docs/vw_can_data_sources.md`).

### Simulator audit
The simulator exercises the protocol logic well, but it is optimistic in ways that will not survive the car:

| Gap | Real car | Simulator today | Fix |
|---|---|---|---|
| Response timing | 5 to 50 ms per answer, more through the gateway | Instant | Per-module latency with jitter |
| Flow control | Modules set block size and minimum separation time | Never throttles | Configurable BS and STmin per module |
| Functional OBD request (7DF) | Engine **and** transmission answer | Engine only | Multiple responders |
| Sessions | Extended session drops after about 5 s without TesterPresent | Never expires | Session timer |
| Ignition and sleep | Modules silent with ignition off; some need the engine running | Always awake | Ignition state (off, on, cranking, running) |
| Gateway | Broadcast traffic probably **not** visible at the OBD port | Broadcast frames on the diagnostic bus | Mode that hides them (the default until the car says otherwise) |
| Live values | Physically consistent (rpm, load, MAF, timing, trims move together) | Independent sawtooth values | A small engine model driven by a throttle trace |
| Fault codes | Status bits change, freeze frames exist | Static list, only report type 02 | Snapshots (19 04), extended data (19 06), state changes |
| Bus errors | Error frames, lost frames, bus-off | Never | Fault injection: drop, corrupt, delay, reorder |
| Security access | Seed and key | Refused outright | Leave refused: we should never need it for reads |
| Measuring values | Hundreds of VW-specific identifiers | A dozen | Grows from real captures (Phase 4) |

**The biggest lever is replay.** Once real captures exist, the simulator should play recorded traffic back, and
the control-module models should answer from recorded request/response pairs. Until then, comma.ai has published
raw CAN recordings from real cars (the `commaCarSegments` dataset; worth checking whether it covers the MQB GTI)
which could feed the broadcast side. Those are recorded at the gateway rather than the OBD port, so they are
realistic for the powertrain bus, not for what the OBD port shows.

**Hardware in the loop:** a second XIAO running the simulator's control-module models on a real CAN bus, wired to
the interface board through real transceivers and termination. That tests bit timing, the transceiver, the
listen-only line and the harness before the car is involved. It reuses the simulator code almost unchanged and
costs one more module.

### Misfire diagnostics in the tools
Add before the first session, because they answer your live question:
- **OBD service 06, misfire monitor**: per-cylinder misfire counts for the current and last ten drive cycles
  (OBD monitor IDs A2 to A5 are cylinders 1 to 4). Standard, so very likely to work through the OBD port.
- Fuel trims (short and long term), timing advance, lambda, MAF and rpm as fast-polled OBD values.
- Freeze-frame data for P0300 to P0304.
- A `vwdiag session` command that runs the whole first-car script below and saves everything.

### The first car session, scripted
Read only, listen-only first, every byte logged to the card and the laptop:
1. Listen-only for 60 s, ignition off then on. Answers: is there any broadcast traffic at the OBD port?
2. Scan every module address, identification of each one. Corrects the address table.
3. Fault codes from every module, with freeze frames.
4. OBD supported values, then a 2-minute idle capture of the misfire set above.
5. A short drive with the same capture, logging to the card, marks at events.

The tools retry, time out cleanly and keep going if a module does not answer, so a partial result is still a
useful result.

## Phase 2: iOS app skeleton (while boards are being made)
**Done (2026-11-15):** `ios/` (see `ios/README.md`). Runs in the iOS Simulator against the simulator's Bluetooth
stand-in, with a Misfire tab added for the live question. Open: Bluetooth against a real board. Added afterwards: copying logs
off the card (commands 0E and 0F), the engine measuring values in Live, and the logging experiments, guided, in a
Visit tab, so a car visit needs only the phone.
SwiftUI app, CoreBluetooth, with the packet protocol in a Swift package that has its own tests against recorded
byte streams. During development it talks to the simulator's Bluetooth stand-in over TCP from the iOS Simulator,
so it is exercised against the same control-module models as the Python tools.

First panels, mirroring what OBDeleven and VCDS put up front:
1. **Connect**: scan, pair, interface status (bus mode, counters, battery voltage).
2. **Autoscan**: every module, present or not, part number, fault count. Tap into a module.
3. **Module view**: identification, fault codes with descriptions and freeze frames, clear (with a confirm).
4. **Live data**: pick values, show gauges and graphs, record.
5. **Logs**: list logs on the card, download, share as CSV or candump.

## Phase 3: hardware bring-up and the first car session
**Prepared (2026-11-16):** `hardware/xiao-esp32s3/docs/bringup_plan.md` is the plan for this board: power, USB,
the card, two boards on a bench bus with the second running the **bench car** (`hardware/xiao-esp32s3/bench-car`,
the simulator's car on a real CAN bus), Bluetooth with a phone, then the car. On the car, the logging experiments
in `docs/logging_experiments.md` gather the data, and `vwdiag calibrate` turns it into simulator settings.

## Phase 4: real data
- **Misfire analysis** (see the section below).
- **Measuring values**: discover which VW identifiers the engine and other modules answer, by careful read-only
  sweeps of documented ranges, and log them. This is how the app gets VCDS-style measuring blocks.
- **Fast logging**: UDS lets a tester define its own identifier that packs many values into one answer
  (service 2C, dynamically defined identifiers). That is how commercial loggers reach 20 to 50 samples a second
  per value instead of a few. Worth implementing once basic polling works.
- **Baselines**: log the stock tune on a fixed route and conditions, then the stage 1 tune the same way, before
  any custom work. Same route, same fuel, similar temperature.

## Phase 5: v0.4.0 hardware: integrated connector, mezzanine, expansion
**Started early, at your request:** the architecture is in
[`hardware/esp32s3-dongle/docs/architecture.md`](../hardware/esp32s3-dongle/docs/architecture.md). The plan below
still applies; schematics come next, layout once the enclosure and the plug are chosen.

### Integrated OBD connector
PCB-mount J1962 male connectors exist (right-angle and straight, solder pins), mostly from Asian connector
houses, without the published drawings a design should be built on. Plan: buy a few samples of two or three
types, measure them, and design around the best one. Alternatives if none is good enough: a short molded
pigtail inside the enclosure, or a printed housing around stamped pins (your call on what is robust enough).

### Mezzanine
Makes sense for a dongle: the OBD plug sets one face of the enclosure, and stacking keeps it short.
- **Bottom board**: OBD connector, protection, buck, CAN transceiver(s). Mechanically tied to the plug.
- **Top board**: module, microSD, LEDs (right-angle at the edge, or light pipes), expansion.
- **Between**: a board-to-board connector carrying 5 V, ground, CAN TX/RX and silent per bus, and OBD sense.

Effort: the electrical split is small (the schematic already divides that way). Two small layouts are about the
same work as one board, plus the stacking connector, plus coordinating heights and keep-outs with your
enclosure. Roughly 3 to 5 sessions on my side once the connector and enclosure are chosen.

### Expansion worth putting on the stack
- **More CAN buses.** The ESP32-S3 has one CAN controller. A gateway tap (below) needs two or three; MCP2518FD
  SPI CAN controllers are the usual answer.
- **GPS** with pulse-per-second (a u-blox module at 10 to 25 Hz): position and speed for drive replay and lap
  timing, and an absolute time reference for everything else.
- **IMU** on the board: cheap, and useful alongside GPS and CAN.
- **Sync I/O** for your camera and IMU rig: a trigger out and a pulse in, timestamped by the same clock as the
  CAN frames.
- **More pins**: the XIAO ESP32-S3 Plus has nine more GPIO on bottom pads, which may be enough to avoid an
  expander.

### The gateway question
The MQB gateway sits between the OBD port and the car's internal buses. If, as expected, the OBD port shows little
broadcast traffic, high-fidelity CAN data needs a tap at the gateway connector (the approach openpilot uses):
a harness that sits between the gateway and the car and sees the powertrain and other buses directly. That is
the right source for sensor-fusion data: wheel speeds, steering angle, yaw rate, accelerations, pedal, brake
pressure, gear, all at their native rates (10 to 100 Hz). It is a different physical connection from the OBD
plug, so it would be its own harness and probably its own variant of the board.

## Phase 6: OBDeleven / VCDS functionality
**Reads first, all achievable:** autoscan, fault codes with VAG descriptions, freeze frames, identification,
measuring values, readiness, service interval reading.

**Writes later, carefully:** service resets, adaptations, long coding, basic settings, output tests. These change
the car. The catch is data: what each coding bit means and which adaptation channels exist comes from VW's own
data (ODX) and the commercial tools' label files, none of which are public. Community-documented codings for
common changes exist and are where to start. Every write takes a backup of the current value first, shows a diff,
and needs a confirmation.

## Phase 7: tuning toolchain
**Before any of this: fix the low-compression cylinder.** A stage 1 tune adds cylinder pressure, which is the
wrong direction for a cylinder that is already not sealing.

- **Logging is the foundation**: fast logging (Phase 4), time-aligned with GPS, compared run against run.
- **Read-only first**: identify the ECU's software version, read what can be read. The MK7.5 GTI engine ECU is a
  Continental Simos 18 family unit.
- **Flashing**: the IE PowerLink does this today. Open-source projects for Simos 18 exist (for example VW_Flash);
  studying them is the starting point for anything we do ourselves. A failed flash can make the ECU unusable, so
  this needs a bench ECU before it ever touches the car's.
- **Our own tools**: map viewer and comparison, log overlays, a calibration diff between stock and stage 1, a
  bench flashing workflow. All in the iPad app, eventually.
- **Custom ECU**: a project in its own right; the logging and the bench setup above are what it would build on.

## Phase 8: dashboards and apps
- **iPhone / iPad dash**: the realistic route for custom gauges. Full control over the UI, mounted in the car.
- **CarPlay**: third-party CarPlay apps are limited to Apple's templates and to approved categories, so a custom
  gauge dashboard is not something CarPlay allows. Worth re-checking against Apple's current rules before
  designing around it.
- **Lap timing**: GPS plus IMU, with CAN data overlaid; track and sector definitions in the app.
- **Drive replay**: map trace, speed, inputs (pedal, brake, steering), engine data and the camera, all on one
  timeline.

## Data worth capturing for drive replay and later projects
| Source | Values | Rate |
|---|---|---|
| OBD port, polled | Engine values, trims, timing, misfire counts | 2 to 50 Hz (polled vs dynamically defined) |
| Gateway tap, broadcast | Wheel speeds, steering angle, yaw rate, lateral and longitudinal acceleration, pedal, brake pressure, gear | 10 to 100 Hz, native |
| GPS | Position, speed, heading, absolute time | 10 to 25 Hz, plus pulse per second |
| IMU on the board | 6-axis acceleration and rotation | 100 Hz to 1 kHz |
| Your camera and IMU rig | Frames, inertial data | As the rig runs, synchronised by the trigger or the pulse |

**Time synchronisation** is what makes this usable for SLAM and sensor fusion: every source stamped against the
same clock, with GPS time as the absolute reference. The log format already carries millisecond timestamps and a
wall-clock record; it would gain microsecond stamps and sync events.

---

## Your misfire: what to look at
Low compression on one cylinder, after the injectors, plugs, coils and coil grounds have been replaced, points
at a **mechanical** cause: a valve not sealing (burnt, bent, carbon on the seat), piston rings or a ring land, or
the head gasket. No log can tell those apart for certain. **A leak-down test can**: pressurise the cylinder at top
dead centre and listen to where the air goes:

| Air comes out of | Points to |
|---|---|
| The intake | Intake valve |
| The exhaust | Exhaust valve |
| The oil filler / crankcase | Rings or piston |
| The coolant (bubbles) or the next cylinder | Head gasket |

A wet compression test (a little oil in the cylinder, then test again) is the cheap second opinion: if the
number comes up, rings; if not, valves or gasket.

What the logs **can** add:
- **Misfire counts per cylinder** (OBD service 06), at idle, cold start, cruise and load. A mechanical leak
  usually misfires most at idle and low load, and when cold.
- **Short- and long-term fuel trims**: a cylinder that pumps less air shifts the mixture the whole engine sees.
- **Lambda** and **MAF** at idle compared with expected values.
- **Ignition timing and knock retard per cylinder**, if the engine module exposes them.
- **Cam timing, actual against specified**: valve timing problems look like low compression.
- **Coolant temperature behaviour** and any coolant loss: a head gasket sign.

The idle and cold-start capture in the first car session script is built around exactly these.

## CAD for the enclosure: who leads
I can write parametric CAD as code (CadQuery or build123d, producing STEP and STL), and KiCad exports the boards
as STEP with every component. For a first rough box that is fine; for a real enclosure, **you should lead**.
What I will provide so the boards and the enclosure meet:
- Board STEP exports with all components, updated with each revision.
- An interface drawing: board outlines, mounting holes, connector and LED positions, card slot, USB access,
  keep-out heights.
- Changes to the board outline from a DXF or STEP you give me, so the board follows the enclosure rather than
  the other way round.
