// Simulated CAN bus with a handful of simulated VW control modules.
// Implements the can_port.h interface, so the application core runs against it
// unchanged. The ECUs use the same ISO-TP code as the firmware.
//
// Part numbers and software versions are real values reported by Golf MK7 / GTI
// cars, taken from comma.ai's opendbc firmware fingerprints (see
// docs/vw_can_data_sources.md). Everything else here (VIN, fault codes, live
// values, the remaining identification items) is invented for testing.

#include <stdio.h>
#include <string.h>

#include "../src/can_port.h"
#include "../src/isotp.h"
#include "../src/platform.h"

#define QUEUE_SIZE 512
#define VIN "WVWZZZAUZJW123456"

struct did_entry {
    uint16_t did;
    const char *text;
};

struct sim_ecu {
    const char *name;
    uint32_t req, resp;
    const struct did_entry *dids;
    uint8_t dtcs[8][4];
    int dtc_count;
    bool obd;            // answers OBD services 01 and 09
    struct isotp_link tp;
    // deferred answer used to exercise "response pending"
    bool pending;
    uint32_t pending_at;
    int pending_stage;
};

static const struct did_entry engine_dids[] = {
    { 0xF190, VIN },
    { 0xF187, "5G0906259L " },   // opendbc: Golf MK7 engine
    { 0xF189, "0002" },
    { 0xF191, "5G0907115D " },
    { 0xF1A3, "H13" },
    { 0xF197, "R4 2.0l TFSI " },
    { 0xF18C, "0000000042" },
    { 0xF19E, "EV_ECM20TFS0205G0906259L" },
    { 0xF1A2, "003012" },
    { 0, NULL },
};
static const struct did_entry gateway_dids[] = {
    { 0xF190, VIN }, { 0xF187, "5Q0907530AK" }, { 0xF189, "2440" }, { 0xF197, "GW MQB High  " },
    { 0xF19E, "EV_GatewNF" }, { 0xF1A2, "016027" }, { 0, NULL },
};
static const struct did_entry cluster_dids[] = {
    { 0xF187, "5G1920791B " }, { 0xF189, "2032" }, { 0xF197, "KOMBI        " }, { 0, NULL },
};
static const struct did_entry trans_dids[] = {
    { 0xF187, "0D9300012  " }, { 0xF189, "4904" }, { 0xF197, "GSG DSG AG6  " }, { 0, NULL },   // opendbc
};
static const struct did_entry abs_dids[] = {
    { 0xF187, "5Q0614517BK" }, { 0xF189, "0612" }, { 0xF197, "ESC          " }, { 0, NULL },
};
static const struct did_entry steer_dids[] = {
    { 0xF187, "5Q0909144AB" }, { 0xF189, "1082" }, { 0xF182, "\x05" "21A00642A1" },   // opendbc
    { 0xF197, "EPS_MQB_ZFLS " }, { 0, NULL },
};
static const struct did_entry airbag_dids[] = {
    { 0xF187, "5Q0959655AA" }, { 0xF189, "0386" },                                    // opendbc
    { 0xF182, "\x11" "1413001113120043114317121C111C9113" }, { 0xF197, "Airbag VW20  " }, { 0, NULL },
};

// The radar (757) and camera (74F) are absent, as on a car without driver
// assistance, so a scan shows gaps.
static struct sim_ecu ecus[] = {
    { .name = "engine", .req = 0x7E0, .resp = 0x7E8, .dids = engine_dids, .obd = true,
      .dtcs = { { 0x02, 0x99, 0x00, 0x09 }, { 0x13, 0x0A, 0x00, 0x28 } }, .dtc_count = 2 },
    { .name = "trans", .req = 0x7E1, .resp = 0x7E9, .dids = trans_dids },
    { .name = "abs", .req = 0x713, .resp = 0x77D, .dids = abs_dids,
      .dtcs = { { 0xC1, 0x21, 0x00, 0x08 } }, .dtc_count = 1 },
    { .name = "cluster", .req = 0x714, .resp = 0x77E, .dids = cluster_dids },
    { .name = "airbag", .req = 0x715, .resp = 0x77F, .dids = airbag_dids },
    { .name = "gateway", .req = 0x710, .resp = 0x77A, .dids = gateway_dids },
    { .name = "steer", .req = 0x712, .resp = 0x77C, .dids = steer_dids },
};
#define ECU_COUNT (sizeof(ecus) / sizeof(ecus[0]))

