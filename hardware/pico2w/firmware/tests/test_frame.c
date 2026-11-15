// Host tests for the packet framing.   make -C hardware/pico2w/firmware/tests
#include <stdio.h>
#include <string.h>

#include "../src/frame.h"

static int failures, checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

static uint8_t payload[FRAME_MAX_PAYLOAD + 1], enc[FRAME_ENCODED_MAX(FRAME_MAX_PAYLOAD)];
static struct frame_decoder dec;

// Encode, then decode feeding `chunk` bytes at a time; return 1 if it round-trips.
static int roundtrip(size_t len)
{
    size_t n = frame_encode(payload, len, enc, sizeof(enc));
    if (n == 0 || n > FRAME_ENCODED_MAX(len) || enc[n - 1] != 0)
        return 0;
    for (size_t i = 0; i + 1 < n; i++)
        if (enc[i] == 0)
            return 0; // a zero inside the packet would split it
    frame_decoder_init(&dec);
    const uint8_t *out = NULL;
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        int r = frame_decoder_push(&dec, enc[i], &out);
        if (r < 0 || (r > 0 && i != n - 1))
            return 0;
        got = r;
    }
    return got == (int)len && memcmp(out, payload, len) == 0;
}

int main(void)
{
    printf("crc reference value\n");
    CHECK(frame_crc16((const uint8_t *)"123456789", 9) == 0x29B1);

    printf("round trips: sizes around the COBS block boundary, and content patterns\n");
    static const size_t sizes[] = { 1, 2, 3, 7, 252, 253, 254, 255, 256, 507, 508, 509, 1000, 4095, FRAME_MAX_PAYLOAD };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        memset(payload, 0x00, sizeof(payload));           CHECK(roundtrip(sizes[s]));
        memset(payload, 0xFF, sizeof(payload));           CHECK(roundtrip(sizes[s]));
        for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 31 + 7);
        CHECK(roundtrip(sizes[s]));
        for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (i % 5) ? (uint8_t)i : 0;
        CHECK(roundtrip(sizes[s]));
    }

    printf("limits\n");
    CHECK(frame_encode(payload, 0, enc, sizeof(enc)) == 0);
    CHECK(frame_encode(payload, FRAME_MAX_PAYLOAD + 1, enc, sizeof(enc)) == 0);
    CHECK(frame_encode(payload, 10, enc, 5) == 0);

    printf("damaged packets are rejected and the decoder recovers\n");
    const uint8_t *out = NULL;
    for (size_t i = 0; i < 64; i++) payload[i] = (uint8_t)(i + 1);
    size_t n = frame_encode(payload, 64, enc, sizeof(enc));
    frame_decoder_init(&dec);
    enc[10] ^= 0x04; // flip a bit
    int r = 0;
    for (size_t i = 0; i < n; i++) r = frame_decoder_push(&dec, enc[i], &out);
    CHECK(r == -1);
    enc[10] ^= 0x04;
    for (size_t i = 0; i < n; i++) r = frame_decoder_push(&dec, enc[i], &out);
    CHECK(r == 64 && memcmp(out, payload, 64) == 0);

    printf("garbage, truncation and repeated separators\n");
    frame_decoder_init(&dec);
    CHECK(frame_decoder_push(&dec, 0x00, &out) == 0);   // lone separators are ignored
    CHECK(frame_decoder_push(&dec, 0x00, &out) == 0);
    static const uint8_t junk[] = { 0x05, 0x11, 0x22 }; // block claims more bytes than present
    for (size_t i = 0; i < sizeof(junk); i++) frame_decoder_push(&dec, junk[i], &out);
    CHECK(frame_decoder_push(&dec, 0x00, &out) == -1);
    for (size_t i = 5; i < n; i++) r = frame_decoder_push(&dec, enc[i], &out); // tail of a packet only
    CHECK(r == -1);
    for (size_t i = 0; i < n; i++) r = frame_decoder_push(&dec, enc[i], &out);
    CHECK(r == 64);

    printf("oversized input does not overrun the decoder\n");
    frame_decoder_init(&dec);
    for (size_t i = 0; i < sizeof(dec.buf) + 500; i++) frame_decoder_push(&dec, 0x41, &out);
    CHECK(frame_decoder_push(&dec, 0x00, &out) == -1);
    for (size_t i = 0; i < n; i++) r = frame_decoder_push(&dec, enc[i], &out);
    CHECK(r == 64);

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
