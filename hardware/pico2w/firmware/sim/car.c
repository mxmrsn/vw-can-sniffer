// The simulated car: VW control modules behind a gateway, an engine model and the
// broadcast traffic, independent of any CAN hardware. The host simulator (sim_bus.c)
// puts it on a pretend bus with fault injection; the bench car
// (hardware/xiao-esp32s3/bench-car) puts it on a real bus through the ESP32-S3's CAN
// controller, so the interface board can be tested against it on the desk.
//
// By default it is pessimistic in the ways a real car is (see sim_config.c): modules
// take 5 to 50 ms to answer and ask for flow control with a block size, an extended
// session drops back after 5 s without a request, the engine and the transmission both
// answer a functional OBD request, broadcast traffic stays behind the gateway, and the
// ignition state decides who answers. Live values come from sim_engine.c.
//
// Part numbers and software versions are real values reported by Golf MK7 / GTI
// cars, taken from comma.ai's opendbc firmware fingerprints (see
// docs/vw_can_data_sources.md). The engine code is this car's (DKFA, EA888 Gen 3).
// Everything else (VIN, fault codes, the remaining identification items) is
// invented for testing.

#include <stdio.h>
#include <string.h>

#include "../src/isotp.h"
#include "../src/platform.h"
#include "sim.h"

#define VIN "WVWZZZAUZJW123456"

struct did_entry {
    uint16_t did;
    const char *text;
};

enum kind { K_ENGINE, K_TRANS, K_GATEWAY, K_OTHER };

struct sim_ecu {
    const char *name;
    enum kind kind;
    uint32_t req, resp;
    const struct did_entry *dids;
    uint8_t dtcs[4][4];          // modules other than the engine: fixed fault memory
    int dtc_count, dtc_initial;
    struct isotp_link tp;

    uint8_t session;             // 1 default, 3 extended
    uint32_t last_request;
    bool functional;             // the message being handled came in on 7DF
    // the answer being prepared, sent once the module's latency has passed
    uint8_t out[256];
    size_t out_len;
    bool out_ready;
    uint32_t out_due;
    // a slow identifier: "response pending" twice, then the answer
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
    { 0xF1AD, "DKFA" },          // engine code letters: this car's
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
    { .name = "engine", .kind = K_ENGINE, .req = 0x7E0, .resp = 0x7E8, .dids = engine_dids },
    { .name = "trans", .kind = K_TRANS, .req = 0x7E1, .resp = 0x7E9, .dids = trans_dids },
    { .name = "abs", .kind = K_OTHER, .req = 0x713, .resp = 0x77D, .dids = abs_dids,
      .dtcs = { { 0xC1, 0x21, 0x00, 0x08 } }, .dtc_initial = 1 },
    { .name = "cluster", .kind = K_OTHER, .req = 0x714, .resp = 0x77E, .dids = cluster_dids },
    { .name = "airbag", .kind = K_OTHER, .req = 0x715, .resp = 0x77F, .dids = airbag_dids },
    { .name = "gateway", .kind = K_GATEWAY, .req = 0x710, .resp = 0x77A, .dids = gateway_dids },
    { .name = "steer", .kind = K_OTHER, .req = 0x712, .resp = 0x77C, .dids = steer_dids },
};
#define ECU_COUNT (sizeof(ecus) / sizeof(ecus[0]))

static uint32_t t0, next_broadcast;
static car_tx_fn car_tx;
static bool started;
static struct car_stats cstats;

static uint32_t now_fn(void *ctx)
{
    (void)ctx;
    return plat_now_ms();
}

static uint32_t rel(uint32_t now)
{
    return now - t0;
}

// Every frame the car sends goes out through the transport it was given.
static int ecu_send(void *ctx, const struct can_frame *f)
{
    (void)ctx;
    cstats.sent++;
    return car_tx ? car_tx(f) : 0;
}

static void reply(struct sim_ecu *e, const uint8_t *data, size_t len)
{
    if (len > sizeof(e->out))
        len = sizeof(e->out);
    memcpy(e->out, data, len);
    e->out_len = len;
    e->out_ready = true;
    e->out_due = plat_now_ms() + (uint32_t)sim_rand_range(sim_cfg.lat_min, sim_cfg.lat_max);
}

static void negative(struct sim_ecu *e, uint8_t sid, uint8_t nrc)
{
    if (e->functional)
        return;   // functional requests get no negative answers (ISO 15765-4)
    uint8_t r[3] = { 0x7F, sid, nrc };
    reply(e, r, 3);
}

// ---- OBD values from the engine model, encoded as SAE J1979 defines them
static int clampi(double v, int lo, int hi)
{
    int i = (int)(v + (v >= 0 ? 0.5 : -0.5));
    return i < lo ? lo : i > hi ? hi : i;
}

