#include "isotp.h"

#include <string.h>

enum { TX_IDLE = 0, TX_WAIT_FC, TX_SEND_CF };
enum { RX_IDLE = 0, RX_IN_PROGRESS, RX_DONE };

#define PCI_SF 0x0
#define PCI_FF 0x1
#define PCI_CF 0x2
#define PCI_FC 0x3

#define FC_CTS 0
#define FC_WAIT 1
#define FC_OVERFLOW 2

#define MAX_FC_WAIT 10
#define MAX_CF_PER_POLL 8

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static int send_frame(struct isotp_link *l, const uint8_t *bytes, uint8_t n)
{
    struct can_frame f;
    f.id = l->tx_id;
    f.ext = l->ext;
    memset(f.data, l->pad_byte, sizeof(f.data));
    memcpy(f.data, bytes, n);
    f.dlc = l->pad ? 8 : n;
    return l->send(l->ctx, &f);
}

static void send_flow_control(struct isotp_link *l, uint8_t status)
{
    // Block size 0 and STmin 0: send everything, as fast as you like.
    uint8_t fc[3] = { (uint8_t)((PCI_FC << 4) | status), 0, 0 };
    (void)send_frame(l, fc, sizeof(fc));
}

void isotp_init(struct isotp_link *l, uint32_t tx_id, uint32_t rx_id, bool ext,
                isotp_send_fn send, isotp_now_fn now, void *ctx)
{
    memset(l, 0, sizeof(*l));
    l->tx_id = tx_id;
    l->rx_id = rx_id;
    l->ext = ext;
    l->pad = true;
    l->pad_byte = 0x55;
    l->timeout_ms = 1000;
    l->send = send;
    l->now = now;
    l->ctx = ctx;
}

void isotp_reset(struct isotp_link *l)
{
    l->tx_state = TX_IDLE;
    l->rx_state = RX_IDLE;
    l->error = ISOTP_OK;
}

bool isotp_tx_busy(const struct isotp_link *l)
{
    return l->tx_state != TX_IDLE;
}

bool isotp_rx_busy(const struct isotp_link *l)
{
    return l->rx_state == RX_IN_PROGRESS;
}

int isotp_take_error(struct isotp_link *l)
{
    int e = l->error;
    l->error = ISOTP_OK;
    return e;
}

int isotp_send(struct isotp_link *l, const uint8_t *data, size_t len)
{
    if (l->tx_state != TX_IDLE)
        return ISOTP_ERR_BUSY;
    if (len == 0 || len > ISOTP_MAX_PAYLOAD)
        return ISOTP_ERR_TOO_LONG;

    uint8_t frame[8];
    if (len <= 7) {
        frame[0] = (uint8_t)((PCI_SF << 4) | len);
        memcpy(&frame[1], data, len);
        return send_frame(l, frame, (uint8_t)(len + 1)) ? ISOTP_ERR_SEND : ISOTP_OK;
    }

    memcpy(l->tx_buf, data, len);
    l->tx_len = (uint16_t)len;
    frame[0] = (uint8_t)((PCI_FF << 4) | (len >> 8));
    frame[1] = (uint8_t)(len & 0xFF);
    memcpy(&frame[2], data, 6);
    if (send_frame(l, frame, 8))
        return ISOTP_ERR_SEND;
    l->tx_off = 6;
    l->tx_sn = 1;
    l->tx_wait_count = 0;
    l->tx_state = TX_WAIT_FC;
    l->tx_deadline = l->now(l->ctx) + l->timeout_ms;
    return ISOTP_OK;
}

static void on_flow_control(struct isotp_link *l, const struct can_frame *f)
{
    if (l->tx_state != TX_WAIT_FC)
        return;
    if (f->dlc < 3) {
        l->error = ISOTP_ERR_FC;
        l->tx_state = TX_IDLE;
        return;
    }
    uint32_t now = l->now(l->ctx);
    switch (f->data[0] & 0x0F) {
    case FC_CTS: {
        uint8_t st = f->data[2];
        l->tx_bs = f->data[1];
        // 0x00-0x7F are milliseconds; 0xF1-0xF9 are 100-900 us (round up to 1 ms);
        // anything else is reserved and treated as the maximum, 127 ms.
        l->tx_stmin_ms = st <= 0x7F ? st : (st >= 0xF1 && st <= 0xF9) ? 1 : 127;
        l->tx_bs_count = 0;
        l->tx_next_cf = now;
        l->tx_deadline = now + l->timeout_ms;
        l->tx_state = TX_SEND_CF;
        break;
    }
    case FC_WAIT:
        if (++l->tx_wait_count > MAX_FC_WAIT) {
            l->error = ISOTP_ERR_WAIT_LIMIT;
            l->tx_state = TX_IDLE;
        } else {
            l->tx_deadline = now + l->timeout_ms;
        }
        break;
    case FC_OVERFLOW:
        l->error = ISOTP_ERR_OVERFLOW;
        l->tx_state = TX_IDLE;
        break;
    default:
        l->error = ISOTP_ERR_FC;
        l->tx_state = TX_IDLE;
        break;
    }
}

