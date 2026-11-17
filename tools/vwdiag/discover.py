"""Measuring-value discovery: which data identifiers a control module answers, read only.

    python3 -m vwdiag --transmit discover                       # the engine: known values and VW/OBD ranges
    python3 -m vwdiag --transmit discover --range 2000-20FF --range 3900-39FF
    python3 -m vwdiag --transmit discover --out discoveries/engine-1 --resume

Only ReadDataByIdentifier (service 0x22), one identifier per request, in the session the module is
already in: nothing is written, no session is changed, no security access is tried. It is paced
(--interval), backs off when the module says it is busy, and stops by itself if the module stops
answering, refuses the service, or refuses everything for "conditions not correct".

Each identifier that answers is read a second time at the end: if the bytes changed, it is a live
value rather than a constant. Results go into a folder: catalogue.json (everything, rewritten as it
goes, so --resume carries on after a stop), catalogue.csv (what answered) and summary.txt.
"""
import csv
import json
import os
import time

from . import simos18, vw
from .uds import NegativeResponse, NoResponse, UdsClient

MIN_INTERVAL = 0.02
CONFIRM_ABOVE = 4096      # sweeps longer than this need --yes
MAX_QUIET = 5             # no answer this many times in a row: the module has gone quiet, stop
MAX_CONDITIONS = 50       # "conditions not correct" this many times in a row: stop
BUSY = (0x21, 0x78)
STOP_NRC = {0x11: "the module does not support reading identifiers",
            0x7F: "the module does not allow reading identifiers in this session"}


def parse_range(text):
    """'2000-20FF' or '2032' -> range of identifiers."""
    lo, _, hi = text.partition("-")
    lo, hi = int(lo, 16), int(hi or lo, 16)
    if not 0 <= lo <= hi <= 0xFFFF:
        raise ValueError("bad identifier range %r (hex, e.g. 2000-20FF)" % text)
    return range(lo, hi + 1)


def default_dids(module):
    """The engine: every Simos 18 measuring value, VW identification F180-F1FF and the OBD mirror
    F400-F4FF. Other modules: the identification range only."""
    dids = set(range(0xF180, 0xF200))
    if module == "engine":
        dids |= {v[0] for v in simos18.VALUES.values()}
        dids |= set(range(0xF400, 0xF500))
    return sorted(dids)


KNOWN = {v[0]: (k, v) for k, v in simos18.VALUES.items()}
ID_NAMES = dict(vw.ID_DIDS)
ID_NAMES[0xF186] = "Active diagnostic session"


def describe(did, data, module):
    """What we already know about an identifier's answer: {"name", "value", "unit"} or {}."""
    if module == "engine" and did in KNOWN:
        key, (_, length, signed, unit, decode, name) = KNOWN[did]
        out = {"name": key}
        if len(data) >= length:
            out["value"] = round(decode(int.from_bytes(data[:length], "big", signed=signed)), 3)
            out["unit"] = unit
        return out
    if did in ID_NAMES:
        return {"name": ID_NAMES[did], "value": vw.text(data)}
    if 0xF400 <= did <= 0xF4FF:
        from . import obd
        name = obd.BY_PID.get(did & 0xFF)
        return {"name": "OBD PID %02X%s" % (did & 0xFF, " (%s)" % name if name else "")}
    return {}


