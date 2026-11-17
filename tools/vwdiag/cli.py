"""Command-line front end.

    python3 -m vwdiag --sim scan
    python3 -m vwdiag --port /dev/cu.usbmodem1101 info engine
    python3 -m vwdiag --port ... live rpm coolant voltage --csv drive.csv

Run from the tools/ directory, or set PYTHONPATH=tools.
"""
import argparse
import csv
import glob
import os
import sys
import time

from . import dbc
from . import dtc as dtcfmt
from . import logfile, obd, simos18, vw
from .binlink import BinaryLink, BleSimLink
from .link import Link, LinkError
from .uds import NegativeResponse, NoResponse, UdsClient, UdsError

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIM_BINARY = os.path.join(REPO, "hardware", "pico2w", "firmware", "sim", "vw_diag_sim")


def open_link(args):
    if args.sim_profile:
        # the simulator reads its scenario from the environment when it starts
        saved = os.environ.get("VWSIM")
        os.environ["VWSIM"] = args.sim_profile
        try:
            return _open_link(args)
        finally:
            if saved is None:
                os.environ.pop("VWSIM", None)
            else:
                os.environ["VWSIM"] = saved
    return _open_link(args)


def _open_link(args):
    Link = BinaryLink if args.binary else globals()["Link"]
    if args.sim_ble:
        if not os.path.exists(SIM_BINARY):
            sys.exit("simulator not built: run  make -C hardware/pico2w/firmware/sim")
        return BleSimLink.simulator(SIM_BINARY)
    if args.sim:
        if not os.path.exists(SIM_BINARY):
            sys.exit("simulator not built: run  make -C hardware/pico2w/firmware/sim")
        return Link.simulator(SIM_BINARY)
    port = args.port
    if not port:
        found = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
        if len(found) != 1:
            sys.exit("pass --port (found %s) or --sim" % (", ".join(found) or "no USB serial ports"))
        port = found[0]
    return Link.serial(port)


def client_for(link, name):
    if name not in vw.ECUS:
        sys.exit("unknown control module %r; choose from: %s" % (name, ", ".join(vw.ECUS)))
    req, resp, _ = vw.ECUS[name]
    return UdsClient(link, req, resp)


def enable_transmit(link, args):
    """The board powers up listen-only. Commands that send need --transmit."""
    if not args.transmit:
        sys.exit("this command transmits on the CAN bus; add --transmit to allow it")
    link.set_silent(False)


# ---------------------------------------------------------------- commands
def cmd_status(link, args):
    s = link.status()
    print("bus %s" % ("listen-only" if s["silent"] else "transmit-enabled"))
    print("rx %(rx)d dropped %(dropped)d tx %(tx)d errors %(errors)d" % s)
    print("keepalive %s, monitor %s" % ("on" if s["keepalive"] else "off", "on" if s["monitor"] else "off"))
    print_log_state(link.log_status())
    if getattr(link, "info", None):
        print("%(name)s, protocol %(version)d" % link.info)


def cmd_scan(link, args):
    enable_transmit(link, args)
    present = []
    for name, (req, resp, desc) in vw.ECUS.items():
        try:
            UdsClient(link, req, resp, timeout_ms=300).tester_present()
            state = "present"
        except NegativeResponse:
            state = "present (refused TesterPresent)"
        except NoResponse:
            state = "no answer"
        if state != "no answer":
            present.append(name)
        print("%-8s %03X -> %03X  %-28s %s" % (name, req, resp, desc, state))
    return present


def cmd_info(link, args):
    enable_transmit(link, args)
    for name in args.ecu or ["engine"]:
        print("%s:" % name)
        items = vw.identify(client_for(link, name))
        if not items:
            print("  no identification data (module absent or not answering)")
        for label, value in items:
            print("  %-17s %s" % (label, value))
        if name == "engine" and items:
            try:
                info = obd.vehicle_info(client_for(link, name))
            except UdsError:
                info = {}
            for key, label in (("calibration_id", "OBD calibration"), ("cvn", "OBD CVN"), ("module_name", "OBD name")):
                if key in info:
                    print("  %-17s %s" % (label, info[key]))


def cmd_dtc(link, args):
    enable_transmit(link, args)
    for name in args.ecu or ["engine"]:
        try:
            _, codes = client_for(link, name).read_dtcs(args.mask)
        except UdsError as e:
            print("%s: %s" % (name, e))
            continue
        print("%s: %d fault code(s)" % (name, len(codes)))
        for code, status in codes:
            print("  " + dtcfmt.format_dtc(code, status))
            if args.details:
                for line in dtc_details(client_for(link, name), code):
                    print("      " + line)