static uint8_t mil_and_count(void)
{
    int n, count = 0;
    bool mil = false;
    struct sim_dtc *d = engine_dtcs(&n);
    for (int i = 0; i < n; i++) {
        if (d[i].status & 0x08)
            count++;
        if (d[i].status & 0x80)
            mil = true;
    }
    return (uint8_t)((mil ? 0x80 : 0) | (count & 0x7F));
}

// Encode one PID from an engine state. Returns the number of data bytes, or -1.
static int encode_pid(uint8_t pid, const struct engine_state *s, uint8_t *out)
{
    int v;
    switch (pid) {
    case 0x01: out[0] = mil_and_count(); out[1] = 0x07; out[2] = 0xE5; out[3] = 0x00; return 4;
    case 0x04: out[0] = (uint8_t)clampi(s->load_pct * 255 / 100, 0, 255); return 1;
    case 0x05: out[0] = (uint8_t)clampi(s->coolant_c + 40, 0, 255); return 1;
    case 0x06: out[0] = (uint8_t)clampi(s->stft_pct * 128 / 100 + 128, 0, 255); return 1;
    case 0x07: out[0] = (uint8_t)clampi(s->ltft_pct * 128 / 100 + 128, 0, 255); return 1;
    case 0x0B: out[0] = (uint8_t)clampi(s->map_kpa, 0, 255); return 1;
    case 0x0C: v = clampi(s->rpm * 4, 0, 65535); out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2;
    case 0x0D: out[0] = (uint8_t)clampi(s->speed_kmh, 0, 255); return 1;
    case 0x0E: out[0] = (uint8_t)clampi((s->timing_deg + 64) * 2, 0, 255); return 1;
    case 0x0F: out[0] = (uint8_t)clampi(s->intake_c + 40, 0, 255); return 1;
    case 0x10: v = clampi(s->maf_gs * 100, 0, 65535); out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2;
    case 0x11: out[0] = (uint8_t)clampi((14 + s->throttle_pct * 0.86) * 255 / 100, 0, 255); return 1;
    case 0x1C: out[0] = 0x01; return 1;                     // OBD-II as defined by CARB
    case 0x1F: v = clampi(s->runtime_s, 0, 65535); out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2;
    case 0x21: out[0] = 0; out[1] = 0; return 2;             // distance with the lamp on
    case 0x2F: out[0] = 153; return 1;                      // fuel 60 %
    case 0x33: out[0] = (uint8_t)clampi(s->baro_kpa, 0, 255); return 1;
    case 0x34:                                              // wideband sensor 1: lambda and current
        v = clampi(s->lambda * 32768, 0, 65535); out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v;
        out[2] = 0x80; out[3] = 0x00; return 4;
    case 0x42: v = clampi(s->volts * 1000, 0, 65535); out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2;
    case 0x44: v = clampi(s->lambda_cmd * 32768, 0, 65535); out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2;
    case 0x46: out[0] = (uint8_t)clampi(s->ambient_c + 40, 0, 255); return 1;
    case 0x5C: out[0] = (uint8_t)clampi(s->oil_c + 40, 0, 255); return 1;
    case 0x23:                                              // direct-injection rail, gauge, 10 kPa per bit
        v = clampi((s->speed_kmh < 1 && s->throttle_pct < 1 ? 40 : 40 + s->load_pct * 1.2) * 10, 0, 65535);
        out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; return 2;
    case 0x52: out[0] = (uint8_t)clampi(10 * 2.55, 0, 255); return 1;   // ethanol 10 %: US pump fuel
    default: return -1;
    }
}

static const uint8_t ENGINE_PIDS[] = { 0x01, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
                                       0x1C, 0x1F, 0x21, 0x23, 0x2F, 0x33, 0x34, 0x42, 0x44, 0x46, 0x52, 0x5C, 0 };
static const uint8_t FREEZE_PIDS[] = { 0x02, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x1F, 0 };
static const uint8_t TRANS_PIDS[] = { 0x01, 0x0D, 0 };

// "Which PIDs from base+1 to base+0x20": bit 31 is base+1; bit 0 says the next range exists.
static void support_bits(const uint8_t *list, uint8_t base, uint8_t *out)
{
    uint32_t bits = 0;
    for (const uint8_t *p = list; *p; p++) {
        if (*p > base && *p <= base + 0x20)
            bits |= 1u << (32 - (*p - base));
        if (*p > base + 0x20)
            bits |= 1u;
    }
    out[0] = (uint8_t)(bits >> 24); out[1] = (uint8_t)(bits >> 16); out[2] = (uint8_t)(bits >> 8); out[3] = (uint8_t)bits;
}