class Discovery:
    def __init__(self, link, module, dids, out_dir, interval=0.05, say=print, resume=False):
        req, resp, _ = vw.ECUS[module]
        self.client = UdsClient(link, req, resp, timeout_ms=300)
        self.module, self.dids, self.dir, self.say = module, list(dids), out_dir, say
        self.interval = max(MIN_INTERVAL, interval)
        os.makedirs(out_dir, exist_ok=True)
        self.data = {"module": module, "started": time.strftime("%Y-%m-%d %H:%M:%S"), "results": {},
                     "stopped": None}
        path = self.path("catalogue.json")
        if resume and os.path.exists(path):
            with open(path) as f:
                old = json.load(f)
            if old.get("module") != module:
                raise ValueError("%s is a sweep of the %s module, not %s" % (path, old.get("module"), module))
            self.data["results"] = old.get("results", {})
            self.data["started"] = old.get("started", self.data["started"])

    def path(self, name):
        return os.path.join(self.dir, name)

    def save(self):
        tmp = self.path("catalogue.json.tmp")
        with open(tmp, "w") as f:
            json.dump(self.data, f, indent=1)
        os.replace(tmp, self.path("catalogue.json"))

    def read(self, did):
        """One identifier, with back-off when busy. Returns ("data", bytes) / ("nrc", code) / ("none", None)."""
        pause = self.interval
        for _ in range(4):
            try:
                return "data", self.client.read_did(did)
            except NegativeResponse as e:
                if e.nrc not in BUSY:
                    return "nrc", e.nrc
            except NoResponse:
                return "none", None
            pause *= 2
            time.sleep(pause)
        return "nrc", 0x21

    def run(self):
        todo = [d for d in self.dids if "%04X" % d not in self.data["results"]]
        self.say("%s: %d identifiers to read (%d already done), one every %.2f s" % (
            self.module, len(todo), len(self.dids) - len(todo), self.interval))
        quiet = conditions = 0
        results = self.data["results"]
        for n, did in enumerate(todo, 1):
            kind, value = self.read(did)
            key = "%04X" % did
            if kind == "data":
                results[key] = dict({"status": "data", "length": len(value), "hex": value.hex().upper()},
                                    **describe(did, value, self.module))
                quiet = conditions = 0
            elif kind == "nrc":
                results[key] = {"status": "refused", "nrc": value}
                quiet = 0
                conditions = conditions + 1 if value == 0x22 else 0
                if value in STOP_NRC:
                    self.data["stopped"] = "%s (NRC %02X)" % (STOP_NRC[value], value)
                elif conditions >= MAX_CONDITIONS:
                    self.data["stopped"] = "%d refusals in a row for 'conditions not correct'" % conditions
            else:
                results[key] = {"status": "no answer"}
                quiet += 1
                if quiet >= MAX_QUIET:
                    # leave these out, so a resumed sweep asks them again
                    for d in todo[n - quiet:n]:
                        results.pop("%04X" % d, None)
                    self.data["stopped"] = "no answer %d times in a row: the module went quiet" % quiet
            if self.data["stopped"]:
                break
            if n % 50 == 0:
                self.save()
                self.say("  %d / %d, %d answered so far" % (n, len(todo), self.count("data")))
            time.sleep(self.interval)
        if not self.data["stopped"]:
            self.second_read()
        self.data["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
        self.save()
        self.write_csv()
        lines = self.summary()
        with open(self.path("summary.txt"), "w") as f:
            f.write("\n".join(lines) + "\n")
        for line in lines:
            self.say(line)
        return self.data

    def second_read(self):
        """Read every answering identifier again: changed bytes mean a live value."""
        answered = [k for k, v in self.data["results"].items() if v["status"] == "data"]
        if not answered:
            return
        time.sleep(1.0)
        for key in answered:
            kind, value = self.read(int(key, 16))
            r = self.data["results"][key]
            if kind == "data":
                r["live"] = value.hex().upper() != r["hex"]
            time.sleep(self.interval)

    def count(self, status):
        return sum(1 for v in self.data["results"].values() if v["status"] == status)

    def write_csv(self):
        with open(self.path("catalogue.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["did", "length", "hex", "ascii", "live", "name", "value", "unit"])
            for key, r in sorted(self.data["results"].items()):
                if r["status"] != "data":
                    continue
                raw = bytes.fromhex(r["hex"])
                w.writerow([key, r["length"], r["hex"], vw.text(raw), {True: "yes", False: "no"}.get(r.get("live"), ""),
                            r.get("name", ""), r.get("value", ""), r.get("unit", "")])

    def summary(self):
        res = self.data["results"]
        answered = {k: v for k, v in res.items() if v["status"] == "data"}
        refused = {}
        for v in res.values():
            if v["status"] == "refused":
                refused[v["nrc"]] = refused.get(v["nrc"], 0) + 1
        out = ["", "%s module: %d identifiers read, %d answered, %d refused, %d without an answer" % (
            self.module, len(res), len(answered), sum(refused.values()), self.count("no answer"))]
        if self.data["stopped"]:
            out.append("stopped early: %s. Run again with --resume to carry on." % self.data["stopped"])
        from .uds import NRC_NAMES
        for nrc, n in sorted(refused.items()):
            note = {0x31: "not there", 0x33: "needs security access (not attempted)",
                    0x22: "not now: may answer with the engine running, or in another session",
                    0x7E: "not in this session", 0x7F: "not in this session"}.get(nrc, NRC_NAMES.get(nrc, ""))
            out.append("  refused %02X x %d: %s" % (nrc, n, note))
        if self.module == "engine":
            known = sorted((v[0], k) for k, v in simos18.VALUES.items())
            yes = [k for did, k in known if "%04X" % did in answered]
            no = [k for did, k in known if "%04X" % did in res and "%04X" % did not in answered]
            out.append("Simos 18 list: %d of %d answer" % (len(yes), len(yes) + len(no)))
            if no:
                out.append("  not on this car (or not now): %s" % ", ".join(no))
        new = [(k, v) for k, v in sorted(answered.items()) if "name" not in v]
        out.append("answering identifiers not in any list here: %d" % len(new))
        for k, v in new[:40]:
            raw = bytes.fromhex(v["hex"])
            out.append("  %s  %2d bytes%s  %s  %s" % (k, v["length"], "  live" if v.get("live") else "      ",
                                                      v["hex"][:32], vw.text(raw)[:24]))
        if len(new) > 40:
            out.append("  ... and %d more in catalogue.csv" % (len(new) - 40))
        out.append("everything is in %s" % self.dir)
        return out
