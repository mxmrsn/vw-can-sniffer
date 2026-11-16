# Logging experiments: real data from the car, and calibrating the simulator

Seven scripted experiments, each answering specific questions about this car (MK7.5 GTI, DKFA, Simos 18) and each
saving the numbers that set the simulator to match it. They only read from the car. Run them with the XIAO board
once stages 7 and 8 of `hardware/xiao-esp32s3/docs/bringup_plan.md` pass.

```bash
cd tools
python3 -m vwdiag --binary experiment listen                    # listen only: no --transmit needed
python3 -m vwdiag --binary --transmit experiment identify
python3 -m vwdiag --binary --transmit experiment cold-start     # first thing in the morning
python3 -m vwdiag calibrate experiments/*                       # the simulator settings that match
python3 -m vwdiag calibrate experiments/* --verify              # ... and the simulator rerun side by side
```

**From the phone:** the app's Visit tab runs the same experiments, guided, and saves the same folders (in the
Files app, under the app's Experiments folder). Share one as a zip, unzip it on the Mac, and `vwdiag calibrate`
reads it like the tools' own.

Use `--binary` (the packet protocol): it timestamps frames on the interface and catches the modules' flow control,
which the text console cannot. Each run saves `experiments/<date-time>-<name>/` with `meta.json`, `summary.json`,
the sample CSVs and `notes.txt`. Every experiment also runs against the simulator (`--sim`), which is how all of
this was tested.

## The experiments
| Experiment | Car state | Takes | Answers | Calibrates |
|---|---|---|---|---|
| `listen` | Ignition off, then on (prompted) | 1 min | Does any broadcast traffic reach the OBD port? Which IDs, how often | `gateway` |
| `identify` | Ignition on, engine off | 3 min | Who answers; response times; the flow control each module asks for; who answers a functional OBD request; which OBD values, monitors and Simos 18 measuring values the engine has. `--session-test` also times the extended session | `latency`, `fc`, `trans_obd`, `s3`, and the code changes listed |
| `timing` | Ignition on | 1 min | How fast the engine answers one value, and whether it takes several in one request (decides how fast logging can go) | reference |
| `cold-start` | Cold soak (6 hours or more, overnight best), then start and idle | 15 min | Idle speed against coolant temperature, warm-up time, spark retard while the catalyst heats, when closed-loop fuelling starts, trims and misfires while cold | `cold_idle`, `warmup`, `trim_cold`, `mis_cold` |
| `warm-idle` | Fully warm, idling, A/C and lights off | 5 min | The reference idle: speed, airflow, manifold pressure, load, timing, lambda, trims, misfires per 100 firings of each cylinder | `idle`, `misfire`, `mis_idle`, `trim_idle` |
| `rev-holds` | Warm, parked, in P; hold each speed when prompted (default idle, 1500, 2500, 3500 rpm) | 5 min | Whether the weak cylinder still misfires off idle; trims at each speed | `mis_light`, `trim_drive` |
| `drive` | A normal drive | as long as you like | Everything on the card at 4 samples a second (rpm, load, speed, trims, timing, coolant, MAP), with marks; the misfire picture before and after | reference for drive replay |

Options: `--seconds` (each phase), `--interval` (between samples, default 0.5 s), `--holds 0,1500,2500`,
`--yes` (no prompts), `--out` (parent folder).

## Order, the first time
1. `listen` on the first visit, before anything transmits.
2. `identify --session-test`, then `timing`.
3. `warm-idle` after a drive, and `rev-holds` straight after it.
4. `cold-start` the next morning, before the car has been driven.
5. `drive` whenever convenient. Copy the log off with `vwdiag files`.
6. `vwdiag calibrate experiments/* --verify`.

Repeat `warm-idle` and `cold-start` on a few days: the misfire rate of a weak cylinder moves with temperature and
fuel, and the calibration is better for the spread. Run the set again after any repair: comparing before and after
is the clearest evidence it worked.

## Measuring the misfire
The misfire rate is computed the same way on the car and in the simulator: the change in the engine's
per-cylinder counter (OBD service 06, the misfire monitor) over the run, divided by the firings of that cylinder
in the same time, from the engine's own run time (PID 1F) and speed. A healthy cylinder shows 0 %; occasional
single misfires are normal. On this car the comparison that matters is the weak cylinder at warm idle against
cold idle against 1500 to 2500 rpm: a compression problem misfires most at idle and cold, and much less once
cylinder pressure rises with load.

What these runs cannot tell apart is valves, rings or head gasket: the leak-down test does that
(`docs/roadmap.md`, "Your misfire").

## Calibration
`vwdiag calibrate` reads the newest run of each experiment and writes `calibration/<date-time>/`:
- `scenario.txt`: one line of simulator settings, e.g.
  `realistic,gateway=closed,latency=8-31,fc=8/5,trans_obd=1,misfire=3,idle=702,...`. Use it with
  `--sim-profile`, as `VWSIM=` for the simulator, or `scenario ...` on the bench car.
- `calibration.md`: every setting with the experiment and the number it came from, the reference values the
  engine model should match (airflow, manifold pressure, load, timing), and anything that needs a change in
  the simulator's code rather than a setting (modules the car has that the simulator lacks, measuring values
  one answers and the other does not).
- With `--verify`: the same experiments rerun on the simulator with those settings (at a faster engine clock for
  the long ones), and a table of car against simulator with the difference for each measure.

Tested against a stand-in car (the simulator with deliberately different numbers): calibration recovered warm and
cold idle speed within 1 %, the warm-up rate within 3 %, the misfire rate and cylinder, the flow control and the
response times, and the verified simulator matched the stand-in within a few percent on every measure.

## Simulator settings the calibration uses
Beyond the scenario settings in `hardware/pico2w/firmware/sim/sim_config.c`:

| Setting | Meaning | Default (estimate until calibrated) |
|---|---|---|
| `idle`, `cold_idle` | Warm idle, and idle at 15 °C coolant, rpm | 700, 1100 |
| `warmup` | Coolant warm-up rate against the model's own estimate | 1.0 |
| `mis_cold`, `mis_idle`, `mis_light`, `mis_load` | Misfires per 100 firings of the weak cylinder: cold, warm idle, light load, high load | 6.0, 2.5, 0.6, 0.2 |
| `trim_cold`, `trim_idle`, `trim_drive` | Total fuel trim the weak cylinder costs, % | 9.0, 6.5, 2.5 |
| `speed` | Engine time runs this many times faster (for long experiments on the simulator) | 1 |

## Safety
Every experiment only reads, except entering the extended diagnostic session in `identify --session-test`,
which scan tools do routinely and which changes nothing. The interface goes back to listen-only at the end of
every run, including when one fails. `rev-holds` is for a parked car in P: hold the speed with the pedal, no
higher than prompted, engine warm. Do not run anything while driving that needs attention; `drive` only asks
for marks, and a passenger can type them.
