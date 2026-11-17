"""Misfire diagnosis from saved sessions: what the numbers point at, and what to test next.

    python3 -m vwdiag diagnose sessions/20261120-0915
    python3 -m vwdiag diagnose sessions/A --compression 1:185,2:180,3:125,4:182 --wet 3:130
    python3 -m vwdiag diagnose sessions/before sessions/after --swap 3:2     # did the misfire follow the coil?

No board needed: it reads what `vwdiag session` saved (session.json, idle.csv) and, if it is in the
folder or given with --log, the drive log copied off the card. The rules are the ones a mechanic
uses on a direct-injection four-cylinder like the EA888; every finding lists the numbers behind it,
so you can disagree with it. It points at where to look; it cannot see inside the engine.
"""
import csv
import json
import os

from . import dtc as dtcfmt
from . import logfile

CYLINDERS = 4
SINGLE_SHARE = 0.8        # one cylinder with this share of the misfires is a single-cylinder fault
COLD_C = 60               # coolant below this is a cold engine
WARM_C = 80               # idle airflow is only judged on a warm engine
IDLE_RPM = 1100
LEAN_TRIM = 10.0          # short + long term, percent: adding this much fuel means it reads lean
RICH_TRIM = -10.0
MAF_IDLE = (2.0, 4.5)     # g/s, a warm 2.0 TSI at idle with no load switched on
LOW_COMPRESSION = 0.85    # a cylinder below 85 % of the others' average is low
WET_RISE = 0.10           # a wet reading this much above the dry one points at the rings

CAM_CRANK = {"P0016", "P0017", "P0018", "P0019", "P0008", "P0009"}
CAM_ADJUST = {"P0010", "P0011", "P0012", "P0013", "P0014", "P0015"}
LEAN = {"P0171", "P2187"}
RICH = {"P0172", "P2188"}
FUEL_PRESSURE = {"P0087", "P0088", "P0089", "P0191", "P0192", "P0193"}


