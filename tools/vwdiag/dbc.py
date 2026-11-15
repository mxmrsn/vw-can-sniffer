"""Decode broadcast CAN frames using a DBC file.

data/vw_mqb.dbc is the Volkswagen MQB message database from comma.ai's opendbc
project (MIT licence, see data/LICENSE.opendbc). It describes the car's internal
powertrain and comfort buses. Whether any of these frames are visible at the OBD
port, which sits behind the gateway, is something to find out on the car.
"""
import os
import re

DEFAULT_DBC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "vw_mqb.dbc")
HOUSEKEEPING = re.compile(r"(CHECKSUM|COUNTER|_CRC|_BZ)$")

_BO = re.compile(r"^BO_ (\d+) (\w+) *: *(\d+) ")
_SG = re.compile(r'^ SG_ (\w+) ?(M|m\d+)? *: *(\d+)\|(\d+)@([01])([+-]) *\(([^,]+),([^)]+)\) *\[[^\]]*\] *"([^"]*)"')


class Signal:
    def __init__(self, name, mux, start, length, little_endian, signed, scale, offset, unit):
        self.name, self.mux, self.start, self.length = name, mux, start, length
        self.little_endian, self.signed, self.scale, self.offset = little_endian, signed, scale, offset
        self.unit = unit[5:] if unit.startswith("Unit_") else unit

    def raw(self, data):
        if self.little_endian:
            value = (int.from_bytes(data, "little") >> self.start) & ((1 << self.length) - 1)
        else:   # Motorola: start is the most significant bit, walking down then to the next byte
            value, byte, bit = 0, self.start // 8, self.start % 8
            for _ in range(self.length):
                value = (value << 1) | ((data[byte] >> bit) & 1 if byte < len(data) else 0)
                bit -= 1
                if bit < 0:
                    byte, bit = byte + 1, 7
        if self.signed and value & (1 << (self.length - 1)):
            value -= 1 << self.length
        return value

    def decode(self, data):
        value = self.raw(data) * self.scale + self.offset
        return round(value, 6)


class Message:
    def __init__(self, can_id, name, size):
        self.id = can_id & 0x1FFFFFFF
        self.extended = bool(can_id & 0x80000000)
        self.name, self.size, self.signals = name, size, []

    def decode(self, data, housekeeping=False):
        """Return {signal name: (value, unit)} for the signals present in this frame."""
        data = bytes(data) + bytes(max(0, self.size - len(data)))
        mux = next((s.raw(data) for s in self.signals if s.mux == "M"), None)
        out = {}
        for s in self.signals:
            if s.mux and s.mux != "M" and int(s.mux[1:]) != mux:
                continue
            if not housekeeping and HOUSEKEEPING.search(s.name):
                continue
            out[s.name] = (s.decode(data), s.unit)
        return out


class Database:
    def __init__(self, path=DEFAULT_DBC):
        self.messages = {}
        current = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = _BO.match(line)
                if m:
                    current = Message(int(m.group(1)), m.group(2), int(m.group(3)))
                    self.messages[(current.id, current.extended)] = current
                    continue
                m = _SG.match(line)
                if m and current:
                    current.signals.append(Signal(m.group(1), m.group(2), int(m.group(3)), int(m.group(4)),
                                                  m.group(5) == "1", m.group(6) == "-", float(m.group(7)),
                                                  float(m.group(8)), m.group(9)))

    def lookup(self, can_id, extended=False):
        return self.messages.get((can_id, extended))

    def by_name(self, name):
        return next((m for m in self.messages.values() if m.name == name), None)


_LOG_LINE = re.compile(r"^([0-9A-Fa-f]{3}|[0-9A-Fa-f]{8}) \[(\d)\]((?: [0-9A-Fa-f]{2})*)\s*$")


def parse_log_line(line):
    """Parse a frame as printed by the firmware's monitor: '3C0 [4] 00 47 03 00'."""
    m = _LOG_LINE.match(line.strip())
    if not m:
        return None
    return int(m.group(1), 16), len(m.group(1)) == 8, bytes(int(b, 16) for b in m.group(3).split())


def describe(db, can_id, extended, data, only_nonzero=False):
    """One readable line for a frame, or None if the database does not know the ID."""
    msg = db.lookup(can_id, extended)
    if not msg:
        return None
    parts = []
    for name, (value, unit) in msg.decode(data).items():
        if only_nonzero and value == 0:
            continue
        text = ("%g" % value) + ((" " + unit) if unit else "")
        parts.append("%s=%s" % (name, text))
    return "%s: %s" % (msg.name, ", ".join(parts))