static struct can_frame queue[QUEUE_SIZE];
static unsigned q_head, q_tail;
static bool silent = true, started;
static struct can_stats stats;
static uint32_t next_broadcast;

static uint32_t now_fn(void *ctx)
{
    (void)ctx;
    return plat_now_ms();
}

static int ecu_send(void *ctx, const struct can_frame *f)
{
    (void)ctx;
    if (q_head - q_tail >= QUEUE_SIZE) {
        stats.rx_dropped++;
        return 0;
    }
    queue[q_head++ % QUEUE_SIZE] = *f;
    return 0;
}

static void reply(struct sim_ecu *e, const uint8_t *data, size_t len)
{
    isotp_send(&e->tp, data, len);
}

static void negative(struct sim_ecu *e, uint8_t sid, uint8_t nrc)
{
    uint8_t r[3] = { 0x7F, sid, nrc };
    reply(e, r, 3);
}

// --- invented live values, slowly varying so a live view has something to show
static unsigned rpm(void) { return 780 + (plat_now_ms() / 40) % 60; }
static unsigned speed_kmh(void) { return (plat_now_ms() / 1000) % 4; }
static unsigned supply_mv(void) { return 14100 + (plat_now_ms() / 200) % 100; }

static int obd_pid(uint8_t pid, uint8_t *out)
{
    switch (pid) {
    case 0x00: out[0] = 0x18; out[1] = 0x3B; out[2] = 0x80; out[3] = 0x03; return 4; // 04 05 0B 0C 0D 0F 10 11 1F 20
    case 0x20: out[0] = 0x00; out[1] = 0x02; out[2] = 0x00; out[3] = 0x01; return 4; // 2F 40
    case 0x40: out[0] = 0x44; out[1] = 0x00; out[2] = 0x00; out[3] = 0x10; return 4; // 42 46 5C
    case 0x04: out[0] = 46; return 1;                       // load 18 %
    case 0x05: out[0] = 90 + 40; return 1;                  // coolant 90 C
    case 0x0B: out[0] = 34; return 1;                       // MAP 34 kPa
    case 0x0C: { unsigned v = rpm() * 4; out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2; }
    case 0x0D: out[0] = (uint8_t)speed_kmh(); return 1;
    case 0x0F: out[0] = 28 + 40; return 1;                  // intake air 28 C
    case 0x10: out[0] = 0x01; out[1] = 0x18; return 2;      // MAF 2.80 g/s
    case 0x11: out[0] = 38; return 1;                       // throttle 14.9 %
    case 0x1F: { unsigned v = plat_now_ms() / 1000; out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2; }
    case 0x2F: out[0] = 153; return 1;                      // fuel 60 %
    case 0x42: { unsigned v = supply_mv(); out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2; }
    case 0x46: out[0] = 17 + 40; return 1;                  // ambient 17 C
    case 0x5C: out[0] = 96 + 40; return 1;                  // oil 96 C
    default: return -1;
    }
}

