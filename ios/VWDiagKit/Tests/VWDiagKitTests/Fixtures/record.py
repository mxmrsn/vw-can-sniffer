"""Record real byte streams from the simulator's Bluetooth stand-in, for the Swift tests.

    python3 record.py        (from this folder; writes simulator.json)

Each entry is a request payload and every byte the simulator sent back until the matching
response (events included), exactly as they came off the socket. The Swift tests replay them
in random-sized pieces with their own sequence numbers.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, *[".."] * 5))
sys.path.insert(0, os.path.join(REPO, "tools"))
from vwdiag import binlink   # noqa: E402

SIM = os.path.join(REPO, "hardware", "pico2w", "firmware", "sim", "vw_diag_sim")


def uds(req, resp, payload, timeout=0):
    return [binlink.CMD_UDS] + list(struct.pack("<IIH", req, resp, timeout)) + payload


STEPS = [
    ("hello", [binlink.CMD_HELLO]),
    ("status", [binlink.CMD_STATUS]),
    ("refused while listen-only", uds(0x7E0, 0x7E8, [0x3E, 0x00])),
    ("transmit on", [binlink.CMD_SET_SILENT, 0]),
    ("vin, multi-frame", uds(0x7E0, 0x7E8, [0x22, 0xF1, 0x90])),
    ("engine code", uds(0x7E0, 0x7E8, [0x22, 0xF1, 0xAD])),
    ("unknown identifier", uds(0x7E0, 0x7E8, [0x22, 0x12, 0x34])),
    ("absent module", uds(0x757, 0x7C1, [0x3E, 0x00], 200)),
    ("engine fault codes", uds(0x7E0, 0x7E8, [0x19, 0x02, 0xFF])),
    ("P0303 snapshot", uds(0x7E0, 0x7E8, [0x19, 0x04, 0x03, 0x03, 0x00, 0xFF])),
    ("supported pids", uds(0x7E0, 0x7E8, [0x01, 0x00])),
    ("supported pids 21-40", uds(0x7E0, 0x7E8, [0x01, 0x20])),
    ("supported pids 41-60", uds(0x7E0, 0x7E8, [0x01, 0x40])),
    ("monitor status", uds(0x7E0, 0x7E8, [0x01, 0x01])),
    ("stored codes", uds(0x7E0, 0x7E8, [0x03])),
    ("freeze frame code", uds(0x7E0, 0x7E8, [0x02, 0x02, 0x00])),
    ("misfire cylinder 3", uds(0x7E0, 0x7E8, [0x06, 0xA4])),
    ("monitor on", [binlink.CMD_SET_MONITOR, 1]),
    ("status while monitoring", [binlink.CMD_STATUS]),
    ("monitor off", [binlink.CMD_SET_MONITOR, 0]),
    ("log status, no card", [binlink.CMD_LOG_STATUS]),
]


def main():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    env = dict(os.environ, VWDIAG_BLE_PORT=str(port), VWDIAG_BLE_MTU="23", VWSIM="gti,latency=0-0,gateway=open", VWDIAG_SD="")
    proc = subprocess.Popen([SIM], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, env=env)
    try:
        for _ in range(100):
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                time.sleep(0.05)
        sock.settimeout(5)
        out = []
        for seq, (name, payload) in enumerate(STEPS, start=1):
            if name == "status while monitoring":
                time.sleep(0.4)   # let some monitored frames arrive first
            sock.sendall(binlink.encode([seq] + payload))
            wire, buf = b"", b""
            while True:
                chunk = sock.recv(4096)
                wire += chunk
                buf += chunk
                done = False
                while b"\x00" in buf:
                    raw, buf = buf.split(b"\x00", 1)
                    p = binlink.decode(raw) if raw else None
                    if p and p[0] == seq:
                        done = True
                if done:
                    break
            out.append({"name": name, "request": bytes(payload).hex(), "wire": wire.hex()})
        with open(os.path.join(HERE, "simulator.json"), "w") as f:
            json.dump(out, f, indent=1)
        print("recorded %d exchanges" % len(out))
    finally:
        proc.kill()


if __name__ == "__main__":
    main()
