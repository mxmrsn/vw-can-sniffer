# Link protocol (USB now, Bluetooth LE next)

How a program or phone app talks to the interface. It is a small binary packet protocol designed so the same
bytes work over the USB serial port today and over Bluetooth LE once the radio is brought up. The firmware side is
`firmware/src/frame.c` and `proto.c`; the Python side is `tools/vwdiag/binlink.py`.

Status: implemented over both links. Tested against the host simulator, including delivery in 7-byte pieces and
through a TCP stand-in that imitates an iPhone's notification size and throughput. **Not yet run on a board**, so
the Bluetooth service has never been exercised against real hardware or a real phone.

The text console (`help`, `mode m`) is unchanged and remains the easiest thing to use by hand.

## Entering and leaving
The console starts in text mode. Send the line `mode b`; the interface answers `binary mode` and a newline, and
from then on speaks only packets. The `EXIT` command returns it to text mode.

A program that cannot know what state the console was left in should send a zero byte, an `EXIT` packet, a
newline and then `mode b`, and discard everything up to and including `binary mode\n`. That works from either
state.

Over BLE there will be no text mode: the BLE service carries packets from the first byte.

## Packets
A packet is the payload followed by a CRC, encoded so it contains no zero bytes, then one zero byte:

```
COBS( payload + CRC-16 high byte + CRC-16 low byte )  00
```

- **CRC**: CRC-16/CCITT-FALSE (polynomial 0x1021, start 0xFFFF, no reflection) over the payload. Check value:
  the ASCII text `123456789` gives 0x29B1.
- **COBS** (consistent overhead byte stuffing) removes zero bytes at a cost of one byte per 254.
- The zero byte marks the end of a packet. A receiver that joins mid-stream or sees damage simply waits for the
  next zero. Empty packets (two zeros in a row) are ignored.
- Payloads are 1 to 4160 bytes: a maximum-size 4095-byte diagnostic message plus headers.
- Packet boundaries have nothing to do with how the transport chops up the stream. A packet may arrive spread
  over many BLE notifications, or several packets in one USB read.

Damaged packets are dropped without a reply, since the sequence number cannot be trusted. The sender notices by
timing out and should send again. The interface counts them (see `STATUS`).

## Messages
Multi-byte numbers are little-endian.

| Kind | Payload |
|---|---|
| Request (app → interface) | `seq` `command` arguments… |
| Response (interface → app) | `seq` `command + 0x80` `status` data… |
| Event (interface → app, unrequested) | `00` `event` data… |

`seq` is 1 to 255, chosen by the app and echoed in the response. 0 is reserved for events. The interface handles
one request at a time and answers each one exactly once.

### Status codes
| Value | Name | Meaning |
|---|---|---|
| 0 | OK | Data follows as described per command |
| 1 | BAD_ARGS | Arguments had the wrong length or value |
| 2 | UNKNOWN_CMD | Command not recognised |
| 3 | SILENT | The interface is listen-only, so nothing was sent to the car |
| 4 | TIMEOUT | The control module did not answer |
| 5 | NEGATIVE | The control module refused; data is one byte, the UDS negative response code |
| 6 | TRANSPORT | ISO-TP failure; data is one signed byte, the error code from `isotp.h` |
| 7 | STORAGE | No card, or the file could not be created, written or read |
| 8 | BUSY | That file is being recorded; stop logging before reading it |

