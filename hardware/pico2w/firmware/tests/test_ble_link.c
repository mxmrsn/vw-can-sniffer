// Host tests for the Bluetooth byte link buffers.   make -C hardware/pico2w/firmware/tests
#include <stdio.h>
#include <string.h>

#include "../src/ble_link.h"

static int failures, checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

static uint8_t pkt[BLE_TX_BUFFER], got[BLE_TX_BUFFER];

int main(void)
{
    for (size_t i = 0; i < sizeof(pkt); i++)
        pkt[i] = (uint8_t)(i * 7 + 1);

    printf("nothing is queued until notifications are on\n");
    CHECK(!ble_link_write(pkt, 10, false));
    CHECK(ble_info.tx_refused == 1 && ble_link_tx_pending() == 0);
    ble_info.subscribed = true;

    printf("packets come out in notification-sized pieces, across the ring's wrap\n");
    size_t total = 0;
    for (int round = 0; round < 50; round++) {
        size_t len = 300 + (size_t)round * 97;
        CHECK(ble_link_write(pkt, len, false));
        size_t n = 0, piece;
        while ((piece = ble_link_tx_take(got + n, 182)) > 0) {
            CHECK(piece <= 182);
            n += piece;
        }
        CHECK(n == len && !memcmp(got, pkt, len));
        total += len;
    }
    CHECK(ble_info.tx_bytes == total);

    printf("events are capped well below the buffer, so replies are never far behind\n");
    size_t queued = 0;
    while (ble_link_write(pkt, 20, true))
        queued += 20;
    CHECK(queued > BLE_TX_EVENT_BACKLOG - 20 && queued <= BLE_TX_EVENT_BACKLOG);
    CHECK(BLE_TX_EVENT_BACKLOG + BLE_TX_RESERVE <= BLE_TX_BUFFER);
    uint32_t refused = ble_info.tx_refused;
    CHECK(!ble_link_write(pkt, 20, true) && ble_info.tx_refused == refused + 1);
    CHECK(ble_link_write(pkt, BLE_TX_RESERVE, false));           // a full-size reply still fits
    while (ble_link_write(pkt, 1000, false))                     // replies may use the rest
        ;
    CHECK(!ble_link_write(pkt, 1000, false));
    CHECK(!ble_link_write(pkt, 20, true));
    ble_link_tx_discard();
    CHECK(ble_link_tx_pending() == 0);

    printf("received bytes arrive in order; overflow is counted, not wrapped over\n");
    uint8_t b;
    CHECK(!ble_link_rx_pop(&b));
    CHECK(ble_link_rx_push(pkt, 5000) == 5000);
    CHECK(ble_link_rx_push(pkt + 5000, 5000) == BLE_RX_BUFFER - 5000);
    CHECK(ble_info.rx_overflow == 10000 - BLE_RX_BUFFER);
    size_t i = 0;
    while (ble_link_rx_pop(&b) && b == pkt[i])
        i++;
    CHECK(i == BLE_RX_BUFFER);

    printf("a new connection throws away the old one's leftovers, once\n");
    CHECK(!ble_link_session_changed());
    ble_link_rx_push(pkt, 30);          // half a packet from the phone that left
    ble_link_new_session();
    CHECK(ble_link_session_changed());
    CHECK(!ble_link_rx_pop(&b));
    CHECK(!ble_link_session_changed());
    ble_link_rx_push(pkt, 3);
    CHECK(ble_link_rx_pop(&b) && b == pkt[0]);

    printf("...but never bytes the new connection already sent\n");
    ble_link_rx_pop(&b);
    ble_link_rx_pop(&b);
    ble_link_new_session();
    ble_link_rx_push(pkt + 10, 4);      // the new phone writes before the main loop looks
    CHECK(ble_link_session_changed());
    for (int i = 0; i < 4; i++)
        CHECK(ble_link_rx_pop(&b) && b == pkt[10 + i]);
    CHECK(!ble_link_rx_pop(&b));

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
