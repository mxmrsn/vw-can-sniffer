"""VW-specific knowledge: control module addresses and identification data.

Sources and confidence are written up in docs/vw_can_data_sources.md. In short:
engine and transmission use the legislated OBD addresses; airbag, steering, radar
and camera, and the part-number / software-version / coding identifiers, match
what comma.ai's opendbc uses on Golf MK7 cars; the rest is unconfirmed. Nothing
has been checked on this car yet. hardware/pico2w/firmware/src/vw_ecus.h holds the same
address table; a test keeps the two in step.
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
    "radar":   (0x757, 0x7C1, "Front radar (13)"),
    "camera":  (0x74F, 0x7B9, "Front camera (A5)"),
}

RX_OFFSET = 0x6A    # response ID = request ID + 0x6A for everything except engine and transmission

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
    (0xF182, "Coding / dataset"),
    (0xF1AD, "Engine code"),        # engine module only; unconfirmed on this car (expect DKFA)
]

# The three identifiers opendbc reads from every module in a single request.
FINGERPRINT_DIDS = [0xF187, 0xF189, 0xF182]


def text(data):
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data).strip()


def fingerprint(client):
    """Part number, software version and coding in one request, as opendbc does it.

    Returns {did: bytes}. Modules leave out identifiers they do not have.
    """
    return client.read_dids(FINGERPRINT_DIDS)


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
