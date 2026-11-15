// Diagnostic CAN identifiers (request -> response) for UDS over ISO-TP.
//
// Engine and transmission use the legislated OBD identifiers. Airbag, steering,
// radar and camera match the addresses comma.ai's opendbc uses on Golf MK7 cars
// (request + 0x6A = response). ABS, cluster and gateway follow the same pattern
// but have no public source behind them. None is confirmed on this car yet.
// See docs/vw_can_data_sources.md.
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
    { "airbag",  0x715, 0x77F, "Airbag (15)" },
    { "gateway", 0x710, 0x77A, "Diagnostic gateway (19), unverified" },
    { "steer",   0x712, 0x77C, "Power steering (44)" },
    { "radar",   0x757, 0x7C1, "Front radar (13)" },
    { "camera",  0x74F, 0x7B9, "Front camera (A5)" },
};

#define VW_ECU_COUNT (sizeof(VW_ECUS) / sizeof(VW_ECUS[0]))
