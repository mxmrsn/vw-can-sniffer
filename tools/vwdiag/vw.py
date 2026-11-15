"""VW-specific knowledge: control module addresses and identification data.

Engine and transmission use the legislated OBD addresses. The other addresses,
and the identification DIDs, are the commonly published VW/MQB values and have
not been confirmed on this car. firmware/pico2w/src/vw_ecus.h holds the same
table; a test keeps the two in step.
"""
from .uds import NegativeResponse, NoResponse

# name: (request id, response id, description)
ECUS = {
    "engine":  (0x7E0, 0x7E8, "Engine control (01)"),
    "trans":   (0x7E1, 0x7E9, "Transmission (02)"),
    "abs":     (0x713, 0x77D, "ABS / ESC (03)"),
    "cluster": (0x714, 0x77E, "Instrument cluster (17)"),
    "airbag":  (0x715, 0x77F, "Airbag (15)"),
    "gateway": (0x710, 0x77A, "Diagnostic gateway (19)"),
    "steer":   (0x712, 0x77C, "Power steering (44)"),
}

# Identification data read with service 0x22.
ID_DIDS = [
    (0xF190, "VIN"),
    (0xF187, "Part number"),
    (0xF189, "Software version"),
    (0xF191, "Hardware number"),
    (0xF1A3, "Hardware version"),
    (0xF197, "System name"),
    (0xF18C, "Serial number"),
    (0xF19E, "ODX file"),
    (0xF1A2, "ODX version"),
]


def text(data):
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data).strip()


def identify(client):
    """Read whatever identification data a module offers. Returns [(label, text)]."""
    out = []
    for did, label in ID_DIDS:
        try:
            out.append((label, text(client.read_did(did))))
        except NegativeResponse:
            continue        # this module does not have that item
        except NoResponse:
            break           # module stopped answering; no point asking for the rest
    return out
