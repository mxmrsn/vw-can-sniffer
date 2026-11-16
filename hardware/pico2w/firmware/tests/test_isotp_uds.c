// Host-side tests for the ISO-TP and UDS layers. No hardware needed:
// a simulated ECU sits on a simulated bus with a fake millisecond clock.
//
//   make -C hardware/pico2w/firmware/tests

#include <stdio.h>
#include <string.h>

#include "../src/isotp.h"
#include "../src/uds.h"

#define TESTER_ID 0x7E0
#define ECU_ID 0x7E8

static int failures, checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------- bus model
struct queue {
    struct can_frame f[1024];
    int n;
};

static uint32_t clk;
static struct queue to_ecu, to_tester;
#define LOG_MAX 4096
static struct can_frame tester_log[LOG_MAX], ecu_log[LOG_MAX];
static uint32_t tester_log_time[LOG_MAX];
static int tester_log_n, ecu_log_n;
static int tester_send_failures; // make the next N tester sends fail (queue full)

static struct isotp_link tester, ecu;
static struct uds_client client;

static uint32_t now_fn(void *ctx) { (void)ctx; return clk; }

static int tester_send(void *ctx, const struct can_frame *f)
{
    (void)ctx;
    if (tester_send_failures > 0) {
        tester_send_failures--;
        return -1;
    }
    to_ecu.f[to_ecu.n++] = *f;
    if (tester_log_n < LOG_MAX) {
        tester_log_time[tester_log_n] = clk;
        tester_log[tester_log_n++] = *f;
    }
    return 0;
}

static int ecu_send(void *ctx, const struct can_frame *f)
{
    (void)ctx;
    to_tester.f[to_tester.n++] = *f;
    if (ecu_log_n < LOG_MAX)
        ecu_log[ecu_log_n++] = *f;
    return 0;
}

static void ecu_raw_send(const uint8_t *bytes, int n)
{
    struct can_frame f = { .id = ECU_ID, .ext = false, .dlc = 8 };
    memset(f.data, 0xAA, 8);
    memcpy(f.data, bytes, (size_t)n);
    ecu_send(NULL, &f);
}

// ---------------------------------------------------------------- ECU model
// Either a well-behaved ECU built on the same ISO-TP code (handler != NULL),
// or a hand-rolled "raw" ECU used to check exact frames and error handling.
static void (*ecu_handler)(const uint8_t *req, size_t len);
static void (*ecu_raw)(const struct can_frame *f);
static void (*ecu_tick)(void);

static void pump(void *ctx)
{
    (void)ctx;
    struct queue q = to_ecu;
    to_ecu.n = 0;
    for (int i = 0; i < q.n; i++) {
        if (ecu_raw)
            ecu_raw(&q.f[i]);
        else
            isotp_on_frame(&ecu, &q.f[i]);
    }
    q = to_tester;
    to_tester.n = 0;
    for (int i = 0; i < q.n; i++)
        isotp_on_frame(&tester, &q.f[i]);

    if (!ecu_raw) {
        static uint8_t buf[ISOTP_MAX_PAYLOAD];
        size_t n;
        isotp_poll(&ecu);
        if (isotp_recv(&ecu, buf, sizeof(buf), &n) == 1 && ecu_handler)
            ecu_handler(buf, n);
    }
    if (ecu_tick)
        ecu_tick();
    isotp_poll(&tester);
    clk++;
}

static void setup(const char *name)
{
    printf("%s\n", name);
    clk = 1000;
    to_ecu.n = to_tester.n = 0;
    tester_log_n = ecu_log_n = 0;
    tester_send_failures = 0;
    ecu_handler = NULL;
    ecu_raw = NULL;
    ecu_tick = NULL;
    isotp_init(&tester, TESTER_ID, ECU_ID, false, tester_send, now_fn, NULL);
    isotp_init(&ecu, ECU_ID, TESTER_ID, false, ecu_send, now_fn, NULL);
    ecu.pad_byte = 0xAA;
    uds_init(&client, &tester, pump, NULL);
}