# ---------------------------------------------------------------- reading a session folder
def _num(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def _counts(raw):
    """{cyl: (this_cycle, avg_10)} from either saved shape: {"3": [8, 47]} or {"3": {"this_cycle": ..}}."""
    out = {}
    for k, v in (raw or {}).items():
        if isinstance(v, dict):
            v = (v.get("this_cycle"), v.get("avg_10_cycles"))
        out[int(k)] = (v[0] or 0, v[1] or 0)
    return out


def load(folder, log_path=None):
    """Everything diagnosis needs from one session folder, as plain data."""
    with open(os.path.join(folder, "session.json")) as f:
        data = json.load(f)
    steps = data.get("steps", {})
    faults = steps.get("faults", {}) if isinstance(steps.get("faults"), dict) else {}
    engine_obd = faults.get("engine_obd", {})
    drive = steps.get("drive", {}) if isinstance(steps.get("drive"), dict) else {}
    s = {"folder": folder, "started": data.get("started"), "codes": set(), "monitor": [],
         "freeze": None, "idle_rows": [], "drive_samples": [], "log": None}

    for entry in faults.get("engine", []) if isinstance(faults.get("engine"), list) else []:
        s["codes"].add(entry["code"])
    for rep in (engine_obd, drive.get("after", {})):
        s["codes"].update(rep.get("stored", []) + rep.get("pending", []))
        ff = rep.get("freeze_frame") or {}
        if ff.get("code") and not s["freeze"]:
            s["freeze"] = ff
    for label, raw in (("faults", engine_obd.get("misfires")), ("idle", (steps.get("idle") or {}).get("misfires")),
                       ("after drive", (drive.get("after") or {}).get("misfires"))):
        if raw:
            s["monitor"].append((label, _counts(raw)))

    idle_csv = os.path.join(folder, "idle.csv")
    if os.path.exists(idle_csv):
        with open(idle_csv, newline="") as f:
            for row in csv.DictReader(f):
                s["idle_rows"].append({k.split("_")[0] if not k.startswith(("mis_c", "knock_cyl")) else k: _num(v)
                                       for k, v in row.items()})

    path = log_path or (os.path.join(folder, drive["log"]) if drive.get("log") else None)
    if path and os.path.exists(path):
        try:
            log = logfile.Log(path)
            s["drive_samples"] = list(log.samples())
            s["log"] = path
        except (OSError, logfile.LogError):
            pass
    return s


# ---------------------------------------------------------------- the numbers
def cylinder_totals(s):
    """{cyl: (this_cycle, avg_10)}: the largest reading of each (the cycle count only grows)."""
    out = {c: (0, 0) for c in range(1, CYLINDERS + 1)}
    for _, counts in s["monitor"]:
        for c in out:
            now, avg = counts.get(c, (0, 0))
            out[c] = (max(out[c][0], now), max(out[c][1], avg))
    return out


def idle_rates(rows):
    """Misfire rate per cylinder at idle, cold and warm: {"cold"|"warm": {cyl: (misfires, firings)}}.

    The monitor is read every few seconds; between two readings each cylinder fired rpm / 120 times
    a second, and the count went up by however many of those misfired.
    """
    out = {"cold": {}, "warm": {}}
    last, prev_t, firings = None, None, 0.0
    for r in rows:
        t = r.get("time")
        if t is None:
            continue
        if prev_t is not None and r.get("rpm"):
            firings += r["rpm"] / 120.0 * (t - prev_t)
        prev_t = t
        counts = [r.get("mis_c%d" % c) for c in range(1, CYLINDERS + 1)]
        if any(v is None for v in counts):
            continue
        if last is not None and firings > 0:
            band = "cold" if (r.get("coolant") is not None and r["coolant"] < COLD_C) else "warm"
            for c in range(1, CYLINDERS + 1):
                d = counts[c - 1] - last[c - 1]
                if d >= 0:
                    m, f = out[band].get(c, (0, 0.0))
                    out[band][c] = (m + d, f + firings)
        last, firings = counts, 0.0
    return out


def _mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


def idle_summary(rows):
    """Averages over the idle capture: trims, airflow, lambda, knock retard per cylinder."""
    if not rows:
        return {}
    out = {k: _mean(r.get(k) for r in rows) for k in ("rpm", "load", "coolant", "maf", "stft", "ltft", "lambda", "timing")}
    out["coolant_min"] = min((r["coolant"] for r in rows if r.get("coolant") is not None), default=None)
    out["coolant_max"] = max((r["coolant"] for r in rows if r.get("coolant") is not None), default=None)
    if out["stft"] is not None and out["ltft"] is not None:
        out["trim"] = out["stft"] + out["ltft"]
    knock = {c: _mean(r.get("knock_cyl_%d" % c) for r in rows) for c in range(1, CYLINDERS + 1)}
    if any(v is not None for v in knock.values()):
        out["knock"] = knock
    return out


def drive_trims(samples):
    """Total fuel trim by driving condition from a card log: {"idle"|"part"|"load": (mean, n)}."""
    state, bins = {}, {"idle": [], "part": [], "load": []}
    for _, name, value, _ in sorted(samples, key=lambda x: x[0]):
        state[name] = value
        if name not in ("stft", "ltft") or "stft" not in state or "ltft" not in state:
            continue
        rpm, load, speed = state.get("rpm"), state.get("load"), state.get("speed")
        if rpm is None or load is None:
            continue
        if (speed or 0) < 2 and rpm < IDLE_RPM:
            band = "idle"
        elif load >= 50:
            band = "load"
        else:
            band = "part"
        bins[band].append(state["stft"] + state["ltft"])
    return {k: (sum(v) / len(v), len(v)) for k, v in bins.items() if v}


def parse_cylinder_values(text):
    """'1:185,2:180,3:125' -> {1: 185.0, 2: 180.0, 3: 125.0}."""
    out = {}
    for part in (text or "").split(","):
        if part.strip():
            c, v = part.split(":")
            c = int(c)
            if not 1 <= c <= CYLINDERS:
                raise ValueError("cylinder %d: this engine has %d" % (c, CYLINDERS))
            out[c] = float(v)
    return out


# ---------------------------------------------------------------- the rules
PARTS = ("coil", "plug", "injector")


def analyze(sessions, compression=None, wet=None, swap=None, replaced=()):
    """Findings, likely causes and next tests. sessions: from load(), oldest first. replaced: parts
    already renewed on the misfiring cylinder ("coil", "plug", "injector"), which move down the list.

    Returns {"cylinders", "worst", "pattern", "findings": [(title, evidence)], "causes": [(cause, why)],
             "tests": [...], "lines": [...]}.
    """
    s = sessions[-1]
    totals = cylinder_totals(s)
    score = {c: now + avg for c, (now, avg) in totals.items()}
    total = sum(score.values())
    worst = max(score, key=lambda c: score[c]) if total else None
    findings, causes, tests = [], [], []
    codes = set().union(*(x["codes"] for x in sessions))
    cyl_codes = sorted(c for c in codes if c.startswith("P030") and c[-1] in "1234")

    # 1. which cylinders
    if not total:
        pattern = "none"
        findings.append(("no misfires counted", "the misfire monitor shows 0 on every cylinder, this cycle "
                         "and over the last ten" + ("; but fault code(s) %s are stored" % ", ".join(cyl_codes)
                                                     if cyl_codes else "")))
    elif score[worst] / total >= SINGLE_SHARE:
        pattern = "single"
        findings.append(("cylinder %d only" % worst, "%d%% of all misfires counted (%s)" % (
            round(100 * score[worst] / total), _counts_text(totals))))
    else:
        pattern = "spread"
        findings.append(("misfires on several cylinders", _counts_text(totals) +
                         ": a cause all cylinders share is more likely than one bad cylinder"))
    if cyl_codes:
        findings.append(("fault code%s %s" % ("s" if len(cyl_codes) > 1 else "", ", ".join(cyl_codes)),
                         "; ".join(dtcfmt.description(c)[0] for c in cyl_codes)))
    if "P0300" in codes:
        findings.append(("P0300 random misfire", "the module saw misfires it could not pin on one cylinder"))

    # 2. when: cold, idle, under load
    rates = idle_rates(s["idle_rows"])
    cond = []
    for band in ("cold", "warm"):
        for c, (m, f) in sorted(rates[band].items()):
            if c == worst and f > 0:
                cond.append("%s idle %.1f%% of cylinder %d's firings (%d of %d)" % (band, 100 * m / f, c, m, f))
    ff = s["freeze"]
    when = []
    if ff:
        v = ff.get("values", {})
        if v.get("rpm") is not None and v["rpm"] < IDLE_RPM and (v.get("load") or 0) < 35:
            when.append("idle")
        elif (v.get("load") or 0) >= 50:
            when.append("load")
        if v.get("coolant") is not None and v["coolant"] < COLD_C:
            when.append("cold")
        cond.append("freeze frame %s at %s" % (ff["code"], ", ".join(
            "%s %s" % (k, _fmt(v[k])) for k in ("rpm", "load", "coolant", "speed") if k in v)))
    if cond:
        findings.append(("when it misfires", "; ".join(cond)))
    cold_rate = _rate(rates["cold"].get(worst)) if worst else None
    warm_rate = _rate(rates["warm"].get(worst)) if worst else None
    idle_dominant = ("idle" in when or "cold" in when or
                     (cold_rate is not None and (warm_rate is None or cold_rate > warm_rate)))
    readings = dict(s["monitor"])
    if worst and "idle" in readings and "after drive" in readings:
        findings.append(("during the drive", "cylinder %d went from %d to %d misfires this cycle" % (
            worst, readings["idle"].get(worst, (0, 0))[0], readings["after drive"].get(worst, (0, 0))[0])))

    # 3. mixture and airflow
    idle = idle_summary(s["idle_rows"])
    trim = idle.get("trim")
    if trim is not None:
        text = "idle: short %+.1f%% + long %+.1f%% = %+.1f%%" % (idle["stft"], idle["ltft"], trim)
        dt = drive_trims(s["drive_samples"])
        if dt:
            text += "; drive log: " + ", ".join("%s %+.1f%% (%d)" % (k, v[0], v[1]) for k, v in dt.items())
        if trim >= LEAN_TRIM:
            lean_only_idle = dt.get("load") is not None and dt["load"][0] < LEAN_TRIM / 2
            findings.append(("reads lean at idle" + (", not under load" if lean_only_idle else ""), text))
        elif trim <= RICH_TRIM:
            findings.append(("reads rich at idle", text))
        else:
            findings.append(("fuel trims normal", text))
    maf = idle.get("maf")
    if maf is not None and (idle.get("coolant_min") or 0) >= WARM_C:
        lo, hi = MAF_IDLE
        if maf < lo:
            findings.append(("low idle airflow", "MAF %.1f g/s warm (expected %.1f to %.1f): air may be getting in "
                             "after the meter" % (maf, lo, hi)))
        elif maf > hi:
            findings.append(("high idle airflow", "MAF %.1f g/s warm (expected %.1f to %.1f): extra load, or idle "
                             "raised to cover a weak cylinder" % (maf, lo, hi)))
    knock = idle.get("knock")
    if knock and worst:
        others = [abs(v) for c, v in knock.items() if c != worst and v is not None]
        mine = abs(knock.get(worst) or 0)
        if others and mine > max(others) + 1.0:
            findings.append(("knock retard on cylinder %d" % worst, "%.1f deg against %.1f at most on the others" % (
                mine, max(others))))

    # 4. other codes that change the picture
    for group, title, why in ((CAM_CRANK, "cam / crank correlation code", "timing chain stretch or a jumped tooth; "
                               "affects every cylinder"),
                              (CAM_ADJUST, "cam adjuster code", "valve timing off; affects every cylinder"),
                              (FUEL_PRESSURE, "fuel pressure code", "high-pressure pump or sensor; affects every cylinder"),
                              (LEAN, "lean code", "air getting in after the meter, or too little fuel"),
                              (RICH, "rich code", "leaking injector or purge valve")):
        hit = sorted(codes & group)
        if hit:
            findings.append(("%s %s" % (title, ", ".join(hit)), why))
    coil = sorted(c for c in codes if c in ("P0351", "P0352", "P0353", "P0354"))
    if coil:
        findings.append(("ignition coil circuit %s" % ", ".join(coil), "the module saw a coil's circuit fail"))
    injector = sorted(c for c in codes if c in ("P0201", "P0202", "P0203", "P0204"))
    if injector:
        findings.append(("injector circuit %s" % ", ".join(injector), "the module saw an injector's circuit fail"))

    # 5. compression readings, if you have them
    mech = None
    if compression:
        avg_others = {c: _mean(v for k, v in compression.items() if k != c) for c in compression}
        low = sorted(c for c, v in compression.items() if avg_others[c] and v < LOW_COMPRESSION * avg_others[c])
        findings.append(("compression", ", ".join("%d: %g" % kv for kv in sorted(compression.items())) +
                         ("; low: %s" % ", ".join(str(c) for c in low) if low else "; all within 15%")))
        mech = low
        for c in low:
            if wet and c in wet:
                rise = (wet[c] - compression[c]) / compression[c]
                findings.append(("wet test, cylinder %d" % c, "%g dry, %g wet (%+.0f%%): %s" % (
                    compression[c], wet[c], 100 * rise, "rises: the rings are not sealing" if rise >= WET_RISE else
                    "barely rises: a valve or the head gasket, not the rings")))

    # 6. before and after a swap
    if swap and len(sessions) >= 2:
        a, b = swap
        old = cylinder_totals(sessions[0])
        old_worst = max(old, key=lambda c: old[c][0] + old[c][1])
        new_now = {c: totals[c][0] for c in totals}
        moved = max(new_now, key=lambda c: new_now[c]) if sum(new_now.values()) else None
        if moved == b and old_worst == a:
            findings.append(("misfire followed the swap", "was cylinder %d, now cylinder %d (this cycle: %s): the part "
                             "you moved is the cause" % (a, b, _counts_text(totals, now_only=True))))
        elif moved == a:
            findings.append(("misfire stayed put", "still cylinder %d after swapping it with %d (this cycle: %s): "
                             "the part you moved is not the cause" % (a, b, _counts_text(totals, now_only=True))))
        else:
            findings.append(("swap inconclusive", "this cycle: %s; run the idle step longer" % _counts_text(
                totals, now_only=True)))

    # ---- likely causes, most likely first
    swap_says = next((t for t, _ in findings if t.startswith("misfire followed") or t.startswith("misfire stayed")), None)
    if swap_says and swap_says.startswith("misfire followed"):
        causes.append(("the part you swapped (coil, plug or injector) on cylinder %d" % swap[1],
                       "the misfire moved with it"))
    if pattern == "single":
        low_known = mech is not None and worst in mech
        why = []
        if low_known:
            why.append("compression on cylinder %d is low" % worst)
        if idle_dominant:
            why.append("it misfires most at idle or cold, when cylinder pressure is lowest")
        new = set(replaced)
        mech_cause = ("mechanical: cylinder %d is not holding compression (intake valve carbon, a burnt or bent valve, "
                      "rings, head gasket)" % worst, "; ".join(why) or "a single cylinder, no circuit codes")
        ign_cause = ("ignition on cylinder %d: coil or spark plug" % worst,
                     "already replaced (%s): only a faulty new part or the coil's wiring and ground remain" % (
                         " and ".join(sorted(new & {"coil", "plug"}))) if new & {"coil", "plug"} else
                     "the cheapest cause to rule out; a coil usually misfires most under load, not at idle")
        inj_cause = ("injector on cylinder %d" % worst, "already replaced: unlikely" if "injector" in new else
                     "less common; also leaves that cylinder lean")
        if new >= {"coil", "plug"}:
            mech_cause = (mech_cause[0], mech_cause[1] + ("; " if why else "") + "coil and plug already replaced")
        if low_known or new >= {"coil", "plug"} or (idle_dominant and not coil):
            causes += [mech_cause, ign_cause, inj_cause]
        else:
            causes += [ign_cause, mech_cause, inj_cause]
        causes.sort(key=lambda c: c[1].startswith("already replaced: unlikely"))
    elif pattern == "spread":
        if codes & CAM_CRANK or codes & CAM_ADJUST:
            causes.append(("valve timing (timing chain, tensioner, cam adjuster)", "cam / crank codes stored"))
        if trim is not None and trim >= LEAN_TRIM:
            causes.append(("air leak after the meter: PCV valve (a common EA888 failure), intake or vacuum hoses",
                           "lean at idle with misfires on several cylinders"))
        if codes & FUEL_PRESSURE:
            causes.append(("fuel pressure: high-pressure pump or rail sensor", "fuel pressure codes stored"))
        causes.append(("something shared: plugs worn together, fuel quality, MAF", "several cylinders misfire"))
    if pattern == "single" and trim is not None and trim >= LEAN_TRIM:
        causes.append(("the lean reading is likely the misfire itself", "unburnt air from the misfiring cylinder "
                       "reaches the oxygen sensor, which reads as lean; check for a leak only if it stays lean once "
                       "the misfire is fixed"))

    # ---- what to do next, cheapest first
    if pattern == "single":
        c = worst
        n = c + 1 if c < CYLINDERS else c - 1
        if not swap and "coil" not in replaced:
            tests.append("Swap the coil on cylinder %d with cylinder %d, clear nothing, run another session, then: "
                         "vwdiag diagnose <this session> <new session> --swap %d:%d" % (c, n, c, n))
        tests.append("Pull the spark plug from cylinder %d: look for oil fouling (rings, valve seals), white deposits "
                     "(coolant: head gasket) or a cracked insulator" % c)
        if not compression:
            tests.append("Compression test, all four cylinders, engine warm, throttle open; pass the readings with "
                         "--compression 1:..,2:..,3:..,4:..")
        elif mech and c in mech and not (wet and c in wet):
            tests.append("Wet compression test on cylinder %d (a spoonful of oil); pass it with --wet %d:.." % (c, c))
        tests.append("Leak-down test on cylinder %d at top dead centre: air at the intake = intake valve, at the "
                     "exhaust = exhaust valve, at the oil filler = rings, bubbles in the coolant = head gasket" % c)
        tests.append("Borescope through the plug hole, and on the intake valves with the manifold off: carbon on "
                     "the intake valves is common on direct-injection EA888 engines and is cleaned by walnut blasting")
    elif pattern == "spread":
        tests.append("Smoke test the intake and listen at the PCV valve on the cam cover with the engine idling")
        tests.append("Have the cam / crank correlation checked (VCDS measuring block 93) before replacing parts")
    else:
        tests.append("Run the session again from a cold start with a longer idle (--idle 600): low compression "
                     "misfires most when cold")

    lines = _lines(sessions, totals, pattern, worst, findings, causes, tests)
    return {"cylinders": {str(c): {"this_cycle": v[0], "avg_10_cycles": v[1]} for c, v in totals.items()},
            "worst": worst, "pattern": pattern, "idle": idle, "rates": {b: {str(c): v for c, v in r.items()}
                                                                         for b, r in rates.items()},
            "findings": findings, "causes": causes, "tests": tests, "lines": lines}


def _rate(mf):
    return mf[0] / mf[1] if mf and mf[1] else None


def _fmt(v):
    return ("%.1f" % v).rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def _counts_text(totals, now_only=False):
    if now_only:
        return ", ".join("%d: %d" % (c, v[0]) for c, v in sorted(totals.items()))
    return ", ".join("%d: %d now / %d avg" % (c, v[0], v[1]) for c, v in sorted(totals.items()))


def _lines(sessions, totals, pattern, worst, findings, causes, tests):
    out = ["misfire diagnosis: %s" % ", ".join("%s (%s)" % (os.path.basename(os.path.normpath(s["folder"])),
                                                              s["started"] or "?") for s in sessions)]
    if sessions[-1]["log"]:
        out.append("drive log: %s" % sessions[-1]["log"])
    out.append("")
    out.append("misfire monitor      this cycle   avg of last 10 cycles")
    for c, (now, avg) in sorted(totals.items()):
        out.append("  cylinder %d %s %10d   %10d" % (c, "<" if c == worst and pattern == "single" else " ", now, avg))
    out.append("")
    out.append("findings")
    for title, evidence in findings:
        out.append("  - %s: %s" % (title, evidence))
    if causes:
        out.append("")
        out.append("likely causes, most likely first")
        for i, (cause, why) in enumerate(causes, 1):
            out.append("  %d. %s" % (i, cause))
            out.append("     why: %s" % why)
    out.append("")
    out.append("next, cheapest first")
    for i, t in enumerate(tests, 1):
        out.append("  %d. %s" % (i, t))
    out.append("")
    out.append("This reads what the engine module reports; it cannot see inside the engine. Confirm with the "
               "tests before buying parts.")
    return out
