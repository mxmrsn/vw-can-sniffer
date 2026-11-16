#include "proto.h"

#include <string.h>

#include "app.h"
#include "can_port.h"
#include "frame.h"
#include "logger.h"
#include "storage.h"
#include "uds.h"

#define HEADER 3 // seq, command, status

#define MAX_PORTS 2

static uint8_t out[HEADER + ISOTP_MAX_PAYLOAD];
static uint8_t encoded[FRAME_ENCODED_MAX(sizeof(out))];
static struct proto_port *ports[MAX_PORTS];
static struct proto_port *current; // the link whose request is being handled

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
        current->write(encoded, n, false);
}

static void respond(uint8_t status, size_t data_len)
{
    out[2] = status;
    transmit(out, HEADER + data_len);
}

void proto_port_init(struct proto_port *port, proto_write_fn write)
{
    frame_decoder_init(&port->decoder);
    port->write = write;
    port->monitor = false;
    port->bad_packets = 0;
    port->events_dropped = 0;
}

void proto_attach(struct proto_port *port)
{
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i] == port)
            return;
    for (int i = 0; i < MAX_PORTS; i++) {
        if (!ports[i]) {
            ports[i] = port;
            return;
        }
    }
}

void proto_detach(struct proto_port *port)
{
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i] == port)
            ports[i] = NULL;
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
    size_t n = 0;
    for (int i = 0; i < MAX_PORTS; i++) {
        struct proto_port *p = ports[i];
        if (!p || !p->monitor)
            continue;
        if (!n)
            n = frame_encode(evt, 11 + dlc, enc, sizeof(enc));
        if (!p->write(enc, n, true))
            p->events_dropped++;
    }
}

// One page of the card listing: entries from `first` on, while they fit.
struct list_page {
    uint16_t index, first, total;
    uint8_t count;
    size_t pos;
    bool full;
    uint8_t *data;
};

static void list_entry(const char *name, uint32_t size, void *ctx)
{
    struct list_page *p = ctx;
    size_t n = strlen(name);
    if (p->total < 0xFFFF)
        p->total++;
    if (p->index++ < p->first || p->full)
        return;
    if (p->count == 255 || p->pos + 5 + n > PROTO_LOG_LIST_BYTES) {
        p->full = true;
        return;
    }
    put_u32(p->data + p->pos, size);
    p->data[p->pos + 4] = (uint8_t)n;
    memcpy(p->data + p->pos + 5, name, n);
    p->pos += 5 + n;
    p->count++;
}

static void handle(struct proto_port *port, const uint8_t *in, size_t len)
{
    current = port;
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
        data[0] = (uint8_t)((app_flags() & ~PROTO_FLAG_MONITOR) | (port->monitor ? PROTO_FLAG_MONITOR : 0));
        put_u32(data + 1, s.rx);
        put_u32(data + 5, s.rx_dropped);
        put_u32(data + 9, s.tx);
        put_u32(data + 13, s.errors);
        put_u32(data + 17, port->bad_packets);
        put_u32(data + 21, port->events_dropped);
        respond(PROTO_OK, 25);
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
            port->monitor = args[0];
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

    case PROTO_CMD_SET_LOG:
    case PROTO_CMD_LOG_STATUS: {
        if (cmd == PROTO_CMD_SET_LOG) {
            if (nargs != 1 || args[0] > 1) {
                respond(PROTO_BAD_ARGS, 0);
                break;
            }
            if (!app_set_logging(args[0])) {
                respond(PROTO_STORAGE, 0);
                break;
            }
        }
        struct logger_stats s;
        logger_get_stats(&s);
        size_t name_len = strlen(s.name);
        data[0] = s.active;
        put_u32(data + 1, s.records);
        put_u32(data + 5, s.bytes);
        put_u32(data + 9, s.lost);
        memcpy(data + 13, s.name, name_len);
        respond(PROTO_OK, 13 + name_len);
        break;
    }

    case PROTO_CMD_SET_TIME:
        if (nargs != 4) {
            respond(PROTO_BAD_ARGS, 0);
            break;
        }
        app_set_time(get_u32(args));
        respond(PROTO_OK, 0);
        break;

    case PROTO_CMD_POLL_ADD:
        if (nargs < 13 || !app_poll_add(get_u32(args), get_u32(args + 4), get_u32(args + 8), args + 12, nargs - 12)) {
            respond(PROTO_BAD_ARGS, 0);
            break;
        }
        respond(PROTO_OK, 0);
        break;

    case PROTO_CMD_POLL_CLEAR:
        app_poll_clear();
        respond(PROTO_OK, 0);
        break;

    case PROTO_CMD_LOG_MARK: {
        char text[201];
        size_t n = nargs > 200 ? 200 : nargs;
        if (!logger_active() || n == 0) {
            respond(PROTO_BAD_ARGS, 0);
            break;
        }
        memcpy(text, args, n);
        text[n] = 0;
        app_log_mark(text);
        respond(PROTO_OK, 0);
        break;
    }

    case PROTO_CMD_LOG_LIST: {
        if (nargs != 0 && nargs != 2) {
            respond(PROTO_BAD_ARGS, 0);
            break;
        }
        struct list_page page = { .first = nargs ? (uint16_t)(args[0] | args[1] << 8) : 0, .pos = 3, .data = data };
        if (storage_list(list_entry, &page) < 0) {
            respond(PROTO_STORAGE, 0);
            break;
        }
        data[0] = (uint8_t)page.total;
        data[1] = (uint8_t)(page.total >> 8);
        data[2] = page.count;
        respond(PROTO_OK, page.pos);
        break;
    }

    case PROTO_CMD_LOG_READ: {
        char name[13];
        if (nargs < 7 || nargs - 6 > 12) {
            respond(PROTO_BAD_ARGS, 0);
            break;
        }
        uint32_t offset = get_u32(args);
        size_t want = (size_t)(args[4] | args[5] << 8);
        if (want == 0 || want > PROTO_LOG_READ_MAX)
            want = PROTO_LOG_READ_MAX;
        memcpy(name, args + 6, nargs - 6);
        name[nargs - 6] = 0;
        _Static_assert(4 + PROTO_LOG_READ_MAX <= ISOTP_MAX_PAYLOAD, "LOG_READ answer must fit the response buffer");
        uint32_t size = 0;
        int n = logger_read_file(name, offset, data + 4, want, &size);
        if (n == LOGGER_READ_BAD_NAME) {
            respond(PROTO_BAD_ARGS, 0);
        } else if (n == LOGGER_READ_BUSY) {
            respond(PROTO_BUSY, 0);
        } else if (n < 0) {
            respond(PROTO_STORAGE, 0);
        } else {
            put_u32(data, size);
            respond(PROTO_OK, 4 + (size_t)n);
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

void proto_rx_byte(struct proto_port *port, uint8_t byte)
{
    const uint8_t *payload;
    int n = frame_decoder_push(&port->decoder, byte, &payload);
    if (n > 0)
        handle(port, payload, (size_t)n);
    else if (n < 0)
        port->bad_packets++;
}
