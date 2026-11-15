// Bluetooth LE byte link (Nordic UART Service): the platform-independent part.
//
// The Bluetooth stack runs in the background (on the Pico, in an interrupt) and
// the protocol runs in the main loop. Two single-producer, single-consumer rings
// connect them, so neither side ever waits for the other:
//   receive:  stack pushes written bytes  -> main loop pops them into proto_rx_byte
//   transmit: main loop queues packets    -> stack takes notification-sized pieces
//
// Packets are queued whole or not at all, so a slow link loses whole events and
// never corrupts the byte stream. Events may only ever occupy a small part of the
// buffer: that keeps room for a reply, and bounds how far behind the link a reply
// can be queued. Responses may use the whole buffer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"
#include "isotp.h"

#define BLE_RX_BUFFER 8192  // power of two, more than one maximum-size packet
#define BLE_TX_BUFFER 16384 // power of two
#define BLE_TX_RESERVE FRAME_ENCODED_MAX(3 + ISOTP_MAX_PAYLOAD)
#define BLE_TX_EVENT_BACKLOG 2048 // at an iPhone's throughput, about 0.1 s of events

struct ble_link_info {
    bool available;    // a Bluetooth controller is present and running
    bool connected;    // a phone is connected
    bool subscribed;   // ...and has turned on notifications, so packets can be sent
    bool pairing_open; // new phones may pair (bonded phones can always reconnect)
    bool emulated;     // the simulator's TCP stand-in, not real Bluetooth
    char name[24];     // advertised name
    char address[18];
    uint32_t passkey;  // six digits the phone asks for when pairing
    uint16_t mtu;
    uint32_t connections;
    uint32_t rx_bytes, tx_bytes;
    uint32_t rx_overflow;         // bytes lost because the main loop fell behind
    uint32_t tx_refused;          // packets not queued (link too slow, or not subscribed)
};

// Written by the platform's Bluetooth code, read by the console.
extern struct ble_link_info ble_info;

// ---- Bluetooth stack side
size_t ble_link_rx_push(const uint8_t *data, size_t len); // returns how many bytes fitted
size_t ble_link_tx_take(uint8_t *out, size_t max);        // next piece to notify, 0 if none
size_t ble_link_tx_pending(void);
void ble_link_tx_discard(void);                           // on disconnect
void ble_link_new_session(void);                          // on connect and disconnect

// ---- main loop side
bool ble_link_write(const uint8_t *packet, size_t len, bool is_event);
bool ble_link_rx_pop(uint8_t *byte);
// Returns true once after each connect or disconnect, after throwing away any
// bytes left from the previous connection. The caller then resets its decoder.
bool ble_link_session_changed(void);