// ---------------------------------------------------------------- handlers
static const char VIN[] = "WVWZZZAUZJW000001";

static void h_basic(const uint8_t *req, size_t len)
{
    uint8_t r[64];
    if (len == 2 && req[0] == 0x3E) {
        r[0] = 0x7E; r[1] = req[1];
        isotp_send(&ecu, r, 2);
    } else if (len == 2 && req[0] == 0x10) {
        uint8_t s[6] = { 0x50, req[1], 0x00, 0x32, 0x01, 0xF4 };
        isotp_send(&ecu, s, 6);
    } else if (len == 3 && req[0] == 0x22 && req[1] == 0xF1 && req[2] == 0x90) {
        r[0] = 0x62; r[1] = 0xF1; r[2] = 0x90;
        memcpy(&r[3], VIN, 17);
        isotp_send(&ecu, r, 20);
    } else if (len == 3 && req[0] == 0x22) {
        uint8_t nr[3] = { 0x7F, 0x22, 0x31 };
        isotp_send(&ecu, nr, 3);
    } else if (len == 3 && req[0] == 0x19 && req[1] == 0x02) {
        uint8_t d[11] = { 0x59, 0x02, 0xFF, 0x01, 0x23, 0x45, 0x09, 0x0A, 0xBC, 0xDE, 0x28 };
        isotp_send(&ecu, d, 11);
    }
}

static uint8_t big[ISOTP_MAX_PAYLOAD];
static void h_big(const uint8_t *req, size_t len)
{
    (void)len;
    big[0] = (uint8_t)(req[0] + 0x40);
    big[1] = req[1];
    big[2] = req[2];
    for (int i = 3; i < ISOTP_MAX_PAYLOAD; i++)
        big[i] = (uint8_t)(i * 7 + 3);
    isotp_send(&ecu, big, ISOTP_MAX_PAYLOAD);
}

// "response pending" twice, 1500 ms apart, then the real answer
static uint32_t pending_start;
static int pending_stage;
static void h_pending(const uint8_t *req, size_t len)
{
    (void)req; (void)len;
    pending_start = clk;
    pending_stage = 1;
}
static void tick_pending(void)
{
    static const uint8_t np[3] = { 0x7F, 0x22, 0x78 };
    static const uint8_t ok[5] = { 0x62, 0x12, 0x34, 0xAB, 0xCD };
    if (pending_stage == 1 && clk - pending_start >= 20) {
        isotp_send(&ecu, np, 3); pending_stage = 2;
    } else if (pending_stage == 2 && clk - pending_start >= 1500) {
        isotp_send(&ecu, np, 3); pending_stage = 3;
    } else if (pending_stage == 3 && clk - pending_start >= 3000) {
        isotp_send(&ecu, ok, 5); pending_stage = 0;
    }
}

// Raw ECU that receives a long request with block size 2 and STmin 5 ms.
static uint8_t raw_rx[128];
static int raw_rx_len, raw_rx_off, raw_cf_in_block, raw_fc_sent;
static void raw_long_request(const struct can_frame *f)
{
    static const uint8_t fc[3] = { 0x30, 0x02, 0x05 };
    if (f->id != TESTER_ID)
        return;
    if ((f->data[0] >> 4) == 1) {
        raw_rx_len = ((f->data[0] & 0x0F) << 8) | f->data[1];
        memcpy(raw_rx, &f->data[2], 6);
        raw_rx_off = 6;
        raw_cf_in_block = 0;
        ecu_raw_send(fc, 3); raw_fc_sent++;
    } else if ((f->data[0] >> 4) == 2) {
        int n = raw_rx_len - raw_rx_off;
        if (n > 7) n = 7;
        memcpy(&raw_rx[raw_rx_off], &f->data[1], (size_t)n);
        raw_rx_off += n;
        if (raw_rx_off >= raw_rx_len) {
            static const uint8_t ok[3] = { 0x02, 0x6E, 0x00 }; // single frame: 6E 00
            ecu_raw_send(ok, 3);
        } else if (++raw_cf_in_block == 2) {
            raw_cf_in_block = 0;
            ecu_raw_send(fc, 3); raw_fc_sent++;
        }
    }
}

