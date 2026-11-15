"""Reader for the interface's log files (LOG00001.VWL and so on).

Format, from hardware/pico2w/docs/logging.md: a 32-byte header, then records of
type (1 byte), time in ms since power-up (4 bytes), and a payload that depends
on the type. Numbers are little-endian. A file cut short by pulling the power is
read up to the last complete record.
"""
import struct
import time

from . import obd

REC_CAN, REC_LOST, REC_MARK, REC_TIME = 1, 2, 3, 4


class LogError(Exception):
    pass


class Frame:
    __slots__ = ("ms", "can_id", "extended", "sent", "data")

    def __init__(self, ms, can_id, extended, sent, data):
        self.ms, self.can_id, self.extended, self.sent, self.data = ms, can_id, extended, sent, data

    def text(self):
        """The text console's monitor format, so the same decoders work on it."""
        return ("%08X" if self.extended else "%03X") % self.can_id + " [%d]" % len(self.data) + \
            "".join(" %02X" % b for b in self.data)


class Log:
    def __init__(self, path):
        with open(path, "rb") as f:
            raw = f.read()
        if len(raw) < 32 or raw[:4] != b"VWLG":
            raise LogError("%s is not an interface log file" % path)
        self.version, header_len = raw[4], raw[5]
        if self.version != 1 or header_len < 32:
            raise LogError("log format version %d is not supported" % self.version)
        self.bitrate, self.start_ms, start_epoch = struct.unpack_from("<III", raw, 8)
        self.frames, self.marks, self.lost_events = [], [], []
        self.truncated = False
        # Wall-clock time is known only if something told the interface: (ms, epoch) pairs.
        self._clock = [(self.start_ms, start_epoch)] if start_epoch else []
        pos = header_len
        while pos < len(raw):
            if pos + 5 > len(raw):
                self.truncated = True
                break
            kind, ms = struct.unpack_from("<BI", raw, pos)
            body = pos + 5
            if kind == REC_CAN:
                if body + 5 > len(raw) or body + 5 + (raw[body + 4] & 0x0F) > len(raw):
                    self.truncated = True
                    break
                can_id, flags = struct.unpack_from("<IB", raw, body)
                n = flags & 0x0F
                self.frames.append(Frame(ms, can_id, bool(flags & 0x80), bool(flags & 0x40), raw[body + 5:body + 5 + n]))
                pos = body + 5 + n
            elif kind in (REC_LOST, REC_TIME):
                if body + 4 > len(raw):
                    self.truncated = True
                    break
                value = struct.unpack_from("<I", raw, body)[0]
                if kind == REC_LOST:
                    self.lost_events.append((ms, value))
                else:
                    self._clock.append((ms, value))
                pos = body + 4
            elif kind == REC_MARK:
                if body + 1 > len(raw) or body + 1 + raw[body] > len(raw):
                    self.truncated = True
                    break
                self.marks.append((ms, raw[body + 1:body + 1 + raw[body]].decode("utf-8", "replace")))
                pos = body + 1 + raw[body]
            else:
                raise LogError("unknown record type 0x%02X at byte %d: the file is damaged" % (kind, pos))

    @property
    def lost(self):
        return sum(n for _, n in self.lost_events)

    @property
    def duration(self):
        return (self.frames[-1].ms - self.start_ms) / 1000.0 if self.frames else 0.0

    def seconds(self, ms):
        """Time since the log started."""
        return ((ms - self.start_ms) & 0xFFFFFFFF) / 1000.0

    def epoch(self, ms):
        """Wall-clock time (Unix seconds) of a record, or None if the interface was never told the time."""
        if not self._clock:
            return None
        base_ms, base_epoch = self._clock[-1]   # the latest setting is the best estimate for the whole file
        return base_epoch + (ms - base_ms) / 1000.0

    def start_time_text(self):
        t = self.epoch(self.start_ms)
        return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) if t else "unknown (the interface was not told the time)"

    def candump_lines(self, interface="can0"):
        """Lines in the can-utils log format, which SavvyCAN, python-can and others read."""
        for f in self.frames:
            t = self.epoch(f.ms)
            stamp = t if t is not None else self.seconds(f.ms)
            ident = ("%08X" if f.extended else "%03X") % f.can_id
            yield "(%.6f) %s %s#%s" % (stamp, interface, ident, f.data.hex().upper())

    def obd_samples(self):
        """Service-01 live values found in the answers, as (seconds, name, value, unit)."""
        for f in self.frames:
            d = f.data
            # A single-frame OBD answer: length, 0x41, PID, data. 7E8-7EF are the legislated answer IDs.
            if f.sent or f.extended or not 0x7E8 <= f.can_id <= 0x7EF or len(d) < 4 or d[0] > 7 or d[1] != 0x41:
                continue
            name = obd.BY_PID.get(d[2])
            if not name:
                continue
            _, unit, size, decode = obd.PIDS[name]
            if d[0] >= 2 + size:
                yield self.seconds(f.ms), name, decode(d[3:3 + size]), unit