def dtc_details(client, code):
    """What the module stored with a fault code: freeze-frame values and counters. Text lines."""
    lines = []
    try:
        for rec, items in sorted(client.dtc_snapshots(code, sizes=obd.SNAPSHOT_SIZES).items()):
            values = obd.decode_snapshot(items)
            text = ", ".join("%s %s %s" % (n, fmt(v), obd.PIDS[n][1]) for n, v in values.items())
            lines.append("snapshot %02X: %s" % (rec, text or "(no values this tool decodes)"))
    except UdsError as e:
        lines.append("snapshot: %s" % e)
    try:
        ext = client.dtc_extended(code)
        if ext:
            lines.append("extended data: %s" % ext.hex(" ").upper())
    except UdsError as e:
        lines.append("extended data: %s" % e)
    return lines


def fmt(v):
    return ("%.2f" % v).rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def cmd_clear(link, args):
    enable_transmit(link, args)
    if not args.yes:
        sys.exit("clearing fault codes erases them from the module; add --yes to confirm")
    for name in args.ecu:
        client_for(link, name).clear_dtcs()
        print("%s: fault codes cleared" % name)


def cmd_pids(link, args):
    enable_transmit(link, args)
    pids = obd.supported_pids(client_for(link, "engine"))
    for pid in pids:
        print("%02X  %s" % (pid, obd.BY_PID.get(pid, "(no decoder)")))


def cmd_live(link, args):
    enable_transmit(link, args)
    names = args.pid or ["rpm", "speed", "coolant", "voltage"]
    for n in names:
        if n not in obd.PIDS:
            sys.exit("unknown value %r; choose from: %s" % (n, ", ".join(obd.PIDS)))
    client = client_for(link, "engine")
    out = open(args.csv, "w", newline="") if args.csv else None
    writer = csv.writer(out) if out else None
    if writer:
        writer.writerow(["time_s"] + ["%s_%s" % (n, obd.PIDS[n][1]) for n in names])
    print("  time_s " + " ".join("%10s" % n for n in names))
    start = time.monotonic()
    count = 0
    try:
        while args.count is None or count < args.count:
            t0 = time.monotonic()
            row = []
            for n in names:
                try:
                    value, _ = obd.read_pid(client, n)
                    row.append(value)
                except (NegativeResponse, NoResponse):
                    row.append(None)
            stamp = t0 - start
            print("%8.2f " % stamp + " ".join("%10s" % ("-" if v is None else ("%.2f" % v).rstrip("0").rstrip(".")) for v in row))
            if writer:
                writer.writerow(["%.3f" % stamp] + ["" if v is None else "%g" % v for v in row])
                out.flush()
            count += 1
            delay = args.interval - (time.monotonic() - t0)
            if delay > 0:
                time.sleep(delay)
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()
            print("wrote %d rows to %s" % (count, args.csv))


def cmd_misfire(link, args):
    """Everything the engine module reports about misfires, for a low-compression hunt."""
    enable_transmit(link, args)
    engine = client_for(link, "engine")
    report = misfire_report(engine)
    print("\n".join(report["lines"]))
    if args.watch:
        watch_misfire(engine, args.watch, args.csv)


def misfire_report(engine):
    """Read the misfire picture once. Returns {"lines": [...], plus the values}."""
    out, lines = {}, []
    try:
        lamp, count = obd.monitor_status(engine)
        out["lamp"], out["stored_count"] = lamp, count
        lines.append("warning lamp %s, %d stored emission code(s)" % ("ON" if lamp else "off", count))
    except UdsError as e:
        lines.append("monitor status: %s" % e)
    for pending in (False, True):
        try:
            codes = [dtcfmt.sae_code(c) for c in obd.emission_dtcs(engine, pending)]
            out["pending" if pending else "stored"] = codes
            lines.append("%s codes: %s" % ("pending" if pending else "stored", "none" if not codes else ""))
            for c in codes:
                lines.append("  %s  %s" % (c, dtcfmt.description(c)[0]))
        except UdsError as e:
            lines.append("%s codes: %s" % ("pending" if pending else "stored", e))
    try:
        code, values = obd.freeze_frame(engine)
        out["freeze_frame"] = {"code": dtcfmt.sae_code(code) if code else None, "values": values}
        if code:
            lines.append("freeze frame (%s, %s): %s" % (dtcfmt.sae_code(code), dtcfmt.describe(code), ", ".join(
                "%s %s %s" % (n, fmt(v), obd.PIDS[n][1]) for n, v in values.items())))
        else:
            lines.append("freeze frame: none stored")
    except UdsError as e:
        lines.append("freeze frame: %s" % e)
    try:
        counts = obd.misfire_counts(engine)
        out["misfires"] = {str(c): {"this_cycle": n, "avg_10_cycles": a} for c, (n, a) in counts.items()}
        lines.append("misfire monitor (service 06)   this cycle   avg of last 10 cycles")
        for cyl, (now, avg) in counts.items():
            lines.append("  %-27s %10s   %10s" % ("all cylinders" if cyl == 0 else "cylinder %d" % cyl, now, avg))
        worst = max((c for c in counts if c), key=lambda c: (counts[c][0] or 0) + (counts[c][1] or 0))
        if (counts[worst][0] or 0) + (counts[worst][1] or 0) > 0:
            lines.append("  most misfires: cylinder %d" % worst)
    except UdsError as e:
        lines.append("misfire monitor: %s" % e)
    trims = {}
    for n in ("stft", "ltft", "rpm", "load", "coolant"):
        try:
            trims[n] = obd.read_pid(engine, n)[0]
        except UdsError:
            pass
    out["now"] = trims
    if trims:
        lines.append("now: " + ", ".join("%s %s %s" % (n, fmt(v), obd.PIDS[n][1]) for n, v in trims.items()))
    out["lines"] = lines
    return out