static bool in_list(const uint8_t *list, uint8_t pid)
{
    for (const uint8_t *p = list; *p; p++)
        if (*p == pid)
            return true;
    return false;
}

static const struct sim_dtc *freeze_frame_dtc(void)
{
    int n;
    struct sim_dtc *d = engine_dtcs(&n);
    for (int i = 0; i < n; i++)
        if (d[i].has_snapshot)
            return &d[i];
    return NULL;
}

// ---- Simos 18 measuring values (service 0x22), as the VW_Flash logger reads them
// (tools/vwdiag/data/simos18_parameters_22.csv): encode the model's values the other way round.
static size_t put16(uint8_t *out, double v)
{
    int i = clampi(v, -32768, 65535);
    out[0] = (uint8_t)((unsigned)i >> 8);
    out[1] = (uint8_t)i;
    return 2;
}

static int simos_did(uint16_t did, const struct engine_state *s, uint8_t *out)
{
    bool idle = s->speed_kmh < 1 && s->throttle_pct < 1;
    int gear = s->speed_kmh < 1 ? 0 : s->speed_kmh < 15 ? 1 : s->speed_kmh < 30 ? 2 : s->speed_kmh < 45 ? 3 : s->speed_kmh < 60 ? 4 : 5;
    double boost = s->map_kpa > s->baro_kpa ? s->map_kpa - s->baro_kpa : 0;   // kPa above ambient
    switch (did) {
    case 0x2032: return (int)put16(out, s->maf_gs * 3.6);                        // airflow, kg/h
    case 0x13CA: return (int)put16(out, s->baro_kpa * 120.60176665439);          // ambient pressure
    case 0x1004: return (int)put16(out, s->ambient_c * 128);                     // ambient temperature
    case 0x14A6: out[0] = (uint8_t)clampi(s->volts / 0.1015625, 0, 255); return 1;
    case 0x11CD: out[0] = (uint8_t)clampi(s->coolant_c + 40, 0, 255); return 1;
    case 0x1001: out[0] = (uint8_t)clampi((s->intake_c + 48) / 0.75, 0, 255); return 1;
    case 0x2004: return (int)put16(out, s->timing_deg * 100);                    // ignition, average
    case 0x200A: case 0x200B: case 0x200C: case 0x200D: {
        // knock retard per cylinder: none at idle, a little now and then under load
        double k = s->load_pct > 60 && sim_rand01() < 0.2 ? -(0.75 + sim_rand01() * 0.75) : 0;
        return (int)put16(out, k * 100);
    }
    case 0x10C0: return (int)put16(out, s->lambda * 1024);
    case 0x39C0: case 0x39C1: return (int)put16(out, s->map_kpa * 10);          // manifold pressure, and its setpoint
    case 0x202A: case 0x2029:                                                    // pressure before the throttle
        return (int)put16(out, ((s->map_kpa > s->baro_kpa ? s->map_kpa : s->baro_kpa) + 3) * 10);
    case 0x2904: {                                                               // misfires, all cylinders
        unsigned n = 0;
        for (int c = 0; c < 4; c++)
            n += s->mis_now[c];
        return (int)put16(out, n > 65535 ? 65535 : n);
    }
    case 0x202F: return (int)put16(out, s->oil_c * 10 + 2731.4);
    case 0x1070: return (int)put16(out, s->throttle_pct * 10.24);                // pedal
    case 0x20BA: return (int)put16(out, (3 + s->throttle_pct) * 10);             // throttle plate
    case 0x2033: return (int)put16(out, s->speed_kmh * 100);
    case 0x210F: return (int)put16(out, gear);
    case 0x201A: return (int)put16(out, (0 + 24) * 10);                          // exhaust cam at its base position
    case 0x201E: return (int)put16(out, (30 - 0) * 10);                          // intake cam at its base position
    case 0x2027: case 0x293B: return (int)put16(out, (idle ? 40 : 40 + s->load_pct * 1.2) * 10);   // direct-injection rail, bar
    case 0x2025: case 0x2932: return (int)put16(out, 5.0 * 1000);               // port-injection rail, bar
    case 0x437C: case 0x4380: return (int)put16(out, (idle ? 15 : s->load_pct * 3.5) * 10);        // torque, Nm
    // The rest of the list. The model has no turbo, fuel pump or valve-lift state of its own, so
    // these follow load, boost and temperature the way the EA888 Gen 3 is described as working.
    case 0x101E: out[0] = (uint8_t)clampi((s->coolant_c - 100) * 10 / 0.390625, 0, 255); return 1;   // fan, %
    case 0x1040: return (int)put16(out, (25 + boost * 1.6) * 163.84);           // turbo speed (the list says rpm; scaled like krpm)
    case 0x1041: return (int)put16(out, (s->intake_c + boost * 0.4 - 144) / 0.005859375);   // charge air, C
    case 0x13A0: return (int)put16(out, (0.35 + s->load_pct * 0.03) * 250);    // direct-injection pulse, ms
    case 0x13AC: return (int)put16(out, (s->load_pct < 60 && s->coolant_c > 60 ? 1.8 + s->load_pct * 0.02 : 0) * 250);
                                                                                 // port injection: part load, warm
    case 0x14EC: return (int)put16(out, (55 + s->rpm / 7000 * 20) / 0.09765625);   // ECU processor load, %
    case 0x167C: out[0] = s->load_pct > 50 || s->rpm > 3000; return 1;         // exhaust cam lift: 0 small, 1 large
    case 0x2028: return (int)put16(out, (25 + s->load_pct * 0.4) * 100);       // low-pressure pump duty, %
    case 0x203C: return (int)put16(out, 0);                                     // cruise control off
    case 0x209A: return (int)put16(out, (10 + s->load_pct * 0.6) * 100);       // high-pressure pump volume, %
    case 0x295C: out[0] = s->load_pct < 40; return 1;                          // charge-motion flap
    case 0x39A2: case 0x39A3: return (int)put16(out, (100 - (100 - boost * 1.2 < 0 ? 0 : 100 - boost * 1.2)) * 100);
                                                                                 // wastegate open %: open off boost
    default: return -1;
    }
}

