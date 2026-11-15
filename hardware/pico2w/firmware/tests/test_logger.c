// Host tests for the logger, with a fake card in memory.   make -C hardware/pico2w/firmware/tests
#include <stdio.h>
#include <string.h>

#include "../src/logger.h"
#include "../src/storage.h"

static int failures, checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

// ---- fake card
static uint8_t card[1 << 20];
static size_t card_len;
static int card_present = 1, card_fail_after = -1, writes, syncs, files;

bool storage_create_log(char *name, size_t cap)
{
    if (!card_present)
        return false;
    snprintf(name, cap, "LOG%05d.VWL", ++files);
    card_len = 0;
    return true;
}
bool storage_write(const uint8_t *d, size_t n)
{
    if (card_fail_after >= 0 && writes >= card_fail_after)
        return false;
    writes++;
    memcpy(card + card_len, d, n);
    card_len += n;
    return true;
}
bool storage_sync(void) { syncs++; return true; }
void storage_close(void) {}
int storage_read_file(const char *n, char *b, size_t c) { (void)n; (void)b; (void)c; return -1; }

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(void)
{
    struct logger_stats s;
    struct can_frame f = { .id = 0x123, .dlc = 3, .data = { 1, 2, 3 } };

    printf("no card\n");
    card_present = 0;
    CHECK(!logger_start(0, 500000, 0));
    CHECK(!logger_active());
    card_present = 1;

    printf("header and records\n");
    logger_set_time(1794787200, 1000);
    CHECK(logger_start(1000, 500000, 7));
    CHECK(card_len == LOG_HEADER_LEN && syncs == 1);   // header written and synced at once
    CHECK(!memcmp(card, "VWLG", 4) && card[4] == LOG_VERSION && u32(card + 8) == 500000 && u32(card + 12) == 1000);
    CHECK(u32(card + 16) == 1794787200);
    logger_can(&f, 1010, false);
    struct can_frame ext = { .id = 0x18DAF110, .ext = true, .dlc = 8 };
    logger_can(&ext, 1011, true);
    logger_mark("hi", 1012);
    CHECK(card_len == LOG_HEADER_LEN);                  // buffered, not written yet
    logger_poll(1100, 7);
    CHECK(card_len == LOG_HEADER_LEN);                  // under the flush period
    logger_poll(1300, 7);
    const uint8_t *r = card + LOG_HEADER_LEN;
    CHECK(card_len == LOG_HEADER_LEN + (5 + 5 + 3) + (5 + 5 + 8) + (5 + 1 + 2));
    CHECK(r[0] == LOG_REC_CAN && u32(r + 1) == 1010 && u32(r + 5) == 0x123 && r[9] == 3 && r[12] == 3);
    r += 13;
    CHECK(r[0] == LOG_REC_CAN && u32(r + 5) == 0x18DAF110 && r[9] == (8 | 0x40 | 0x80));
    r += 18;
    CHECK(r[0] == LOG_REC_MARK && r[5] == 2 && !memcmp(r + 6, "hi", 2));
    logger_poll(3400, 7);
    CHECK(syncs == 2);                                  // synced within the sync period

    printf("frames the CAN driver dropped are reported in place\n");
    size_t before = card_len;
    logger_poll(3500, 10);                              // driver counter went 7 -> 10
    logger_can(&f, 3501, false);
    logger_poll(3800, 10);
    r = card + before;
    CHECK(r[0] == LOG_REC_LOST && u32(r + 5) == 3 && r[9] == LOG_REC_CAN);
    logger_get_stats(&s);
    CHECK(s.lost == 3 && s.records == 3);

    printf("a full buffer loses frames but says how many\n");
    for (int i = 0; i < 1000; i++)
        logger_can(&f, 4000, false);                    // 13 bytes each, buffer is 8 kB
    logger_get_stats(&s);
    uint32_t lost_now = s.lost - 3;
    CHECK(lost_now > 0 && s.records + lost_now == 3 + 1000);
    logger_poll(4300, 10);                              // drains
    logger_can(&f, 4301, false);
    logger_poll(4600, 10);
    CHECK(card[card_len - 13 - 9] == LOG_REC_LOST && u32(card + card_len - 13 - 9 + 5) == lost_now);

    printf("stop writes everything out; a failing card stops logging\n");
    logger_can(&f, 5000, false);
    logger_stop();
    CHECK(!logger_active() && card[card_len - 13] == LOG_REC_CAN);
    CHECK(logger_start(6000, 500000, 0));
    card_fail_after = writes;
    logger_can(&f, 6001, false);
    logger_poll(6400, 0);
    CHECK(!logger_active());
    card_fail_after = -1;
    CHECK(logger_start(7000, 500000, 0) && files == 3);

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
