"""UDS (ISO 14229) requests through the interface, over either kind of link."""

NRC_NAMES = {
    0x10: "general reject", 0x11: "service not supported", 0x12: "sub-function not supported",
    0x13: "incorrect message length or format", 0x14: "response too long", 0x21: "busy, repeat request",
    0x22: "conditions not correct", 0x24: "request sequence error", 0x31: "request out of range",
    0x33: "security access denied", 0x35: "invalid key", 0x36: "exceeded number of attempts",
    0x37: "required time delay not expired", 0x72: "general programming failure",
    0x78: "response pending", 0x7E: "sub-function not supported in active session",
    0x7F: "service not supported in active session",
}


class UdsError(Exception):
    pass


class NoResponse(UdsError):
    """The control module did not answer in time."""


class NegativeResponse(UdsError):
    def __init__(self, nrc):
        self.nrc = nrc
        super().__init__("refused by control module: NRC 0x%02X (%s)" % (nrc, NRC_NAMES.get(nrc, "unlisted")))


class InterfaceRefused(UdsError):
    """The interface itself would not send (listen-only, bad arguments, transport error)."""


class UdsClient:
    def __init__(self, link, request_id, response_id, timeout_ms=None):
        self.link, self.request_id, self.response_id, self.timeout_ms = link, request_id, response_id, timeout_ms

    def request(self, payload):
        """Send a request, return the positive response bytes (starting with SID + 0x40)."""
        return self.link.uds_request(self.request_id, self.response_id, payload, self.timeout_ms)

    # ---- services
    def tester_present(self):
        self.request([0x3E, 0x00])

    def session(self, kind):
        self.request([0x10, kind])

    def read_did(self, did):
        resp = self.request([0x22, did >> 8, did & 0xFF])
        if len(resp) < 3 or resp[1] != did >> 8 or resp[2] != did & 0xFF:
            raise UdsError("malformed ReadDataByIdentifier response")
        return resp[3:]

    def read_dids(self, dids):
        """Read several identifiers in one request. Returns {did: bytes}.

        The response is the identifiers and their data run together with no
        lengths, so it is split at each requested identifier in order. That is
        ambiguous if an identifier's bytes also occur inside earlier data.
        """
        payload = [0x22]
        for did in dids:
            payload += [did >> 8, did & 0xFF]
        body = self.request(payload)[1:]
        found = []
        pos = 0
        for did in dids:
            at = body.find(bytes([did >> 8, did & 0xFF]), pos)
            if at >= 0:
                found.append((did, at))
                pos = at + 2
        out = {}
        for i, (did, at) in enumerate(found):
            end = found[i + 1][1] if i + 1 < len(found) else len(body)
            out[did] = body[at + 2:end]
        return out

    def read_dtcs(self, status_mask=0xFF):
        """Return (availability_mask, [(dtc_bytes, status), ...])."""
        resp = self.request([0x19, 0x02, status_mask])
        if len(resp) < 3 or resp[1] != 0x02:
            raise UdsError("malformed ReadDTCInformation response")
        body = resp[3:]
        return resp[2], [(body[i:i + 3], body[i + 3]) for i in range(0, len(body) - 3, 4)]

    def dtc_count(self, status_mask=0xFF):
        resp = self.request([0x19, 0x01, status_mask])
        if len(resp) < 6 or resp[1] != 0x01:
            raise UdsError("malformed ReadDTCInformation count response")
        return resp[4] << 8 | resp[5]

    def dtc_snapshots(self, dtc, record=0xFF, sizes=None):
        """Freeze-frame records stored with one fault code: {record: {did: bytes}}.

        A record lists identifiers and their data with no lengths, so `sizes` ({did: length})
        is needed to split them; reading stops at the first identifier of unknown length and
        keeps the rest under the key None.
        """
        resp = self.request([0x19, 0x04] + list(dtc) + [record])
        if len(resp) < 6 or resp[1] != 0x04:
            raise UdsError("malformed snapshot response")
        out, body, i = {}, resp[6:], 0
        sizes = sizes or {}
        while i + 2 <= len(body):
            rec, count = body[i], body[i + 1]
            i += 2
            items = out.setdefault(rec, {})
            for _ in range(count):
                if i + 2 > len(body):
                    break
                did = body[i] << 8 | body[i + 1]
                if did not in sizes:
                    items[None] = body[i:]
                    return out
                items[did] = body[i + 2:i + 2 + sizes[did]]
                i += 2 + sizes[did]
        return out

    def dtc_extended(self, dtc, record=0xFF):
        """Extended data stored with one fault code (counters), as raw bytes after the status."""
        resp = self.request([0x19, 0x06] + list(dtc) + [record])
        if len(resp) < 6 or resp[1] != 0x06:
            raise UdsError("malformed extended-data response")
        return resp[6:]

    def clear_dtcs(self):
        """Erase stored fault codes in this module. This changes the car's state."""
        self.request([0x14, 0xFF, 0xFF, 0xFF])
