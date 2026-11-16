"""Calibrate the simulator from experiments run on the car.

    python3 -m vwdiag calibrate experiments/2026...-identify experiments/2026...-warm-idle ...
    python3 -m vwdiag calibrate experiments/* --verify

Reads the summaries the experiments saved, turns them into simulator settings (a VWSIM scenario: the
same key=value settings sim_config.c understands), and writes calibration.md next to them with every
number traced to the experiment it came from. With --verify it then runs the same experiments against
the simulator with those settings and puts car and simulator side by side, so what still differs is
visible rather than assumed.

What it cannot set is listed too: modules the car has and the simulator lacks (or the reverse), and
values the engine answers that the simulator does not, need changes in the simulator's code.
"""
import json
import os
import subprocess
import sys
import time

# The simulator's warm-up model (sim_engine.c), to turn a measured warm-up time into a rate factor.
MODEL_IDLE, MODEL_COLD_IDLE, MODEL_LOAD = 700.0, 1100.0, 24.7


def model_warmup_seconds(c_from, c_to, idle=MODEL_IDLE, cold_idle=MODEL_COLD_IDLE):
    c, t = c_from, 0.0
    while c < c_to and t < 7200:
        rpm = idle if c >= 75 else max(idle, min(cold_idle, cold_idle - (c - 15) * ((cold_idle - idle) / 60.0)))
        c += (0.08 + 0.00008 * rpm + 0.004 * MODEL_LOAD) * 1.0
        t += 1.0
    return t


def load(folders):
    runs = {}
    for d in folders:
        try:
            with open(os.path.join(d, "meta.json")) as f:
                meta = json.load(f)
            with open(os.path.join(d, "summary.json")) as f:
                summary = json.load(f)
        except (OSError, ValueError):
            continue
        if "error" in summary:
            continue
        # the newest run of each experiment wins
        name = meta["experiment"]
        if name not in runs or meta["started"] > runs[name][0]["started"]:
            runs[name] = (meta, summary, d)
    return runs


