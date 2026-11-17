"""Binary packet connection to the interface (the protocol an app will use).

Same methods as link.Link, but the console is switched to the packet protocol
of hardware/pico2w/docs/link_protocol.md: COBS framing, CRC-16, numbered
requests. It tolerates the byte stream arriving in pieces of any size, which is
what a BLE connection does to it.
"""
import os
import socket
import struct
import subprocess
import time

from .link import Link, LinkError
from .uds import InterfaceRefused, NegativeResponse, NoResponse

CMD_HELLO, CMD_STATUS, CMD_SET_SILENT, CMD_SET_MONITOR, CMD_UDS, CMD_SET_KEEPALIVE, CMD_EXIT = 1, 2, 3, 4, 5, 6, 0x7F
CMD_SET_LOG, CMD_SET_TIME, CMD_LOG_STATUS, CMD_POLL_ADD, CMD_POLL_CLEAR, CMD_LOG_MARK = 8, 9, 10, 11, 12, 13
CMD_LOG_LIST, CMD_LOG_READ, CMD_POWER, CMD_GNSS, CMD_SELFTEST = 0x0E, 0x0F, 0x10, 0x11, 0x12
SELFTEST_RESULTS = ["pass", "warn", "fail", "skip", "look"]
EVT_CAN_FRAME = 1
OK, BAD_ARGS, UNKNOWN_CMD, SILENT, TIMEOUT, NEGATIVE, TRANSPORT, STORAGE, BUSY = range(9)
STATUS_NAMES = ["ok", "bad arguments", "unknown command", "silent", "timeout", "negative response", "transport error",
                "no card, or the file could not be written or read", "that file is being recorded: stop logging first"]
MAX_PAYLOAD = 4160
POWER_HOLDS = ["usb", "phone", "activity", "engine", "bus", "logging", "stay"]
POWER_WAKES = ["unknown", "power-on", "usb", "can", "engine", "button"]


def gnss_dict(present, fix, locked, gps_clock, quality, sats, utc, lat, lon, alt_m, speed_ms, course_deg, hdop,
              drift_ppb, sync_hz, pulses, paired, sync_edges, sentences, bad):
    """The GPS state, the same from either link."""
    return {"present": bool(present), "fix": bool(fix), "pulse_locked": bool(locked), "log_clock_from_gps": bool(gps_clock),
            "quality": quality, "sats": sats, "utc": utc, "lat": lat, "lon": lon, "alt_m": alt_m, "speed_ms": speed_ms,
            "course_deg": course_deg, "hdop": hdop, "drift_ppb": drift_ppb, "sync_hz": sync_hz, "pulses": pulses,
            "paired": paired, "sync_edges": sync_edges, "sentences": sentences, "bad_sentences": bad}


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(payload):
    """One packet: COBS(payload + CRC-16 big-endian) + 0x00."""
    raw = bytes(payload) + struct.pack(">H", crc16(payload))
    out = bytearray()
    block = bytearray()
    for b in raw:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block = bytearray()
        else:
            block.append(b)
            if len(block) == 254:
                out.append(0xFF)
                out += block
                block = bytearray()
    out.append(len(block) + 1)
    out += block
    out.append(0)
    return bytes(out)


def decode(packet):
    """Inverse of encode() for one packet without its trailing 0x00. None if damaged."""
    out = bytearray()
    i = 0
    while i < len(packet):
        code = packet[i]
        if code == 0 or i + code > len(packet):
            return None
        out += packet[i + 1:i + code]
        i += code
        if code != 0xFF and i < len(packet):
            out.append(0)
    if len(out) < 3 or crc16(out[:-2]) != struct.unpack(">H", out[-2:])[0]:
        return None
    return bytes(out[:-2])


def frame_text(can_id, extended, data):
    """A CAN frame in the text console's monitor format."""
    return ("%08X" if extended else "%03X") % can_id + " [%d]" % len(data) + "".join(" %02X" % b for b in data)


