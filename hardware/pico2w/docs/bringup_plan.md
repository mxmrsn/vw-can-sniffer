# Hardware bring-up plan

What to test, in what order, once a Pico 2 W and then the v0.2.0 board arrive. Each stage only depends on the
ones before it, and each has a clear pass condition. Stop at the first failure and fix it before moving on.

Everything in stages 0–2 can be done with a bare Pico 2 W and a CAN transceiver breakout, before the board exists.

Commands shown as `vwdiag ...` mean `python3 -m vwdiag --port <port> ...` run from the `tools/` directory.
Commands in `this style` without `vwdiag` are typed into the board's serial console.

## Stage 0: tools on the computer (no hardware)
Already passing; re-run after any change.

| # | Test | Pass condition |
|---|---|---|
| 0.1 | `make -C hardware/pico2w/firmware/tests` | 54 checks, 0 failed |
| 0.2 | `python3 -m unittest discover -s tools/tests` | all tests OK |
| 0.3 | Build the firmware (see `hardware/pico2w/firmware/README.md`) | `build/vw_diag.uf2` produced, no warnings in project code |

## Stage 1: bare Pico 2 W on USB
Proves the firmware boots and the console and tool link work. No CAN hardware attached.

| # | Test | Pass condition |
|---|---|---|
| 1.1 | Hold BOOTSEL, plug in, copy `vw_diag.uf2` | Pico reboots and a USB serial port appears |
| 1.2 | Open the port in a terminal, type `help` | Command list prints |
| 1.3 | `status` | `bus listen-only`, all counters 0 |
| 1.4 | `vwdiag status` | Same information, no "no reply" error |
| 1.5 | `silent off`, then `tp` | `- timeout` after about 1 s (nothing is connected, so this is correct) |

If 1.5 hangs or the Pico resets, the CAN driver is misbehaving with no transceiver attached; note it and continue
with a transceiver connected.

## Stage 2: Pico plus transceiver, on the bench
Proves CAN bit timing and the transport layer against real hardware, without the car. Needs a 3.3 V-compatible
CAN transceiver breakout wired to GP17 (TX), GP18 (RX) and GP16 (silent), and a second CAN node that acknowledges
frames: a USB CAN adapter, or a second Pico running this firmware. Fit 120 Ω at both ends of the bench bus.

| # | Test | Pass condition |
|---|---|---|
| 2.1 | Second node sends frames at 500 kbit/s; `mon on` | Frames print with the right ID and data; `status` shows `rx` rising and `errors` 0 |
| 2.2 | `silent off`, `tp` with the second node only listening | Second node sees `7E0 [8] 02 3E 00 55 55 55 55 55`; `tx` rises |
| 2.3 | Scope or logic analyser on GP17 during 2.2 | Bit time 2.0 µs (500 kbit/s) |
| 2.4 | Second node replies `7E8 [8] 02 7E 00 ...` to the request | `tp` prints `+ ECU answered` |
| 2.5 | Second node sends a burst of several hundred frames | `status` shows `dropped 0` |
| 2.6 | `silent on`, then send from the second node with no other node present | Second node reports no acknowledgement: confirms listen-only really does not acknowledge |

2.6 matters for the car: it is what makes listen-only safe.

## Stage 3: the v0.2.0 board, on the bench, no Pico fitted
Power-path checks with a current-limited bench supply. Do these before plugging in a Pico.

| # | Test | Pass condition |
|---|---|---|
| 3.1 | Visual check: D5/D6/D7 orientation, U3 and U4 pin 1, JP1 open | Matches the silkscreen and layout |
| 3.2 | Resistance from +12V, +5V, VSYS and +3V3 to GND | No shorts |
| 3.3 | 12 V on J1 pin 1, limit 100 mA, S3 off | Draw under 1 mA; +5V rail at 0 V |
| 3.4 | S3 on | +5V rail 4.87–5.13 V; draw a few mA |
| 3.5 | Measure VSYS | About 0.3 V below the +5V rail |
| 3.6 | Sweep the input 9 V to 16 V | +5V stays in range |
| 3.7 | Load VSYS with about 10 Ω (0.5 A) for a minute | +5V in range; U3 and L1 warm, not hot |
| 3.8 | Scope on +5V under that load | Ripple of a few tens of mV, no low-frequency oscillation |
| 3.9 | Reverse the 12 V input briefly, limit 100 mA | Almost no current; nothing gets warm |

3.8 is the check the simulation could not do: it is the only evidence of regulator loop stability.

