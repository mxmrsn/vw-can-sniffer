// Self-test helpers shared by the two ESP32-S3 CAN drivers; see twai_selftest.c.
// Needs CAN_TX_PIN and CAN_RX_PIN from the board's pins.h.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "pins.h"
#include "selftest.h"

enum selftest_result twai_selftest_check(bool silent, char *detail, size_t cap);
// Reinstall the driver in no-acknowledge self-test mode. The caller holds its driver lock, and
// reinstalls its normal mode afterwards.
esp_err_t twai_selftest_install(void);
// Send one frame and wait for it to come back. The transceiver must be able to drive the bus.
enum selftest_result twai_selftest_loopback(char *detail, size_t cap);
