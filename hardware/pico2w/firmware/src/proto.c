#include "proto.h"

#include <string.h>

#include "app.h"
#include "can_port.h"
#include "frame.h"
#include "platform.h"
#include "uds.h"

#define HEADER 3 // seq, command, status

static struct frame_decoder decoder;
static uint8_t out[HEADER + ISOTP_MAX_PAYLOAD];
static uint8_t encoded[FRAME_ENCODED_MAX(sizeof(out))];
static uint32_t bad_packets;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void transmit(const uint8_t *payload, size_t len)
{
    size_t n = frame_encode(payload, len, encoded, sizeof(encoded));
    if (n)
        plat_write(encoded, n);
}

static void respond(uint8_t status, size_t data_len)
{
    out[2] = status;
    transmit(out, HEADER + data_len);
}

void proto_init(void)
{
    frame_decoder_init(&decoder);
}

void proto_send_can_event(const struct can_frame *f, uint32_t time_ms)
{
    // Own buffers: this runs while a UDS request is waiting for its answer.
    uint8_t evt[2 + 4 + 4 + 1 + 8], enc[FRAME_ENCODED_MAX(sizeof(evt))];
    uint8_t dlc = f->dlc > 8 ? 8 : f->dlc;
    evt[0] = 0;
    evt[1] = PROTO_EVT_CAN_FRAME;
    put_u32(evt + 2, time_ms);
    put_u32(evt + 6, f->id | (f->ext ? 0x80000000u : 0));
    evt[10] = dlc;
    memcpy(evt + 11, f->data, dlc);
    size_t n = frame_encode(evt, 11 + dlc, enc, sizeof(enc));
    if (n)
        plat_write(enc, n);
}

static void handle(const uint8_t *in, size_t len)
{
    if (len < 2 || in[0] == 0)
        return; // too short to answer, or uses the sequence number reserved for events
    uint8_t cmd = in[1];
    const uint8_t *args = in + 2;
    size_t nargs = len - 2;
    uint8_t *data = out + HEADER;
    out[0] = in[0];
    out[1] = cmd | 0x80;

    switch (cmd) {
    case PROTO_CMD_HELLO:
        data[0] = PROTO_VERSION;
        data[1] = (uint8_t)FRAME_MAX_PAYLOAD;
        data[2] = (uint8_t)(FRAME_MAX_PAYLOAD >> 8);
        memcpy(data + 3, PROTO_NAME, sizeof(PROTO_NAME) - 1);
        respond(PROTO_OK, 3 + sizeof(PROTO_NAME) - 1);
        break;

    case PROTO_CMD_STATUS: {
        struct can_stats s;
        can_port_get_stats(&s);
        data[0] = app_flags();
        put_u32(data + 1, s.rx);
        put_u32(data + 5, s.rx_dropped);
        put_u32(data + 9, s.tx);
        put_u32(data + 13, s.errors);
        put_u32(data + 17, bad_packets);
        respond(PROTO_OK, 21);
        break;
    }

    case PROTO_CMD_SET_SILENT:
    case PROTO_CMD_SET_MONITOR:
    case PROTO_CMD_SET_KEEPALIVE:
        if (nargs != 1 || args[0] > 1) {
            respond(PROTO_BAD_ARGS, 0);
            break;
        }
        if (cmd == PROTO_CMD_SET_SILENT)
            app_set_silent(args[0]);
        else if (cmd == PROTO_CMD_SET_MONITOR)
            app_set_monitor(args[0]);
        else
            app_set_keepalive(args[0]);
        respond(PROTO_OK, 0);
        break;

    case PROTO_CMD_UDS: {
        if (nargs < 11 || nargs - 10 > ISOTP_MAX_PAYLOAD) {
            respond(PROTO_BAD_ARGS, 0);
            break;
        }
        if (can_port_is_silent()) {
            respond(PROTO_SILENT, 0);
            break;
        }
        uint32_t timeout = (uint32_t)args[8] | (uint32_t)args[9] << 8;
        size_t n = 0;
        int detail = 0;
        // `args` points into the decoder, which nothing touches until we return.
        int rc = app_uds_request(get_u32(args), get_u32(args + 4), timeout, args + 10, nargs - 10,
                                 data, ISOTP_MAX_PAYLOAD, &n, &detail);
        if (rc == UDS_OK) {
            respond(PROTO_OK, n);
        } else if (rc == UDS_ERR_NEGATIVE) {
            data[0] = (uint8_t)detail;
            respond(PROTO_NEGATIVE, 1);
        } else if (rc == UDS_ERR_TIMEOUT) {
            respond(PROTO_TIMEOUT, 0);
        } else {
            data[0] = (uint8_t)detail;
            respond(PROTO_TRANSPORT, 1);
        }
        break;
    }

    case PROTO_CMD_EXIT:
        respond(PROTO_OK, 0);
        app_leave_binary();
        break;

    default:
        respond(PROTO_UNKNOWN_CMD, 0);
        break;
    }
}

void proto_rx_byte(uint8_t byte)
{
    const uint8_t *payload;
    int n = frame_decoder_push(&decoder, byte, &payload);
    if (n > 0)
        handle(payload, (size_t)n);
    else if (n < 0)
        bad_packets++;
}
