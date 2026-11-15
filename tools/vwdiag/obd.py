"""OBD-II live data (SAE J1979 service 01). These PIDs and formulas are standardised."""
from .uds import UdsError

# name: (pid, unit, number of data bytes, decoder)
PIDS = {
    "load":       (0x04, "%",    1, lambda d: d[0] * 100 / 255),
    "coolant":    (0x05, "C",    1, lambda d: d[0] - 40),
    "map":        (0x0B, "kPa",  1, lambda d: d[0]),
    "rpm":        (0x0C, "rpm",  2, lambda d: (d[0] * 256 + d[1]) / 4),
    "speed":      (0x0D, "km/h", 1, lambda d: d[0]),
    "timing":     (0x0E, "deg",  1, lambda d: d[0] / 2 - 64),
    "intake":     (0x0F, "C",    1, lambda d: d[0] - 40),
    "maf":        (0x10, "g/s",  2, lambda d: (d[0] * 256 + d[1]) / 100),
    "throttle":   (0x11, "%",    1, lambda d: d[0] * 100 / 255),
    "runtime":    (0x1F, "s",    2, lambda d: d[0] * 256 + d[1]),
    "fuel":       (0x2F, "%",    1, lambda d: d[0] * 100 / 255),
    "baro":       (0x33, "kPa",  1, lambda d: d[0]),
    "voltage":    (0x42, "V",    2, lambda d: (d[0] * 256 + d[1]) / 1000),
    "ambient":    (0x46, "C",    1, lambda d: d[0] - 40),
    "oil":        (0x5C, "C",    1, lambda d: d[0] - 40),
}
BY_PID = {v[0]: k for k, v in PIDS.items()}


def read_pid(client, name):
    """Read one named value from the engine module. Returns (value, unit)."""
    pid, unit, size, decode = PIDS[name]
    resp = client.request([0x01, pid])
    if len(resp) < 2 + size or resp[1] != pid:
        raise UdsError("malformed OBD response for PID %02X" % pid)
    return decode(resp[2:2 + size]), unit


def supported_pids(client):
    """Ask the engine module which service-01 PIDs it implements."""
    from .uds import NegativeResponse, NoResponse
    found = []
    base = 0x00
    while True:
        try:
            resp = client.request([0x01, base])
        except (NegativeResponse, NoResponse):
            break
        bits = int.from_bytes(resp[2:6], "big")
        found += [base + i + 1 for i in range(32) if bits & (1 << (31 - i))]
        if not bits & 1 or base >= 0xC0:   # last bit set means the next range exists
            break
        base += 0x20
    return [p for p in found if p % 0x20 != 0]


def read_vin(client):
    resp = client.request([0x09, 0x02])
    return resp[3:].decode("ascii", "replace").strip()
