# XIAO ESP32-S3 board (v0.3.0): bring-up plan

What to test, in what order, when the boards and parts arrive. Each stage depends only on the ones before it and
has a clear pass condition: stop at the first failure and fix it before moving on. This replaces the Pico
board's plan (`../../pico2w/docs/bringup_plan.md`) for this board; the car stages are the same in spirit.

Build two boards. The second one runs the **bench car** firmware (`../bench-car/`): it answers like the car's
control modules on a real CAN bus, so stages 4 to 6 happen on the desk, not in the car.

Conventions: `vwdiag ...` means `python3 -m vwdiag --port <port> ...` run from `tools/`. `this style` without
`vwdiag` is typed into the board's USB serial console. Designators are from the v0.3.0 schematic.

## What to have on the desk
- Two assembled boards, two XIAO ESP32-S3 modules (headers soldered on the modules, sockets on the boards), two
  microSD cards (FAT32), two USB-C cables.
- Bench supply with current limit, multimeter, a scope or logic analyser (for the bit timing and the buck).
- Three short wires for the bench bus: CAN-H to CAN-H, CAN-L to CAN-L, GND to GND (harness connector J1:
  pin 1 +12 V, pin 2 CAN-L, pin 3 CAN-H, pin 4 GND).
- Solder for JP1 (the 120 Ω terminator): **bridged on both bench boards, open on the board that goes in the car**.
- Optional: a USB-CAN adapter as an independent third node.

## Stage 0: on the computer (no hardware)
| # | Test | Pass condition |
|---|---|---|
| 0.1 | `make -C hardware/pico2w/firmware/tests` | every suite reports 0 failed |
| 0.2 | `python3 -m unittest discover -s tools/tests` | all tests OK |
| 0.3 | `cd ios/VWDiagKit && swift test` | all tests pass |
| 0.4 | `idf.py build` in `hardware/xiao-esp32s3/firmware` and in `hardware/xiao-esp32s3/bench-car` | both build with no warnings in project code |

## Stage 1: bare XIAO modules on USB (before they go on a board)
| # | Test | Pass condition |
|---|---|---|
| 1.1 | `idf.py -p <port> flash` the interface firmware | flashes without pressing BOOT; the port reappears |
| 1.2 | Open the port, `help` | the command list prints |
| 1.3 | `status` | `bus listen-only`, counters 0 |
| 1.4 | `vwdiag status`, then `vwdiag --binary status` | the same, over both protocols; no `\r` in binary mode |
| 1.5 | `ble` | a name `VW-Diag-XXXX`, an address and a six-digit passkey; pairing open (just powered up) |
| 1.6 | `silent off`, `tp`, `silent on` | `- timeout` after about 1 s, no reset (nothing is connected yet) |
| 1.7 | Flash the second module with the bench car, open its port, `help` and `status` | its own command list; scenario `realistic` |

## Stage 2: boards, power only (no module fitted)
Current-limited bench supply on J1 (12 V on pin 1, ground on pin 4). Do this before fitting a module.

