"""Fault code formatting.

A UDS fault code is three bytes. The first two are the SAE J2012 code (the
familiar P0299 style); the third is the failure type. VW tools also show their
own five- or six-digit numbers; that mapping is proprietary and not included.
"""

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
    return "%s-%02X  (raw %s, status %02X: %s)" % (
        sae_code(dtc), dtc[2], dtc.hex().upper(), status, ", ".join(flags) if flags else "no flags")