// ---- engine services beyond the generic ones
static bool obd_service(struct sim_ecu *e, const uint8_t *req, size_t len, uint8_t *r)
{
    uint8_t sid = req[0];
    const struct engine_state *s = engine();

    if (sid == 0x01 && len == 2) {
        int n = 0;
        if ((req[1] & 0x1F) == 0 && req[1] <= 0xC0) {
            support_bits(ENGINE_PIDS, req[1], &r[2]);
            n = 4;
        } else if (in_list(ENGINE_PIDS, req[1])) {
            n = encode_pid(req[1], s, &r[2]);
        }
        if (n <= 0) {
            negative(e, sid, 0x12);
            return true;
        }
        r[0] = 0x41; r[1] = req[1];
        reply(e, r, (size_t)n + 2);
        return true;
    }
    if (sid == 0x01 && len > 2 && len <= 7) {
        // up to six PIDs in one request (ISO 15765-4): each answered PID followed by its data
        size_t n = 1;
        r[0] = 0x41;
        for (size_t i = 1; i < len; i++) {
            if (!in_list(ENGINE_PIDS, req[i]))
                continue;
            r[n] = req[i];
            int k = encode_pid(req[i], s, &r[n + 1]);
            if (k > 0)
                n += 1 + (size_t)k;
        }
        if (n == 1)
            negative(e, sid, 0x12);
        else
            reply(e, r, n);
        return true;
    }
    if (sid == 0x02 && len == 3) {
        // freeze frame 0: the values when the highest-priority code was stored
        const struct sim_dtc *d = freeze_frame_dtc();
        uint8_t pid = req[1];
        int n = -1;
        if (req[2] != 0x00) {
            negative(e, sid, 0x31);
            return true;
        }
        r[0] = 0x42; r[1] = pid; r[2] = 0x00;
        if ((pid & 0x1F) == 0 && pid <= 0xC0) {
            support_bits(FREEZE_PIDS, pid, &r[3]);
            n = 4;
        } else if (pid == 0x02) {
            r[3] = d ? d->code[0] : 0; r[4] = d ? d->code[1] : 0;
            n = 2;
        } else if (d && in_list(FREEZE_PIDS, pid)) {
            n = encode_pid(pid, &d->snap, &r[3]);
        }
        if (n <= 0) {
            negative(e, sid, 0x31);
            return true;
        }
        reply(e, r, (size_t)n + 3);
        return true;
    }
    if ((sid == 0x03 || sid == 0x07) && len == 1) {
        // stored (confirmed) or pending emission codes, two bytes each
        int count;
        struct sim_dtc *d = engine_dtcs(&count);
        size_t n = 2;
        r[0] = (uint8_t)(sid + 0x40);
        uint8_t want = sid == 0x03 ? 0x08 : 0x04;
        int found = 0;
        for (int i = 0; i < count; i++) {
            if (d[i].status & want) {
                r[n++] = d[i].code[0];
                r[n++] = d[i].code[1];
                found++;
            }
        }
        r[1] = (uint8_t)found;
        reply(e, r, n);
        return true;
    }
    if (sid == 0x04 && len == 1) {
        engine_clear_dtcs();
        r[0] = 0x44;
        reply(e, r, 1);
        return true;
    }
    if (sid == 0x06 && len == 2) {
        // on-board monitoring results; the misfire monitor is MID A1 (all) and A2..A5 (cylinders 1-4)
        uint8_t mid = req[1];
        r[0] = 0x46; r[1] = mid;
        if ((mid & 0x1F) == 0 && mid <= 0xE0) {
            static const uint8_t mids[] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0 };
            support_bits(mids, mid, &r[2]);
            reply(e, r, 6);
            return true;
        }
        if (mid >= 0xA1 && mid <= 0xA5) {
            unsigned avg = 0, now = 0;
            for (int c = 0; c < 4; c++) {
                if (mid == 0xA1 || mid == 0xA2 + c) {
                    avg += s->mis_avg[c];
                    now += s->mis_now[c];
                }
            }
            if (avg > 65535) avg = 65535;
            if (now > 65535) now = 65535;
            // TID 0B: average of the last ten driving cycles; TID 0C: this driving cycle. Unit 24 = counts.
            const uint8_t rec[] = {
                0x0B, 0x24, (uint8_t)(avg >> 8), (uint8_t)avg, 0x00, 0x00, 0xFF, 0xFF,
                0x0C, 0x24, (uint8_t)(now >> 8), (uint8_t)now, 0x00, 0x00, 0xFF, 0xFF,
            };
            memcpy(&r[2], rec, sizeof(rec));
            reply(e, r, 2 + sizeof(rec));
            return true;
        }
        negative(e, sid, 0x31);
        return true;
    }
    if (sid == 0x09 && len == 2) {
        // Vehicle information (SAE J1979): supported list, VIN, calibration ID, verification number,
        // module name. The calibration ID is the part number and software version; the CVN is invented.
        r[0] = 0x49; r[1] = req[1];
        switch (req[1]) {
        case 0x00: r[2] = 0x54; r[3] = 0x40; r[4] = 0x00; r[5] = 0x00; reply(e, r, 6); return true;   // 02 04 06 0A
        case 0x02: r[2] = 0x01; memcpy(&r[3], VIN, 17); reply(e, r, 20); return true;
        case 0x04: r[2] = 0x01; memcpy(&r[3], "5G0906259L  0002", 16); reply(e, r, 19); return true;
        case 0x06: r[2] = 0x01; r[3] = 0x3A; r[4] = 0x7C; r[5] = 0x01; r[6] = 0xD4; reply(e, r, 7); return true;
        case 0x0A: r[2] = 0x01; memcpy(&r[3], "ECM\0-EngineControl\0", 20); reply(e, r, 23); return true;
        }
        negative(e, sid, 0x31);
        return true;
    }
    return false;
}