def derive(runs):
    """Settings, with where each came from, and what needs code changes. Returns (settings, notes, todo)."""
    settings, notes, todo = {}, [], []

    def put(key, value, why):
        settings[key] = value
        notes.append((key, value, why))

    if "listen" in runs:
        s = runs["listen"][1]
        put("gateway", "open" if s.get("broadcast_visible") else "closed",
            "listen: %d frames with the ignition on" % s["on"]["frames"])

    if "identify" in runs:
        s = runs["identify"][1]
        mods = s.get("modules", {})
        lat = [m["latency_ms"] for m in mods.values() if m.get("present") and m.get("latency_ms")]
        if lat:
            lo = max(0, int(min(m["p10"] for m in lat)))
            hi = int(max(m["p90"] for m in lat) + 0.5)
            put("latency", "%d-%d" % (lo, hi), "identify: TesterPresent round trips, fastest p10 to slowest p90 over %d modules" % len(lat))
        fcs = {}
        for name, m in mods.items():
            for fc in m.get("flow_control") or []:
                if name != "engine":
                    key = (fc["bs"], fc["stmin"])
                    fcs[key] = fcs.get(key, 0) + 1
        if fcs:
            (bs, st), n = max(fcs.items(), key=lambda kv: kv[1])
            put("fc", "%d/%d" % (bs, st), "identify: flow control most modules asked for (%d of %d)" % (n, sum(fcs.values())))
        answers = s.get("functional_obd_answers")
        if answers is not None:
            put("trans_obd", "1" if "7E9" in answers else "0", "identify: 7DF answered by %s" % ", ".join(answers))
        if "s3" in s:
            waits = sorted((int(w), v) for w, v in s["s3"].items())
            dropped = next((w for w, v in waits if v == 1), None)
            if dropped:
                put("s3", str(dropped * 1000 - 1000), "identify --session-test: extended session gone after %d s" % dropped)
        sim_has = {"engine", "trans", "abs", "cluster", "airbag", "gateway", "steer"}
        car_has = {n for n, m in mods.items() if m.get("present")}
        if car_has and car_has != sim_has:
            todo.append("modules: the car answers %s; the simulator has %s. Change the ECU table in "
                        "hardware/pico2w/firmware/sim/car.c (and vw.py / vw_ecus.h if an address is wrong)."
                        % (", ".join(sorted(car_has)), ", ".join(sorted(sim_has))))
        values = s.get("simos18_values", {})
        missing = sorted(k for k, v in values.items() if not v.get("answered"))
        if missing:
            notes.append(("(measuring values)", "%d of %d" % (len(values) - len(missing), len(values)),
                          "identify: Simos 18 values answered in the default session"))

    misfire_cyl = None
    if "warm-idle" in runs:
        s = runs["warm-idle"][1]
        v = s.get("values", {})
        if v.get("rpm"):
            put("idle", "%.0f" % v["rpm"]["mean"], "warm-idle: mean engine speed")
        mis = s.get("misfire")
        if mis:
            worst = max(range(1, 5), key=lambda c: mis["cyl%d" % c]["misfires"])
            pct = mis["cyl%d" % worst]["pct"] or 0
            if pct >= 0.05:
                misfire_cyl = worst
                put("misfire", str(worst), "warm-idle: cylinder %d misfires most (%s %%)" % (worst, pct))
                put("mis_idle", "%.2f" % pct, "warm-idle: cylinder %d misfire rate over %s engine-s" % (worst, mis["seconds"]))
            else:
                put("misfire", "0", "warm-idle: no cylinder above 0.05 %")
        if s.get("trim_total"):
            put("trim_idle", "%.1f" % s["trim_total"]["mean"], "warm-idle: short- plus long-term trim")
        for name in ("maf", "map", "load", "timing"):
            if v.get(name):
                notes.append(("(%s)" % name, v[name]["mean"], "warm-idle: reference for the engine model's %s" % name))

    if "cold-start" in runs:
        s = runs["cold-start"][1]
        bands = s.get("idle_rpm_by_coolant", {})
        pts = [((int(k.split("-")[0]) + 5), rpm) for k, rpm in bands.items() if int(k.split("-")[0]) < 70]
        if len(pts) >= 2:
            n = len(pts)
            mx = sum(p[0] for p in pts) / n
            my = sum(p[1] for p in pts) / n
            slope = sum((x - mx) * (y - my) for x, y in pts) / max(sum((x - mx) ** 2 for x, _ in pts), 1e-9)
            put("cold_idle", "%.0f" % (my + slope * (15 - mx)), "cold-start: idle speed against coolant, straight line taken to 15 C")
        t40, t80 = s.get("runtime_to_40C"), s.get("runtime_to_80C")
        if t40 is not None and t80 is not None and t80 > t40:
            factor = model_warmup_seconds(40, 80) / (t80 - t40)
            put("warmup", "%.2f" % factor, "cold-start: 40 to 80 C took %d s; the model takes %.0f s" % (t80 - t40, model_warmup_seconds(40, 80)))
        if s.get("trim_cold"):
            put("trim_cold", "%.1f" % s["trim_cold"]["mean"], "cold-start: trims below 60 C, closed loop")
        mis = s.get("misfire_cold")
        if mis and misfire_cyl:
            put("mis_cold", "%.2f" % (mis["cyl%d" % misfire_cyl]["pct"] or 0), "cold-start: cylinder %d below 60 C" % misfire_cyl)
        settings["start"] = "cold"

    if "rev-holds" in runs and misfire_cyl:
        holds = runs["rev-holds"][1]["holds"]
        light = [h["misfire"]["cyl%d" % misfire_cyl]["pct"] for rpm, h in holds.items()
                 if 1000 <= int(rpm) <= 3000 and h.get("misfire") and h["misfire"]["cyl%d" % misfire_cyl]["pct"] is not None]
        if light:
            put("mis_light", "%.2f" % (sum(light) / len(light)), "rev-holds: cylinder %d at 1000-3000 rpm" % misfire_cyl)
        trims = [h["trim_total"]["mean"] for rpm, h in holds.items() if int(rpm) >= 1500 and h.get("trim_total")]
        if trims:
            put("trim_drive", "%.1f" % (sum(trims) / len(trims)), "rev-holds: trims off idle")

    if "timing" in runs:
        s = runs["timing"][1]
        if s.get("single_pid_ms"):
            notes.append(("(timing)", s["single_pid_ms"]["p50"], "timing: one PID round trip, ms (p50)"))
        notes.append(("(multi-PID)", s.get("multi_pid", {}).get("supported"), "timing: several PIDs in one request"))
    return settings, notes, todo


def spec(settings, base):
    order = ["gateway", "latency", "fc", "trans_obd", "s3", "misfire", "idle", "cold_idle", "warmup",
             "mis_idle", "mis_cold", "mis_light", "trim_idle", "trim_cold", "trim_drive", "start"]
    parts = [base] + ["%s=%s" % (k, settings[k]) for k in order if k in settings]
    if settings.get("misfire", "0") != "0" and "history" not in base:
        parts.append("history=0")   # the car's own codes come from the car, not the scenario
    return ",".join(parts)


# metrics compared by --verify: (experiment, label, path into summary)
COMPARE = [
    ("warm-idle", "idle rpm", ("values", "rpm", "mean")),
    ("warm-idle", "airflow g/s", ("values", "maf", "mean")),
    ("warm-idle", "manifold kPa", ("values", "map", "mean")),
    ("warm-idle", "load %", ("values", "load", "mean")),
    ("warm-idle", "timing deg", ("values", "timing", "mean")),
    ("warm-idle", "total trim %", ("trim_total", "mean")),
    ("cold-start", "s to 60 C", ("runtime_to_60C",)),
    ("cold-start", "s to 80 C", ("runtime_to_80C",)),
    ("cold-start", "closed loop after s", ("closed_loop_after_s",)),
    ("cold-start", "cold trim %", ("trim_cold", "mean")),
    ("listen", "frames, ignition on", ("on", "frames")),
    ("timing", "PID round trip ms", ("single_pid_ms", "p50")),
]