## Stage 4: the v0.2.0 board with the Pico fitted
| # | Test | Pass condition |
|---|---|---|
| 4.1 | USB only, no 12 V | Console works; +3V3 present at U4 pin 5 |
| 4.2 | 12 V only, no USB | Pico boots (LEDs); VSYS about 4.7 V |
| 4.3 | USB and 12 V together | No fault; unplugging either leaves the board running |
| 4.4 | Each LED and both buttons | LEDs light on the documented pins; buttons read high when pressed |
| 4.5 | Repeat 2.1–2.6 through J1 | Same results as on the breakout |
| 4.6 | Read GP28 with 12.0 V on J1 | ADC reads 12.0 V ÷ 5.545 ≈ 2.16 V |
| 4.7 | microSD: FAT32 card in, `log on`, wait 10 s, `log off`; copy the file to a computer and run `vwdiag logread` on it | A `LOG00001.VWL` that reads cleanly; Wi-Fi LED on while logging |
| 4.8 | Pull the card while logging | Logging stops and the LED goes off; `log on` works again after reinserting |
| 4.8a | `ble` on the console | A name, address and six-digit passkey; "no phone" |
| 4.8b | Pair from an iPhone with a BLE tool (nRF Connect, LightBlue): it should ask for the passkey | Pairing succeeds with the right passkey, is refused with a wrong one |
| 4.8c | Subscribe to 6E400003 and write a HELLO packet (`05 01 01 3E 1F 00`) to 6E400002 | The interface answers; `ble` shows the phone connected and the MTU |
| 4.8d | `python3 -m vwdiag --sim-ble` equivalents through the phone, then walk out of range mid-request | The link drops cleanly; reconnecting works without a reset |
| 4.8e | Power-cycle, then reconnect from the same phone without pairing again | It connects (the bond is kept in flash); a second, unpaired phone is refused until `ble pair` |
| 4.9 | Put the `AUTORUN.TXT` example from `logging.md` (without `silent off` and the `poll` lines) on the card and power up from 12 V only | A new log starts by itself |

## Stage 5: on the car, listening only
The board stays in listen-only for this whole stage, so it cannot disturb the car. JP1 must be open.

| # | Test | Pass condition |
|---|---|---|
| 5.1 | Ignition off, plug into the OBD port, S3 on | Board powers; `status` shows `listen-only` |
| 5.2 | `vwdiag monitor --seconds 10 --out idle.log`, ignition off | Record what appears (possibly nothing) |
| 5.3 | Same with ignition on, engine off | Record; `errors` stays 0 |
| 5.3a | `vwdiag decode idle.log --summary` on each capture | Shows which known messages appear, and lists IDs not in the database |
| 5.3b | `vwdiag log start`, drive or idle a few minutes, `vwdiag log stop`; `logread --ids` on the file | The same picture as 5.3 over a longer time; `lost` is 0 |
| 5.4 | Read GP28 with engine off, then running | About 12.0–12.8 V off, 13.5–14.5 V running |

5.2 and 5.3 answer the open question of how much traffic the gateway lets through to the OBD port.

## Stage 6: on the car, reading
First time the board transmits on the car. Ignition on, engine off, car stationary. Every command here only reads.

| # | Test | Pass condition |
|---|---|---|
| 6.1 | `vwdiag --transmit scan` | Engine answers at least. Note which modules answer: this confirms or corrects the address table |
| 6.2 | `vwdiag --transmit info engine` | VIN matches the car; part number looks like `5G0906259x` or similar, software version is 4 digits |
| 6.3 | `vwdiag --transmit pids` | A list of supported live values |
| 6.4 | `vwdiag --transmit live rpm coolant voltage --count 20` | Engine off: rpm 0, coolant near ambient, voltage about 12 V |
| 6.5 | `vwdiag --transmit dtc engine` | A list (possibly empty); compare with another scan tool if one is available |
| 6.6 | `vwdiag --transmit info` and `dtc` for each module found in 6.1 | Record results; fix any wrong addresses in `vw_ecus.h` and `vw.py` |
| 6.7 | Start the engine, repeat 6.4 with `--csv idle.csv` | rpm 700–900 at idle; voltage 13.5–14.5 V; log file written |
| 6.7a | `vwdiag --transmit log start --poll rpm speed coolant --interval 0.2`, drive, `log stop`, then `logread --csv` | A live-data spreadsheet from the card that matches the dash |
| 6.8 | Watch the dash throughout | No warning lights, no new fault codes afterwards (repeat 6.5) |

If nothing answers in 6.1: check `status` for `tx` rising and `errors`, check CAN-H/CAN-L are not swapped, and
try `x 7DF 7E8 01 00` (the legislated OBD request, which every car must answer).

## Stage 7: later, with care
Not to be attempted until stage 6 is solid.

- Extended diagnostic session (`session 03`) with `keepalive on`, then read data that needs it.
- Clearing fault codes (`vwdiag --transmit clear <module> --yes`). This erases stored codes and freeze frames.
- Anything that writes (adaptations, coding, security access). The tools have no support for these on purpose,
  and the `raw` command will send them if asked. Know exactly what a request does first.

## What to record
For each stage, keep the console output and any log files. The results that feed back into the code are:
the list of modules that answer and their addresses (6.1, 6.6), which identification items each module returns
(6.2), which live values the engine supports (6.3), and how much traffic the OBD port carries (5.2, 5.3).