void isotp_on_frame(struct isotp_link *l, const struct can_frame *f)
{
    if (f->id != l->rx_id || f->ext != l->ext || f->dlc < 1)
        return;

    uint8_t pci = f->data[0] >> 4;
    uint32_t now = l->now(l->ctx);

    switch (pci) {
    case PCI_SF: {
        uint8_t len = f->data[0] & 0x0F;
        if (len == 0 || len > 7 || len > f->dlc - 1)
            return;
        memcpy(l->rx_buf, &f->data[1], len);
        l->rx_len = len;
        l->rx_state = RX_DONE; // a new message replaces any unfinished or unread one
        break;
    }
    case PCI_FF: {
        if (f->dlc < 8)
            return;
        uint16_t len = (uint16_t)(((f->data[0] & 0x0F) << 8) | f->data[1]);
        if (len < 8)
            return;
        memcpy(l->rx_buf, &f->data[2], 6);
        l->rx_len = len;
        l->rx_off = 6;
        l->rx_sn = 1;
        l->rx_state = RX_IN_PROGRESS;
        l->rx_deadline = now + l->timeout_ms;
        send_flow_control(l, FC_CTS);
        break;
    }
    case PCI_CF: {
        if (l->rx_state != RX_IN_PROGRESS)
            return;
        if ((f->data[0] & 0x0F) != l->rx_sn) {
            l->error = ISOTP_ERR_SEQ;
            l->rx_state = RX_IDLE;
            return;
        }
        uint16_t remaining = (uint16_t)(l->rx_len - l->rx_off);
        uint8_t n = (uint8_t)(f->dlc - 1);
        if (n > remaining)
            n = (uint8_t)remaining;
        memcpy(&l->rx_buf[l->rx_off], &f->data[1], n);
        l->rx_off = (uint16_t)(l->rx_off + n);
        l->rx_sn = (uint8_t)((l->rx_sn + 1) & 0x0F);
        if (l->rx_off >= l->rx_len) {
            l->rx_state = RX_DONE;
        } else if (n < 7) {
            // short frame in the middle of a message
            l->error = ISOTP_ERR_SEQ;
            l->rx_state = RX_IDLE;
        } else {
            l->rx_deadline = now + l->timeout_ms;
        }
        break;
    }
    case PCI_FC:
        on_flow_control(l, f);
        break;
    default:
        break;
    }
}

void isotp_poll(struct isotp_link *l)
{
    uint32_t now = l->now(l->ctx);

    if (l->rx_state == RX_IN_PROGRESS && time_reached(now, l->rx_deadline)) {
        l->error = ISOTP_ERR_TIMEOUT_CF;
        l->rx_state = RX_IDLE;
    }

    if (l->tx_state == TX_WAIT_FC) {
        if (time_reached(now, l->tx_deadline)) {
            l->error = ISOTP_ERR_TIMEOUT_FC;
            l->tx_state = TX_IDLE;
        }
        return;
    }
    if (l->tx_state != TX_SEND_CF)
        return;

    for (int burst = 0; burst < MAX_CF_PER_POLL; burst++) {
        if (!time_reached(now, l->tx_next_cf))
            return;

        uint8_t frame[8];
        uint16_t remaining = (uint16_t)(l->tx_len - l->tx_off);
        uint8_t n = remaining > 7 ? 7 : (uint8_t)remaining;
        frame[0] = (uint8_t)((PCI_CF << 4) | l->tx_sn);
        memcpy(&frame[1], &l->tx_buf[l->tx_off], n);
        if (send_frame(l, frame, (uint8_t)(n + 1))) {
            // driver queue full: try again next poll, give up after the timeout
            if (time_reached(now, l->tx_deadline)) {
                l->error = ISOTP_ERR_SEND;
                l->tx_state = TX_IDLE;
            }
            return;
        }
        l->tx_off = (uint16_t)(l->tx_off + n);
        l->tx_sn = (uint8_t)((l->tx_sn + 1) & 0x0F);
        l->tx_deadline = now + l->timeout_ms;

        if (l->tx_off >= l->tx_len) {
            l->tx_state = TX_IDLE;
            return;
        }
        if (l->tx_bs && ++l->tx_bs_count >= l->tx_bs) {
            l->tx_wait_count = 0;
            l->tx_state = TX_WAIT_FC;
            return;
        }
        l->tx_next_cf = now + l->tx_stmin_ms;
    }
}

int isotp_recv(struct isotp_link *l, uint8_t *buf, size_t cap, size_t *len)
{
    if (l->rx_state != RX_DONE)
        return 0;
    l->rx_state = RX_IDLE;
    if (l->rx_len > cap)
        return ISOTP_ERR_OVERFLOW;
    memcpy(buf, l->rx_buf, l->rx_len);
    *len = l->rx_len;
    return 1;
}
