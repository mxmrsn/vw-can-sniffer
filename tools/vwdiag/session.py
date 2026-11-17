"""The first car session, as one command: read only, everything saved.

    python3 -m vwdiag session                 # asks before each step that needs you at the car
    python3 -m vwdiag session --yes --listen 60 --idle 120 --drive 600

Steps (docs/roadmap.md, "The first car session, scripted"):
  1. Listen only, ignition off then on: is there any broadcast traffic at the OBD port?
  2. Scan every module address; identification of each one that answers.
  3. Fault codes from every module, with freeze frames and counters; the engine's emission view.
  4. Supported OBD values, then an idle capture of the misfire set.
  5. A drive, logged to the card (marks on Enter), with the misfire picture read again at the end.

Every request is retried a few times and a module that never answers is noted and skipped, so
a partial result is still a result. Everything goes into one folder: session.json (all of it,
rewritten after each step), report.txt, CSV files and the raw frames.
"""
import csv
import json
import os
import time

from . import dtc as dtcfmt
from . import obd, simos18, vw
from .link import LinkError
from .uds import InterfaceRefused, NegativeResponse, NoResponse, UdsClient, UdsError

IDLE_VALUES = ["rpm", "load", "coolant", "intake", "maf", "map", "stft", "ltft", "timing", "lambda", "voltage"]
VW_VALUES = ["knock_cyl_1", "knock_cyl_2", "knock_cyl_3", "knock_cyl_4", "misfires", "ign_avg", "int_cam_pos",
             "exhaust_cam_pos", "inj_pw_di", "inj_pw_mpi"]
DRIVE_POLL = ["rpm", "load", "speed", "stft", "ltft", "timing", "coolant", "map"]
RETRY_NRC = (0x21, 0x22, 0x78)   # busy, conditions not correct (e.g. while cranking), pending


def retry(fn, tries=3, pause=0.2):
    """Run fn(); try again on no answer or a 'busy' refusal. Re-raises the last error."""
    for attempt in range(tries):
        try:
            return fn()
        except NoResponse:
            if attempt == tries - 1:
                raise
        except NegativeResponse as e:
            if e.nrc not in RETRY_NRC or attempt == tries - 1:
                raise
        time.sleep(pause)


