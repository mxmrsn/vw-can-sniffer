# Logging mode

The board can record every CAN frame it sees to the microSD card, with or without a computer or phone attached.
Each log is a new file on the card; nothing is ever overwritten.

Status: implemented and tested end to end in the simulator, where a folder stands in for the card. The Pico build
compiles with the microSD driver, but **nothing has been run on a real card yet** (bring-up step 4.7).

## What goes in a log
- Every frame received from the bus, and every frame the board itself sends (requests, flow control,
  keepalives), each with a millisecond timestamp.
- Notes ("marks") you add while driving, for example "full throttle now".
- The wall-clock time, if a computer or phone told the board the time. The board has no clock of its own; without
  this, times are relative to power-up only.
- A count of any frames that could not be recorded, at the point where they were lost.

**Live data needs requests.** The OBD port sits behind the car's gateway, which may pass little or nothing except
answers to diagnostic requests (see `docs/vw_can_data_sources.md`). So a log taken purely by listening may be
nearly empty. The board can repeat up to 8 short requests on a timer (the "poll list"), for example OBD
engine speed every 100 ms, and the answers then land in the log. Polling transmits, so it needs listen-only off.

## Using it
With the Python tools (the board must be connected; the simulator also works with `VWDIAG_SD=<folder>`):

```bash
python3 -m vwdiag log start                                         # record what is on the bus
python3 -m vwdiag --transmit log start --poll rpm speed coolant --interval 0.2   # plus live data
python3 -m vwdiag log mark "on the highway"
python3 -m vwdiag log status
python3 -m vwdiag log stop
```

`log start` also tells the board the time. Logging and polling carry on after the tools disconnect, until
`log stop`, the card is removed, or power is lost.

Reading a log copied off the card needs no board:

```bash
python3 -m vwdiag logread LOG00003.VWL                       # summary: start time, length, frames, losses, marks
python3 -m vwdiag logread LOG00003.VWL --csv drive.csv       # OBD live values as a spreadsheet
python3 -m vwdiag logread LOG00003.VWL --candump drive.log   # can-utils format for SavvyCAN, python-can, cantools
python3 -m vwdiag logread LOG00003.VWL --decode              # VW broadcast messages, via the MQB database
python3 -m vwdiag logread LOG00003.VWL --ids --frames        # per-ID counts, and every frame
```

From the console: `log on`, `log off`, `log`, `log mark <text>`, `time <unix seconds>`, `poll <req> <resp> <ms>
<hex bytes>`, `poll list`, `poll clear`. The binary protocol has the same operations (commands 08 to 0D in
`link_protocol.md`).

## Logging with nothing attached
At power-up the board runs the commands in `AUTORUN.TXT` on the card, one per line, as if typed. Lines starting
with `#` are ignored. For example, to log engine speed, road speed and coolant temperature whenever the car is on:

```
# AUTORUN.TXT
silent off
poll 7E0 7E8 200 01 0C
poll 7E0 7E8 500 01 0D
poll 7E0 7E8 2000 01 05
log on
```

Leave `silent off` and the `poll` lines out to log by listening only. Remember the board stays powered (and so
keeps polling the engine module) for as long as OBD pin 16 is live, which on most cars is always: switch it off
with S3 when parked. Logs from autorun have no wall-clock time, since nothing told the board the time.

## Indicators
The Wi-Fi LED (GP13) shows logging: on while a log is being written. It goes off if the card fails or is removed,
and logging stops; start it again after reinserting the card.

## Robustness
- Data is written to the card at least every 250 ms and made safe against power loss (`f_sync`) every 2 seconds.
  Pulling the power loses at most the last 2 seconds. A file cut off mid-record is read normally up to that point,
  and `logread` says so.
- An 8 kB buffer covers the card's occasional slow writes. If it fills anyway, frames are counted, not silently
  dropped, and the count is written to the log where the gap occurred.
- Rough sizes: a frame takes 10 to 18 bytes. A fully loaded 500 kbit/s bus is about 4,000 frames a second, about
  60 kB/s or 200 MB an hour, well within what a microSD card can write over SPI. Polling a few values is a few
  kB/s.
- FAT32 and exFAT cards both work. The card is read only by the logger, so take it out to copy logs.

## File format (version 1)
All numbers are little-endian. Times are milliseconds since the board powered up.

Header, 32 bytes:

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | `VWLG` |
| 4 | 1 | Format version, 1 |
| 5 | 1 | Header length, 32. Readers skip to this offset, so later versions can add fields |
| 6 | 2 | Zero |
| 8 | 4 | CAN bit rate |
| 12 | 4 | Time the log started (ms since power-up) |
| 16 | 4 | Wall-clock time at that moment, Unix seconds, or 0 if unknown |
| 20 | 12 | Zero |

Then records, each `type (1 byte)`, `time (4 bytes)`, payload:

| Type | Name | Payload |
|---|---|---|
| 01 | CAN frame | ID (4 bytes), flags (1 byte: bits 0–3 data length, bit 6 sent by the board, bit 7 29-bit ID), data |
| 02 | Lost | Number of frames that could not be recorded since the previous record (4 bytes) |
| 03 | Mark | Text length (1 byte), text (UTF-8, up to 200 bytes) |
| 04 | Time | Wall-clock time, Unix seconds, at this record's time (4 bytes) |

Files are named `LOG00001.VWL` upward; the board picks the number after the highest one on the card.
