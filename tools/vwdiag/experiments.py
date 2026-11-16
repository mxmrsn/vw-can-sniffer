"""Logging experiments: scripted, repeatable captures from the car that answer specific questions and
calibrate the simulator (docs/logging_experiments.md).

    python3 -m vwdiag --transmit experiment warm-idle
    python3 -m vwdiag --sim --sim-profile gti --transmit experiment warm-idle --seconds 60

Every run writes a folder experiments/<date-time>-<name>/ with:
  meta.json     what was run, against what (car or simulator scenario), when, with which settings
  summary.json  the numbers the experiment exists to measure (what `vwdiag calibrate` reads)
  *.csv         the samples behind them
  notes.txt     what happened, step by step, including anything that failed

Experiments only read from the car. Times inside an experiment are the engine module's own run
time (OBD PID 1F) where that matters, so a simulator run at a faster engine clock (speed=) is
directly comparable with the car.
"""
import csv
import json
import os
import statistics
import time

from . import obd, simos18, vw
from .link import LinkError
from .uds import InterfaceRefused, NegativeResponse, NoResponse, UdsClient, UdsError

IDLE_SET = ["rpm", "load", "coolant", "intake", "maf", "map", "stft", "ltft", "timing", "lambda", "voltage", "runtime"]
VW_SET = ["knock_cyl_1", "knock_cyl_2", "knock_cyl_3", "knock_cyl_4", "ign_avg", "misfires"]


def retry(fn, tries=3, pause=0.2):
    for attempt in range(tries):
        try:
            return fn()
        except NoResponse:
            if attempt == tries - 1:
                raise
        except NegativeResponse as e:
            if e.nrc not in (0x21, 0x22, 0x78) or attempt == tries - 1:
                raise
        time.sleep(pause)


