"""Binary packet connection to the interface (the protocol an app will use).

Same methods as link.Link, but the console is switched to the packet protocol
of hardware/pico2w/docs/link_protocol.md: COBS framing, CRC-16, numbered
requests. It tolerates the byte stream arriving in pieces of any size, which is
what a BLE connection does to it.
"""
import struct
import time

from .link import Link, LinkError
from .uds import InterfaceRefused, NegativeResponse, NoResponse

CMD_HELLO, CMD_STATUS, CMD_SET_SILENT, CMD_SET_MONITOR, CMD_UDS, CMD_SET_KEEPALIVE, CMD_EXIT = 1, 2, 3, 4, 5, 6, 0x7F
CMD_SET_LOG, CMD_SET_TIME, CMD_LOG_STATUS, CMD_POLL_ADD, CMD_POLL_CLEAR, CMD_LOG_MARK = 8, 9, 10, 11, 12, 13
EVT_CAN_FRAME = 1
OK, BAD_ARGS, UNKNOWN_CMD, SILENT, TIMEOUT, NEGATIVE, TRANSPORT, STORAGE = range(8)
STATUS_NAMES = ["ok", "bad arguments", "unknown command", "silent", "timeout", "negative response", "transport error",
                "no card, or the log file could not be written"]
MAX_PAYLOAD = 4160


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

    def _exchange(self, cmd, args=b"", timeout=3.0):
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

    def _call(self, cmd, args=b"", timeout=3.0):
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

    def status(self):
        flags, rx, dropped, tx, errors, bad = struct.unpack("<B5I", self._call(CMD_STATUS)[:21])
        return {"silent": bool(flags & 1), "monitor": bool(flags & 2), "keepalive": bool(flags & 4),
                "logging": bool(flags & 8), "rx": rx, "dropped": dropped, "tx": tx, "errors": errors, "bad_packets": bad}

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
