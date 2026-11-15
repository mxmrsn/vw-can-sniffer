// Packet framing for byte streams: COBS encoding with a CRC-16, one 0x00 byte
// between packets. It does not care how the stream is chopped up in transit,
// so the same code serves USB serial and BLE notifications of any size.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FRAME_MAX_PAYLOAD 4160 // a full 4095-byte UDS message plus headers

// Worst-case encoded size for a payload of `len` bytes.
#define FRAME_ENCODED_MAX(len) ((len) + 2 + ((len) + 2) / 254 + 2)

struct frame_decoder {
    uint8_t buf[FRAME_ENCODED_MAX(FRAME_MAX_PAYLOAD)];
    size_t len;
    int overflow;
};

uint16_t frame_crc16(const uint8_t *data, size_t len);

// Encode one packet, including the trailing 0x00. Returns the number of bytes
// written, or 0 if it does not fit in `cap`.
size_t frame_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

void frame_decoder_init(struct frame_decoder *d);

// Feed one received byte. Returns the payload length (> 0) and sets *payload
// when a complete, valid packet has arrived; 0 while waiting for more bytes;
// -1 if a packet ended but was damaged (it is dropped and decoding continues).
// *payload points into the decoder and is valid until the next call.
int frame_decoder_push(struct frame_decoder *d, uint8_t byte, const uint8_t **payload);