def stats(values):
    v = [x for x in values if x is not None]
    if not v:
        return None
    s = sorted(v)
    r = lambda x: round(x, 2)
    return {"n": len(v), "mean": r(statistics.fmean(v)), "sd": r(statistics.pstdev(v)), "min": r(s[0]),
            "p10": r(s[int(0.1 * (len(s) - 1))]), "p50": r(s[len(s) // 2]), "p90": r(s[int(0.9 * (len(s) - 1))]),
            "max": r(s[-1])}


class Run:
    """One experiment run: its folder, its notes, and helpers for reading the car."""

    def __init__(self, link, args, name, ask=input, say=print):
        self.link, self.args, self.name, self.ask, self.say = link, args, name, ask, say
        root = args.out or "experiments"
        self.dir = os.path.join(root, time.strftime("%Y%m%d-%H%M%S") + "-" + name)
        os.makedirs(self.dir, exist_ok=True)
        self.notes = []
        self.meta = {"experiment": name, "started": time.strftime("%Y-%m-%d %H:%M:%S"),
                     "target": "simulator" if (args.sim or args.sim_ble) else "car",
                     "scenario": os.environ.get("VWSIM", "") if (args.sim or args.sim_ble) else None,
                     "settings": {k: getattr(args, k) for k in ("seconds", "interval", "holds") if hasattr(args, k)}}
        self.engine = UdsClient(link, *vw.ECUS["engine"][:2])
        # a simulator running its engine clock faster: read the misfire counter as often in engine time
        self.speed = 1.0
        for item in (self.meta["scenario"] or "").split(","):
            if item.startswith("speed="):
                self.speed = float(item[6:])

    def note(self, text):
        self.notes.append(text)
        self.say(text)

    def prompt(self, text):
        if self.args.yes:
            self.say(">> " + text)
            return ""
        return self.ask(">> %s  [Enter when ready] " % text)

    def path(self, name):
        return os.path.join(self.dir, name)

    def save(self, summary):
        self.meta["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
        with open(self.path("meta.json"), "w") as f:
            json.dump(self.meta, f, indent=2)
        with open(self.path("summary.json"), "w") as f:
            json.dump(summary, f, indent=2, default=str)
        with open(self.path("notes.txt"), "w") as f:
            f.write("\n".join(self.notes) + "\n")

    # ---- reading
    def read(self, name):
        """One value by name: OBD PID names, or Simos 18 names. None if it cannot be read."""
        try:
            if name in obd.PIDS:
                return retry(lambda: obd.read_pid(self.engine, name)[0], tries=2)
            return round(simos18.read(self.engine, name)[0], 3)
        except UdsError:
            return None

    def available(self, names):
        return [n for n in names if self.read(n) is not None]

    def misfires(self):
        try:
            c = obd.misfire_counts(self.engine)
            return [c[i][0] for i in range(1, 5)]
        except UdsError:
            return None

    def sample(self, names, seconds, filename, interval, misfire_every=10.0, stop=None):
        """Read `names` every `interval` s for `seconds`; per-cylinder misfire counts every
        `misfire_every` s. Writes a CSV; returns the rows as dicts."""
        rows = []
        with open(self.path(filename), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=["t"] + names + ["mis_c1", "mis_c2", "mis_c3", "mis_c4"])
            w.writeheader()
            start = time.monotonic()
            next_mis = start
            misfire_every /= self.speed
            while time.monotonic() - start < seconds:
                t0 = time.monotonic()
                row = {"t": round(t0 - start, 2)}
                for n in names:
                    row[n] = self.read(n)
                if t0 >= next_mis:
                    m = self.misfires()
                    if m:
                        row.update({"mis_c%d" % (i + 1): m[i] for i in range(4)})
                    next_mis = t0 + misfire_every
                w.writerow(row)
                f.flush()
                rows.append(row)
                if stop and stop(row):
                    break
                time.sleep(max(0.0, interval - (time.monotonic() - t0)))
        return rows


# ---- summaries shared by the engine experiments
def misfire_rates(rows):
    """Misfires per 100 firings of each cylinder over the rows, from the counter and the engine's
    own run time and speed (each cylinder fires once per two revolutions)."""
    with_mis = [r for r in rows if r.get("mis_c1") is not None]
    if len(with_mis) < 2:
        return None
    a, b = with_mis[0], with_mis[-1]
    rpm = statistics.fmean([r["rpm"] for r in rows if r.get("rpm")] or [0])
    if a.get("runtime") is not None and b.get("runtime") is not None and b["runtime"] > a["runtime"]:
        seconds, clock = b["runtime"] - a["runtime"], "engine"
    else:
        seconds, clock = b["t"] - a["t"], "wall"
    firings = rpm / 120.0 * seconds
    out = {"seconds": round(seconds, 1), "clock": clock, "mean_rpm": round(rpm, 1), "firings_per_cylinder": round(firings)}
    for c in range(1, 5):
        d = (b["mis_c%d" % c] or 0) - (a["mis_c%d" % c] or 0)
        out["cyl%d" % c] = {"misfires": d, "pct": round(100.0 * d / firings, 3) if firings > 0 and d >= 0 else None}
    return out


def value_stats(rows, names):
    return {n: stats([r.get(n) for r in rows]) for n in names}


def total_trim(rows):
    return stats([r["stft"] + r["ltft"] for r in rows if r.get("stft") is not None and r.get("ltft") is not None])


# ---- the experiments
def exp_listen(run):
    """Listen only, ignition off then on: what broadcast traffic reaches the OBD port, and at what rates."""
    run.link.set_silent(True)
    out = {}
    for phase, what in (("off", "Ignition OFF (key out, doors shut a minute)."), ("on", "Ignition ON, engine OFF.")):
        run.prompt(what)
        run.link.set_monitor(True)
        frames = run.link.read_frames_timed(run.args.seconds / 2.0)
        run.link.set_monitor(False)
        with open(run.path("frames_%s.csv" % phase), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t", "id", "ext", "data"])
            for t, i, ext, data in frames:
                w.writerow(["%.4f" % t, "%X" % i, int(ext), data.hex()])
        ids = {}
        for t, i, ext, data in frames:
            ids.setdefault("%X" % i, []).append(t)
        rates = {}
        for i, ts in sorted(ids.items()):
            periods = [1000 * (b - a) for a, b in zip(ts, ts[1:])]
            rates[i] = {"count": len(ts), "hz": round(len(ts) / max(run.args.seconds / 2.0, 1e-6), 2),
                        "period_ms": stats(periods)}
        out[phase] = {"frames": len(frames), "ids": rates}
        run.note("ignition %s: %d frames, %d IDs" % (phase, len(frames), len(ids)))
    out["broadcast_visible"] = out["on"]["frames"] > 0
    run.note("broadcast traffic at the OBD port: %s" % ("yes" if out["broadcast_visible"] else "none (behind the gateway)"))
    return out


def exp_identify(run):
    """Ignition on, engine off. Who answers, how fast, how they pace long answers, what the engine supports."""
    run.prompt("Ignition ON, engine OFF.")
    run.link.set_silent(False)
    out = {"modules": {}}
    multi = getattr(run.link, "take_frames", None)
    for name, (req, resp, desc) in vw.ECUS.items():
        client = UdsClient(run.link, req, resp, timeout_ms=300)
        latencies = []
        try:
            retry(client.tester_present)
        except (NoResponse, InterfaceRefused):
            out["modules"][name] = {"present": False}
            run.note("%-8s no answer" % name)
            continue
        except NegativeResponse:
            pass
        for _ in range(20):
            t0 = time.monotonic()
            try:
                client.tester_present()
                latencies.append(1000 * (time.monotonic() - t0))
            except UdsError:
                pass
        entry = {"present": True, "latency_ms": stats(latencies)}
        # a long answer, to see the module's flow control: identification items run together
        if multi:
            run.link.set_monitor(True)
            run.link.take_frames()
        try:
            UdsClient(run.link, req, resp).request([0x22, 0xF1, 0x87, 0xF1, 0x89, 0xF1, 0x97, 0xF1, 0x90])
        except UdsError:
            pass
        if multi:
            fcs = [d for _, i, _, d in run.link.take_frames() if i == resp and d and d[0] >> 4 == 3]
            run.link.set_monitor(False)
            entry["flow_control"] = [{"bs": d[1], "stmin": d[2]} for d in fcs[:3]] if fcs else []
        try:
            entry["identification"] = dict(vw.identify(UdsClient(run.link, req, resp)))
        except UdsError as e:
            entry["identification"] = {"error": str(e)}
        out["modules"][name] = entry
        lat = entry["latency_ms"]
        run.note("%-8s present, TesterPresent %s ms (p50), flow control %s" % (
            name, lat["p50"] if lat else "-", entry.get("flow_control", "not seen (text link)")))
    # functional OBD request: who else answers besides the engine
    if multi:
        run.link.set_monitor(True)
        run.link.take_frames()
        try:
            UdsClient(run.link, 0x7DF, 0x7E8).request([0x01, 0x00])
        except UdsError:
            pass
        time.sleep(0.3)
        answering = sorted({"%X" % i for _, i, _, d in run.link.take_frames() + run.link.read_frames_timed(0.2)
                            if 0x7E8 <= i <= 0x7EF})
        run.link.set_monitor(False)
        out["functional_obd_answers"] = answering
        run.note("functional OBD request answered by: %s" % ", ".join(answering))
    engine = run.engine
    try:
        out["obd_pids"] = ["%02X" % p for p in obd.supported_pids(engine)]
    except UdsError as e:
        out["obd_pids"] = {"error": str(e)}
    mids = []
    for base in range(0x00, 0x100, 0x20):
        try:
            r = engine.request([0x06, base])
        except UdsError:
            break
        bits = int.from_bytes(r[2:6], "big")
        mids += ["%02X" % (base + i + 1) for i in range(32) if bits & (1 << (31 - i)) and (base + i + 1) % 0x20]
        if not bits & 1:
            break
    out["monitor_ids"] = mids
    out["simos18_values"] = {}
    for key, (did, length, signed, unit, decode, label) in sorted(simos18.VALUES.items()):
        try:
            data = engine.read_did(did)
            out["simos18_values"][key] = {"did": "%04X" % did, "answered": True, "bytes": len(data)}
        except NegativeResponse as e:
            out["simos18_values"][key] = {"did": "%04X" % did, "answered": False, "nrc": "%02X" % e.nrc}
        except UdsError:
            out["simos18_values"][key] = {"did": "%04X" % did, "answered": False}
    answered = sum(v["answered"] for v in out["simos18_values"].values())
    run.note("engine: %s OBD values, %d monitors, %d of %d Simos 18 values answered in the default session" % (
        len(out["obd_pids"]) if isinstance(out["obd_pids"], list) else "?", len(mids), answered, len(simos18.VALUES)))
    if run.args.session_test:
        out["s3"] = s3_test(run)
    return out


def s3_test(run):
    """How long the engine keeps an extended session without requests (entering one only reads)."""
    result = {}
    for wait in (2, 4, 6, 8):
        try:
            run.engine.session(0x03)
            time.sleep(wait)
            result[str(wait)] = run.engine.read_did(0xF186)[0]
        except UdsError as e:
            result[str(wait)] = str(e)
    try:
        run.engine.session(0x01)
    except UdsError:
        pass
    run.note("extended session still active after: %s" % ", ".join("%s s: %s" % kv for kv in result.items()))
    return result


def engine_capture(run, prompt, default_seconds):
    run.link.set_silent(False)
    names = run.available(IDLE_SET)
    vw_names = run.available(VW_SET)
    run.note("reading %s; engine measuring values %s" % (", ".join(names), ", ".join(vw_names) or "none"))
    run.prompt(prompt)
    seconds = run.args.seconds or default_seconds
    return run.sample(names + vw_names, seconds, "samples.csv", run.args.interval), names + vw_names


def exp_cold_start(run):
    """From a cold soak to warm: idle speed, spark, trims and misfires as the coolant comes up."""
    run.link.set_silent(False)
    before = run.read("coolant")
    run.note("coolant before start %s C (cold soak means close to the air temperature)" % before)
    rows, names = engine_capture(run, "Start the engine and leave it idling, hands off, for the whole run.", 900)
    first = [r for r in rows if r.get("runtime") is not None and r["runtime"] <= 60]
    out = {"coolant_at_start": before, "rows": len(rows)}
    # idle speed against coolant: what the model's cold_idle and idle are fitted from
    pairs = [(r["coolant"], r["rpm"]) for r in rows if r.get("coolant") is not None and r.get("rpm")]
    bands = {}
    for lo in range(-10, 110, 10):
        v = [rpm for c, rpm in pairs if lo <= c < lo + 10]
        if v:
            bands["%d-%d" % (lo, lo + 10)] = round(statistics.fmean(v), 1)
    out["idle_rpm_by_coolant"] = bands
    cool = [(r["runtime"], r["coolant"]) for r in rows if r.get("runtime") is not None and r.get("coolant") is not None]
    for target in (40, 60, 80):
        hit = next((t for t, c in cool if c >= target), None)
        out["runtime_to_%dC" % target] = hit
    out["timing_first_60s"] = stats([r.get("timing") for r in first])
    closed = next((r["runtime"] for r in rows if r.get("stft") not in (None, 0) and r.get("runtime") is not None), None)
    out["closed_loop_after_s"] = closed
    cold = [r for r in rows if r.get("coolant") is not None and r["coolant"] < 60]
    warm = [r for r in rows if r.get("coolant") is not None and r["coolant"] >= 75]
    out["trim_cold"] = total_trim([r for r in cold if r.get("stft") not in (None, 0)])
    out["trim_warm"] = total_trim(warm)
    out["misfire_cold"] = misfire_rates(cold)
    out["misfire_warm"] = misfire_rates(warm)
    out["values"] = value_stats(rows, names)
    run.note("coolant reached 60 C after %s s, 80 C after %s s; closed loop after %s s" % (
        out["runtime_to_60C"], out["runtime_to_80C"], closed))
    return out


def exp_warm_idle(run):
    """Fully warm, idling, nothing switched on: the reference every other run is compared with."""
    rows, names = engine_capture(run, "Engine warm (coolant at 90 C or more), idling, A/C and lights off.", 300)
    out = {"rows": len(rows), "values": value_stats(rows, names), "trim_total": total_trim(rows),
           "misfire": misfire_rates(rows)}
    v = out["values"]
    run.note("warm idle: %s rpm, %s g/s, %s kPa, trims %s %%, worst cylinder %s" % (
        v["rpm"]["mean"] if v.get("rpm") else "-", v["maf"]["mean"] if v.get("maf") else "-",
        v["map"]["mean"] if v.get("map") else "-", out["trim_total"]["mean"] if out["trim_total"] else "-",
        worst(out["misfire"])))
    return out


def worst(rates):
    if not rates:
        return "-"
    c = max(range(1, 5), key=lambda i: rates["cyl%d" % i]["misfires"])
    return "cyl %d (%s %%)" % (c, rates["cyl%d" % c]["pct"])


def exp_rev_holds(run):
    """Parked, in neutral or P: hold steady engine speeds; misfires and trims at each."""
    run.link.set_silent(False)
    names = run.available(IDLE_SET)
    holds = [int(h) for h in run.args.holds.split(",")]
    out = {"holds": {}}
    for rpm in holds:
        run.prompt("Hold %d rpm steadily (parked)." % rpm if rpm else "Let it idle.")
        rows = run.sample(names, run.args.seconds or 60, "hold_%d.csv" % rpm, run.args.interval)
        out["holds"][str(rpm)] = {"values": value_stats(rows, names), "trim_total": total_trim(rows),
                                  "misfire": misfire_rates(rows)}
        run.note("%d rpm: worst %s" % (rpm, worst(out["holds"][str(rpm)]["misfire"])))
    return out


def exp_timing(run):
    """How fast the engine answers, and so how fast live data can be logged."""
    run.link.set_silent(False)
    out = {}
    lat = []
    for _ in range(100):
        t0 = time.monotonic()
        try:
            run.engine.request([0x01, 0x0C])
            lat.append(1000 * (time.monotonic() - t0))
        except UdsError:
            pass
    out["single_pid_ms"] = stats(lat)
    try:
        r = run.engine.request([0x01, 0x0C, 0x0D, 0x05, 0x04, 0x06, 0x07])
        out["multi_pid"] = {"supported": True, "bytes": len(r)}
    except UdsError as e:
        out["multi_pid"] = {"supported": False, "error": str(e)}
    lat = []
    for _ in range(30):
        t0 = time.monotonic()
        try:
            run.engine.read_did(0xF40C)
            lat.append(1000 * (time.monotonic() - t0))
        except UdsError:
            break
    out["did_read_ms"] = stats(lat)
    run.note("engine answers a PID in %s ms (p50); several PIDs in one request: %s" % (
        out["single_pid_ms"]["p50"] if out["single_pid_ms"] else "-", out["multi_pid"]["supported"]))
    return out


def exp_drive(run):
    """A drive logged to the card, with marks; the misfire picture before and after."""
    from .cli import misfire_report
    run.link.set_silent(False)
    before = misfire_report(run.engine)
    before.pop("lines")
    req, resp, _ = vw.ECUS["engine"]
    poll = ["rpm", "load", "speed", "stft", "ltft", "timing", "coolant", "map"]
    run.link.set_time(int(time.time()))
    run.link.poll_clear()
    for n in poll:
        run.link.poll_add(req, resp, 250, [0x01, obd.PIDS[n][0]])
    state = run.link.log_start()
    run.note("logging to %s, polling %s every 0.25 s" % (state["name"], ", ".join(poll)))
    marks = []
    if run.args.seconds:
        run.prompt("Drive. Ends after %g s." % run.args.seconds)
        time.sleep(run.args.seconds)
    else:
        run.prompt("Drive. Type text + Enter to mark a moment, q + Enter to finish.")
        while True:
            text = run.ask("mark> ").strip()
            if text.lower() in ("q", "quit", "end"):
                break
            run.link.log_mark(text or "mark")
            marks.append(text or "mark")
    run.link.poll_clear()
    state = run.link.log_stop()
    after = misfire_report(run.engine)
    after.pop("lines")
    run.note("card log %s: %d frames, %d lost; copy it with  vwdiag files %s" % (state["name"], state["frames"],
                                                                                 state["lost"], state["name"]))
    return {"log": state, "marks": marks, "misfire_before": before, "misfire_after": after}


EXPERIMENTS = {
    "listen": (exp_listen, False, 60, "Listen only, ignition off then on: is there broadcast traffic at the OBD port?"),
    "identify": (exp_identify, True, 0, "Who answers, response times, flow control, supported values"),
    "timing": (exp_timing, True, 0, "Response times and multi-PID requests: how fast logging can go"),
    "cold-start": (exp_cold_start, True, 900, "Cold soak to warm: idle speed, spark, trims, misfires"),
    "warm-idle": (exp_warm_idle, True, 300, "Warm idle reference: values, trims, misfire rate per cylinder"),
    "rev-holds": (exp_rev_holds, True, 60, "Parked rpm holds: misfires and trims at each engine speed"),
    "drive": (exp_drive, True, 0, "A drive logged to the card with marks; misfire picture before and after"),
}


def run_experiment(link, args, ask=input, say=print):
    fn, transmits, default_seconds, title = EXPERIMENTS[args.name]
    if args.seconds is None:
        args.seconds = default_seconds
    run = Run(link, args, args.name, ask=ask, say=say)
    run.note("experiment %s: %s" % (args.name, title))
    try:
        summary = fn(run)
    except (UdsError, LinkError) as e:
        summary = {"error": str(e)}
        run.note("stopped: %s" % e)
    finally:
        try:
            link.set_silent(True)
        except (UdsError, LinkError):
            pass
    run.save(summary)
    run.note("saved in %s" % run.dir)
    return run.dir, summary
