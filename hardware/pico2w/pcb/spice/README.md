# Regulator simulation (U3, AP63301)

`buck_power_stage.py` runs an ngspice switching simulation of the 5 V buck's power stage. Output of the last run is
in [`results.txt`](./results.txt).

## What it covers
- The two internal switches (datasheet on-resistances), L1 = 6.8 µH, C4/C5 derated for DC bias, 500 kHz.
- Input 9 V, 12 V, 14.4 V and 29 V (the TVS clamp level); load 50 mA to 1 A.
- The feedback set-point tolerance, calculated from the divider.

## What it does not cover
There is no SPICE model of the AP63301 in this simulation. The duty cycle is fixed, so **loop stability, load-step
response, soft-start and current limiting are not simulated**. For those the design relies on using the exact
component values from the datasheet's 5 V row (158 k / 30.1 k, 47 pF, 6.8 µH, 2 × 22 µF).

Three inputs are estimates, not datasheet values: inductor DCR (0.1 Ω), output capacitance under bias (12 µF each)
and capacitor ESR (5 mΩ).

## Results
| Check | Result | Limit | Verdict |
|---|---|---|---|
| Output set-point | 5.00 V nominal, 4.87–5.13 V worst case | — | OK |
| VSYS after D5 (~0.3 V drop) | about 4.57 V worst case | TJA1051 VCC ≥ 4.5 V | OK, thin margin |
| Inductor ripple current, 9–14.4 V in | 0.65–0.97 A p-p | datasheet target 0.9–1.5 A | OK |
| Peak inductor current at 0.5 A load | ≤ 0.98 A (1.11 A at 29 V) | inductor Isat 2.0 A | OK |
| Peak inductor current at 1 A load | ≤ 1.48 A (1.78 A at 29 V with L −20 %) | inductor Isat 2.0 A | OK |
| Inductor RMS current at 1 A load | 1.06 A | rated 1.45 A | OK |
| Output ripple | 7–13 mV p-p (16 mV worst) | — | OK |
| Minimum on-time at 29 V in | 345 ns | 80 ns | OK |

## Things the numbers show
- **Light load:** the AP63301 is forced-PWM, so the inductor current goes negative below about 0.45 A load. That is
  normal for this part and is why it draws more at idle than a PFM regulator.
- **Overload:** the inductor saturates at 2 A but the regulator's own current limit is 4.1–4.9 A, so the inductor
  is not protected by the chip in a hard overload. The 0.75 A input fuse limits sustained output to roughly 1.5 A.
  Expected load is under 0.5 A.