def watch_misfire(engine, seconds, csv_path):
    names = ["rpm", "load", "coolant", "stft", "ltft"]
    out = open(csv_path, "w", newline="") if csv_path else None
    writer = csv.writer(out) if out else None
    head = ["time_s"] + names + ["mis_c%d" % c for c in range(1, 5)]
    if writer:
        writer.writerow(head)
    print(" ".join("%8s" % h for h in head))
    start = time.monotonic()
    try:
        while time.monotonic() - start < seconds:
            t0 = time.monotonic()
            row = []
            for n in names:
                try:
                    row.append(obd.read_pid(engine, n)[0])
                except UdsError:
                    row.append(None)
            try:
                counts = obd.misfire_counts(engine)
                row += [counts[c][0] for c in range(1, 5)]
            except UdsError:
                row += [None] * 4
            cells = ["%.1f" % (t0 - start)] + ["" if v is None else fmt(round(v, 1) if isinstance(v, float) else v) for v in row]
            print(" ".join("%8s" % c for c in cells))
            if writer:
                writer.writerow(cells)
                out.flush()
            time.sleep(max(0, 1.0 - (time.monotonic() - t0)))
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()


def cmd_measure(link, args):
    """Simos 18 measuring values (engine module, service 0x22), e.g. knock retard per cylinder."""
    if args.list or not args.value:
        for k, (did, _, _, unit, _, name) in sorted(simos18.VALUES.items()):
            print("%-16s %04X  %-6s %s" % (k, did, unit, name))
        return
    for n in args.value:
        if n not in simos18.VALUES:
            sys.exit("unknown value %r; see  vwdiag measure --list" % n)
    enable_transmit(link, args)
    engine = client_for(link, "engine")
    for _ in range(args.count):
        cells = []
        for n in args.value:
            try:
                v, unit = simos18.read(engine, n)
                cells.append("%s %s %s" % (n, fmt(round(v, 2)), unit))
            except UdsError as e:
                cells.append("%s: %s" % (n, e))
        print(", ".join(cells))
        if args.count > 1:
            time.sleep(args.interval)


def cmd_selftest(link, args):
    """Bring-up checklist: every part of the board, with what to probe where something fails."""
    from . import selftest
    if args.force and not args.active:
        sys.exit("--force only applies to --active")
    if args.active and not args.transmit:
        sys.exit("--active drives the CAN bus for one loopback frame; add --transmit to allow it")
    if args.active:
        print("running, with the CAN loopback and the sync output; watch the LEDs ...")
    else:
        print("running; watch the LEDs ...")
    items = link.selftest(active=args.active, force=args.force)
    print("\n".join(selftest.report(items)))
    if any(i["result"] == "fail" for i in items):
        sys.exit(1)


def cmd_discover(link, args):
    """Which data identifiers a module answers: service 0x22 only, paced, stops by itself."""
    from . import discover
    enable_transmit(link, args)
    if args.module not in vw.ECUS:
        sys.exit("unknown control module %r; choose from: %s" % (args.module, ", ".join(vw.ECUS)))
    try:
        dids = sorted({d for r in args.range for d in discover.parse_range(r)}) if args.range else \
            discover.default_dids(args.module)
        skip = {d for r in args.skip for d in discover.parse_range(r)}
    except ValueError as e:
        sys.exit("error: %s" % e)
    dids = [d for d in dids if d not in skip]
    if len(dids) > discover.CONFIRM_ABOVE and not args.yes:
        sys.exit("%d identifiers is a long sweep (about %.0f minutes); add --yes to go ahead" % (
            len(dids), len(dids) * (args.interval + 0.03) / 60))
    out = args.out or os.path.join("discoveries", "%s-%s" % (args.module, time.strftime("%Y%m%d-%H%M%S")))
    try:
        discover.Discovery(link, args.module, dids, out, args.interval, resume=args.resume).run()
    except ValueError as e:
        sys.exit("error: %s" % e)


