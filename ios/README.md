# iOS app (Phase 2 skeleton)

A SwiftUI app for iPhone and iPad that talks to the interface over Bluetooth LE, or to the simulator over TCP
while there is no hardware. The protocol and the VW knowledge live in a Swift package with no UI, so they are
tested on their own.

| Path | What it is |
|---|---|
| `VWDiagKit/` | Swift package: packet framing, the protocol client, UDS / OBD / misfire decoding, engine measuring values (Simos 18), the logging experiments, TCP and Bluetooth transports |
| `VWDiag/` | The app. `project.yml` is an XcodeGen spec; the Xcode project is generated, not checked in |

## Screens
| Tab | What it does |
|---|---|
| Interface | Connect to the simulator (host and port) or scan for a board over Bluetooth; listen-only switch; counters; on the dongle, its power: why it woke, the battery, what keeps it awake, stay awake, idle time, sleep now |
| Modules | Autoscan: every known module, present or not, part number, fault count. Tap in for identification, fault codes with status and freeze frame, and Clear (asks first; it changes the car) |
| Live | Pick standard OBD values and engine measuring values (the Simos 18 list: knock retard per cylinder, misfire total, cams, rail pressures...), see them and a graph, recorded to CSV and shareable |
| Misfire | Misfires per cylinder (this drive and the ten-drive average), trims, warning lamp, stored and pending codes, freeze frame |
| Visit | The logging experiments of `docs/logging_experiments.md`, guided: what state the car should be in, a Ready prompt at each step, progress, marks and Finish for the drive. Results are saved in the same folder format as `vwdiag experiment` and shared as a zip, so `vwdiag calibrate` reads them unchanged. Also the card: recording, marks, and copying files to the phone |

The interface powers up listen-only; the app turns sending on when a screen first needs to read from a module.
Nothing writes to the car except Clear fault codes, behind a confirmation.

## Running it
```bash
make -C hardware/pico2w/firmware/sim
VWDIAG_BLE_PORT=5555 VWSIM=gti hardware/pico2w/firmware/sim/vw_diag_sim    # the car, with cylinder 3 misfiring
```
```bash
cd ios/VWDiag && xcodegen && open VWDiag.xcodeproj
```
Run on an iPhone simulator and connect to `127.0.0.1:5555` (the defaults). On a real phone, use the Mac's address
on the same network; to run on a phone, pick your team under Signing in Xcode.

## Tests
```bash
cd ios/VWDiagKit && swift test
```
- Framing: the CRC check value and the worked example from `hardware/pico2w/docs/link_protocol.md`, round trips,
  delivery in pieces of any size, damaged packets.
- Replay: byte streams recorded from the simulator (`Tests/VWDiagKitTests/Fixtures/record.py` writes
  `simulator.json`) played back in random pieces: hello, status, listen-only refusal, multi-frame VIN, negative and
  missing answers, fault codes, snapshot, OBD services 01/02/03/06, monitored frames between replies.
- Simulator: on the Mac the tests also start the real simulator and run a scan, the misfire scenario, the engine
  measuring values and card copying over TCP (skipped if it is not built).
- Experiments: identify, listen and warm-idle run from Swift against a stand-in car with known numbers, then
  `vwdiag calibrate` (Python) reads the folders the app wrote and must recover those numbers.

`make_simos18.py` regenerates `Simos18.swift` from the tools' list (`tools/vwdiag/data/simos18_parameters_22.csv`),
and `make_dtc.py` regenerates `DTCDescriptions.swift` from the tools' fault-code lists, so the app and the tools
describe every code the same way.

## Not yet
- **Bluetooth on a board.** `BLETransport` follows the board's service (Nordic UART layout, pairing with a
  passkey) but has never met real hardware; the simulator has no radio.
- Engine measuring values (the Simos 18 list), sessions and dashboards: later phases.
