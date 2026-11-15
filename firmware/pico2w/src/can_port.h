// CAN bus access for the Pico 2 W board (can2040 in PIO + TJA1051T/3).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "isotp.h"

struct can_stats {
    uint32_t rx;        // frames received
    uint32_t rx_dropped; // frames lost because the receive queue was full
    uint32_t tx;        // frames confirmed sent
    uint32_t errors;    // can2040 error notifications
};

void can_port_init(uint32_t bitrate);

// Listen-only: the transceiver cannot drive the bus, so this node neither
// transmits nor acknowledges. The board starts in this mode.
void can_port_set_silent(bool silent);
bool can_port_is_silent(void);

// Pop one received frame. Returns false if none is waiting.
bool can_port_receive(struct can_frame *out);

// Queue a frame. Returns 0 on success, non-zero if silent or the queue is full.
int can_port_send(const struct can_frame *f);

void can_port_get_stats(struct can_stats *out);