class BinaryLink(Link):
    MARKER = b"binary mode\n"

    def __init__(self, read_fd, write_fd, proc=None):
        super().__init__(read_fd, write_fd, proc)
        self._seq = 0
        self._frames = []   # (time_ms, id, extended, data) received while monitoring
        self.bad_packets = 0
        self.info = None

    def _enter(self):
        # If a previous program left the console in binary mode the exit packet
        # ends that; in text mode it is noise that the newline flushes away.
        self._write_bytes(b"\x00" + encode([1, CMD_EXIT]) + b"\nmode b\n")
        deadline = time.monotonic() + 3.0
        while self.MARKER not in self._buf:
            self._fill(deadline)
        self._buf = self._buf.split(self.MARKER, 1)[1]
        reply = self._call(CMD_HELLO)
        version, max_payload = struct.unpack_from("<BH", reply)
        self.info = {"version": version, "max_payload": max_payload, "name": reply[3:].decode("ascii", "replace")}
        if version != 1:
            raise LinkError("interface speaks protocol version %d; these tools speak 1" % version)

    def _leave(self):
        self._write_bytes(encode([1, CMD_EXIT]))

    # ---- packets
    def _read_packet(self, deadline):
        while True:
            if b"\x00" in self._buf:
                raw, self._buf = self._buf.split(b"\x00", 1)
                if not raw:
                    continue
                payload = decode(raw)
                if payload is None or len(payload) < 2:
                    self.bad_packets += 1
                    continue
                return payload
            self._fill(deadline)

    def _on_event(self, payload):
        if payload[1] == EVT_CAN_FRAME and len(payload) >= 11:
            stamp, can_id, dlc = struct.unpack_from("<IIB", payload, 2)
            self._frames.append((stamp, can_id & 0x1FFFFFFF, bool(can_id >> 31), payload[11:11 + dlc]))

    def _exchange(self, cmd, args=b"", timeout=15.0):
        self._seq = self._seq % 255 + 1
        self._write_bytes(encode(bytes([self._seq, cmd]) + bytes(args)))
        deadline = time.monotonic() + timeout
        while True:
            p = self._read_packet(deadline)
            if p[0] == 0:
                self._on_event(p)
            elif p[0] == self._seq and p[1] == cmd | 0x80 and len(p) >= 3:
                return p[2], p[3:]
            # anything else is a stale reply to an earlier request: ignore it

    def _call(self, cmd, args=b"", timeout=15.0):
        status, data = self._exchange(cmd, args, timeout)
        if status != OK:
            raise LinkError("interface refused command 0x%02X: %s" % (cmd, STATUS_NAMES[status] if status < len(STATUS_NAMES) else status))
        return data

    # ---- the interface, same as Link
    def command(self, text, timeout=15.0):
        raise LinkError("text commands are not available on a binary link")

    def set_silent(self, on):
        self._call(CMD_SET_SILENT, [int(bool(on))])

    def set_monitor(self, on):
        self._call(CMD_SET_MONITOR, [int(bool(on))])

    def set_keepalive(self, on):
        self._call(CMD_SET_KEEPALIVE, [int(bool(on))])

    def status(self, timeout=15.0):
        data = self._call(CMD_STATUS, timeout=timeout)
        flags, rx, dropped, tx, errors, bad = struct.unpack_from("<B5I", data)
        events_dropped = struct.unpack_from("<I", data, 21)[0] if len(data) >= 25 else 0
        return {"silent": bool(flags & 1), "monitor": bool(flags & 2), "keepalive": bool(flags & 4),
                "logging": bool(flags & 8), "rx": rx, "dropped": dropped, "tx": tx, "errors": errors, "bad_packets": bad,
                "events_dropped": events_dropped}

    def uds_request(self, request_id, response_id, payload, timeout_ms=None):
        args = struct.pack("<IIH", request_id, response_id, timeout_ms or 0) + bytes(payload)
        status, data = self._exchange(CMD_UDS, args, timeout=(timeout_ms or 1000) / 1000.0 + 70.0)
        if status == OK:
            return data
        if status == NEGATIVE:
            raise NegativeResponse(data[0])
        if status == TIMEOUT:
            raise NoResponse("no response from %X" % response_id)
        if status == SILENT:
            raise InterfaceRefused("silent (the interface is listen-only)")
        if status == TRANSPORT:
            raise InterfaceRefused("transport %d" % struct.unpack("b", data[:1])[0])
        raise InterfaceRefused(STATUS_NAMES[status] if status < len(STATUS_NAMES) else "status %d" % status)

    # ---- logging to the card
    @staticmethod
    def _log_reply(data):
        active, frames, size, lost = struct.unpack_from("<B3I", data)
        return {"active": bool(active), "name": data[13:].decode("ascii", "replace"),
                "frames": frames, "bytes": size, "lost": lost}

    def set_time(self, unix_seconds):
        self._call(CMD_SET_TIME, struct.pack("<I", int(unix_seconds)))

    def log_start(self):
        return self._log_reply(self._call(CMD_SET_LOG, [1]))

    def log_stop(self):
        return self._log_reply(self._call(CMD_SET_LOG, [0]))

    def log_status(self):
        return self._log_reply(self._call(CMD_LOG_STATUS))

    def log_mark(self, text):
        self._call(CMD_LOG_MARK, text.encode("utf-8")[:200])

    def poll_add(self, request_id, response_id, period_ms, payload):
        status, _ = self._exchange(CMD_POLL_ADD, struct.pack("<III", request_id, response_id, period_ms) + bytes(payload))
        if status != OK:
            raise LinkError("the interface refused the poll request (too many, too long, or too fast)")

    def poll_clear(self):
        self._call(CMD_POLL_CLEAR)

    READ_CHUNK = 4000

    def power(self, op=None, value=0):
        """Power state of a board that sleeps (the dongle); op 1 stay, 2 idle s, 3 cutoff mV, 4 sleep now."""
        args = b"" if op is None else struct.pack("<BI", op, value)
        status, d = self._exchange(CMD_POWER, args)
        if status == UNKNOWN_CMD:
            raise LinkError("this board has no power management")
        if status != OK:
            raise LinkError("the interface refused that power setting")
        holds, wake, sleeping, stay, vbat, sleep_in, low_for, idle = struct.unpack_from("<BBBBHHHI", d)
        cutoff, running = struct.unpack_from("<HH", d, 14)
        return {"holds": [n for i, n in enumerate(POWER_HOLDS) if holds & (1 << i)],
                "wake": POWER_WAKES[wake] if wake < len(POWER_WAKES) else str(wake),
                "battery_mv": vbat, "sleeps_in_s": sleep_in, "battery_low_for_s": low_for,
                "idle_s": idle, "cutoff_mv": cutoff, "running_mv": running, "stay_awake": bool(stay)}

    def gnss(self, sync_hz=None):
        """GPS and sync-output state of the dongle; sync_hz sets the sync output (0 off, 1-100)."""
        args = b"" if sync_hz is None else struct.pack("<H", sync_hz)
        status, d = self._exchange(CMD_GNSS, args)
        if status == UNKNOWN_CMD:
            raise LinkError("this board has no GPS connector")
        if status != OK:
            raise LinkError("the interface refused that sync rate (1-100 Hz, or 0 for off)")
        flags, quality, sats = d[0], d[1], d[2]
        utc, utc_ms, lat, lon, alt, speed, course, hdop, drift, hz = struct.unpack_from("<IHiiiHHHiH", d, 4)
        pulses, paired, edges, sentences, bad = struct.unpack_from("<IIIII", d, 34)
        return gnss_dict(flags & 1, flags & 2, flags & 4, flags & 16, quality, sats, utc + utc_ms / 1000.0,
                         lat / 1e7, lon / 1e7, alt / 100.0, speed / 100.0, course / 100.0, hdop / 100.0,
                         drift if flags & 8 else None, hz, pulses, paired, edges, sentences, bad)

    def selftest(self, active=False, force=False):
        """The board's bring-up self-test: [{"result", "id", "detail"}], board first (see selftest.h)."""
        status, d = self._exchange(CMD_SELFTEST, bytes([(1 if active else 0) | (2 if force else 0)]), timeout=20.0)
        if status == UNKNOWN_CMD:
            raise LinkError("this firmware has no self-test; update it")
        if status != OK:
            raise LinkError("self-test refused (%s)" % STATUS_NAMES[status])
        out, pos = [], 1
        for _ in range(d[0]):
            result, n = d[pos], d[pos + 1]
            ident = d[pos + 2:pos + 2 + n].decode("ascii", "replace")
            pos += 2 + n
            n = d[pos]
            detail = d[pos + 1:pos + 1 + n].decode("ascii", "replace")
            pos += 1 + n
            out.append({"result": SELFTEST_RESULTS[result] if result < len(SELFTEST_RESULTS) else "?",
                        "id": ident, "detail": detail})
        return out

    def log_files(self):
        out, first = [], 0
        while True:
            data = self._call(CMD_LOG_LIST, struct.pack("<H", first))
            total, count = struct.unpack_from("<HB", data)
            pos = 3
            for _ in range(count):
                size, n = struct.unpack_from("<IB", data, pos)
                out.append((data[pos + 5:pos + 5 + n].decode("ascii", "replace"), size))
                pos += 5 + n
            first += count
            if count == 0 or first >= total:
                return out

    def log_read(self, name, offset, length):
        data = self._call(CMD_LOG_READ, struct.pack("<IH", offset, min(length, self.READ_CHUNK)) + name.encode("ascii"))
        return struct.unpack_from("<I", data)[0], bytes(data[4:])

    def read_frames_timed(self, seconds):
        """Monitored frames with the interface's own millisecond timestamps."""
        deadline = time.monotonic() + seconds
        try:
            while True:
                p = self._read_packet(deadline)
                if p[0] == 0:
                    self._on_event(p)
        except LinkError as e:
            if "closed" in str(e):
                raise
        frames, self._frames = self._frames, []
        if not frames:
            return []
        t0 = frames[0][0]
        return [((stamp - t0) / 1000.0, can_id, ext, bytes(data)) for stamp, can_id, ext, data in frames]

    def take_frames(self):
        frames, self._frames = self._frames, []
        return [(stamp / 1000.0, can_id, ext, bytes(data)) for stamp, can_id, ext, data in frames]

    def read_frames(self, seconds):
        deadline = time.monotonic() + seconds
        try:
            while True:
                p = self._read_packet(deadline)
                if p[0] == 0:
                    self._on_event(p)
        except LinkError as e:
            if "closed" in str(e):
                raise
        frames, self._frames = self._frames, []
        return [frame_text(can_id, ext, data) for _, can_id, ext, data in frames]


