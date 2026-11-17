// Bring-up self-test: one line per part of the board, so the first power-on of a new board says
// what works and what to probe. Console: selftest [active] [force]; protocol command 0x12;
// vwdiag selftest adds what to check for each failure.
//
// Passive checks only read: firmware, CAN controller and transceiver state, the card (a small
// test file written, read back and deleted), Bluetooth, supply voltage, GPS input. The LEDs light
// one after another for a person to watch.
//
// Active checks drive outputs, so they run only when asked: the CAN loopback sends one frame
// (ID 7FF, "SELFTEST") through the transceiver and reads it back, and the sync output pulses for a
// moment. The loopback refuses while it sees bus traffic or, on the dongle, battery voltage at the
// OBD port, so it is not sent into a car by accident; "force" overrides that on a bench supply.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum selftest_result {
    SELFTEST_PASS = 0,
    SELFTEST_WARN = 1, // works, but something deserves a look
    SELFTEST_FAIL = 2,
    SELFTEST_SKIP = 3, // not run: not on this board, not asked for, or not safe right now
    SELFTEST_LOOK = 4, // a person has to check it by eye
};

#define SELFTEST_MAX 16
#define SELFTEST_ID_LEN 12
#define SELFTEST_DETAIL_LEN 96

struct selftest_item {
    uint8_t result;
    char id[SELFTEST_ID_LEN];         // short and stable: "can", "card", "gps" ... (the tools key hints on it)
    char detail[SELFTEST_DETAIL_LEN]; // what was measured, in words
};

struct selftest_report {
    size_t count;
    struct selftest_item items[SELFTEST_MAX];
};

enum { SELFTEST_ACTIVE = 1, SELFTEST_FORCE = 2 };

void selftest_add(struct selftest_report *r, const char *id, enum selftest_result result, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
const char *selftest_result_name(enum selftest_result result);
