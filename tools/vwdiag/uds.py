"""UDS (ISO 14229) requests through the interface's "x" command."""

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
    def __init__(self, link, request_id, response_id):
        self.link, self.request_id, self.response_id = link, request_id, response_id

    def request(self, payload):
        """Send a request, return the positive response bytes (starting with SID + 0x40)."""
        hexs = " ".join("%02X" % b for b in payload)
        lines = self.link.command("x %X %X %s" % (self.request_id, self.response_id, hexs))
        if not lines:
            raise UdsError("empty reply from the interface")
        reply = lines[-1]
        if reply.startswith("+"):
            return bytes(int(tok, 16) for tok in reply[1:].split())
        words = reply[1:].split()
        if words[:1] == ["nrc"]:
            raise NegativeResponse(int(words[1], 16))
        if words[:1] == ["timeout"]:
            raise NoResponse("no response from %X" % self.response_id)
        raise InterfaceRefused(reply[1:].strip())

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

    def read_dtcs(self, status_mask=0xFF):
        """Return (availability_mask, [(dtc_bytes, status), ...])."""
        resp = self.request([0x19, 0x02, status_mask])
        if len(resp) < 3 or resp[1] != 0x02:
            raise UdsError("malformed ReadDTCInformation response")
        body = resp[3:]
        return resp[2], [(body[i:i + 3], body[i + 3]) for i in range(0, len(body) - 3, 4)]

    def clear_dtcs(self):
        """Erase stored fault codes in this module. This changes the car's state."""
        self.request([0x14, 0xFF, 0xFF, 0xFF])