def cmd_session(link, args):
    from .session import Session
    if not args.transmit:
        sys.exit("the session transmits after the listen-only step; add --transmit to allow it")
    out = args.out or os.path.join("sessions", time.strftime("%Y%m%d-%H%M%S"))
    Session(link, args, out).run()


def cmd_experiment(link, args):
    from .experiments import EXPERIMENTS, run_experiment
    if args.name not in EXPERIMENTS:
        sys.exit("unknown experiment; choose from: %s" % ", ".join(EXPERIMENTS))
    if EXPERIMENTS[args.name][1] and not args.transmit:
        sys.exit("this experiment sends requests to the car (read only); add --transmit to allow it")
    run_experiment(link, args)


def cmd_power(link, args):
    """The dongle's power state, and its settings."""
    if args.stay:
        link.power(1, args.stay == "on")
    if args.idle is not None:
        link.power(2, args.idle)
    if args.cutoff is not None:
        link.power(3, args.cutoff)
    if args.sleep:
        link.power(4)
        print("asked to sleep (it stays awake while a computer is on USB)")
        return
    p = link.power()
    print("woke by %s; battery %.2f V" % (p["wake"], p["battery_mv"] / 1000.0))
    print("held awake by: %s" % (", ".join(p["holds"]) or "nothing (it sleeps now)"))
    if p["sleeps_in_s"]:
        print("sleeps in %d s unless something holds it" % p["sleeps_in_s"])
    if p["battery_low_for_s"]:
        print("battery low for %d s" % p["battery_low_for_s"])
    print("idle %d s, battery cutoff %.2f V, engine running above %.2f V, stay awake %s" % (
        p["idle_s"], p["cutoff_mv"] / 1000.0, p["running_mv"] / 1000.0, "on" if p["stay_awake"] else "off"))


def print_gnss(g):
    print("GPS: %s, %s, pulse %s" % ("receiver present" if g["present"] else "no receiver",
                                     "fix" if g["fix"] else "no fix", "locked" if g["pulse_locked"] else "not locked"))
    if g["present"]:
        print("  %.7f, %.7f, %.1f m; %d satellites, quality %d, hdop %.2f" % (
            g["lat"], g["lon"], g["alt_m"], g["sats"], g["quality"], g["hdop"]))
        print("  %.2f m/s heading %.1f deg; UTC %s" % (
            g["speed_ms"], g["course_deg"],
            time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(g["utc"])) if g["utc"] else "not known"))
    if g["drift_ppb"] is not None:
        print("  the interface's clock runs %+.2f ppm against GPS" % (g["drift_ppb"] / 1000.0))
    print("  log clock %s; %d sentences (%d bad), %d pulses (%d paired)" % (
        "from GPS" if g["log_clock_from_gps"] else "not from GPS", g["sentences"], g["bad_sentences"],
        g["pulses"], g["paired"]))
    print("sync output: %s, %d edges sent" % ("%d Hz" % g["sync_hz"] if g["sync_hz"] else "off", g["sync_edges"]))


def cmd_gps(link, args):
    """The dongle's GPS receiver, pulse lock and clock drift; the camera sync output."""
    sync = None if args.sync is None else (0 if args.sync == "off" else int(args.sync))
    g = link.gnss(sync)
    print_gnss(g)
    end = time.monotonic() + (args.watch or 0)
    while time.monotonic() < end:
        time.sleep(1.0)
        g = link.gnss()
        print("%s  %s %d sats  %.7f %.7f  %.2f m/s  drift %s" % (
            time.strftime("%H:%M:%S"), "fix" if g["fix"] else "no fix", g["sats"], g["lat"], g["lon"], g["speed_ms"],
            "%+.2f ppm" % (g["drift_ppb"] / 1000.0) if g["drift_ppb"] is not None else "-"))


def cmd_raw(link, args):
    enable_transmit(link, args)
    payload = [int(b, 16) for b in args.bytes]
    resp = client_for(link, args.ecu).request(payload)
    print(" ".join("%02X" % b for b in resp))


def decode_lines(lines, summary):
    """Decode monitor lines with the VW MQB database. Returns text lines."""
    db = dbc.Database()
    out, latest, unknown = [], {}, {}
    for line in lines:
        frame = dbc.parse_log_line(line)
        if not frame:
            continue
        text = dbc.describe(db, *frame, only_nonzero=summary)
        if text is None:
            unknown[frame[0]] = unknown.get(frame[0], 0) + 1
        elif summary:
            latest[frame[0]] = text
        else:
            out.append("%s  %s" % (line.strip(), text))
    if summary:
        out += ["%03X %s" % (can_id, latest[can_id]) for can_id in sorted(latest)]
    if unknown:
        out.append("not in the database: " + ", ".join("%03X x%d" % kv for kv in sorted(unknown.items())))
    return out