static void handle(struct sim_ecu *e, const uint8_t *req, size_t len)
{
    static uint8_t r[256];
    uint8_t sid = req[0];

    if (sid == 0x3E && len == 2) {
        if (!(req[1] & 0x80)) {
            r[0] = 0x7E; r[1] = 0x00;
            reply(e, r, 2);
        }
    } else if (sid == 0x10 && len == 2) {
        if (req[1] != 0x01 && req[1] != 0x03) {
            negative(e, sid, 0x12);
            return;
        }
        r[0] = 0x50; r[1] = req[1]; r[2] = 0x00; r[3] = 0x32; r[4] = 0x01; r[5] = 0xF4;
        reply(e, r, 6);
    } else if (sid == 0x22 && len > 3 && len % 2 == 1) {
        // several identifiers in one request: answers are concatenated
        size_t n = 1;
        r[0] = 0x62;
        for (size_t i = 1; i + 1 < len; i += 2) {
            uint16_t did = (uint16_t)((req[i] << 8) | req[i + 1]);
            const struct did_entry *d = e->dids;
            while (d->text && d->did != did)
                d++;
            if (!d->text)
                continue; // identifiers the module does not have are left out
            r[n++] = req[i];
            r[n++] = req[i + 1];
            memcpy(&r[n], d->text, strlen(d->text));
            n += strlen(d->text);
        }
        if (n == 1)
            negative(e, sid, 0x31);
        else
            reply(e, r, n);
    } else if (sid == 0x22 && len == 3) {
        uint16_t did = (uint16_t)((req[1] << 8) | req[2]);
        r[0] = 0x62; r[1] = req[1]; r[2] = req[2];
        if (e->obd && did == 0xF17C) {
            // slow DID: answers "response pending" first
            e->pending = true;
            e->pending_stage = 0;
            e->pending_at = plat_now_ms() + 20;
            return;
        }
        if (e->obd && (did & 0xFF00) == 0xF400) {
            int n = obd_pid((uint8_t)did, &r[3]); // OBD PIDs mirrored as DIDs F4xx
            if (n > 0) {
                reply(e, r, (size_t)n + 3);
                return;
            }
        }
        for (const struct did_entry *d = e->dids; d->text; d++) {
            if (d->did == did) {
                size_t n = strlen(d->text);
                memcpy(&r[3], d->text, n);
                reply(e, r, n + 3);
                return;
            }
        }
        negative(e, sid, 0x31);
    } else if (sid == 0x19 && len == 3 && req[1] == 0x02) {
        size_t n = 3;
        r[0] = 0x59; r[1] = 0x02; r[2] = 0xFF;
        for (int i = 0; i < e->dtc_count; i++) {
            if (e->dtcs[i][3] & req[2]) {
                memcpy(&r[n], e->dtcs[i], 4);
                n += 4;
            }
        }
        reply(e, r, n);
    } else if (sid == 0x19) {
        negative(e, sid, 0x12);
    } else if (sid == 0x14 && len == 4) {
        e->dtc_count = 0;
        r[0] = 0x54;
        reply(e, r, 1);
    } else if (sid == 0x01 && e->obd && len == 2) {
        int n = obd_pid(req[1], &r[2]);
        if (n < 0) {
            negative(e, sid, 0x12);
            return;
        }
        r[0] = 0x41; r[1] = req[1];
        reply(e, r, (size_t)n + 2);
    } else if (sid == 0x09 && e->obd && len == 2 && req[1] == 0x02) {
        r[0] = 0x49; r[1] = 0x02; r[2] = 0x01;
        memcpy(&r[3], VIN, 17);
        reply(e, r, 20);
    } else if (sid == 0x27) {
        negative(e, sid, 0x33);
    } else if (sid == 0x2E || sid == 0x31 || sid == 0x11) {
        negative(e, sid, 0x7F); // not in the default session
    } else {
        negative(e, sid, 0x11);
    }
}

