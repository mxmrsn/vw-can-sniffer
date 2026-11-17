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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"
#include "isotp.h"

#define PROTO_VERSION 1
#define PROTO_NAME "vw-diag pico2w 0.2.0"

enum proto_command {
    PROTO_CMD_HELLO = 0x01,         // -> [version][max payload u16][name...]
    PROTO_CMD_STATUS = 0x02,        // -> [flags][rx u32][dropped u32][tx u32][errors u32][bad packets u32][events dropped u32]
    PROTO_CMD_SET_SILENT = 0x03,    // [0|1]
    PROTO_CMD_SET_MONITOR = 0x04,   // [0|1]
    PROTO_CMD_UDS = 0x05,           // [req id u32][resp id u32][timeout ms u16, 0 = default][request...] -> [response...]
    PROTO_CMD_SET_KEEPALIVE = 0x06, // [0|1]
    PROTO_CMD_SET_LOG = 0x08,       // [0|1] -> [frames u32][bytes u32][lost u32][file name...]
    PROTO_CMD_SET_TIME = 0x09,      // [unix seconds u32]
    PROTO_CMD_LOG_STATUS = 0x0A,    // -> [active][frames u32][bytes u32][lost u32][file name...]
    PROTO_CMD_POLL_ADD = 0x0B,      // [req id u32][resp id u32][period ms u32][request, 1-7 bytes]
    PROTO_CMD_POLL_CLEAR = 0x0C,
    PROTO_CMD_LOG_MARK = 0x0D,      // [text...]
    PROTO_CMD_LOG_LIST = 0x0E,      // [first index u16, optional] -> [total u16][count] then count x [size u32][name length][name]
    PROTO_CMD_LOG_READ = 0x0F,      // [offset u32][max length u16][name...] -> [file size u32][data...]
    PROTO_CMD_POWER = 0x10,         // [] or [op][value u32] -> power state; UNKNOWN_CMD on boards without it
    PROTO_CMD_GNSS = 0x11,          // [] or [sync hz u16] -> GPS and sync state; UNKNOWN_CMD on boards without it
    PROTO_CMD_SELFTEST = 0x12,      // [flags: 1 active, 2 force, optional] -> [count] then count x
                                    // [result][id length][id][detail length][detail]
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
    PROTO_STORAGE = 7,   // no card, or the file could not be written or read
    PROTO_BUSY = 8,      // that file is being recorded: stop logging first
};

#define PROTO_LOG_LIST_BYTES 1000 // a listing page stays small, for slow Bluetooth links
#define PROTO_LOG_READ_MAX 4000   // the most one LOG_READ returns (fits the response buffer with its header)

enum { PROTO_FLAG_SILENT = 1, PROTO_FLAG_MONITOR = 2, PROTO_FLAG_KEEPALIVE = 4, PROTO_FLAG_LOGGING = 8 };

// One link that speaks the protocol: the USB console in binary mode, or Bluetooth LE.
// Requests are answered on the link they came in on. Each link has its own
// monitor switch, so a phone can watch the bus without flooding USB or the reverse.
//
// write() sends one whole encoded packet. It may refuse (return false), e.g.
// when a Bluetooth link is too slow to keep up; events are then counted as dropped.
typedef bool (*proto_write_fn)(const uint8_t *data, size_t len, bool is_event);

struct proto_port {
    struct frame_decoder decoder;
    proto_write_fn write;
    bool monitor;
    uint32_t bad_packets;
    uint32_t events_dropped;
};

void proto_port_init(struct proto_port *port, proto_write_fn write); // also resets a port
void proto_attach(struct proto_port *port);                         // start sending it events
void proto_detach(struct proto_port *port);
void proto_rx_byte(struct proto_port *port, uint8_t byte);

// Send a received CAN frame to every attached link that has monitoring on.
void proto_send_can_event(const struct can_frame *f, uint32_t time_ms);