| # | Test | Pass condition |
|---|---|---|
| 2.1 | Visual: D5, D6, D7 orientation; U3, U4, U5 pin 1; LED orientation (chamfer at DIN); JP1 as planned | matches the silkscreen |
| 2.2 | Resistance from +12V, VOBD, +5V and VSYS to GND | no shorts |
| 2.3 | 12 V, limit 100 mA, S3 off | under 1 mA; +5V at 0 V |
| 2.4 | S3 on | +5V 4.87 to 5.13 V (feedback 158k / 30.1k on 0.8 V); a few mA |
| 2.5 | VSYS (module socket 5V pin) | about 0.3 V under +5V (D5) |
| 2.6 | Sweep the input 9 to 16 V | +5V stays in range |
| 2.7 | About 10 Ω on VSYS (0.5 A) for a minute | +5V in range; U3 and L1 warm, not hot |
| 2.8 | Scope on +5V under that load | a few tens of mV of ripple, no slow oscillation (the only check of the loop's stability) |
| 2.9 | Reverse the input briefly, limit 100 mA | almost no current, nothing warms |
| 2.10 | D10 pad on the socket with 12.0 V in | 12.0 × 22 / 122 = 2.16 V |

## Stage 3: boards with modules
| # | Test | Pass condition |
|---|---|---|
| 3.1 | USB only, no 12 V | console works; VSYS at least 4.5 V (the transceiver's minimum; it runs from VSYS) |
| 3.2 | 12 V only (S3 on), no USB | boots; LED chain lights |
| 3.3 | USB and 12 V together, then unplug either | no glitch, keeps running |
| 3.4 | LED chain at power-up | status LED amber (listen-only); `silent off` turns it green |
| 3.5 | Press MODE | `ble` shows pairing open for two minutes |
| 3.6 | Card in: `log on`, wait 10 s, `log off`, `log ls` | `LOG00001.VWL` listed with a size; logging LED on while recording |
| 3.7 | `vwdiag files all` | the log copies to `./logs` and `vwdiag logread logs/LOG00001.VWL` reads cleanly |
| 3.8 | Pull the card while logging | logging stops, LED off; `log on` works after reinserting |

## Stage 4: two boards on the bench bus
Interface board and bench-car board, wired CAN-H, CAN-L and GND through J1, both on USB, **JP1 bridged on
both** (60 Ω between CAN-H and CAN-L with power off). On the bench car: `scenario realistic,gateway=open`.

| # | Test | Pass condition |
|---|---|---|
| 4.1 | Interface: `mon on` | the bench car's broadcast frames (3C0, 0FD, 640, ...) print; `status` rx rising, errors 0 |
| 4.2 | Interface stays listen-only; bench car `status` | the bench car's frames are **not acknowledged** by the interface: its transmit errors rise. With a third node (USB-CAN adapter) attached the errors stop. This is what makes listen-only safe on the car |
| 4.3 | Scope on CTX (D4) or the bus during 4.5 | bit time 2.00 µs; sample point near 80 % if visible |
| 4.4 | `silent off`, then `tp` straight away | `+` answered: the TWAI reinstall loses nothing |
| 4.5 | `vwdiag --transmit scan`, then `info engine` | seven modules present, radar and camera absent; engine code DKFA |
| 4.6 | `vwdiag --transmit misfire` with bench car `scenario gti` | cylinder 3 flagged; P0303 with its freeze frame |
| 4.7 | `vwdiag --transmit session --yes --listen 10 --idle 30 --drive 30` | a complete session folder, no step failed |
| 4.8 | Bench car `scenario lossy`, repeat 4.5 | answers still arrive (retries); interface `status` shows errors counted |
| 4.9 | Bench car sends a burst (`burst 2000`) | interface `status` shows dropped 0 |

## Stage 5: Bluetooth with a phone
| # | Test | Pass condition |
|---|---|---|
| 5.1 | nRF Connect or LightBlue: find `VW-Diag-XXXX`, connect, subscribe to 6E400003 | iOS asks for the passkey; right one pairs, wrong one is refused |
| 5.2 | Write `05 01 01 3E 1F 00` (HELLO) to 6E400002 | an answer notifies back; `ble` shows the phone and the MTU |
| 5.3 | The app (`ios/`), Bluetooth tab | the interface is listed; connect; Interface tab shows the counters |
| 5.4 | App: Modules scan, Misfire, Live, Logs (copy a file) against the bench car | as in the iOS Simulator against the simulator |
| 5.5 | Walk out of range during Live, come back | the app reports the drop; reconnecting works without resetting the board |
| 5.6 | Power-cycle the board, reconnect from the same phone | no pairing prompt (bond kept); a second phone is refused until `ble pair` |

## Stage 6: power and long running
| # | Test | Pass condition |
|---|---|---|
| 6.1 | Leave the bench pair logging for an hour with the bench car on `drive=1` | no resets; `lost` 0; log copies and reads |
| 6.2 | 12 V supply, S3 off | draw under 1 mA: safe to leave plugged into the car |

## Stage 7: on the car, listening only
JP1 **open**. Ignition off when plugging in. The board stays listen-only for the whole stage.

| # | Test | Pass condition |
|---|---|---|
| 7.1 | Plug in, S3 on | powers up; status LED amber; `status` listen-only |
| 7.2 | Experiment `listen` (`docs/logging_experiments.md`) | captures with ignition off and on; `errors` stays 0 |
| 7.3 | Meter on D10, engine off then running | about 2.1 to 2.3 V off, 2.4 to 2.6 V running |

7.2 answers the open question of how much traffic the gateway lets through to the OBD port.

## Stage 8: on the car, reading
First time the board transmits on the car. Ignition on, engine off, car parked. Everything here only reads.

| # | Test | Pass condition |
|---|---|---|
| 8.1 | `vwdiag --transmit scan` | at least the engine answers; note which modules do (it corrects the address table) |
| 8.2 | `vwdiag --transmit info engine` | the car's VIN; engine code DKFA if 0xF1AD is answered |
| 8.3 | `vwdiag --transmit dtc engine --details` | the engine's codes; compare with another scan tool if you have one |
| 8.4 | Experiments `identify`, `cold-start`, `warm-idle` (see `docs/logging_experiments.md`) | complete experiment folders |
| 8.5 | Watch the dash throughout, then `dtc` again | no new warnings, no new codes |

If nothing answers in 8.1: `status` for `tx` rising and `errors`; CAN-H and CAN-L not swapped; then
`x 7DF 7E8 01 00`, the legislated request every car must answer.

## Stage 9: later, with care
Extended sessions with keepalive, clearing codes (it erases the freeze frames the misfire hunt needs: save them
first), and anything that writes. The tools refuse writes on purpose; `raw` will send whatever it is given.

## What to record
Every console output and log file, and the experiment folders. What feeds back into the code: which modules
answer and at which addresses (8.1), their identification (8.2), how much traffic the OBD port carries (7.2), and
the experiment results, which calibrate the simulator (`vwdiag calibrate`).