// ---- fault memory, one view over the engine model's and the other modules' fixed lists
struct dtc_view {
    uint8_t code[3], status;
    const struct sim_dtc *rich;    // engine codes carry a snapshot and counters
};

static int dtc_list(struct sim_ecu *e, struct dtc_view *out, int cap)
{
    int n = 0;
    if (e->kind == K_ENGINE) {
        int count;
        struct sim_dtc *d = engine_dtcs(&count);
        for (int i = 0; i < count && n < cap; i++, n++) {
            memcpy(out[n].code, d[i].code, 3);
            out[n].status = d[i].status;
            out[n].rich = &d[i];
        }
    } else {
        for (int i = 0; i < e->dtc_count && n < cap; i++, n++) {
            memcpy(out[n].code, e->dtcs[i], 3);
            out[n].status = e->dtcs[i][3];
            out[n].rich = NULL;
        }
    }
    return n;
}

static void read_dtc_info(struct sim_ecu *e, const uint8_t *req, size_t len, uint8_t *r)
{
    struct dtc_view v[SIM_MAX_DTC];
    int count = dtc_list(e, v, SIM_MAX_DTC);
    uint8_t sub = len > 1 ? req[1] : 0;
    size_t n;

    if ((sub == 0x01 || sub == 0x02) && len == 3) {
        int matches = 0;
        n = 3;
        r[0] = 0x59; r[1] = sub; r[2] = 0xFF;
        for (int i = 0; i < count; i++) {
            if (!(v[i].status & req[2]))
                continue;
            matches++;
            if (sub == 0x02) {
                memcpy(&r[n], v[i].code, 3);
                r[n + 3] = v[i].status;
                n += 4;
            }
        }
        if (sub == 0x01) {
            r[3] = 0x01;   // ISO 14229-1 format
            r[4] = (uint8_t)(matches >> 8);
            r[5] = (uint8_t)matches;
            n = 6;
        }
        reply(e, r, n);
        return;
    }
    if ((sub == 0x04 || sub == 0x06) && len == 6) {
        const struct dtc_view *d = NULL;
        for (int i = 0; i < count; i++)
            if (!memcmp(v[i].code, &req[2], 3))
                d = &v[i];
        if (!d) {
            negative(e, req[0], 0x31);
            return;
        }
        uint8_t rec = req[5];
        r[0] = 0x59; r[1] = sub;
        memcpy(&r[2], d->code, 3);
        r[5] = d->status;
        n = 6;
        if (sub == 0x04 && d->rich && d->rich->has_snapshot && (rec == 0x01 || rec == 0xFF)) {
            // snapshot record 1: the freeze frame, as OBD identifiers F4xx
            static const uint8_t pids[] = { 0x0C, 0x0D, 0x05, 0x04, 0x06, 0x07, 0x0B, 0x0F, 0x0E, 0x1F, 0x42 };
            r[n++] = 0x01;
            r[n++] = (uint8_t)sizeof(pids);
            for (size_t i = 0; i < sizeof(pids); i++) {
                r[n++] = 0xF4;
                r[n++] = pids[i];
                n += (size_t)encode_pid(pids[i], &d->rich->snap, &r[n]);
            }
        } else if (sub == 0x04 && rec != 0x01 && rec != 0xFF) {
            negative(e, req[0], 0x31);
            return;
        }
        if (sub == 0x06) {
            if (rec != 0x01 && rec != 0x02 && rec != 0xFF) {
                negative(e, req[0], 0x31);
                return;
            }
            uint8_t occ = d->rich ? d->rich->occurrences : 1, aging = d->rich ? d->rich->aging : 0;
            if (rec == 0x01 || rec == 0xFF) { r[n++] = 0x01; r[n++] = occ; }     // occurrence counter
            if (rec == 0x02 || rec == 0xFF) { r[n++] = 0x02; r[n++] = aging; }   // driving cycles since it last failed
        }
        reply(e, r, n);
        return;
    }
    negative(e, req[0], 0x12);
}

