# VW CAN data: sources and confidence

What the tools and simulator know about the car, where each piece came from, and how far to trust it before it
has been checked on the actual MK7.5 GTI. Nothing here has been confirmed on the car yet.

## Source: comma.ai opendbc
[opendbc](https://github.com/commaai/opendbc) is the open-source vehicle database behind openpilot, MIT licensed.
It is built from logs of real cars, including the "Volkswagen Golf GTI 2015-21" under its Golf MK7 platform entry.
Files used, from commit `27fa1c5d8eed`:

| File | What it gave us |
|---|---|
| `opendbc/dbc/vw_mqb.dbc` | 113 broadcast messages with 1,348 signals, copied to `tools/vwdiag/data/vw_mqb.dbc` |
| `opendbc/car/volkswagen/values.py` | Diagnostic addresses, the response offset, and which identifiers to read |
| `opendbc/car/volkswagen/fingerprints.py` | Real part numbers and software versions from Golf MK7 cars |

## Diagnostic addresses
| Module | Request → response | Basis |
|---|---|---|
| Engine | 7E0 → 7E8 | Legislated OBD address; also used by opendbc |
| Transmission | 7E1 → 7E9 | Legislated OBD address; also used by opendbc |
| Airbag | 715 → 77F | opendbc |
| Power steering | 712 → 77C | opendbc |
| Front radar | 757 → 7C1 | opendbc; a GTI without adaptive cruise will not have it |
| Front camera | 74F → 7B9 | opendbc; only on cars with lane assist |
| ABS / ESC | 713 → 77D | Unsourced; follows the same +0x6A pattern |
| Instrument cluster | 714 → 77E | Unsourced; follows the same +0x6A pattern |
| Gateway | 710 → 77A | Unsourced; follows the same +0x6A pattern |

opendbc confirms the rule that the response ID is the request ID plus 0x6A for everything except engine and
transmission.

One caveat: openpilot connects at the car's gateway connector, not the OBD port. The addresses are the same
either way, but whether every module can be reached through the OBD port is still to be found out.

## Identification data
opendbc reads three identifiers from each module in a single request (`22 F1 87 F1 89 F1 82`), and the module
answers with all three run together:

| Identifier | Meaning | Example from a Golf MK7 |
|---|---|---|
| F187 | Part number, 11 characters | `5G0906259L ` (engine) |
| F189 | Software version, 4 characters | `0002` |
| F182 | Coding / dataset, variable length | `05 "21A00642A1"` (steering) |

Engine and transmission answer with only the first two. The simulator returns real strings from the fingerprint
list for engine, transmission, steering and airbag, and a test checks the exact bytes.

The other identifiers the tools try (VIN F190, hardware number F191, system name F197, serial F18C, ODX file F19E
and so on) are standard UDS identifier numbers that VW is commonly reported to use. They are not in opendbc and are
unconfirmed.

## Broadcast messages
The DBC file decodes the frames modules send continuously on the car's internal buses, for example:

| Message | ID | Useful signals |
|---|---|---|
| `Klemmen_Status_01` | 3C0 | Ignition state |
| `ESP_21` | 0FD | Vehicle speed |
| `Motor_07` | 640 | Intake, oil and coolant temperature |
| `Motor_20` | 121 | Accelerator pedal |
| `Motor_Code_01` | 641 | Cylinders, displacement, power |
| `Diagnose_01` | 6B2 | Odometer, date and time |
| `VIN_01` | 6B4 | VIN, sent in three parts |

`vwdiag monitor --decode` and `vwdiag decode <log>` use it.

**The big unknown:** these frames live on the powertrain and comfort buses. The OBD port is on the diagnostic bus
behind the gateway, which normally passes diagnostic requests and little else. It is quite possible that none of
these frames appear at the OBD port. Stage 5 of `hardware/pico2w/docs/bringup_plan.md` is where that gets answered. If they do not
appear, seeing them needs a tap at the gateway connector, which is how openpilot does it, and that is a different
hardware connection from the OBD plug.

The file is written for the whole MQB platform, so some messages belong to variants this car does not have
(hybrid, diesel, driver assistance).

## Live data through the OBD port
The 15 live values in `tools/vwdiag/obd.py` are standard OBD-II (SAE J1979) requests with their standard
formulas. Every car sold in the US since 2008 must answer these on CAN, so they are the safest bet for first
contact, independent of anything VW-specific.

## What is still invented in the simulator
The VIN, the fault codes, all live values, and every identification string not listed above.

## Not found
I did not find a public, trustworthy list of VW-specific measuring-value identifiers (the equivalent of VCDS
measuring blocks) for this engine, or VW's mapping from fault codes to its own five-digit numbers. Those would
have to come from logging a commercial tool on the car, or from the car itself by trial.