static void raw_bad_sequence(const struct can_frame *f)
{
    if ((f->data[0] >> 4) == 0) { // request: answer with FF then a CF numbered 2
        static const uint8_t ff[8] = { 0x10, 0x14, 0x62, 0xF1, 0x90, 'A', 'B', 'C' };
        ecu_raw_send(ff, 8);
    } else if ((f->data[0] >> 4) == 3) {
        static const uint8_t cf[8] = { 0x22, 1, 2, 3, 4, 5, 6, 7 };
        ecu_raw_send(cf, 8);
    }
}

static void raw_ff_only(const struct can_frame *f)
{
    if ((f->data[0] >> 4) == 0) {
        static const uint8_t ff[8] = { 0x10, 0x14, 0x62, 0xF1, 0x90, 'A', 'B', 'C' };
        ecu_raw_send(ff, 8);
    }
}

static void raw_fc_overflow(const struct can_frame *f)
{
    if ((f->data[0] >> 4) == 1) {
        static const uint8_t fc[3] = { 0x32, 0x00, 0x00 };
        ecu_raw_send(fc, 3);
    }
}

// ---------------------------------------------------------------- tests
static void test_single_frame(void)
{
    setup("single-frame request and response (TesterPresent)");
    ecu_handler = h_basic;
    CHECK(uds_tester_present(&client) == UDS_OK);
    static const uint8_t want[8] = { 0x02, 0x3E, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55 };
    CHECK(tester_log_n == 1);
    CHECK(tester_log[0].id == TESTER_ID && !tester_log[0].ext && tester_log[0].dlc == 8);
    CHECK(memcmp(tester_log[0].data, want, 8) == 0);
    CHECK(uds_session_control(&client, 0x03) == UDS_OK);
}

static void test_multi_frame_response(void)
{
    setup("multi-frame response (read VIN)");
    ecu_handler = h_basic;
    uint8_t out[32];
    size_t n = 0;
    CHECK(uds_read_did(&client, 0xF190, out, sizeof(out), &n) == UDS_OK);
    CHECK(n == 17 && memcmp(out, VIN, 17) == 0);
    // tester: request, then one flow control "continue, no limit, no delay"
    static const uint8_t fc[8] = { 0x30, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55 };
    CHECK(tester_log_n == 2 && memcmp(tester_log[1].data, fc, 8) == 0);
    // ECU: first frame 0x014 bytes, then two consecutive frames numbered 1 and 2
    CHECK(ecu_log_n == 3);
    CHECK(ecu_log[0].data[0] == 0x10 && ecu_log[0].data[1] == 0x14 && ecu_log[0].data[2] == 0x62);
    CHECK(ecu_log[1].data[0] == 0x21 && ecu_log[2].data[0] == 0x22);
}

static void test_receiver_paces_sender(void)
{
    setup("receiver asks for block size 1 and STmin 4 ms");
    ecu_handler = h_basic;
    tester.rx_bs = 1;
    tester.rx_stmin = 4;
    uint8_t out[32];
    size_t n = 0;
    CHECK(uds_read_did(&client, 0xF190, out, sizeof(out), &n) == UDS_OK);
    CHECK(n == 17 && memcmp(out, VIN, 17) == 0);
    // request, a flow control after the first frame, another after the one-frame block
    static const uint8_t fc[3] = { 0x30, 0x01, 0x04 };
    CHECK(tester_log_n == 3);
    CHECK(memcmp(tester_log[1].data, fc, 3) == 0 && memcmp(tester_log[2].data, fc, 3) == 0);
    CHECK(ecu_log_n == 3 && ecu_log[2].data[0] == 0x22);
}

