// ISO 15765-2 (ISO-TP) transport over classic CAN, normal addressing.
// One link = one request/response ID pair. Platform independent: the caller
// supplies a frame-send function and a millisecond clock, feeds received
// frames in with isotp_on_frame() and calls isotp_poll() regularly.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ISOTP_MAX_PAYLOAD 4095

struct can_frame {
    uint32_t id;   // 11-bit or 29-bit identifier, no flag bits
    bool ext;      // true = 29-bit identifier
    uint8_t dlc;   // 0..8
    uint8_t data[8];
};

enum isotp_err {
    ISOTP_OK = 0,
    ISOTP_ERR_BUSY = -1,       // a transmission is already in progress
    ISOTP_ERR_TOO_LONG = -2,   // payload exceeds ISOTP_MAX_PAYLOAD (or is empty)
    ISOTP_ERR_SEND = -3,       // CAN driver refused a frame for too long
    ISOTP_ERR_TIMEOUT_FC = -4, // no flow control after our first frame
    ISOTP_ERR_TIMEOUT_CF = -5, // peer stopped sending consecutive frames
    ISOTP_ERR_SEQ = -6,        // consecutive frame out of sequence
    ISOTP_ERR_OVERFLOW = -7,   // peer reported overflow, or message too big for us
    ISOTP_ERR_FC = -8,         // invalid flow control frame
    ISOTP_ERR_WAIT_LIMIT = -9, // peer sent too many flow control "wait" frames
};

// Return 0 if the frame was accepted for transmission, non-zero if the
// driver's queue is full (the link retries on the next poll).
typedef int (*isotp_send_fn)(void *ctx, const struct can_frame *frame);
typedef uint32_t (*isotp_now_fn)(void *ctx);

struct isotp_link {
    // configuration
    uint32_t tx_id, rx_id;
    bool ext;
    bool pad;          // pad every frame to 8 bytes
    uint8_t pad_byte;
    uint32_t timeout_ms; // N_Bs / N_Cr / N_As
    isotp_send_fn send;
    isotp_now_fn now;
    void *ctx;

    // transmit state
    uint8_t tx_state;
    uint8_t tx_buf[ISOTP_MAX_PAYLOAD];
    uint16_t tx_len, tx_off;
    uint8_t tx_sn, tx_bs, tx_bs_count, tx_stmin_ms, tx_wait_count;
    uint32_t tx_deadline, tx_next_cf;

    // receive state
    uint8_t rx_state;
    uint8_t rx_buf[ISOTP_MAX_PAYLOAD];
    uint16_t rx_len, rx_off;
    uint8_t rx_sn;
    uint32_t rx_deadline;

    int error; // sticky until isotp_take_error()
};

void isotp_init(struct isotp_link *l, uint32_t tx_id, uint32_t rx_id, bool ext,
                isotp_send_fn send, isotp_now_fn now, void *ctx);

// Start sending a message. Returns ISOTP_OK or a negative isotp_err.
int isotp_send(struct isotp_link *l, const uint8_t *data, size_t len);
bool isotp_tx_busy(const struct isotp_link *l);
// True while a multi-frame message is being received.
bool isotp_rx_busy(const struct isotp_link *l);

// Feed every received CAN frame here; frames for other IDs are ignored.
void isotp_on_frame(struct isotp_link *l, const struct can_frame *f);

// Drive timeouts and consecutive-frame transmission. Call often.
void isotp_poll(struct isotp_link *l);

// Fetch a completed message. Returns 1 and sets *len if one was copied,
// 0 if none is ready, ISOTP_ERR_OVERFLOW if it does not fit in `cap`
// (the message is discarded).
int isotp_recv(struct isotp_link *l, uint8_t *buf, size_t cap, size_t *len);

// Return and clear the last asynchronous error (0 if none).
int isotp_take_error(struct isotp_link *l);

// Abort any transfer in progress and discard received data.
void isotp_reset(struct isotp_link *l);