def cmd_monitor(link, args):
    link.set_monitor(True)
    lines = link.read_frames(args.seconds)
    link.set_monitor(False)
    if args.out:
        with open(args.out, "w") as f:
            f.write("\n".join(lines) + "\n")
        print("captured %d frames to %s" % (len(lines), args.out))
    if args.decode:
        print("\n".join(decode_lines(lines, summary=True)))
    elif not args.out:
        print("\n".join(lines))


def print_log_state(s):
    if not s["name"]:
        print("log off")
        return
    print("log %s: %s, %d frames, %d bytes%s" % ("on" if s["active"] else "off", s["name"], s["frames"], s["bytes"],
                                                 ", %d frames lost" % s["lost"] if s["lost"] else ""))


def poll_payload(name):
    """The engine-module request that asks for one value: an OBD value (service 01), or a Simos 18 measuring
    value (ReadDataByIdentifier, whose answer fits a single frame, as the poll list needs)."""
    if name in obd.PIDS:
        return [0x01, obd.PIDS[name][0]]
    did = simos18.VALUES[name][0]
    return [0x22, did >> 8, did & 0xFF]


def cmd_log(link, args):
    if args.action == "start":
        link.set_time(int(time.time()))
        if args.poll:
            for n in args.poll:
                if n not in obd.PIDS and n not in simos18.VALUES:
                    sys.exit("unknown value %r; choose from the OBD values (%s) or the engine's measuring values "
                             "(vwdiag measure --list)" % (n, ", ".join(obd.PIDS)))
            if len(args.poll) > 8:
                sys.exit("the interface repeats at most 8 requests; choose 8 values or fewer")
            enable_transmit(link, args)
            req, resp, _ = vw.ECUS["engine"]
            link.poll_clear()
            for n in args.poll:
                link.poll_add(req, resp, int(args.interval * 1000), poll_payload(n))
        print_log_state(link.log_start())
        if args.poll:
            print("polling %s every %g s; this continues after the tools disconnect" % (", ".join(args.poll), args.interval))
    elif args.action == "stop":
        link.poll_clear()
        print_log_state(link.log_stop())
    elif args.action == "mark":
        link.log_mark(" ".join(args.text) or "mark")
        print_log_state(link.log_status())
    else:
        print_log_state(link.log_status())


def cmd_files(link, args):
    """List the card's files, or copy some of them to this computer."""
    files = link.log_files()
    if not args.name:
        for name, size in sorted(files):
            print("%-14s %10d bytes" % (name, size))
        print("%d file(s)" % len(files))
        return
    sizes = dict(files)
    names = [n for n, _ in sorted(files) if n.upper().endswith(".VWL")] if args.name == ["all"] else args.name
    os.makedirs(args.out, exist_ok=True)
    for name in names:
        if name not in sizes:
            sys.exit("no file %s on the card" % name)
        path = os.path.join(args.out, name)
        if os.path.exists(path) and os.path.getsize(path) == sizes[name]:
            print("%s: already here" % name)
            continue
        start = time.monotonic()
        data = link.fetch(name, progress=lambda done, total: print("\r%s: %d of %d bytes" % (name, done, total), end="", flush=True))
        with open(path, "wb") as f:
            f.write(data)
        rate = len(data) / max(time.monotonic() - start, 1e-6)
        print("\r%s: %d bytes to %s (%.1f kB/s)" % (name, len(data), path, rate / 1000))


def cmd_calibrate(args):
    from .calibrate import run
    run(args.folders, base=args.base, do_verify=args.verify, out=args.out)