### Commands
| Code | Name | Arguments | Data in an OK response |
|---|---|---|---|
| 01 | HELLO | none | protocol version (u8, currently 1), largest payload (u16), then the firmware name as ASCII |
| 02 | STATUS | none | flags (u8), then six u32 counters: frames received, receive drops, frames sent, bus errors, damaged packets, events dropped |
| 03 | SET_SILENT | `00` or `01` | none. 01 = listen-only, the power-up state |
| 04 | SET_MONITOR | `00` or `01` | none. While on, every received CAN frame is sent as an event |
| 05 | UDS | request ID (u32), response ID (u32), timeout in ms (u16, 0 = default 1000), request bytes | the positive response bytes, starting with the service ID + 0x40 |
| 06 | SET_KEEPALIVE | `00` or `01` | none. While on, TesterPresent goes to the last-used module every 2 s |
| 08 | SET_LOG | `00` or `01` | logging state, as for LOG_STATUS |
| 09 | SET_TIME | Unix seconds (u32) | none. The board has no clock; this timestamps log files |
| 0A | LOG_STATUS | none | active (u8), frames written (u32), bytes written (u32), frames lost (u32), then the file name as ASCII |
| 0B | POLL_ADD | request ID (u32), response ID (u32), period in ms (u32, at least 20), request bytes (1 to 7) | none. Up to 8 entries; BAD_ARGS when full |
| 0C | POLL_CLEAR | none | none |
| 0D | LOG_MARK | text, up to 200 bytes | none. BAD_ARGS when not logging |
| 0E | LOG_LIST | first index (u16), optional, default 0 | total files (u16), entries in this answer (u8), then per entry: size (u32), name length (u8), name. One answer holds at most about 1000 bytes of entries; ask again from the next index for the rest |
| 0F | LOG_READ | offset (u32), most bytes wanted (u16, 0 = as many as allowed), file name | file size (u32), then up to 4000 bytes from the offset (none past the end). BAD_ARGS for a name that is not a plain 8.3 name; BUSY for the file being recorded; STORAGE if there is no card or no such file |
| 10 | POWER | none, or an operation (u8) and a value (u32): 1 stay awake (0/1), 2 idle seconds (30 to 86400), 3 battery cutoff mV (10500 to 13000), 4 sleep now | what keeps it awake (u8 bits: 0 USB, 1 phone, 2 activity, 3 engine, 4 bus, 5 logging, 6 stay), wake reason (u8: 0 unknown, 1 power-on, 2 USB, 3 CAN, 4 engine, 5 button), sleep reason (u8), stay awake (u8), battery mV (u16), seconds until it sleeps (u16, 0 = not counting), seconds the battery has been low (u16), idle seconds (u32), cutoff mV (u16), engine-running mV (u16). UNKNOWN_CMD on boards that do not sleep; BAD_ARGS for a value out of range |
| 11 | GNSS | none, or the sync output rate in Hz (u16: 0 off, 1 to 100) | flags (u8: 0 receiver present, 1 fix, 2 pulse locked, 3 drift valid, 4 log clock set from GPS), fix quality (u8), satellites (u8), zero (u8), UTC second (u32), milliseconds (u16), latitude and longitude (i32 each, degrees x 10^7), altitude cm (i32), speed cm/s (u16), course 0.01 degree (u16), HDOP x 100 (u16), clock drift ppb (i32, positive: the interface's clock runs fast), sync Hz (u16), pulses (u32), pulses paired (u32), sync edges sent (u32), sentences (u32), bad sentences (u32): 54 bytes. UNKNOWN_CMD on boards without the GPS / sync connector; BAD_ARGS for a rate over 100 |
| 12 | SELFTEST | flags (u8, optional: bit 0 active, bit 1 force) | number of checks (u8), then per check: result (u8: 0 pass, 1 warn, 2 fail, 3 skip, 4 look), id length (u8), id (ASCII: `board`, `firmware`, `can`, `can_loop`, `card`, `bluetooth`, `supply`, `gps`, `pps`, `sync`, `leds`), detail length (u8), detail (ASCII). Takes about 1.5 s (the LEDs light in turn). Active also sends one CAN loopback frame and pulses the sync output; the loopback is skipped while the bus has traffic or, on the dongle, the OBD port has battery voltage, unless forced. See `src/selftest.h` |
| 7F | EXIT | none | none, then the console is back in text mode |

Flags in `STATUS`: bit 0 listen-only, bit 1 monitor on, bit 2 keepalive on, bit 3 logging. Monitoring, the
damaged-packet count and the dropped-event count belong to the link asking, not to the interface: USB and a phone
can be connected at once, and each sees only its own. Everything else is shared. Logging and the
poll list are described in `logging.md`.

For `UDS`, IDs above 0x7FF are sent as 29-bit. The interface handles segmentation, flow control and "response
pending" (NRC 0x78) itself; the app sees one request and one final answer. A request can take much longer than
its timeout if the module keeps asking for more time.

### Events
| Code | Name | Data |
|---|---|---|
| 01 | CAN_FRAME | time in ms since power-up (u32), CAN ID (u32, bit 31 set for a 29-bit ID), length (u8), data bytes |

Events can arrive between a request and its response, so an app must sort incoming packets by their first byte.

## Worked example
Read the VIN from the engine control module (request `22 F1 90` to 7E0, answer from 7E8), as request number 2
with the default timeout:

```
payload   02 05  E0 07 00 00  E8 07 00 00  00 00  22 F1 90
on wire   05 02 05 E0 07 01 03 E8 07 01 01 01 06 22 F1 90 AD 12 00
```

The response payload is `02 85 00 62 F1 90` followed by the 17 characters of the VIN.

The smallest packet, `HELLO` as request number 1, is `05 01 01 3E 1F 00` on the wire.

## Bluetooth LE
Implemented in `firmware/src/ble_pico.c` (BTstack on the Pico 2 W's CYW43439) and `src/ble_link.c`. Never run on
hardware.

- **Service**: the Nordic UART Service layout, which iOS's CoreBluetooth and most BLE tools already understand.

  | | UUID | Use |
  |---|---|---|
  | Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | |
  | RX characteristic | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | App writes packets here (write without response) |
  | TX characteristic | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Interface sends packets here (notify) |

- The two characteristics are one byte stream in each direction, carrying exactly the packets above. A write or
  notification holds whatever fits in the negotiated size (20 bytes at worst, around 180 to 240 with iOS), and
  the framing reassembles it. There is no text mode on Bluetooth: packets flow from the first byte.
- **Advertising**: the name is `VW-Diag-XXXX`, ending in the last two bytes of the Bluetooth address, with the
  service UUID in the advertisement.
- **Throughput**: BLE on iOS moves very roughly 2 to 10 kB/s. Diagnostic requests fit comfortably. A full
  500 kbit/s bus monitor does not (a busy bus is over 30 kB/s as events), so monitoring over Bluetooth drops
  frames; full captures belong on the microSD card. Queued events are capped at 2 kB, so what a phone receives
  is recent rather than minutes stale, and a reply is never stuck behind a long backlog. Dropped events are
  counted and reported by `STATUS`. An ID filter command is still to be designed.
- **Access control**: anyone in radio range could otherwise send diagnostic requests to the car, so both
  characteristics require an authenticated, bonded LE Secure Connections link with a 16-byte key. An unpaired
  phone can see the service but can neither write nor subscribe. Pairing uses a six-digit passkey that is fixed
  per board (derived from its unique chip ID) and shown by the console's `ble` command. New phones may pair only
  during a two-minute window: after power-up, after the MODE button is pressed, or after `ble pair`. Bonded
  phones reconnect at any time; `ble forget` erases them. The interface still powers up listen-only.
- **Still to do on hardware**: everything. Pairing from an iPhone, the negotiated MTU, throughput, range, and
  behaviour when the phone walks away mid-request are all unverified.

### Testing it without hardware
The simulator can stand in for the radio: it serves the same packets over TCP on localhost, with an imitated
notification size and throughput, so an app can be developed against it.

```bash
python3 -m vwdiag --sim-ble status                      # the tools, through the stand-in
VWDIAG_BLE_PORT=5555 hardware/pico2w/firmware/sim/vw_diag_sim   # for an app to connect to
```

`VWDIAG_BLE_MTU` (default 185, as iOS negotiates) and `VWDIAG_BLE_RATE` (default 20000 bytes a second) set what
is imitated. A connected client counts as paired and subscribed; there is no pairing step in the stand-in.
