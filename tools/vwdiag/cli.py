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
from . import logfile, obd, vw
from .binlink import BinaryLink, BleSimLink
from .link import Link, LinkError
from .uds import NegativeResponse, NoResponse, UdsClient, UdsError

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIM_BINARY = os.path.join(REPO, "hardware", "pico2w", "firmware", "sim", "vw_diag_sim")


def open_link(args):
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


def cmd_log(link, args):
    if args.action == "start":
        link.set_time(int(time.time()))
        if args.poll:
            for n in args.poll:
                if n not in obd.PIDS:
                    sys.exit("unknown value %r; choose from: %s" % (n, ", ".join(obd.PIDS)))
            enable_transmit(link, args)
            req, resp, _ = vw.ECUS["engine"]
            link.poll_clear()
            for n in args.poll:
                link.poll_add(req, resp, int(args.interval * 1000), [0x01, obd.PIDS[n][0]])
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
    if args.candump:
        with open(args.candump, "w") as f:
            f.write("\n".join(log.candump_lines()) + "\n")
        print("wrote %d frames to %s (can-utils format)" % (len(log.frames), args.candump))
    if args.csv:
        samples = list(log.obd_samples())
        names = sorted({s[1] for s in samples}, key=list(obd.PIDS).index)
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["time_s"] + ["%s_%s" % (n, obd.PIDS[n][1]) for n in names])
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
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("status", help="interface state and counters").set_defaults(fn=cmd_status)
    sub.add_parser("scan", help="find which control modules answer").set_defaults(fn=cmd_scan)
    s = sub.add_parser("info", help="read identification data"); s.add_argument("ecu", nargs="*"); s.set_defaults(fn=cmd_info)
    s = sub.add_parser("dtc", help="read fault codes"); s.add_argument("ecu", nargs="*")
    s.add_argument("--mask", type=lambda v: int(v, 16), default=0xFF); s.set_defaults(fn=cmd_dtc)
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
    s.add_argument("--poll", nargs="+", metavar="VALUE", help="also request these OBD live values on a timer")
    s.add_argument("--interval", type=float, default=0.5, help="seconds between requests for each value")
    s.set_defaults(fn=cmd_log)
    s = sub.add_parser("logread", help="read a log file from the card (no board needed)"); s.add_argument("file")
    s.add_argument("--candump", metavar="OUT", help="convert to can-utils format, for SavvyCAN and others")
    s.add_argument("--csv", metavar="OUT", help="extract OBD live values to a spreadsheet file")
    s.add_argument("--ids", action="store_true", help="count frames per CAN ID")
    s.add_argument("--frames", action="store_true", help="print every frame")
    s.add_argument("--decode", action="store_true", help="latest decoded value of each known broadcast message")
    s.set_defaults(fn=None, offline=cmd_logread)
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