class BleSimLink(BinaryLink):
    """The simulator's stand-in for Bluetooth LE: a TCP connection carrying the
    packets exactly as the Nordic UART Service would, with an imitated MTU and
    throughput. There is no text mode on this link; packets flow from the start.
    """

    def __init__(self, sock, proc=None):
        super().__init__(sock.fileno(), sock.fileno(), proc)
        self._sock = sock

    @classmethod
    def simulator(cls, binary, read_chunk=4096, mtu=185, rate=20000, port=None):
        if port is None:
            probe = socket.socket()
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
            probe.close()
        env = dict(os.environ, VWDIAG_BLE_PORT=str(port), VWDIAG_BLE_MTU=str(mtu), VWDIAG_BLE_RATE=str(rate))
        proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0, env=env)
        link = cls.connect("127.0.0.1", port, proc=proc)
        link.read_chunk = read_chunk
        return link

    @classmethod
    def connect(cls, host, port, proc=None, timeout=3.0):
        """Connect to a simulator that is already running with VWDIAG_BLE_PORT set."""
        deadline = time.monotonic() + timeout
        while True:
            try:
                sock = socket.create_connection((host, port), timeout=1.0)
                break
            except OSError:
                if time.monotonic() > deadline:
                    if proc:
                        proc.kill()
                    raise LinkError("no simulator listening on %s:%d" % (host, port))
                time.sleep(0.05)
        sock.setblocking(False)
        link = cls(sock, proc)
        link._enter()
        return link

    def _enter(self):
        reply = self._call(CMD_HELLO)
        version, max_payload = struct.unpack_from("<BH", reply)
        self.info = {"version": version, "max_payload": max_payload, "name": reply[3:].decode("ascii", "replace")}

    def _leave(self):
        pass

    def console(self, text):
        """Type a line into the simulator's USB console (which stays in text mode)."""
        self._proc.stdin.write(text.encode() + b"\n")

    def close(self):
        if self._sock:
            self._sock.close()
            self._sock = None
        if self._proc:
            self._proc.kill()
            self._proc.wait()
            for pipe in (self._proc.stdin, self._proc.stdout):
                try:
                    pipe.close()
                except OSError:
                    pass   # already gone
            self._proc = None
        self._rfd = self._wfd = None