static void test_multi_frame_request(void)
{
    setup("multi-frame request with block size 2 and STmin 5 ms");
    ecu_raw = raw_long_request;
    raw_fc_sent = 0;
    uint8_t req[34], resp[8];
    size_t n = 0;
    req[0] = 0x2E; // content is irrelevant to the transport; never sent to a real ECU here
    for (int i = 1; i < 34; i++)
        req[i] = (uint8_t)(0x40 + i);
    CHECK(uds_request(&client, req, sizeof(req), resp, sizeof(resp), &n) == UDS_OK);
    CHECK(n == 2 && resp[0] == 0x6E);
    CHECK(raw_rx_len == 34 && memcmp(raw_rx, req, 34) == 0);
    CHECK(raw_fc_sent == 2);
    // FF + 4 consecutive frames numbered 1..4
    CHECK(tester_log_n == 5);
    CHECK(tester_log[0].data[0] == 0x10 && tester_log[0].data[1] == 34);
    for (int i = 1; i <= 4 && i < tester_log_n; i++)
        CHECK(tester_log[i].data[0] == (0x20 | i));
    // within a block the two frames are at least STmin apart
    CHECK(tester_log_time[2] - tester_log_time[1] >= 5);
    CHECK(tester_log_time[4] - tester_log_time[3] >= 5);
    // last frame carries 7 payload bytes here, so no padding is visible; check FF dlc
    CHECK(tester_log[0].dlc == 8);
}

static void test_negative_response(void)
{
    setup("negative response");
    ecu_handler = h_basic;
    uint8_t out[8];
    size_t n = 0;
    CHECK(uds_read_did(&client, 0x1234, out, sizeof(out), &n) == UDS_ERR_NEGATIVE);
    CHECK(client.last_nrc == 0x31);
}

static void test_response_pending(void)
{
    setup("response pending (0x78) extends the timeout");
    ecu_handler = h_pending;
    ecu_tick = tick_pending;
    pending_stage = 0;
    uint8_t out[8];
    size_t n = 0;
    uint32_t t0 = clk;
    CHECK(uds_read_did(&client, 0x1234, out, sizeof(out), &n) == UDS_OK);
    CHECK(n == 2 && out[0] == 0xAB && out[1] == 0xCD);
    CHECK(clk - t0 >= 3000); // well past the plain 1000 ms timeout
}

static void test_timeout(void)
{
    setup("no response times out");
    uint8_t out[8];
    size_t n = 0;
    uint32_t t0 = clk;
    CHECK(uds_read_did(&client, 0xF190, out, sizeof(out), &n) == UDS_ERR_TIMEOUT);
    CHECK(clk - t0 >= 1000 && clk - t0 < 1100);
}

static void test_bad_sequence(void)
{
    setup("consecutive frame out of sequence");
    ecu_raw = raw_bad_sequence;
    uint8_t out[32];
    size_t n = 0;
    CHECK(uds_read_did(&client, 0xF190, out, sizeof(out), &n) == UDS_ERR_TRANSPORT);
    CHECK(client.last_tp_error == ISOTP_ERR_SEQ);
}

static void test_cf_timeout(void)
{
    setup("peer stops after first frame");
    ecu_raw = raw_ff_only;
    uint8_t out[32];
    size_t n = 0;
    CHECK(uds_read_did(&client, 0xF190, out, sizeof(out), &n) == UDS_ERR_TRANSPORT);
    CHECK(client.last_tp_error == ISOTP_ERR_TIMEOUT_CF);
}

static void test_fc_overflow_and_missing_fc(void)
{
    setup("flow control: overflow, then none at all");
    uint8_t req[20] = { 0x2E }, resp[8];
    size_t n = 0;
    ecu_raw = raw_fc_overflow;
    CHECK(uds_request(&client, req, sizeof(req), resp, sizeof(resp), &n) == UDS_ERR_TRANSPORT);
    CHECK(client.last_tp_error == ISOTP_ERR_OVERFLOW);
    ecu_raw = raw_ff_only; // ignores first frames, so no flow control comes back
    CHECK(uds_request(&client, req, sizeof(req), resp, sizeof(resp), &n) == UDS_ERR_TRANSPORT);
    CHECK(client.last_tp_error == ISOTP_ERR_TIMEOUT_FC);
}

