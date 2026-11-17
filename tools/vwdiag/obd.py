"""OBD-II live data (SAE J1979 service 01). These PIDs and formulas are standardised."""
from .uds import UdsError

# name: (pid, unit, number of data bytes, decoder)
PIDS = {
    "load":       (0x04, "%",    1, lambda d: d[0] * 100 / 255),
    "coolant":    (0x05, "C",    1, lambda d: d[0] - 40),
    "stft":       (0x06, "%",    1, lambda d: (d[0] - 128) * 100 / 128),   # short-term fuel trim, bank 1
    "ltft":       (0x07, "%",    1, lambda d: (d[0] - 128) * 100 / 128),   # long-term fuel trim, bank 1
    "map":        (0x0B, "kPa",  1, lambda d: d[0]),
    "rpm":        (0x0C, "rpm",  2, lambda d: (d[0] * 256 + d[1]) / 4),
    "speed":      (0x0D, "km/h", 1, lambda d: d[0]),
    "timing":     (0x0E, "deg",  1, lambda d: d[0] / 2 - 64),
    "intake":     (0x0F, "C",    1, lambda d: d[0] - 40),
    "maf":        (0x10, "g/s",  2, lambda d: (d[0] * 256 + d[1]) / 100),
    "throttle":   (0x11, "%",    1, lambda d: d[0] * 100 / 255),
    "runtime":    (0x1F, "s",    2, lambda d: d[0] * 256 + d[1]),
    "rail":       (0x23, "kPa",  2, lambda d: (d[0] * 256 + d[1]) * 10),   # fuel rail pressure (gauge)
    "fuel":       (0x2F, "%",    1, lambda d: d[0] * 100 / 255),
    "baro":       (0x33, "kPa",  1, lambda d: d[0]),
    "lambda":     (0x34, "ratio", 4, lambda d: (d[0] * 256 + d[1]) / 32768),   # wideband sensor 1, measured
    "voltage":    (0x42, "V",    2, lambda d: (d[0] * 256 + d[1]) / 1000),
    "lambda_cmd": (0x44, "ratio", 2, lambda d: (d[0] * 256 + d[1]) / 32768),   # commanded
    "ambient":    (0x46, "C",    1, lambda d: d[0] - 40),
    "oil":        (0x5C, "C",    1, lambda d: d[0] - 40),
    "ethanol":    (0x52, "%",    1, lambda d: d[0] * 100 / 255),
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


def vehicle_info(client):
    """Service 09: {"vin", "calibration_id", "cvn", "module_name"} for whatever the module offers."""
    from .uds import NegativeResponse
    out = {}
    for infotype, key in ((0x02, "vin"), (0x04, "calibration_id"), (0x06, "cvn"), (0x0A, "module_name")):
        try:
            resp = client.request([0x09, infotype])
        except NegativeResponse:
            continue
        body = resp[3:]
        if key == "cvn":
            out[key] = body[:4].hex().upper()
        else:
            out[key] = body.replace(b"\0", b" ").decode("ascii", "replace").strip()
    return out


# ---- emissions diagnostics: fault codes, freeze frame, on-board monitors (SAE J1979 services 01-07)
def monitor_status(client):
    """Warning lamp and stored-code count (PID 01). Returns (lamp_on, count)."""
    resp = client.request([0x01, 0x01])
    return bool(resp[2] & 0x80), resp[2] & 0x7F


def emission_dtcs(client, pending=False):
    """Stored (service 03) or pending (service 07) emission codes, as 2-byte values."""
    resp = client.request([0x07 if pending else 0x03])
    count = resp[1]
    return [resp[2 + 2 * i:4 + 2 * i] for i in range(count)]


# Freeze-frame values worth showing, in the order a mechanic reads them.
FREEZE = ["rpm", "speed", "load", "coolant", "intake", "stft", "ltft", "map", "maf", "timing", "throttle", "runtime"]


def freeze_frame(client):
    """Freeze frame 0 (service 02): the code that triggered it and the values at that moment.

    Returns (code_bytes or None, {name: value}). Values the module did not store are left out.
    """
    from .uds import NegativeResponse
    resp = client.request([0x02, 0x02, 0x00])
    code = resp[3:5]
    if code == b"\x00\x00":
        return None, {}
    values = {}
    for name in FREEZE:
        pid, _, size, decode = PIDS[name]
        try:
            r = client.request([0x02, pid, 0x00])
        except NegativeResponse:
            continue
        if len(r) >= 3 + size:
            values[name] = decode(r[3:3 + size])
    return code, values


MISFIRE_MIDS = {0: 0xA1, 1: 0xA2, 2: 0xA3, 3: 0xA4, 4: 0xA5}   # 0 = all cylinders


def monitor_tests(client, mid):
    """Service 06 results for one monitor: {test_id: (value, min, max, unit_id)}."""
    resp = client.request([0x06, mid])
    out, body = {}, resp[2:]
    for i in range(0, len(body) - 7, 8):
        tid, uas = body[i], body[i + 1]
        val, lo, hi = (int.from_bytes(body[i + j:i + j + 2], "big") for j in (2, 4, 6))
        out[tid] = (val, lo, hi, uas)
    return out


def misfire_counts(client, cylinders=4):
    """Per-cylinder misfire counts from the misfire monitor (service 06, MIDs A2..).

    Returns {cylinder: (this_cycle, average_of_last_ten_cycles)}; cylinder 0 is the engine total.
    """
    out = {}
    for cyl in range(cylinders + 1):
        tests = monitor_tests(client, MISFIRE_MIDS[cyl])
        out[cyl] = (tests.get(0x0C, (None,))[0], tests.get(0x0B, (None,))[0])
    return out


# Sizes of the OBD identifiers a module may use in snapshot records (F4xx = PID xx).
SNAPSHOT_SIZES = {0xF400 | v[0]: v[2] for v in PIDS.values()}


def decode_snapshot(items):
    """{did: bytes} from a UDS snapshot -> {name: value} for the OBD identifiers among them."""
    out = {}
    for did, data in items.items():
        if did is not None and did >> 8 == 0xF4 and did & 0xFF in BY_PID:
            name = BY_PID[did & 0xFF]
            out[name] = PIDS[name][3](data)
    return out
