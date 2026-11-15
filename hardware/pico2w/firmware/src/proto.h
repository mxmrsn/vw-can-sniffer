// Binary link protocol: the messages carried inside the packets of frame.h.
// The same bytes work over USB serial now and over BLE later; the full
// description is in hardware/pico2w/docs/link_protocol.md.
//
//   request   [seq 1..255][command][arguments]
//   response  [seq][command | 0x80][status][data]
//   event     [0][event][data]                    (not requested, e.g. monitor)
//
// Multi-byte numbers are little-endian.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "isotp.h"

#define PROTO_VERSION 1
#define PROTO_NAME "vw-diag pico2w 0.2.0"

enum proto_command {
    PROTO_CMD_HELLO = 0x01,         // -> [version][max payload u16][name...]
    PROTO_CMD_STATUS = 0x02,        // -> [flags][rx u32][dropped u32][tx u32][errors u32][bad packets u32]
    PROTO_CMD_SET_SILENT = 0x03,    // [0|1]
    PROTO_CMD_SET_MONITOR = 0x04,   // [0|1]
    PROTO_CMD_UDS = 0x05,           // [req id u32][resp id u32][timeout ms u16, 0 = default][request...] -> [response...]
    PROTO_CMD_SET_KEEPALIVE = 0x06, // [0|1]
    PROTO_CMD_EXIT = 0x7F,          // back to the text console
};

enum proto_event {
    PROTO_EVT_CAN_FRAME = 0x01, // [time ms u32][id u32, bit 31 = 29-bit id][dlc][data...]
};

enum proto_status {
    PROTO_OK = 0,
    PROTO_BAD_ARGS = 1,
    PROTO_UNKNOWN_CMD = 2,
    PROTO_SILENT = 3,    // listen-only: nothing was sent
    PROTO_TIMEOUT = 4,   // the control module did not answer
    PROTO_NEGATIVE = 5,  // data = [NRC]
    PROTO_TRANSPORT = 6, // data = [ISO-TP error code, signed]
};

enum { PROTO_FLAG_SILENT = 1, PROTO_FLAG_MONITOR = 2, PROTO_FLAG_KEEPALIVE = 4 };

void proto_init(void);
void proto_rx_byte(uint8_t byte);
void proto_send_can_event(const struct can_frame *f, uint32_t time_ms);
