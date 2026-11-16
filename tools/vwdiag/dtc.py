"""Fault code formatting and descriptions.

A UDS fault code is three bytes. The first two are the SAE J2012 code (the
familiar P0299 style); the third is the failure type. VW tools also show their
own five- or six-digit numbers; that mapping is proprietary and not included.

Descriptions, in order of preference:
  data/dtc_generic.csv  about 470 generic powertrain codes from the dtcdb project
                        (github.com/todrobbins/dtcdb, MIT licence, data/LICENSE.dtcdb)
  data/dtc_extra.csv    codes that list lacks and that matter on this car (turbo boost, cam
                        timing, fuel pressure, idle trims, lost-communication U-codes), written
                        for this project from the SAE J2012 code titles
  the code's group      what SAE J2012 says that block of codes is for; for manufacturer codes
                        (VW's P1xxx and most B, C and U1/U2 codes) the meaning is in VW's own data
"""
import csv
import os

STATUS_BITS = [
    (0x01, "test failed"),
    (0x02, "failed this operation cycle"),
    (0x04, "pending"),
    (0x08, "confirmed"),
    (0x10, "not tested since clear"),
    (0x20, "failed since clear"),
    (0x40, "not tested this cycle"),
    (0x80, "warning lamp requested"),
]


def sae_code(dtc):
    """First two bytes -> 'P0299' style code."""
    high, low = dtc[0], dtc[1]
    return "%s%d%X%02X" % ("PCBU"[high >> 6], (high >> 4) & 0x3, high & 0xF, low)


def describe_status(status):
    return [name for bit, name in STATUS_BITS if status & bit]


def format_dtc(dtc, status):
    flags = describe_status(status)
    return "%s-%02X  %s  (raw %s, status %02X: %s)" % (
        sae_code(dtc), dtc[2], describe(dtc), dtc.hex().upper(), status, ", ".join(flags) if flags else "no flags")


_DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")


def _load(name):
    with open(os.path.join(_DATA, name), encoding="utf-8") as f:
        return {row["code"]: row["description"] for row in csv.DictReader(f)}


GENERIC = _load("dtc_generic.csv")
EXTRA = _load("dtc_extra.csv")

# SAE J2012 blocks: (first characters, meaning). The longest matching prefix wins.
GROUPS = [
    ("P0", "generic powertrain"),
    ("P00", "fuel and air metering, auxiliary emission controls"),
    ("P01", "fuel and air metering"),
    ("P02", "fuel and air metering (injector circuits)"),
    ("P03", "ignition system or misfire"),
    ("P04", "auxiliary emission controls"),
    ("P05", "vehicle speed, idle control and auxiliary inputs"),
    ("P06", "computer and auxiliary outputs"),
    ("P07", "transmission"), ("P08", "transmission"), ("P09", "transmission"),
    ("P0A", "hybrid propulsion"),
    ("P1", "manufacturer-specific powertrain"),
    ("P2", "generic powertrain (fuel, air, emissions)"),
    ("P3", "manufacturer-specific or generic powertrain"),
    ("P34", "cylinder deactivation"),
    ("C0", "generic chassis"),
    ("C1", "manufacturer-specific chassis"), ("C2", "manufacturer-specific chassis"),
    ("B0", "generic body"),
    ("B1", "manufacturer-specific body"), ("B2", "manufacturer-specific body"),
    ("U0", "generic network communication"),
    ("U1", "manufacturer-specific network communication"), ("U2", "manufacturer-specific network communication"),
]


def group(code):
    best = ""
    for prefix, meaning in GROUPS:
        if code.startswith(prefix) and len(prefix) > len(best):
            best, found = prefix, meaning
    return found if best else "reserved"


def description(code):
    """(text, source) for a 'P0303' style code; source is 'list', 'added' or 'group'."""
    code = code.upper()
    if code in GENERIC:
        return GENERIC[code], "list"
    if code in EXTRA:
        return EXTRA[code], "added"
    g = group(code)
    if "manufacturer" in g:
        return "%s%s code: VW's own description is not public" % (g[0].upper(), g[1:]), "group"
    return "%s%s code, not in this list" % (g[0].upper(), g[1:]), "group"


def describe(dtc):
    """Plain-language description of a 3-byte UDS code, or a 2-byte OBD one."""
    return description(sae_code(dtc))[0]
