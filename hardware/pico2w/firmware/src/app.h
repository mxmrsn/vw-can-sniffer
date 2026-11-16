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

bool app_set_logging(bool on); // false if it could not be started (no card)
void app_set_time(uint32_t unix_seconds);
void app_log_mark(const char *text);

// Requests repeated on a timer (and so recorded in the log). Single CAN frame only.
#define APP_POLL_MAX_LEN 7
bool app_poll_add(uint32_t request_id, uint32_t response_id, uint32_t period_ms, const uint8_t *req, size_t len);
void app_poll_clear(void);

// Power management, on boards built with APP_POWER (the OBD dongle). Both return false elsewhere.
#include "power.h"
bool app_power_get(struct power_status *st, struct power_settings *set);
// op: 1 stay awake (0/1), 2 idle seconds, 3 battery cutoff mV, 4 sleep now
bool app_power_set(uint8_t op, uint32_t value);

// One UDS request to any address. timeout_ms 0 uses the console's setting.
// Returns a uds.h result; *detail is the NRC or the ISO-TP error code.
int app_uds_request(uint32_t request_id, uint32_t response_id, uint32_t timeout_ms,
                    const uint8_t *req, size_t req_len,
                    uint8_t *resp, size_t resp_cap, size_t *resp_len, int *detail);
