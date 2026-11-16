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
- On the dongle with a GPS receiver on its GPS / sync connector: every GPS pulse with the UTC second it marks,
  every fix (position, speed, course, satellites), and every edge sent on the camera sync output. Pulses and
  edges carry microsecond stamps, and the first paired pulse sets the wall-clock time, so a phone is not needed
  for that (see "GPS time" below).

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

Logs can be copied off the card without taking it out, over USB or Bluetooth (the file being recorded only once
recording stops):

```bash
python3 -m vwdiag files                  # what is on the card
python3 -m vwdiag files all              # copy every log into ./logs (files already copied are skipped)
python3 -m vwdiag files LOG00003.VWL --out drives
```

Over USB that runs at the console's speed; over Bluetooth expect a few kB a second, so a long busy log is quicker
off the card itself. The iOS app does the same from its Logs tab.

Reading a log, however it was copied, needs no board:

```bash
python3 -m vwdiag logread LOG00003.VWL                       # summary: start time, length, frames, losses, marks
python3 -m vwdiag logread LOG00003.VWL --csv drive.csv       # OBD live values as a spreadsheet
python3 -m vwdiag logread LOG00003.VWL --candump drive.log   # can-utils format for SavvyCAN, python-can, cantools
python3 -m vwdiag logread LOG00003.VWL --decode              # VW broadcast messages, via the MQB database
python3 -m vwdiag logread LOG00003.VWL --ids --frames        # per-ID counts, and every frame
python3 -m vwdiag logread LOG00003.VWL --gpx drive.gpx       # the GPS track (dongle with a GPS receiver)
python3 -m vwdiag logread LOG00003.VWL --sync-csv sync.csv   # each camera sync edge with its UTC time
```

From the console: `log on`, `log off`, `log`, `log mark <text>`, `log ls`, `log get <file> <offset> <length>`,
`time <unix seconds>`, `poll <req> <resp> <ms> <hex bytes>`, `poll list`, `poll clear`. The binary protocol has the
same operations (commands 08 to 0F in `link_protocol.md`).

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
- FAT32 and exFAT cards both work. Files can be copied off over the link (above) or by taking the card out.
  Only plain 8.3 names in the top folder can be read that way: no paths.

## GPS time
The dongle stamps everything with one microsecond clock: CAN frames (to the millisecond), GPS pulses and sync
edges (to the microsecond, as the interrupt saw them). Each GPS pulse marks the start of a UTC second, and the
receiver's time sentence for that second follows within a second; the firmware pairs the two. In a log, each
paired pulse is an exact (local microseconds, UTC) point, and between them the reader draws straight lines, so
any stamp in the file converts to UTC to within the pulse stamp's accuracy (a few microseconds, plus the
receiver's own pulse error, typically tens of nanoseconds). The local crystal's drift (tens of ppm, changing with
temperature) drops out, because each second is measured. `logread` prints that drift.

The **sync output** is a square wave at 1 to 100 Hz (`sync <hz>` on the console, `vwdiag gps --sync <hz>`, or the
app). Each GPS pulse restarts it with a rising edge, so with a fix the rising edges fall on the GPS second and
equal divisions of it; every rising edge is logged with its number. A camera or IMU rig that triggers on it, or
records it, can then be lined up with the CAN data exactly: `--sync-csv` lists edge number, local microseconds
and UTC for each edge. Without GPS it runs from the local clock and the edges are still logged.

The receiver is any module that sends NMEA RMC (and ideally GGA) at 9600 baud by default (`gps baud <n>` to
change, for example in `AUTORUN.TXT`) and has a pulse-per-second output: u-blox M8 or M10 modules are typical.
Set its rate (10 Hz suits drive replay) once with the maker's tool; it keeps it.

## File format (version 2)
All numbers are little-endian. Times are milliseconds since the board powered up, cut to 32 bits (they wrap after
49.7 days). Version 2 added records 05 to 07; readers of version 1 files need nothing new.

Header, 32 bytes:

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | `VWLG` |
| 4 | 1 | Format version, 2 |
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
| 05 | GPS pulse | Edge time in microseconds since power-up (8 bytes), the UTC second it marks (4 bytes, 0 if it could not be paired with a time sentence). The record's time is the edge time / 1000 |
| 06 | GPS fix | UTC second (4), milliseconds (2), flags (1: bit 0 valid), quality (1), satellites (1), latitude and longitude (4 each, degrees x 10^7, signed), altitude cm (4, signed), speed cm/s (2), course 0.01 degree (2), HDOP x 100 (2), zero (2): 29 bytes. The record's time is when the sentences were read |
| 07 | Sync edge | Edge time in microseconds since power-up (8 bytes), edge number (4 bytes, from 1 at power-up) |

Files are named `LOG00001.VWL` upward; the board picks the number after the highest one on the card.