def cmd_logread(args):
    try:
        log = logfile.Log(args.file)
    except (OSError, logfile.LogError) as e:
        sys.exit("error: %s" % e)
    ids = {}
    for f in log.frames:
        ids[(f.can_id, f.extended)] = ids.get((f.can_id, f.extended), 0) + 1
    print("started   %s" % log.start_time_text())
    print("duration  %.1f s" % log.duration)
    print("frames    %d (%d sent by the interface), %d different IDs" % (
        len(log.frames), sum(f.sent for f in log.frames), len(ids)))
    if log.lost:
        print("lost      %d frames in %d gap(s): the card could not keep up" % (log.lost, len(log.lost_events)))
    if log.truncated:
        print("note      the file ends mid-record (power was removed while logging); everything before that is intact")
    for ms, text in log.marks:
        print("mark      %8.3f s  %s" % (log.seconds(ms), text))
    if log.pulses or log.fixes or log.syncs:
        paired = sum(1 for _, utc in log.pulses if utc)
        drift = log.clock_drift_ppm()
        print("gps       %d fixes (%d valid), %d pulses (%d paired with UTC)%s" % (
            len(log.fixes), sum(f.valid for f in log.fixes), len(log.pulses), paired,
            ", clock %+.2f ppm against GPS" % drift if drift is not None else ""))
        if log.syncs:
            print("sync      %d edges sent on the sync output" % len(log.syncs))
    if args.gpx:
        with open(args.gpx, "w") as f:
            f.write(log.gpx())
        print("wrote %d track points to %s" % (sum(f.valid for f in log.fixes), args.gpx))
    if args.sync_csv:
        with open(args.sync_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["edge", "local_us", "utc_s"])
            for us, count in log.syncs:
                t = log.utc_of_us(us)
                w.writerow([count, us, "%.6f" % t if t is not None else ""])
        print("wrote %d sync edges to %s" % (len(log.syncs), args.sync_csv))
    if args.candump:
        with open(args.candump, "w") as f:
            f.write("\n".join(log.candump_lines()) + "\n")
        print("wrote %d frames to %s (can-utils format)" % (len(log.frames), args.candump))
    if args.csv:
        samples = log.samples()
        units = {s[1]: s[3] for s in samples}
        order = list(obd.PIDS)
        names = sorted(units, key=lambda n: (order.index(n) if n in order else len(order), n))
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["time_s"] + ["%s_%s" % (n, units[n]) if units[n] else n for n in names])
            for t, name, value, _ in samples:
                w.writerow(["%.3f" % t] + ["%g" % value if n == name else "" for n in names])
        print("wrote %d live-data samples to %s" % (len(samples), args.csv))
    if args.ids:
        for (can_id, ext), n in sorted(ids.items()):
            print(("%08X" if ext else "%03X") % can_id + "  x%d" % n)
    if args.frames:
        for f in log.frames:
            print("%10.3f %s %s" % (log.seconds(f.ms), "tx" if f.sent else "rx", f.text()))
    if args.decode:
        print("\n".join(decode_lines([f.text() for f in log.frames], summary=True)))


def cmd_replay(args):
    from . import replay
    try:
        log = logfile.Log(args.file)
    except (OSError, logfile.LogError) as e:
        sys.exit("error: %s" % e)
    out = args.out or os.path.splitext(os.path.basename(args.file))[0] + ".html"
    with open(out, "w") as f:
        f.write(replay.render(log))
    data = replay.collect(log)
    print("wrote %s: %d charts, %d track points, %d marks, %.1f s" % (
        out, len(data["series"]), len(data["track"]), len(data["marks"]), data["duration"]))


def cmd_diagnose(args):
    from . import diagnose
    try:
        compression = diagnose.parse_cylinder_values(args.compression)
        wet = diagnose.parse_cylinder_values(args.wet)
        swap = tuple(int(c) for c in args.swap.split(":")) if args.swap else None
        replaced = [p.strip() for p in (args.replaced or "").split(",") if p.strip()]
        for part in replaced:
            if part not in diagnose.PARTS:
                raise ValueError("--replaced takes %s" % ", ".join(diagnose.PARTS))
        if swap and (len(swap) != 2 or not all(1 <= c <= diagnose.CYLINDERS for c in swap)):
            raise ValueError("--swap takes two cylinders, e.g. 3:2")
        sessions = [diagnose.load(d, args.log if i == len(args.session) - 1 else None)
                    for i, d in enumerate(args.session)]
    except (OSError, ValueError) as e:
        sys.exit("error: %s" % e)
    if swap and len(sessions) < 2:
        sys.exit("--swap compares two sessions: before and after the swap")
    result = diagnose.analyze(sessions, compression, wet, swap, replaced)
    text = "\n".join(result["lines"]) + "\n"
    print(text, end="")
    out = args.out or os.path.join(args.session[-1], "diagnosis.txt")
    with open(out, "w") as f:
        f.write(text)
    print("(saved to %s)" % out)


def cmd_decode(args):
    with open(args.log) as f:
        lines = f.read().split("\n")
    print("\n".join(decode_lines(lines, summary=args.summary)))


