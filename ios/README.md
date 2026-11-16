# iOS app (Phase 2 skeleton)

A SwiftUI app for iPhone and iPad that talks to the interface over Bluetooth LE, or to the simulator over TCP
while there is no hardware. The protocol and the VW knowledge live in a Swift package with no UI, so they are
tested on their own.

| Path | What it is |
|---|---|
| `VWDiagKit/` | Swift package: packet framing, the protocol client, UDS / OBD / misfire decoding, TCP and Bluetooth transports |
| `VWDiag/` | The app. `project.yml` is an XcodeGen spec; the Xcode project is generated, not checked in |

## Screens
| Tab | What it does |
|---|---|
| Interface | Connect to the simulator (host and port) or scan for a board over Bluetooth; listen-only switch; counters |
| Modules | Autoscan: every known module, present or not, part number, fault count. Tap in for identification, fault codes with status and freeze frame, and Clear (asks first; it changes the car) |
| Live | Pick OBD values, see them and a graph, recorded to CSV and shareable |
| Misfire | Misfires per cylinder (this drive and the ten-drive average), trims, warning lamp, stored and pending codes, freeze frame |
| Logs | Start and stop recording on the interface's card, optionally polling the misfire set; marks; list the card's files and copy them to the phone (Files app), then share |

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
- Simulator: on the Mac the tests also start the real simulator and run a scan and the misfire scenario over TCP
  (skipped if it is not built).

## Not yet
- **Bluetooth on a board.** `BLETransport` follows the board's service (Nordic UART layout, pairing with a
  passkey) but has never met real hardware; the simulator has no radio.
- Engine measuring values (the Simos 18 list), sessions and dashboards: later phases.
