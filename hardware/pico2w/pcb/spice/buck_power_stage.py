#!/usr/bin/env python3
"""Switching simulation of the U3 (AP63301) buck power stage in ngspice.

What this models: the two internal switches (datasheet on-resistances), L1, the output
capacitors derated for DC bias, and a constant-current load, switching at 500 kHz.

What it does NOT model: the AP63301's control loop. No SPICE model of the IC is used. The duty
cycle is fixed at the value that gives 5 V (computed from the conduction losses) and the run
starts from the matching steady-state inductor current, so the circuit is in periodic steady
state from the first cycle. Loop stability, load-step response, soft-start and current limit
are therefore outside this simulation.

Run:  python3 buck_power_stage.py      (needs ngspice on PATH)
"""
import itertools, re, subprocess, tempfile, os

FSW = 500e3          # datasheet typical; 450-550 kHz range
RDS_HS, RDS_LS = 0.075, 0.040   # datasheet typical
L_NOM, L_DCR = 6.8e-6, 0.10     # Wurth 74404042068; DCR is an estimate
C_EFF, C_ESR = 12e-6, 0.005     # each 22 uF 16 V X5R 0805 at 5 V bias: assumed ~55% of nominal
ISAT, IRMS_RATED = 2.0, 1.45    # inductor ratings from the DigiKey listing
I_PEAK_LIMIT = 4.1              # AP63301 high-side peak limit, minimum

NETLIST = """* AP63301 buck power stage, VIN={vin} ILOAD={iload} L={l}
Vin vin 0 {vin}
Vsaw saw 0 PULSE(0 1 0 {tr} 5n 5n {per})
Vduty duty 0 {duty}
Bgh gh 0 V = u(v(duty) - v(saw))
Bgl gl 0 V = 1 - v(gh)
S1 vin sw gh 0 hs
S2 sw 0 gl 0 ls
.model hs SW(Ron={rhs} Roff=10Meg Vt=0.5 Vh=0.05)
.model ls SW(Ron={rls} Roff=10Meg Vt=0.5 Vh=0.05)
L1 sw n1 {l} ic={ivalley}
Rdcr n1 out {dcr}
C4 out c4e {c}
Resr4 c4e 0 {esr}
C5 out c5e {c}
Resr5 c5e 0 {esr}
Iload out 0 {iload}
.ic v(out)=5
.tran 5n 600u 500u 5n uic
.meas tran il_pp PP i(L1) from=500u to=600u
.meas tran il_max MAX i(L1) from=500u to=600u
.meas tran il_min MIN i(L1) from=500u to=600u
.meas tran il_rms RMS i(L1) from=500u to=600u
.meas tran vout_avg AVG v(out) from=500u to=600u
.meas tran vout_pp PP v(out) from=500u to=600u
.meas tran duty AVG v(gh) from=500u to=600u
.end
"""

def run(vin, iload, l):
    per = 1 / FSW
    duty = (5.0 + iload * (RDS_LS + L_DCR)) / (vin - iload * (RDS_HS - RDS_LS))
    ripple = (vin - 5.0 - iload * (RDS_HS + L_DCR)) * duty / (l * FSW)
    net = NETLIST.format(vin=vin, iload=iload, l=l, tr=per - 10e-9, per=per, rhs=RDS_HS, rls=RDS_LS,
                         dcr=L_DCR, c=C_EFF, esr=C_ESR, duty=duty, ivalley=iload - ripple / 2)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "buck.cir")
        open(path, "w").write(net)
        r = subprocess.run(["ngspice", "-b", path], capture_output=True, text=True, timeout=300)
        out = r.stdout + r.stderr
    vals = {m.group(1): float(m.group(2)) for m in re.finditer(r"^(\w+)\s+=\s+([-+\d.eE]+)", out, re.M)}
    if "il_pp" not in vals:
        raise SystemExit("simulation failed:\n" + out[-2000:])
    return vals

def main():
    print(f"{'Vin':>5} {'Iload':>6} {'L uH':>5} | {'duty':>5} {'Vout':>6} {'ripple mV':>9} | {'IL p-p':>6} {'IL pk':>6} {'IL min':>6} {'IL rms':>6} | notes")
    worst_pk = 0
    for vin, iload, l in itertools.product((9.0, 12.0, 14.4, 29.0), (0.05, 0.3, 0.5, 1.0), (L_NOM,)):  # 29 V = TVS clamp level
        v = run(vin, iload, l)
        notes = []
        if v["il_max"] > ISAT: notes.append("peak > inductor Isat")
        if v["il_rms"] > IRMS_RATED: notes.append("rms > inductor rating")
        if v["il_min"] < 0: notes.append("reverse current (forced PWM)")
        worst_pk = max(worst_pk, v["il_max"])
        print(f"{vin:5.1f} {iload:6.2f} {l*1e6:5.1f} | {v['duty']:5.2f} {v['vout_avg']:6.3f} {v['vout_pp']*1e3:9.1f} | "
              f"{v['il_pp']:6.2f} {v['il_max']:6.2f} {v['il_min']:6.2f} {v['il_rms']:6.2f} | {', '.join(notes)}")
    print("\nInductor at -20% tolerance (5.44 uH), worst ripple case Vin=29 V:")
    for iload in (0.5, 1.0):
        v = run(29.0, iload, L_NOM * 0.8)
        print(f"  Iload {iload:.2f} A: IL p-p {v['il_pp']:.2f} A, peak {v['il_max']:.2f} A, Vout ripple {v['vout_pp']*1e3:.1f} mV")
    print(f"\nWorst peak inductor current at nominal L: {worst_pk:.2f} A (inductor Isat {ISAT} A, IC peak limit >= {I_PEAK_LIMIT} A)")

    print("\nSet-point from the feedback divider (no simulation needed):")
    r1, r2 = 158e3, 30.1e3
    nom = 0.8 * (1 + r1 / r2)
    lo = 0.792 * (1 + r1 * 0.99 / (r2 * 1.01)); hi = 0.808 * (1 + r1 * 1.01 / (r2 * 0.99))
    print(f"  nominal {nom:.3f} V, worst case {lo:.3f} to {hi:.3f} V (VFB 792-808 mV, 1% resistors)")
    print(f"  feed-forward zero 1/(2*pi*R15*C11) = {1/(2*3.14159*r1*47e-12)/1e3:.1f} kHz")

if __name__ == "__main__":
    main()