// Write `len` bits of `value` at DBC little-endian start bit `start`.
static void put_bits(uint8_t *data, int start, int len, uint32_t value)
{
    for (int i = 0; i < len; i++) {
        int bit = start + i;
        if (value & (1u << i))
            data[bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
}

static void broadcast(void)
{
    static unsigned tick;
    struct can_frame f;

    // Klemmen_Status_01 (0x3C0): ignition on
    memset(&f, 0, sizeof(f)); f.id = 0x3C0; f.dlc = 4;
    put_bits(f.data, 8, 4, tick & 0xF);
    put_bits(f.data, 16, 1, 1);   // ZAS_Kl_S
    put_bits(f.data, 17, 1, 1);   // ZAS_Kl_15
    ecu_send(NULL, &f);

    // ESP_21 (0x0FD): vehicle speed, 0.01 km/h per bit
    memset(&f, 0, sizeof(f)); f.id = 0x0FD; f.dlc = 8;
    put_bits(f.data, 32, 16, speed_kmh() * 100);
    ecu_send(NULL, &f);

    if (tick % 10 == 0) {
        // Motor_07 (0x640): intake 27 C, oil 96 C, coolant 90 C
        memset(&f, 0, sizeof(f)); f.id = 0x640; f.dlc = 8;
        put_bits(f.data, 8, 8, 100);    // (27 + 48) / 0.75
        put_bits(f.data, 16, 8, 156);   // 96 + 60
        put_bits(f.data, 24, 8, 184);   // (90 + 48) / 0.75
        ecu_send(NULL, &f);

        // Motor_Code_01 (0x641): 4 cylinders, 2.0 l, 169 kW
        memset(&f, 0, sizeof(f)); f.id = 0x641; f.dlc = 8;
        put_bits(f.data, 32, 4, 4);
        put_bits(f.data, 40, 7, 20);
        put_bits(f.data, 48, 9, 169);
        ecu_send(NULL, &f);

        // Diagnose_01 (0x6B2): odometer 48213 km, 2026-11-15 14:30:05
        memset(&f, 0, sizeof(f)); f.id = 0x6B2; f.dlc = 8;
        put_bits(f.data, 8, 20, 48213);
        put_bits(f.data, 28, 7, 26);
        put_bits(f.data, 35, 4, 11);
        put_bits(f.data, 39, 5, 15);
        put_bits(f.data, 44, 5, 14);
        put_bits(f.data, 49, 6, 30);
        put_bits(f.data, 55, 6, 5);
        ecu_send(NULL, &f);
    }
    tick++;
}

static void step(void)
{
    static uint8_t buf[ISOTP_MAX_PAYLOAD];
    uint32_t now = plat_now_ms();

    if (!started) {
        for (size_t i = 0; i < ECU_COUNT; i++) {
            isotp_init(&ecus[i].tp, ecus[i].resp, ecus[i].req, false, ecu_send, now_fn, NULL);
            ecus[i].tp.pad_byte = 0xAA;
        }
        next_broadcast = now + 100;
        started = true;
    }

    for (size_t i = 0; i < ECU_COUNT; i++) {
        struct sim_ecu *e = &ecus[i];
        size_t n;
        isotp_poll(&e->tp);
        (void)isotp_take_error(&e->tp);
        if (isotp_recv(&e->tp, buf, sizeof(buf), &n) == 1)
            handle(e, buf, n);
        if (e->pending && (int32_t)(now - e->pending_at) >= 0 && !isotp_tx_busy(&e->tp)) {
            if (e->pending_stage < 2) {
                negative(e, 0x22, 0x78);
                e->pending_stage++;
                e->pending_at = now + 150;
            } else {
                static const uint8_t done[] = { 0x62, 0xF1, 0x7C, 'T', 'S', 'T', '-', '0', '1', '.', '0', '1', '.', '2', '0', '1', '8' };
                reply(e, done, sizeof(done));
                e->pending = false;
            }
        }
    }

    // Background traffic laid out as in the public VW MQB message database, so
    // the decoder has realistic frames to work on.
    if ((int32_t)(now - next_broadcast) >= 0) {
        broadcast();
        next_broadcast = now + 100;
    }
}

void can_port_init(uint32_t bitrate)
{
    (void)bitrate;
}

void can_port_set_silent(bool on)
{
    silent = on;
}

bool can_port_is_silent(void)
{
    return silent;
}

bool can_port_receive(struct can_frame *out)
{
    step();
    if (q_tail == q_head)
        return false;
    *out = queue[q_tail++ % QUEUE_SIZE];
    stats.rx++;
    return true;
}

int can_port_send(const struct can_frame *f)
{
    if (silent)
        return -1;
    step();
    stats.tx++;
    for (size_t i = 0; i < ECU_COUNT; i++) {
        struct can_frame g = *f;
        if (f->id == 0x7DF && ecus[i].obd)
            g.id = ecus[i].req; // functional OBD request reaches the engine ECU
        isotp_on_frame(&ecus[i].tp, &g);
    }
    return 0;
}

void can_port_get_stats(struct can_stats *out)
{
    *out = stats;
}
