// Application core: serial console, target selection, UDS commands.
// Platform independent; see platform.h and can_port.h for what it needs.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void app_init(void);
void app_poll(void); // call continuously from the main loop

// Used by the binary protocol (proto.c).
void app_set_silent(bool on);
void app_set_monitor(bool on);
void app_set_keepalive(bool on);
uint8_t app_flags(void);     // PROTO_FLAG_* bits
void app_leave_binary(void); // return the console to text mode

// One UDS request to any address. timeout_ms 0 uses the console's setting.
// Returns a uds.h result; *detail is the NRC or the ISO-TP error code.
int app_uds_request(uint32_t request_id, uint32_t response_id, uint32_t timeout_ms,
                    const uint8_t *req, size_t req_len,
                    uint8_t *resp, size_t resp_cap, size_t *resp_len, int *detail);
