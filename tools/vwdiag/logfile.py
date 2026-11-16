"""Reader for the interface's log files (LOG00001.VWL and so on).

Format, from hardware/pico2w/docs/logging.md: a 32-byte header, then records of
type (1 byte), time in ms since power-up (4 bytes), and a payload that depends
on the type. Numbers are little-endian. A file cut short by pulling the power is
read up to the last complete record.

Version 2 added the dongle's GPS and sync records: GPS pulses paired with the UTC second
they mark, fixes, and the edges sent on the sync output, each pulse and edge stamped in
microseconds by the clock the millisecond stamps come from. With paired pulses in the file,
any stamp converts to UTC to within a few microseconds (utc_of_us).
"""
import bisect
import struct
import time

from . import obd, simos18

REC_CAN, REC_LOST, REC_MARK, REC_TIME, REC_PPS, REC_FIX, REC_SYNC = 1, 2, 3, 4, 5, 6, 7
FIX_LEN = 29


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


class Fix:
    """A GPS fix: position in degrees, altitude in m, speed in m/s, course in degrees."""
    __slots__ = ("ms", "utc", "valid", "quality", "sats", "lat", "lon", "alt_m", "speed_ms", "course_deg", "hdop")

    def __init__(self, ms, raw):
        utc, utc_ms, flags, self.quality, self.sats, lat, lon, alt, speed, course, hdop = \
            struct.unpack_from("<IHBBBiiiHHH", raw)
        self.ms, self.utc, self.valid = ms, utc + utc_ms / 1000.0 if utc else None, bool(flags & 1)
        self.lat, self.lon, self.alt_m = lat / 1e7, lon / 1e7, alt / 100.0
        self.speed_ms, self.course_deg, self.hdop = speed / 100.0, course / 100.0, hdop / 100.0


class Log:
    def __init__(self, path):
        with open(path, "rb") as f:
            raw = f.read()
        if len(raw) < 32 or raw[:4] != b"VWLG":
            raise LogError("%s is not an interface log file" % path)
        self.version, header_len = raw[4], raw[5]
        if self.version not in (1, 2) or header_len < 32:
            raise LogError("log format version %d is not supported" % self.version)
        self.bitrate, self.start_ms, start_epoch = struct.unpack_from("<III", raw, 8)
        self.frames, self.marks, self.lost_events = [], [], []
        self.pulses, self.fixes, self.syncs = [], [], []   # (us, utc or 0), Fix, (us, count)
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
            elif kind in (REC_PPS, REC_SYNC):
                if body + 12 > len(raw):
                    self.truncated = True
                    break
                (self.pulses if kind == REC_PPS else self.syncs).append(struct.unpack_from("<QI", raw, body))
                pos = body + 12
            elif kind == REC_FIX:
                if body + FIX_LEN > len(raw):
                    self.truncated = True
                    break
                self.fixes.append(Fix(ms, raw[body:body + FIX_LEN]))
                pos = body + FIX_LEN
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

    # ---- GPS time
    def _pairs(self):
        if not hasattr(self, "_pair_cache"):
            pairs = sorted((us, utc) for us, utc in self.pulses if utc)
            self._pair_cache = ([p[0] for p in pairs], pairs)
        return self._pair_cache

    def utc_of_us(self, us):
        """UTC (Unix seconds, float) of a microsecond stamp, from the paired GPS pulses around it: exact at
        each pulse, straight lines between them, the nearest second's rate beyond the ends. None without GPS."""
        keys, pairs = self._pairs()
        if not pairs:
            return None
        if len(pairs) == 1:
            return pairs[0][1] + (us - pairs[0][0]) / 1e6
        i = min(max(bisect.bisect_right(keys, us), 1), len(pairs) - 1)
        (u0, t0), (u1, t1) = pairs[i - 1], pairs[i]
        return t0 + (us - u0) * (t1 - t0) / (u1 - u0)

    def clock_drift_ppm(self):
        """How fast the interface's clock ran against GPS over the file (positive: fast). None without GPS."""
        _, pairs = self._pairs()
        if len(pairs) < 2 or pairs[-1][1] == pairs[0][1]:
            return None
        local, true = pairs[-1][0] - pairs[0][0], (pairs[-1][1] - pairs[0][1]) * 1e6
        return (local - true) / true * 1e6

    def gpx(self):
        """The valid fixes as a GPX 1.1 track."""
        out = ['<?xml version="1.0" encoding="UTF-8"?>',
               '<gpx version="1.1" creator="vwdiag" xmlns="http://www.topografix.com/GPX/1/1">', "<trk><trkseg>"]
        for f in self.fixes:
            if not f.valid:
                continue
            stamp = ""
            if f.utc:
                stamp = "<time>%s.%03dZ</time>" % (time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(int(f.utc))),
                                                   round((f.utc % 1) * 1000) % 1000)
            out.append('<trkpt lat="%.7f" lon="%.7f"><ele>%.2f</ele>%s<sat>%d</sat></trkpt>' % (
                f.lat, f.lon, f.alt_m, stamp, f.sats))
        out += ["</trkseg></trk>", "</gpx>"]
        return "\n".join(out) + "\n"

    def us_of_ms(self, ms):
        """The microsecond clock at a millisecond stamp (its middle). The millisecond stamps are that clock
        divided by 1000 and cut to 32 bits, so they wrap every 49.7 days of uptime; the pulses are not cut."""
        keys, _ = self._pairs()
        ref = keys[0] // 1000 if keys else ms
        delta = (ms - ref) & 0xFFFFFFFF
        if delta >= 1 << 31:
            delta -= 1 << 32
        return (ref + delta) * 1000 + 500

    def epoch(self, ms):
        """Wall-clock time (Unix seconds) of a record, or None if the interface was never told the time.
        GPS pulses, when the file has them, are the reference; otherwise the latest time setting."""
        t = self.utc_of_us(self.us_of_ms(ms))
        if t is not None:
            return t
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

    def measure_samples(self):
        """Simos 18 measuring values found in the engine module's answers (service 0x22, one value per request,
        as the poll list asks for them), as (seconds, name, value, unit)."""
        by_did = {did: (name, length, signed, unit, decode)
                  for name, (did, length, signed, unit, decode, _) in simos18.VALUES.items()}
        for f in self.frames:
            d = f.data
            # A single-frame positive answer from the engine module: length, 0x62, identifier (2), value.
            if f.sent or f.extended or f.can_id != 0x7E8 or len(d) < 5 or d[0] > 7 or d[1] != 0x62:
                continue
            entry = by_did.get(d[2] << 8 | d[3])
            if not entry:
                continue
            name, length, signed, unit, decode = entry
            if d[0] == 3 + length and len(d) >= 4 + length:
                yield self.seconds(f.ms), name, decode(int.from_bytes(d[4:4 + length], "big", signed=signed)), unit

    def samples(self):
        """Every live value in the log, OBD and measuring values together, in time order."""
        return sorted(list(self.obd_samples()) + list(self.measure_samples()), key=lambda s: s[0])

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
