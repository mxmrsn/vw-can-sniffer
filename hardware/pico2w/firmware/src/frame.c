#include "frame.h"

#include <string.h>

uint16_t frame_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF; // CRC-16/CCITT-FALSE
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t frame_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
    if (len == 0 || len > FRAME_MAX_PAYLOAD || cap < FRAME_ENCODED_MAX(len))
        return 0;

    uint16_t crc = frame_crc16(payload, len);
    uint8_t tail[2] = { (uint8_t)(crc >> 8), (uint8_t)crc };

    size_t code_at = 0, n = 1; // out[code_at] is filled in when its block closes
    uint8_t code = 1;
    for (size_t i = 0; i < len + 2; i++) {
        uint8_t b = i < len ? payload[i] : tail[i - len];
        if (b == 0) {
            out[code_at] = code;
            code_at = n++;
            code = 1;
        } else {
            out[n++] = b;
            if (++code == 0xFF) {
                out[code_at] = code;
                code_at = n++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    out[n++] = 0x00;
    return n;
}

void frame_decoder_init(struct frame_decoder *d)
{
    d->len = 0;
    d->overflow = 0;
}

int frame_decoder_push(struct frame_decoder *d, uint8_t byte, const uint8_t **payload)
{
    if (byte != 0x00) {
        if (d->len < sizeof(d->buf))
            d->buf[d->len++] = byte;
        else
            d->overflow = 1;
        return 0;
    }

    size_t in_len = d->len;
    int bad = d->overflow;
    d->len = 0;
    d->overflow = 0;
    if (in_len == 0)
        return 0; // empty packet: just a separator
    if (bad)
        return -1;

    // COBS decode in place (output never overtakes input).
    size_t in = 0, out = 0;
    while (in < in_len) {
        uint8_t code = d->buf[in++];
        if (code == 0 || in + code - 1 > in_len)
            return -1;
        for (uint8_t i = 1; i < code; i++)
            d->buf[out++] = d->buf[in++];
        if (code != 0xFF && in < in_len)
            d->buf[out++] = 0x00;
    }
    if (out < 3)
        return -1;
    uint16_t want = (uint16_t)((d->buf[out - 2] << 8) | d->buf[out - 1]);
    if (frame_crc16(d->buf, out - 2) != want)
        return -1;
    *payload = d->buf;
    return (int)(out - 2);
}
