#include "uds.h"

#include <string.h>

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

void uds_init(struct uds_client *c, struct isotp_link *tp, void (*pump)(void *ctx), void *ctx)
{
    memset(c, 0, sizeof(*c));
    c->tp = tp;
    c->pump = pump;
    c->ctx = ctx;
    c->p2_ms = 1000;      // generous: gateways add latency over the nominal 50 ms
    c->p2_star_ms = 5000;
    c->max_pending = 10;
}

int uds_request(struct uds_client *c, const uint8_t *req, size_t req_len,
                uint8_t *resp, size_t cap, size_t *resp_len)
{
    struct isotp_link *tp = c->tp;
    size_t len = 0;

    if (req_len == 0)
        return UDS_ERR_FORMAT;

    // Drop anything left over from an earlier exchange.
    isotp_reset(tp);
    c->last_nrc = 0;
    c->last_tp_error = 0;

    int rc = isotp_send(tp, req, req_len);
    if (rc != ISOTP_OK) {
        c->last_tp_error = rc;
        return UDS_ERR_TRANSPORT;
    }

    uint32_t deadline = tp->now(tp->ctx) + c->p2_ms;
    uint8_t pending = 0;

    for (;;) {
        c->pump(c->ctx);
        uint32_t now = tp->now(tp->ctx);

        int err = isotp_take_error(tp);
        if (err) {
            c->last_tp_error = err;
            return UDS_ERR_TRANSPORT;
        }

        // The response timer only starts once the whole request is out, and
        // stops mattering once a multi-frame response has started arriving:
        // from then on the transport's own timeouts apply.
        if (isotp_tx_busy(tp) || isotp_rx_busy(tp))
            deadline = now + c->p2_ms;

        rc = isotp_recv(tp, resp, cap, &len);
        if (rc == ISOTP_ERR_OVERFLOW)
            return UDS_ERR_OVERFLOW;
        if (rc == 1) {
            if (len >= 3 && resp[0] == 0x7F && resp[1] == req[0]) {
                if (resp[2] == UDS_NRC_RESPONSE_PENDING) {
                    if (++pending > c->max_pending)
                        return UDS_ERR_TIMEOUT;
                    deadline = now + c->p2_star_ms;
                    continue;
                }
                c->last_nrc = resp[2];
                return UDS_ERR_NEGATIVE;
            }
            if (len >= 1 && resp[0] == (uint8_t)(req[0] + 0x40)) {
                *resp_len = len;
                return UDS_OK;
            }
            // Not an answer to this request: keep waiting.
        }

        if (time_reached(now, deadline))
            return UDS_ERR_TIMEOUT;
    }
}

int uds_session_control(struct uds_client *c, uint8_t session)
{
    uint8_t req[2] = { UDS_SID_SESSION_CONTROL, session };
    uint8_t resp[16];
    size_t n;
    int rc = uds_request(c, req, sizeof(req), resp, sizeof(resp), &n);
    if (rc != UDS_OK)
        return rc;
    return (n >= 2 && resp[1] == session) ? UDS_OK : UDS_ERR_FORMAT;
}

int uds_tester_present(struct uds_client *c)
{
    uint8_t req[2] = { UDS_SID_TESTER_PRESENT, 0x00 };
    uint8_t resp[8];
    size_t n;
    int rc = uds_request(c, req, sizeof(req), resp, sizeof(resp), &n);
    if (rc != UDS_OK)
        return rc;
    return n >= 2 ? UDS_OK : UDS_ERR_FORMAT;
}

int uds_read_did(struct uds_client *c, uint16_t did, uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t req[3] = { UDS_SID_READ_DATA_BY_ID, (uint8_t)(did >> 8), (uint8_t)did };
    static uint8_t resp[ISOTP_MAX_PAYLOAD];
    size_t n;
    int rc = uds_request(c, req, sizeof(req), resp, sizeof(resp), &n);
    if (rc != UDS_OK)
        return rc;
    if (n < 3 || resp[1] != req[1] || resp[2] != req[2])
        return UDS_ERR_FORMAT;
    if (n - 3 > cap)
        return UDS_ERR_OVERFLOW;
    memcpy(out, &resp[3], n - 3);
    *out_len = n - 3;
    return UDS_OK;
}

int uds_read_dtcs(struct uds_client *c, uint8_t status_mask, uint8_t *availability_mask,
                  uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t req[3] = { UDS_SID_READ_DTC_INFO, 0x02, status_mask };
    static uint8_t resp[ISOTP_MAX_PAYLOAD];
    size_t n;
    int rc = uds_request(c, req, sizeof(req), resp, sizeof(resp), &n);
    if (rc != UDS_OK)
        return rc;
    if (n < 3 || resp[1] != 0x02)
        return UDS_ERR_FORMAT;
    if (availability_mask)
        *availability_mask = resp[2];
    size_t records = (n - 3) / 4 * 4; // ignore a trailing partial record
    if (records > cap)
        return UDS_ERR_OVERFLOW;
    memcpy(out, &resp[3], records);
    *out_len = records;
    return UDS_OK;
}

const char *uds_result_str(int result)
{
    switch (result) {
    case UDS_OK: return "ok";
    case UDS_ERR_TIMEOUT: return "timeout (no response)";
    case UDS_ERR_TRANSPORT: return "transport error";
    case UDS_ERR_OVERFLOW: return "response too large";
    case UDS_ERR_NEGATIVE: return "negative response";
    case UDS_ERR_FORMAT: return "malformed response";
    default: return "unknown";
    }
}

const char *uds_nrc_str(uint8_t nrc)
{
    switch (nrc) {
    case 0x10: return "general reject";
    case 0x11: return "service not supported";
    case 0x12: return "sub-function not supported";
    case 0x13: return "incorrect message length or format";
    case 0x14: return "response too long";
    case 0x21: return "busy, repeat request";
    case 0x22: return "conditions not correct";
    case 0x24: return "request sequence error";
    case 0x31: return "request out of range";
    case 0x33: return "security access denied";
    case 0x35: return "invalid key";
    case 0x36: return "exceeded number of attempts";
    case 0x37: return "required time delay not expired";
    case 0x72: return "general programming failure";
    case 0x78: return "response pending";
    case 0x7E: return "sub-function not supported in active session";
    case 0x7F: return "service not supported in active session";
    default: return "unlisted NRC";
    }
}