class Session:
    def __init__(self, link, args, out_dir, ask=input, say=print):
        self.link, self.args, self.dir, self.ask, self.say = link, args, out_dir, ask, say
        self.data = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "steps": {}}
        self.report = []
        os.makedirs(out_dir, exist_ok=True)

    # ---- bookkeeping
    def note(self, text):
        self.report.append(text)
        self.say(text)

    def save(self):
        with open(self.path("session.json"), "w") as f:
            json.dump(self.data, f, indent=2, default=str)
        with open(self.path("report.txt"), "w") as f:
            f.write("\n".join(self.report) + "\n")

    def path(self, name):
        return os.path.join(self.dir, name)

    def prompt(self, text):
        if self.args.yes:
            self.say(">> " + text)
            return ""
        return self.ask(">> %s  [Enter when ready] " % text)

    def step(self, key, title, fn):
        self.note("")
        self.note("== %s" % title)
        try:
            self.data["steps"][key] = fn()
        except (UdsError, LinkError) as e:
            # one failed step never ends the session
            self.data["steps"][key] = {"error": str(e)}
            self.note("step failed: %s" % e)
        self.save()

    def client(self, name, timeout_ms=None):
        req, resp, _ = vw.ECUS[name]
        return UdsClient(self.link, req, resp, timeout_ms=timeout_ms)

    # ---- the steps
    def listen(self):
        from .cli import decode_lines
        self.link.set_silent(True)
        out = {}
        half = self.args.listen / 2.0
        for phase, what in (("off", "Ignition OFF (key out or car locked)."), ("on", "Ignition ON, engine OFF.")):
            self.prompt(what)
            self.link.set_monitor(True)
            frames = self.link.read_frames(half)
            self.link.set_monitor(False)
            with open(self.path("listen_%s.log" % phase), "w") as f:
                f.write("\n".join(frames) + ("\n" if frames else ""))
            ids = sorted({line.split()[0] for line in frames if line.split()})
            out[phase] = {"frames": len(frames), "ids": ids, "decoded": decode_lines(frames, summary=True)}
            self.note("ignition %s: %d frames, %d IDs in %.0f s" % (phase, len(frames), len(ids), half))
        if out["off"]["frames"] + out["on"]["frames"] == 0:
            self.note("verdict: no broadcast traffic at the OBD port; the gateway keeps it in. "
                      "High-rate data will need the gateway tap.")
        else:
            self.note("verdict: broadcast traffic IS visible at the OBD port; see listen_*.log")
        return out

    def scan(self):
        self.link.set_silent(False)
        found, out = [], {}
        for name, (req, resp, desc) in vw.ECUS.items():
            try:
                retry(lambda: self.client(name, 300).tester_present())
                state = "present"
            except NegativeResponse:
                state = "present (refused TesterPresent)"
            except (NoResponse, InterfaceRefused):
                state = "no answer"
            out[name] = {"request": "%03X" % req, "response": "%03X" % resp, "state": state}
            self.note("%-8s %03X -> %03X  %-28s %s" % (name, req, resp, desc, state))
            if state != "no answer":
                found.append(name)
        extra = []
        if self.args.sweep:
            # TesterPresent to every other 11-bit diagnostic address (response = request + 0x6A)
            known = {v[0] for v in vw.ECUS.values()}
            for req in range(0x700, 0x796):
                if req in known:
                    continue
                try:
                    UdsClient(self.link, req, req + vw.RX_OFFSET, timeout_ms=100).tester_present()
                    extra.append("%03X" % req)
                except NegativeResponse:
                    extra.append("%03X" % req)
                except (NoResponse, InterfaceRefused):
                    pass
            self.note("address sweep: %s" % (", ".join(extra) or "nothing else answers"))
        self.found = found
        idents = {}
        for name in found:
            try:
                items = vw.identify(self.client(name))
            except UdsError as e:
                items = [("error", str(e))]
            idents[name] = dict(items)
            for label, value in items:
                self.note("  %-8s %-17s %s" % (name, label, value))
        return {"modules": out, "sweep": extra, "identification": idents}

    def faults(self):
        from .cli import dtc_details, misfire_report
        out = {}
        for name in getattr(self, "found", list(vw.ECUS)):
            client = self.client(name)
            try:
                _, codes = retry(lambda: client.read_dtcs())
            except UdsError as e:
                out[name] = {"error": str(e)}
                self.note("%s: %s" % (name, e))
                continue
            entries = []
            self.note("%s: %d fault code(s)" % (name, len(codes)))
            for code, status in codes:
                text = dtcfmt.format_dtc(code, status)
                details = dtc_details(client, code)
                entries.append({"code": dtcfmt.sae_code(code), "description": dtcfmt.describe(code),
                                "raw": code.hex().upper(), "status": status,
                                "text": text, "details": details})
                self.note("  " + text)
                for line in details:
                    self.note("      " + line)
            out[name] = entries
        if "engine" in out or "engine" in getattr(self, "found", []):
            self.note("engine, emission view:")
            rep = misfire_report(self.client("engine"))
            for line in rep.pop("lines"):
                self.note("  " + line)
            out["engine_obd"] = rep
        return out

    def idle(self):
        engine = self.client("engine")
        supported = retry(lambda: obd.supported_pids(engine))
        names = [n for n in IDLE_VALUES if obd.PIDS[n][0] in supported]
        self.note("engine supports %d OBD values: %s" % (len(supported), " ".join("%02X" % p for p in supported)))
        vw_names = []
        for n in VW_VALUES:
            try:
                simos18.read(engine, n)
                vw_names.append(n)
            except UdsError:
                pass
        self.note("engine measuring values available: %s" % (", ".join(vw_names) or "none (may need another session)"))
        self.prompt("Start the engine and let it idle; hands off the pedal.")
        rows = self.capture(engine, names, vw_names, self.args.idle, "idle.csv")
        counts = obd.misfire_counts(engine)
        self.note("idle capture: %d rows; misfires this cycle by cylinder: %s" % (
            len(rows), ", ".join("%d:%s" % (c, counts[c][0]) for c in range(1, 5))))
        return {"supported_pids": ["%02X" % p for p in supported], "values": names, "vw_values": vw_names,
                "rows": len(rows), "misfires": {str(c): v for c, v in counts.items()}}

    def capture(self, engine, names, vw_names, seconds, filename):
        rows = []
        head = ["time_s"] + ["%s_%s" % (n, obd.PIDS[n][1]) for n in names] + vw_names + \
               ["mis_c%d" % c for c in range(1, 5)]
        with open(self.path(filename), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(head)
            start = time.monotonic()
            next_mis = start
            while time.monotonic() - start < seconds:
                t0 = time.monotonic()
                row = ["%.2f" % (t0 - start)]
                for n in names:
                    try:
                        row.append("%g" % retry(lambda: obd.read_pid(engine, n)[0], tries=2))
                    except UdsError:
                        row.append("")
                for n in vw_names:
                    try:
                        row.append("%g" % round(simos18.read(engine, n)[0], 3))
                    except UdsError:
                        row.append("")
                if t0 >= next_mis:   # the misfire monitor changes slowly: every few seconds is enough
                    try:
                        counts = obd.misfire_counts(engine)
                        row += [str(counts[c][0]) for c in range(1, 5)]
                    except UdsError:
                        row += [""] * 4
                    next_mis = t0 + 5
                else:
                    row += [""] * 4
                w.writerow(row)
                f.flush()
                rows.append(row)
                time.sleep(max(0, self.args.interval - (time.monotonic() - t0)))
        return rows

    def drive(self):
        from .cli import misfire_report
        engine = self.client("engine")
        req, resp, _ = vw.ECUS["engine"]
        out = {}
        self.link.set_time(int(time.time()))
        self.link.poll_clear()
        try:
            for n in DRIVE_POLL:
                self.link.poll_add(req, resp, 500, [0x01, obd.PIDS[n][0]])
            state = self.link.log_start()
            out["log"] = state["name"]
            self.note("logging to the card: %s (polling %s every 0.5 s)" % (state["name"], ", ".join(DRIVE_POLL)))
            on_card = True
        except (LinkError, UdsError) as e:
            self.link.poll_clear()
            self.note("no card logging (%s); recording from this computer instead" % e)
            on_card = False
        self.prompt("Drive. %s" % ("Ends after %g s." % self.args.drive if self.args.drive else
                                   "Type text + Enter to mark a moment, q + Enter to finish."))
        marks = []
        if on_card and not self.args.drive:
            while True:
                text = self.ask("mark> ").strip()
                if text.lower() in ("q", "quit", "end"):
                    break
                self.link.log_mark(text or "mark")
                marks.append(text or "mark")
        elif on_card:
            time.sleep(self.args.drive)
        else:
            self.capture(engine, [n for n in DRIVE_POLL], [], self.args.drive or 60, "drive.csv")
        if on_card:
            self.link.poll_clear()
            state = self.link.log_stop()
            out.update(frames=state["frames"], lost=state["lost"], marks=marks)
            self.note("card log %s: %d frames, %d lost, %d marks" % (state["name"], state["frames"], state["lost"], len(marks)))
        rep = misfire_report(engine)
        for line in rep.pop("lines"):
            self.note("  " + line)
        out["after"] = rep
        return out

    def run(self):
        self.note("first car session, %s; results in %s" % (self.data["started"], self.dir))
        status = self.link.status()
        self.data["interface"] = status
        try:
            self.data["selftest"] = self.link.selftest()
            bad = [i for i in self.data["selftest"] if i["result"] in ("fail", "warn")]
            self.note("interface self-test: %s" % ("; ".join("%s %s: %s" % (i["result"], i["id"], i["detail"])
                                                             for i in bad) or "nothing failed"))
        except (LinkError, AttributeError):
            pass
        if self.args.listen:
            self.step("listen", "1. listen only", self.listen)
        self.step("scan", "2. modules and identification", self.scan)
        self.step("faults", "3. fault codes", self.faults)
        if self.args.idle:
            self.step("idle", "4. idle capture", self.idle)
        if self.args.drive or (self.args.drive is None and not self.args.yes):
            self.step("drive", "5. drive", self.drive)
        else:
            self.note("")
            self.note("== 5. drive: skipped (pass --drive SECONDS)")
        self.link.set_silent(True)
        self.data["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
        self.note("")
        self.note("done; the bus is back to listen-only. Everything is in %s" % self.dir)
        self.save()
        return self.data