static void handle(struct sim_ecu *e, const uint8_t *req, size_t len)
{
    static uint8_t r[256];
    uint8_t sid = req[0];
    uint32_t now = plat_now_ms();
    enum ignition ign = sim_ignition(car_time());

    e->last_request = now;

    if (e->kind == K_ENGINE && ign == IGN_CRANK && sid != 0x3E) {
        negative(e, sid, 0x21);   // busy starting the engine: ask again
        return;
    }
    if (e->functional && e->kind == K_TRANS) {
        // the transmission answers only the few OBD values it has
        if (sid == 0x01 && len == 2) {
            int n = 0;
            if ((req[1] & 0x1F) == 0 && req[1] <= 0xC0) {
                support_bits(TRANS_PIDS, req[1], &r[2]);
                n = 4;
            } else if (req[1] == 0x01) {
                r[2] = 0x00; r[3] = 0x07; r[4] = 0x00; r[5] = 0x00;
                n = 4;
            } else if (req[1] == 0x0D) {
                n = encode_pid(0x0D, engine(), &r[2]);
            }
            if (n > 0) {
                r[0] = 0x41; r[1] = req[1];
                reply(e, r, (size_t)n + 2);
            }
        }
        return;
    }
    if (e->kind == K_ENGINE && obd_service(e, req, len, r))
        return;
    if (e->functional)
        return;

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
        e->session = req[1];
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
        if (did == 0xF186) {
            r[3] = e->session;       // the active diagnostic session
            reply(e, r, 4);
            return;
        }
        if (e->kind == K_ENGINE && did == 0xF17C) {
            // slow identifier: answers "response pending" first
            e->pending = true;
            e->pending_stage = 0;
            e->pending_at = now + 20;
            return;
        }
        if (e->kind == K_ENGINE) {
            int n = simos_did(did, engine(), &r[3]);
            if (n > 0) {
                reply(e, r, (size_t)n + 3);
                return;
            }
        }
        if (e->kind == K_ENGINE && (did & 0xFF00) == 0xF400 && in_list(ENGINE_PIDS, (uint8_t)did)) {
            int n = encode_pid((uint8_t)did, engine(), &r[3]);   // OBD PIDs mirrored as DIDs F4xx
            reply(e, r, (size_t)n + 3);
            return;
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
    } else if (sid == 0x19) {
        read_dtc_info(e, req, len, r);
    } else if (sid == 0x14 && len == 4) {
        if (e->kind == K_ENGINE)
            engine_clear_dtcs();
        else
            e->dtc_count = 0;
        r[0] = 0x54;
        reply(e, r, 1);
    } else if (sid == 0x27) {
        negative(e, sid, e->session == 0x01 ? 0x7F : 0x33);
    } else if (sid == 0x2E || sid == 0x31 || sid == 0x2F || sid == 0x2C) {
        // nothing here ever changes: refused outright, for a reason that depends on the session
        negative(e, sid, e->session == 0x01 ? 0x7F : 0x33);
    } else if (sid == 0x11) {
        negative(e, sid, e->session == 0x01 ? 0x7F : 0x22);
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

static void broadcast(enum ignition ign)
{
    static unsigned tick;
    struct can_frame f;
    const struct engine_state *s = engine();

    // Klemmen_Status_01 (0x3C0): key in, ignition on
    memset(&f, 0, sizeof(f)); f.id = 0x3C0; f.dlc = 4;
    put_bits(f.data, 8, 4, tick & 0xF);
    put_bits(f.data, 16, 1, 1);                     // ZAS_Kl_S
    put_bits(f.data, 17, 1, ign != IGN_OFF);        // ZAS_Kl_15
    ecu_send(NULL, &f);

    // ESP_21 (0x0FD): vehicle speed, 0.01 km/h per bit
    memset(&f, 0, sizeof(f)); f.id = 0x0FD; f.dlc = 8;
    put_bits(f.data, 32, 16, (uint32_t)(s->speed_kmh * 100 + 0.5));
    ecu_send(NULL, &f);

    // The drive-replay set, every 100 ms, signal layouts from opendbc's vw_mqb.dbc. Counters run;
    // the checksum bytes stay zero (the message-specific CRC constants are not reproduced here).
    static double last_kmh;
    double accel = (s->speed_kmh - last_kmh) / 3.6 / 0.1;   // m/s^2 over this 100 ms
    last_kmh = s->speed_kmh;
    bool braking = accel < -1.0 && s->throttle_pct < 1;
    bool idle = s->speed_kmh < 1 && s->throttle_pct < 1;

    // Motor_12 (0x0A8): engine speed, 0.25 rpm per bit
    memset(&f, 0, sizeof(f)); f.id = 0x0A8; f.dlc = 8;
    put_bits(f.data, 48, 16, (uint32_t)clampi(s->rpm * 4, 0, 65535));
    ecu_send(NULL, &f);

    // Motor_20 (0x121): accelerator pedal 0.4 % per bit, idle flag, idle speed target 10 rpm per bit
    memset(&f, 0, sizeof(f)); f.id = 0x121; f.dlc = 8;
    put_bits(f.data, 8, 4, tick & 0xF);
    put_bits(f.data, 12, 8, (uint32_t)clampi(s->throttle_pct / 0.4, 0, 254));
    put_bits(f.data, 37, 1, idle);
    put_bits(f.data, 40, 8, (uint32_t)clampi(sim_cfg.idle_rpm / 10, 0, 254));
    ecu_send(NULL, &f);

    // ESP_19 (0x0B2): the four wheel speeds, 0.0075 km/h per bit
    memset(&f, 0, sizeof(f)); f.id = 0x0B2; f.dlc = 8;
    for (int w = 0; w < 4; w++)
        put_bits(f.data, w * 16, 16, (uint32_t)clampi(s->speed_kmh / 0.0075, 0, 65534));
    ecu_send(NULL, &f);

    // ESP_02 (0x101): longitudinal acceleration (0.03125 m/s^2, offset -16), no lateral or yaw
    memset(&f, 0, sizeof(f)); f.id = 0x101; f.dlc = 8;
    put_bits(f.data, 8, 4, tick & 0xF);
    put_bits(f.data, 16, 8, 127);                                       // lateral 0 g
    put_bits(f.data, 24, 10, (uint32_t)clampi((accel + 16) / 0.03125, 0, 1022));
    ecu_send(NULL, &f);

    // ESP_05 (0x106): brake pressure (0.3 bar, offset -30) and "driver brakes"
    memset(&f, 0, sizeof(f)); f.id = 0x106; f.dlc = 8;
    put_bits(f.data, 8, 4, tick & 0xF);
    put_bits(f.data, 16, 10, (uint32_t)clampi(((braking ? -accel * 8 : 0) + 30) / 0.3, 0, 1022));
    put_bits(f.data, 26, 1, braking);
    ecu_send(NULL, &f);

    if (tick % 10 == 0) {
        // Motor_07 (0x640): intake, oil and coolant temperatures
        memset(&f, 0, sizeof(f)); f.id = 0x640; f.dlc = 8;
        put_bits(f.data, 8, 8, (uint32_t)clampi((s->intake_c + 48) / 0.75, 0, 255));
        put_bits(f.data, 16, 8, (uint32_t)clampi(s->oil_c + 60, 0, 255));
        put_bits(f.data, 24, 8, (uint32_t)clampi((s->coolant_c + 48) / 0.75, 0, 255));
        ecu_send(NULL, &f);

        // Motor_Code_01 (0x641): 4 cylinders, 2.0 l, 170 kW (the US MK7.5 GTI's 228 hp)
        memset(&f, 0, sizeof(f)); f.id = 0x641; f.dlc = 8;
        put_bits(f.data, 32, 4, 4);
        put_bits(f.data, 40, 7, 20);
        put_bits(f.data, 48, 9, 170);
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

static bool awake(const struct sim_ecu *e, enum ignition ign)
{
    // With the ignition off only the gateway listens (it wakes for a diagnostic request).
    return ign != IGN_OFF || e->kind == K_GATEWAY;
}

bool car_init(const char *scenario, car_tx_fn tx)
{
    uint32_t now = plat_now_ms();
    bool ok = sim_config_set(scenario);
    engine_reset();
    car_tx = tx;
    t0 = now;
    memset(&cstats, 0, sizeof(cstats));
    for (size_t i = 0; i < ECU_COUNT; i++) {
        struct sim_ecu *e = &ecus[i];
        isotp_init(&e->tp, e->resp, e->req, false, ecu_send, now_fn, NULL);
        e->tp.pad_byte = 0xAA;
        e->session = 0x01;
        e->out_ready = e->pending = e->functional = false;
        e->dtc_count = e->dtc_initial;
        if (e->kind != K_ENGINE) {
            // the modules behind the gateway pace long requests
            e->tp.rx_bs = sim_cfg.fc_bs;
            e->tp.rx_stmin = sim_cfg.fc_stmin;
        }
    }
    next_broadcast = now + 100;
    started = true;
    return ok;
}

uint32_t car_time(void)
{
    // the engine's clock: real time, or faster for long experiments (speed=)
    return (uint32_t)(rel(plat_now_ms()) * sim_cfg.speed);
}

void car_get_stats(struct car_stats *out)
{
    *out = cstats;
}

void car_poll(void)
{
    static uint8_t buf[ISOTP_MAX_PAYLOAD];
    uint32_t now = plat_now_ms();
    if (!started)
        return;

    engine_step(car_time());
    enum ignition ign = sim_ignition(car_time());

    for (size_t i = 0; i < ECU_COUNT; i++) {
        struct sim_ecu *e = &ecus[i];
        size_t n;
        if (!awake(e, ign)) {
            isotp_reset(&e->tp);
            e->out_ready = e->pending = false;
            e->session = 0x01;
            continue;
        }
        isotp_poll(&e->tp);
        (void)isotp_take_error(&e->tp);
        if (isotp_recv(&e->tp, buf, sizeof(buf), &n) == 1) {
            cstats.requests++;
            handle(e, buf, n);
            e->functional = false;
        }
        if (sim_cfg.s3_ms && e->session != 0x01 && (int32_t)(now - e->last_request) > sim_cfg.s3_ms)
            e->session = 0x01;   // no request for too long: back to the default session
        if (e->out_ready && (int32_t)(now - e->out_due) >= 0 && !isotp_tx_busy(&e->tp)) {
            isotp_send(&e->tp, e->out, e->out_len);
            e->out_ready = false;
        }
        if (e->pending && !e->out_ready && (int32_t)(now - e->pending_at) >= 0 && !isotp_tx_busy(&e->tp)) {
            if (e->pending_stage < 2) {
                static const uint8_t wait[] = { 0x7F, 0x22, 0x78 };
                isotp_send(&e->tp, wait, sizeof(wait));
                e->pending_stage++;
                e->pending_at = now + 150;
            } else {
                static const uint8_t done[] = { 0x62, 0xF1, 0x7C, 'T', 'S', 'T', '-', '0', '1', '.', '0', '1', '.', '2', '0', '1', '8' };
                isotp_send(&e->tp, done, sizeof(done));
                e->pending = false;
            }
        }
    }

    // Background traffic laid out as in the public VW MQB message database. On the
    // car the gateway probably keeps it off the OBD port; then nothing shows here.
    if ((int32_t)(now - next_broadcast) >= 0) {
        if (sim_cfg.gateway_open && ign != IGN_OFF)
            broadcast(ign);
        next_broadcast = now + 100;
    }
}

// A frame from the bus (the tester's requests and flow control).
void car_on_frame(const struct can_frame *f)
{
    if (!started)
        return;
    cstats.received++;
    enum ignition ign = sim_ignition(car_time());
    for (size_t i = 0; i < ECU_COUNT; i++) {
        struct sim_ecu *e = &ecus[i];
        if (!awake(e, ign))
            continue;
        struct can_frame g = *f;
        if (f->id == 0x7DF && (e->kind == K_ENGINE || (e->kind == K_TRANS && sim_cfg.trans_obd))) {
            g.id = e->req;           // a functional OBD request reaches every emissions module
            e->functional = true;
        }
        isotp_on_frame(&e->tp, &g);
    }
}
