// Diagnostic CAN identifiers (request -> response) for UDS over ISO-TP.
//
// Engine and transmission use the legislated OBD identifiers and are reliable.
// The other rows are the commonly published VW MQB assignments and have NOT
// been confirmed on this car: verify each one before relying on it.
#pragma once

#include <stdint.h>

struct ecu_address {
    const char *name;
    uint32_t request_id;
    uint32_t response_id;
    const char *description;
};

static const struct ecu_address VW_ECUS[] = {
    { "engine",  0x7E0, 0x7E8, "Engine control (01)" },
    { "trans",   0x7E1, 0x7E9, "Transmission (02)" },
    { "abs",     0x713, 0x77D, "ABS / ESC (03), unverified" },
    { "cluster", 0x714, 0x77E, "Instrument cluster (17), unverified" },
    { "airbag",  0x715, 0x77F, "Airbag (15), unverified" },
    { "gateway", 0x710, 0x77A, "Diagnostic gateway (19), unverified" },
    { "steer",   0x712, 0x77C, "Power steering (44), unverified" },
};

#define VW_ECU_COUNT (sizeof(VW_ECUS) / sizeof(VW_ECUS[0]))
