// Minimal UDS (ISO 14229) client on top of an ISO-TP link.
// Requests are synchronous: uds_request() calls the user's pump function in a
// loop until a response arrives or the timeout expires. The pump must feed
// received CAN frames into isotp_on_frame() and call isotp_poll().
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "isotp.h"

enum uds_result {
    UDS_OK = 0,
    UDS_ERR_TIMEOUT = -1,   // no (final) response in time
    UDS_ERR_TRANSPORT = -2, // ISO-TP error, see uds_client.last_tp_error
    UDS_ERR_OVERFLOW = -3,  // response larger than the caller's buffer
    UDS_ERR_NEGATIVE = -4,  // ECU refused, see uds_client.last_nrc
    UDS_ERR_FORMAT = -5,    // positive response too short / wrong echo
};

// Service identifiers used here
#define UDS_SID_SESSION_CONTROL 0x10
#define UDS_SID_READ_DTC_INFO 0x19
#define UDS_SID_READ_DATA_BY_ID 0x22
#define UDS_SID_TESTER_PRESENT 0x3E

#define UDS_NRC_RESPONSE_PENDING 0x78

struct uds_client {
    struct isotp_link *tp;
    void (*pump)(void *ctx);
    void *ctx;
    uint32_t p2_ms;       // wait for the first response
    uint32_t p2_star_ms;  // wait after each "response pending"
    uint8_t max_pending;  // how many "response pending" replies to tolerate
    uint8_t last_nrc;
    int last_tp_error;
};

void uds_init(struct uds_client *c, struct isotp_link *tp, void (*pump)(void *ctx), void *ctx);

// Send `req` and wait for the matching response. On UDS_OK, `resp` holds the
// whole positive response including the response service id.
int uds_request(struct uds_client *c, const uint8_t *req, size_t req_len,
                uint8_t *resp, size_t cap, size_t *resp_len);

// 0x10 DiagnosticSessionControl. 0x01 default, 0x03 extended.
int uds_session_control(struct uds_client *c, uint8_t session);

// 0x3E TesterPresent (expects the ECU's reply).
int uds_tester_present(struct uds_client *c);

// 0x22 ReadDataByIdentifier. On UDS_OK, `out` holds only the data record.
int uds_read_did(struct uds_client *c, uint16_t did, uint8_t *out, size_t cap, size_t *out_len);

// 0x19 0x02 ReportDTCByStatusMask. On UDS_OK, `out` holds 4-byte records:
// 3 bytes DTC, 1 byte status. *availability_mask may be NULL.
int uds_read_dtcs(struct uds_client *c, uint8_t status_mask, uint8_t *availability_mask,
                  uint8_t *out, size_t cap, size_t *out_len);

const char *uds_result_str(int result);
const char *uds_nrc_str(uint8_t nrc);