def dig(d, path):
    for p in path:
        if not isinstance(d, dict) or p not in d:
            return None
        d = d[p]
    return d


def verify(runs, scenario, out_dir, say=print):
    """Run each experiment against the simulator with the calibrated scenario; return its summaries."""
    sim_runs = {}
    plan = {"listen": (None, 1), "identify": (None, 1), "timing": (None, 1), "warm-idle": (60, 5), "cold-start": (900, 15)}
    for name, (meta, summary, _) in runs.items():
        if name not in plan:
            continue
        default_seconds, speed = plan[name]
        seconds = meta.get("settings", {}).get("seconds") or default_seconds
        if name == "cold-start":
            # long enough in engine time to reach what the car reached
            seconds = max(float(seconds or 0), 1.3 * float(summary.get("runtime_to_80C") or 900))
        spec_run = scenario + (",speed=%d" % speed if speed > 1 else "")
        if name in ("warm-idle",):
            spec_run = spec_run.replace(",start=cold", "") + ",start=warm"
        cmd = [sys.executable, "-m", "vwdiag", "--sim", "--binary", "--sim-profile", spec_run, "--transmit",
               "experiment", name, "--yes", "--out", os.path.join(out_dir, "sim")]
        if seconds:
            cmd += ["--seconds", str(max(5.0, float(seconds) / speed))]
        say("simulator: %s (%s)" % (name, spec_run))
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        subprocess.run(cmd, cwd=here, stdout=subprocess.DEVNULL, check=False)
    sim = load([os.path.join(out_dir, "sim", d) for d in sorted(os.listdir(os.path.join(out_dir, "sim")))]) \
        if os.path.isdir(os.path.join(out_dir, "sim")) else {}
    return {k: v[1] for k, v in sim.items()}


def run(folders, base="realistic", do_verify=False, out=None, say=print):
    runs = load(folders)
    if not runs:
        raise SystemExit("no finished experiments in those folders")
    settings, notes, todo = derive(runs)
    scenario = spec(settings, base)
    out_dir = out or os.path.join("calibration", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out_dir, exist_ok=True)
    lines = ["# Simulator calibration", "",
             "From %s." % ", ".join("%s (%s, %s)" % (n, m["started"], m["target"]) for n, (m, _, _) in sorted(runs.items())),
             "", "## Scenario", "", "```", "VWSIM=" + scenario, "```", "",
             "Use it with `--sim-profile '%s'`, or on the bench car with `scenario %s`." % (scenario, scenario), "",
             "## Where each setting comes from", "", "| Setting | Value | From |", "|---|---|---|"]
    lines += ["| `%s` | %s | %s |" % (k, v, why) for k, v, why in notes]
    if todo:
        lines += ["", "## Needs a change in the simulator's code", ""] + ["- " + t for t in todo]
    if any(m["target"] == "simulator" for m, _, _ in runs.values()):
        lines += ["", "Note: some of these experiments ran against the simulator, not the car."]
    if do_verify:
        sim = verify(runs, scenario, out_dir, say=say)
        if "identify" in runs and "identify" in sim:
            car_vals = {k for k, v in runs["identify"][1].get("simos18_values", {}).items() if v.get("answered")}
            sim_vals = {k for k, v in sim["identify"].get("simos18_values", {}).items() if v.get("answered")}
            extra, lacking = sorted(sim_vals - car_vals), sorted(car_vals - sim_vals)
            fix = []
            if extra:
                fix.append("the simulator answers %s, the car does not: make simos_did() in car.c refuse them" % ", ".join(extra))
            if lacking:
                fix.append("the car answers %s, the simulator does not: add them to simos_did() in car.c" % ", ".join(lacking))
            if fix:
                lines += ["", "## Measuring values", ""] + ["- " + f for f in fix]
        lines += ["", "## Car and calibrated simulator, side by side", "", "| Experiment | Measure | Car | Simulator | Difference |",
                  "|---|---|---|---|---|"]
        for exp, label, path in COMPARE:
            if exp not in runs or exp not in sim:
                continue
            a, b = dig(runs[exp][1], path), dig(sim[exp], path)
            diff = "%+.0f %%" % (100.0 * (b - a) / a) if isinstance(a, (int, float)) and isinstance(b, (int, float)) and a else "-"
            lines.append("| %s | %s | %s | %s | %s |" % (exp, label, a, b, diff))
    with open(os.path.join(out_dir, "calibration.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(out_dir, "scenario.txt"), "w") as f:
        f.write(scenario + "\n")
    say("\n".join(lines))
    say("\nwritten to %s" % out_dir)
    return scenario, out_dir