def build_parser():
    p = argparse.ArgumentParser(prog="vwdiag", description="VW diagnostic interface tools")
    p.add_argument("--port", help="USB serial port of the board")
    p.add_argument("--sim", action="store_true", help="use the host simulator instead of a board")
    p.add_argument("--binary", action="store_true", help="use the binary packet protocol (what an app uses)")
    p.add_argument("--sim-ble", action="store_true", help="use the simulator through its Bluetooth stand-in")
    p.add_argument("--transmit", action="store_true", help="allow sending on the CAN bus")
    p.add_argument("--sim-profile", metavar="SPEC", help="simulator scenario, e.g. gti or realistic,gateway=open "
                   "(see hardware/pico2w/firmware/sim/sim_config.c)")
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("status", help="interface state and counters").set_defaults(fn=cmd_status)
    sub.add_parser("scan", help="find which control modules answer").set_defaults(fn=cmd_scan)
    s = sub.add_parser("info", help="read identification data"); s.add_argument("ecu", nargs="*"); s.set_defaults(fn=cmd_info)
    s = sub.add_parser("dtc", help="read fault codes"); s.add_argument("ecu", nargs="*")
    s.add_argument("--mask", type=lambda v: int(v, 16), default=0xFF)
    s.add_argument("--details", action="store_true", help="also read each code's freeze frame and counters")
    s.set_defaults(fn=cmd_dtc)
    s = sub.add_parser("session", help="the scripted first car session (read only), everything saved")
    s.add_argument("--out", help="folder for the results (default sessions/<date-time>)")
    s.add_argument("--listen", type=float, default=60, help="seconds of listen-only capture, split ignition off/on (0 skips)")
    s.add_argument("--idle", type=float, default=120, help="seconds of idle capture (0 skips)")
    s.add_argument("--drive", type=float, help="seconds of drive logging (default: until you type q; 0 skips)")
    s.add_argument("--interval", type=float, default=1.0, help="seconds between samples in the idle capture")
    s.add_argument("--sweep", action="store_true", help="also try every other diagnostic address 700-795")
    s.add_argument("--yes", action="store_true", help="do not wait at the prompts")
    s.set_defaults(fn=cmd_session)
    s = sub.add_parser("experiment", help="a scripted logging experiment (read only), saved for vwdiag calibrate")
    s.add_argument("name", help="listen, identify, timing, cold-start, warm-idle, rev-holds or drive")
    s.add_argument("--seconds", type=float, help="how long to capture (each phase); defaults per experiment")
    s.add_argument("--interval", type=float, default=0.5, help="seconds between samples")
    s.add_argument("--holds", default="0,1500,2500,3500", help="rev-holds: the engine speeds to hold")
    s.add_argument("--session-test", action="store_true", help="identify: also time the extended session")
    s.add_argument("--out", help="parent folder (default ./experiments)")
    s.add_argument("--yes", action="store_true", help="do not wait at the prompts")
    s.set_defaults(fn=cmd_experiment)
    s = sub.add_parser("power", help="the dongle's power state; set how and when it sleeps")
    s.add_argument("--stay", choices=["on", "off"], help="never sleep for idleness (battery protection still applies)")
    s.add_argument("--idle", type=int, metavar="SECONDS", help="sleep after this long with nothing holding it (30-86400)")
    s.add_argument("--cutoff", type=int, metavar="MV", help="battery cutoff, engine off (10500-13000 mV)")
    s.add_argument("--sleep", action="store_true", help="sleep now")
    s.set_defaults(fn=cmd_power)
    s = sub.add_parser("gps", help="the dongle's GPS fix, pulse lock and clock drift; the camera sync output")
    s.add_argument("--sync", metavar="HZ", help="set the sync output: 1-100 Hz, or off")
    s.add_argument("--watch", type=float, metavar="SECONDS", help="then follow the fix once a second")
    s.set_defaults(fn=cmd_gps)
    s = sub.add_parser("selftest", help="bring-up check of every part of the board, with what to probe for failures")
    s.add_argument("--active", help="also send one CAN loopback frame and pulse the sync output (needs --transmit)",
                   action="store_true")
    s.add_argument("--force", action="store_true", help="run the loopback even with battery voltage or bus traffic "
                   "(a bench supply, never a car)")
    s.set_defaults(fn=cmd_selftest)
    s = sub.add_parser("discover", help="which data identifiers a module answers (read only, paced), saved as a catalogue")
    s.add_argument("--module", default="engine", help="control module (default engine)")
    s.add_argument("--range", action="append", default=[], help="hex identifiers, e.g. 2000-20FF (repeatable); "
                   "default: the known values and the VW / OBD ranges")
    s.add_argument("--skip", action="append", default=[], help="hex identifiers to leave out (repeatable)")
    s.add_argument("--interval", type=float, default=0.05, help="seconds between requests (at least 0.02)")
    s.add_argument("--out", help="folder (default discoveries/<module>-<date-time>)")
    s.add_argument("--resume", action="store_true", help="carry on a stopped sweep in --out")
    s.add_argument("--yes", action="store_true", help="allow sweeps over 4096 identifiers")
    s.set_defaults(fn=cmd_discover)
    s = sub.add_parser("measure", help="engine measuring values (Simos 18 list), e.g. knock_cyl_1 misfires")
    s.add_argument("value", nargs="*"); s.add_argument("--list", action="store_true")
    s.add_argument("--count", type=int, default=1); s.add_argument("--interval", type=float, default=0.5)
    s.set_defaults(fn=cmd_measure)
    s = sub.add_parser("misfire", help="misfire counts per cylinder, trims, freeze frame, emission codes")
    s.add_argument("--watch", type=float, metavar="SECONDS", help="then follow counts and trims once a second")
    s.add_argument("--csv", help="with --watch: also write the readings here")
    s.set_defaults(fn=cmd_misfire)
    s = sub.add_parser("clear", help="erase fault codes (changes the car)"); s.add_argument("ecu", nargs="+")
    s.add_argument("--yes", action="store_true"); s.set_defaults(fn=cmd_clear)
    sub.add_parser("pids", help="list OBD live values the engine supports").set_defaults(fn=cmd_pids)
    s = sub.add_parser("live", help="poll OBD live values, optionally logging to CSV"); s.add_argument("pid", nargs="*")
    s.add_argument("--interval", type=float, default=0.5); s.add_argument("--count", type=int)
    s.add_argument("--csv"); s.set_defaults(fn=cmd_live)
    s = sub.add_parser("raw", help="send any UDS request (can change the car)"); s.add_argument("ecu")
    s.add_argument("bytes", nargs="+"); s.set_defaults(fn=cmd_raw)
    s = sub.add_parser("monitor", help="capture raw CAN frames (does not transmit)")
    s.add_argument("--seconds", type=float, default=5.0); s.add_argument("--out")
    s.add_argument("--decode", action="store_true", help="show the latest decoded value of each known message")
    s.set_defaults(fn=cmd_monitor)
    s = sub.add_parser("log", help="record CAN frames to the board's microSD card")
    s.add_argument("action", choices=["start", "stop", "status", "mark"]); s.add_argument("text", nargs="*")
    s.add_argument("--poll", nargs="+", metavar="VALUE",
                   help="also request these values on a timer: OBD values (rpm, speed, ...) or the engine's "
                        "measuring values (misfires, knock_cyl_3, ...: vwdiag measure --list); 8 at most")
    s.add_argument("--interval", type=float, default=0.5, help="seconds between requests for each value")
    s.set_defaults(fn=cmd_log)
    s = sub.add_parser("files", help="list the card's files, or copy them here (no SD card reader needed)")
    s.add_argument("name", nargs="*", help="files to copy, or 'all' for every log")
    s.add_argument("--out", default="logs", help="folder to copy into (default ./logs)")
    s.set_defaults(fn=cmd_files)
    s = sub.add_parser("calibrate", help="turn experiment results into simulator settings (no board needed)")
    s.add_argument("folders", nargs="+", help="experiment folders (from vwdiag experiment)")
    s.add_argument("--base", default="realistic", help="scenario to start from (default realistic)")
    s.add_argument("--verify", action="store_true", help="rerun the experiments on the simulator and compare")
    s.add_argument("--out", help="folder for calibration.md (default calibration/<date-time>)")
    s.set_defaults(fn=None, offline=cmd_calibrate)
    s = sub.add_parser("logread", help="read a log file from the card (no board needed)"); s.add_argument("file")
    s.add_argument("--candump", metavar="OUT", help="convert to can-utils format, for SavvyCAN and others")
    s.add_argument("--csv", metavar="OUT", help="extract OBD live values to a spreadsheet file")
    s.add_argument("--gpx", metavar="OUT", help="write the GPS track (dongle with a GPS receiver)")
    s.add_argument("--sync-csv", metavar="OUT", help="write each sync-output edge with its UTC time, for a camera rig")
    s.add_argument("--ids", action="store_true", help="count frames per CAN ID")
    s.add_argument("--frames", action="store_true", help="print every frame")
    s.add_argument("--decode", action="store_true", help="latest decoded value of each known broadcast message")
    s.set_defaults(fn=None, offline=cmd_logread)
    s = sub.add_parser("replay", help="a log as an HTML page: engine values, GPS speed and track on one timeline")
    s.add_argument("file"); s.add_argument("--out", help="output file (default: the log's name with .html)")
    s.set_defaults(fn=None, offline=cmd_replay)
    s = sub.add_parser("diagnose", help="misfire diagnosis from saved sessions: likely causes, next tests (no board needed)")
    s.add_argument("session", nargs="+", help="session folder(s), oldest first")
    s.add_argument("--log", help="the drive log copied off the card (default: looked for in the session folder)")
    s.add_argument("--compression", help="dry compression readings, e.g. 1:185,2:180,3:125,4:182 (any unit)")
    s.add_argument("--wet", help="wet compression readings, same form")
    s.add_argument("--replaced", help="parts already renewed on the misfiring cylinder: coil,plug,injector")
    s.add_argument("--swap", help="A:B: you swapped a part (coil, plug) between cylinders A and B between the sessions")
    s.add_argument("--out", help="where to save the text (default diagnosis.txt in the last session folder)")
    s.set_defaults(fn=None, offline=cmd_diagnose)
    s = sub.add_parser("decode", help="decode a saved monitor log (no board needed)"); s.add_argument("log")
    s.add_argument("--summary", action="store_true", help="only the latest value of each message")
    s.set_defaults(fn=None, offline=cmd_decode)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    if getattr(args, "offline", None):
        return args.offline(args)
    try:
        with open_link(args) as link:
            args.fn(link, args)
    except (UdsError, LinkError) as e:
        sys.exit("error: %s" % e)


if __name__ == "__main__":
    main()
