# Link protocol (USB now, Bluetooth LE next)

How a program or phone app talks to the interface. It is a small binary packet protocol designed so the same
bytes work over the USB serial port today and over Bluetooth LE once the radio is brought up. The firmware side is
`firmware/src/frame.c` and `proto.c`; the Python side is `tools/vwdiag/binlink.py`.

Status: implemented and tested over USB serial framing against the host simulator, including delivery in 7-byte
pieces to imitate BLE. Not yet run on a board, and the BLE service itself is not written.

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

### Commands
| Code | Name | Arguments | Data in an OK response |
|---|---|---|---|
| 01 | HELLO | none | protocol version (u8, currently 1), largest payload (u16), then the firmware name as ASCII |
| 02 | STATUS | none | flags (u8), then five u32 counters: frames received, receive drops, frames sent, bus errors, damaged packets |
| 03 | SET_SILENT | `00` or `01` | none. 01 = listen-only, the power-up state |
| 04 | SET_MONITOR | `00` or `01` | none. While on, every received CAN frame is sent as an event |
| 05 | UDS | request ID (u32), response ID (u32), timeout in ms (u16, 0 = default 1000), request bytes | the positive response bytes, starting with the service ID + 0x40 |
| 06 | SET_KEEPALIVE | `00` or `01` | none. While on, TesterPresent goes to the last-used module every 2 s |
| 7F | EXIT | none | none, then the console is back in text mode |

Flags in `STATUS`: bit 0 listen-only, bit 1 monitor on, bit 2 keepalive on.

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

## Bluetooth LE plan
Not implemented yet. The intended design, so the app side can be written against it:

- **Service**: the Nordic UART Service layout, which iOS's CoreBluetooth and most BLE tools already understand.

  | | UUID | Use |
  |---|---|---|
  | Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | |
  | RX characteristic | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | App writes packets here (write without response) |
  | TX characteristic | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Interface sends packets here (notify) |

- The two characteristics are one byte stream in each direction, carrying exactly the packets above. A write or
  notification holds whatever fits in the negotiated size (20 bytes at worst, around 180 to 240 with iOS), and
  the framing reassembles it.
- **Throughput**: BLE on iOS moves very roughly 2 to 10 kB/s. Diagnostic requests fit comfortably. A full
  500 kbit/s bus monitor does not (a busy bus is over 30 kB/s as events), so over BLE monitoring will need
  ID filters or will drop frames, and full captures belong on the microSD card. That filtering command is not
  designed yet.
- **Access control**: anyone in radio range of an unpaired dongle could send diagnostic requests to the car.
  The BLE build should require bonding with a passkey, and should keep powering up listen-only.
- **Firmware work needed**: add the `cyw43-driver` and `btstack` parts of the Pico SDK, a GATT server exposing
  the service, and a second byte source and sink next to the USB console. `proto.c` does not change.