static void test_dtcs(void)
{
    setup("read DTCs");
    ecu_handler = h_basic;
    uint8_t out[64], mask = 0;
    size_t n = 0;
    CHECK(uds_read_dtcs(&client, 0xFF, &mask, out, sizeof(out), &n) == UDS_OK);
    static const uint8_t want[8] = { 0x01, 0x23, 0x45, 0x09, 0x0A, 0xBC, 0xDE, 0x28 };
    CHECK(mask == 0xFF && n == 8 && memcmp(out, want, 8) == 0);
}

static void test_max_length(void)
{
    setup("4095-byte response (sequence number wraps many times)");
    ecu_handler = h_big;
    static uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t n = 0;
    CHECK(uds_read_did(&client, 0x0102, out, sizeof(out), &n) == UDS_OK);
    CHECK(n == ISOTP_MAX_PAYLOAD - 3 && memcmp(out, &big[3], n) == 0);
    uint8_t small[16];
    CHECK(uds_read_did(&client, 0x0102, small, sizeof(small), &n) == UDS_ERR_OVERFLOW);
}

static void test_ignores_other_traffic(void)
{
    setup("unrelated frames and stale responses are ignored");
    ecu_handler = h_basic;
    struct can_frame other = { .id = 0x123, .dlc = 8, .data = { 0x02, 0x7E, 0x00 } };
    struct can_frame ext = { .id = ECU_ID, .ext = true, .dlc = 8, .data = { 0x02, 0x7E, 0x00 } };
    struct can_frame stale = { .id = ECU_ID, .dlc = 8, .data = { 0x03, 0x62, 0x00, 0x01 } };
    to_tester.f[to_tester.n++] = other;
    to_tester.f[to_tester.n++] = ext;
    to_tester.f[to_tester.n++] = stale; // answer to a different service
    CHECK(uds_tester_present(&client) == UDS_OK);
}

static void test_send_queue_full(void)
{
    setup("driver queue full during consecutive frames is retried");
    ecu_raw = raw_long_request;
    raw_fc_sent = 0;
    uint8_t req[34], resp[8];
    size_t n = 0;
    for (int i = 0; i < 34; i++)
        req[i] = (uint8_t)(0x2E + i);
    // the first frame goes out, then three sends fail before frames flow again
    CHECK(isotp_send(&tester, req, sizeof(req)) == ISOTP_OK);
    tester_send_failures = 3;
    int got = 0;
    for (int i = 0; i < 500 && got != 1; i++) {
        pump(NULL);
        got = isotp_recv(&tester, resp, sizeof(resp), &n);
    }
    CHECK(got == 1 && isotp_take_error(&tester) == 0);
    CHECK(raw_rx_len == 34 && memcmp(raw_rx, req, 34) == 0);
    CHECK(tester_send_failures == 0);
}

static void test_busy_and_limits(void)
{
    setup("argument and busy checks");
    uint8_t req[20] = { 0 };
    CHECK(isotp_send(&tester, req, 0) == ISOTP_ERR_TOO_LONG);
    CHECK(isotp_send(&tester, req, ISOTP_MAX_PAYLOAD + 1) == ISOTP_ERR_TOO_LONG);
    CHECK(isotp_send(&tester, req, sizeof(req)) == ISOTP_OK);
    CHECK(isotp_tx_busy(&tester));
    CHECK(isotp_send(&tester, req, 2) == ISOTP_ERR_BUSY);
}

int main(void)
{
    test_single_frame();
    test_multi_frame_response();
    test_multi_frame_request();
    test_receiver_paces_sender();
    test_negative_response();
    test_response_pending();
    test_timeout();
    test_bad_sequence();
    test_cf_timeout();
    test_fc_overflow_and_missing_fc();
    test_dtcs();
    test_max_length();
    test_ignores_other_traffic();
    test_send_queue_full();
    test_busy_and_limits();
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
