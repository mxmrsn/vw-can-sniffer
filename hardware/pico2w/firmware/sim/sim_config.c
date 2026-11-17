// Scenario settings for the simulator, read from the VWSIM environment variable.
//
//   VWSIM=realistic                      the default: real-car timing, gateway closed
//   VWSIM=ideal                          instant answers, broadcast visible, sessions never expire
//   VWSIM=gti                            realistic, plus this car: cylinder 3 low on compression,
//                                        a stored misfire code from earlier drives, cold start
//   VWSIM=lossy                          realistic, plus missed frames, error frames and delays
//   VWSIM=realistic,gateway=open,misfire=2,ignition=off:0/on:2000/crank:5000/run:5600
//
// Profiles and key=value settings apply left to right. Keys:
//   latency=MIN-MAX ms   s3=MS   fc=BS/STMIN   gateway=open|closed   trans_obd=0|1
//   ignition=STATE:MS/...  (off, on, crank, run)   misfire=0..4   history=0|1
//   fault=can|card|can+card  the self-test's CAN loopback or card check fails
//   start=cold|warm   drive=0|1   drop=PCT   errors=PCT   delay=PCT:MS   busoff=AT:MS   seed=N
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"

struct sim_config sim_cfg;
static uint32_t rng_state = 1;

uint32_t sim_rand(void)
{
    // xorshift32: small, fast and the same on every machine
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

double sim_rand01(void)
{
    return (sim_rand() >> 8) / 16777216.0;
}

int sim_rand_range(int lo, int hi)
{
    if (hi <= lo)
        return lo;
    return lo + (int)(sim_rand() % (uint32_t)(hi - lo + 1));
}

static bool config_ok;

static void bad(const char *what)
{
    (void)what;
    config_ok = false;
}

static void engine_defaults(void)
{
    // Forum-level estimates for an EA888 Gen 3 (docs/vw_can_data_sources.md) until the car's own logs replace them.
    sim_cfg.idle_rpm = 700;
    sim_cfg.cold_idle_rpm = 1100;
    sim_cfg.warmup = 1.0;
    sim_cfg.mis_cold = 6.0;
    sim_cfg.mis_idle = 2.5;
    sim_cfg.mis_light = 0.6;
    sim_cfg.mis_load = 0.2;
    sim_cfg.trim_cold = 9.0;
    sim_cfg.trim_idle = 6.5;
    sim_cfg.trim_drive = 2.5;
    sim_cfg.speed = 1.0;
}

static void profile_realistic(void)
{
    sim_cfg.lat_min = 5;
    sim_cfg.lat_max = 50;
    sim_cfg.s3_ms = 5000;
    sim_cfg.fc_bs = 8;
    sim_cfg.fc_stmin = 5;
    sim_cfg.gateway_open = false;
    sim_cfg.trans_obd = true;
}

static void profile_ideal(void)
{
    sim_cfg.lat_min = sim_cfg.lat_max = 0;
    sim_cfg.s3_ms = 0;
    sim_cfg.fc_bs = sim_cfg.fc_stmin = 0;
    sim_cfg.gateway_open = true;
    sim_cfg.trans_obd = false;
}

static enum ignition ign_name(const char *s, size_t n)
{
    static const char *names[] = { "off", "on", "crank", "run" };
    for (int i = 0; i < 4; i++)
        if (strlen(names[i]) == n && strncmp(s, names[i], n) == 0)
            return (enum ignition)i;
    bad(s);
    return IGN_RUN;
}

static void parse_ignition(const char *v)
{
    // off:0/on:2000/run:5000
    sim_cfg.ign_count = 0;
    while (*v) {
        const char *colon = strchr(v, ':');
        if (!colon || sim_cfg.ign_count == SIM_MAX_IGN_STEPS) {
            bad(v);
            return;
        }
        enum ignition st = ign_name(v, (size_t)(colon - v));
        char *end;
        unsigned long at = strtoul(colon + 1, &end, 10);
        sim_cfg.ign[sim_cfg.ign_count].at = (uint32_t)at;
        sim_cfg.ign[sim_cfg.ign_count].state = st;
        sim_cfg.ign_count++;
        if (*end && *end != '/') {
            bad(end);
            return;
        }
        v = *end == '/' ? end + 1 : end;
    }
}

static void apply(char *item)
{
    char *eq = strchr(item, '=');
    if (!eq) {
        if (!strcmp(item, "realistic")) {
            profile_realistic();
        } else if (!strcmp(item, "ideal")) {
            profile_ideal();
        } else if (!strcmp(item, "gti")) {
            profile_realistic();
            sim_cfg.misfire_cyl = 3;
            sim_cfg.history = true;
            sim_cfg.cold_start = true;
        } else if (!strcmp(item, "lossy")) {
            profile_realistic();
            sim_cfg.drop_pct = 0.5;
            sim_cfg.error_pct = 1.0;
            sim_cfg.delay_pct = 2.0;
            sim_cfg.delay_ms = 30;
        } else if (*item) {
            bad(item);
        }
        return;
    }
    *eq = 0;
    const char *k = item, *v = eq + 1;
    if (!strcmp(k, "latency")) {
        if (sscanf(v, "%d-%d", &sim_cfg.lat_min, &sim_cfg.lat_max) != 2 || sim_cfg.lat_min < 0 || sim_cfg.lat_max < sim_cfg.lat_min)
            bad(v);
    } else if (!strcmp(k, "s3")) {
        sim_cfg.s3_ms = atoi(v);
    } else if (!strcmp(k, "fc")) {
        unsigned bs, st;
        if (sscanf(v, "%u/%u", &bs, &st) != 2 || bs > 255 || st > 127)
            bad(v);
        sim_cfg.fc_bs = (uint8_t)bs;
        sim_cfg.fc_stmin = (uint8_t)st;
    } else if (!strcmp(k, "gateway")) {
        if (strcmp(v, "open") && strcmp(v, "closed"))
            bad(v);
        sim_cfg.gateway_open = !strcmp(v, "open");
    } else if (!strcmp(k, "trans_obd")) {
        sim_cfg.trans_obd = atoi(v) != 0;
    } else if (!strcmp(k, "ignition")) {
        parse_ignition(v);
    } else if (!strcmp(k, "misfire")) {
        sim_cfg.misfire_cyl = atoi(v);
        if (sim_cfg.misfire_cyl < 0 || sim_cfg.misfire_cyl > 4)
            bad(v);
    } else if (!strcmp(k, "fault")) {
        sim_cfg.fault_can = strstr(v, "can") != NULL;
        sim_cfg.fault_card = strstr(v, "card") != NULL;
        if (!sim_cfg.fault_can && !sim_cfg.fault_card)
            bad(v);
    } else if (!strcmp(k, "history")) {
        sim_cfg.history = atoi(v) != 0;
    } else if (!strcmp(k, "start")) {
        if (strcmp(v, "cold") && strcmp(v, "warm"))
            bad(v);
        sim_cfg.cold_start = !strcmp(v, "cold");
    } else if (!strcmp(k, "drive")) {
        sim_cfg.drive = atoi(v) != 0;
    } else if (!strcmp(k, "drop")) {
        sim_cfg.drop_pct = atof(v);
    } else if (!strcmp(k, "errors")) {
        sim_cfg.error_pct = atof(v);
    } else if (!strcmp(k, "delay")) {
        if (sscanf(v, "%lf:%d", &sim_cfg.delay_pct, &sim_cfg.delay_ms) != 2)
            bad(v);
    } else if (!strcmp(k, "busoff")) {
        unsigned at, ms;
        if (sscanf(v, "%u:%u", &at, &ms) != 2)
            bad(v);
        else {
            sim_cfg.busoff_at = at;
            sim_cfg.busoff_ms = ms;
        }
    } else if (!strcmp(k, "idle") || !strcmp(k, "cold_idle") || !strcmp(k, "warmup") || !strcmp(k, "mis_cold")
               || !strcmp(k, "mis_idle") || !strcmp(k, "mis_light") || !strcmp(k, "mis_load") || !strcmp(k, "trim_cold")
               || !strcmp(k, "trim_idle") || !strcmp(k, "trim_drive") || !strcmp(k, "speed")) {
        char *end;
        double x = strtod(v, &end);
        if (*end || x < 0) {
            bad(v);
            return;
        }
        double *slot = !strcmp(k, "idle") ? &sim_cfg.idle_rpm : !strcmp(k, "cold_idle") ? &sim_cfg.cold_idle_rpm
                     : !strcmp(k, "warmup") ? &sim_cfg.warmup : !strcmp(k, "mis_cold") ? &sim_cfg.mis_cold
                     : !strcmp(k, "mis_idle") ? &sim_cfg.mis_idle : !strcmp(k, "mis_light") ? &sim_cfg.mis_light
                     : !strcmp(k, "mis_load") ? &sim_cfg.mis_load : !strcmp(k, "trim_cold") ? &sim_cfg.trim_cold
                     : !strcmp(k, "trim_idle") ? &sim_cfg.trim_idle : !strcmp(k, "trim_drive") ? &sim_cfg.trim_drive
                     : &sim_cfg.speed;
        *slot = x;
        if (sim_cfg.speed <= 0)
            sim_cfg.speed = 1;
    } else if (!strcmp(k, "seed")) {
        sim_cfg.seed = (uint32_t)strtoul(v, NULL, 0);
    } else {
        bad(k);
    }
}

bool sim_config_set(const char *spec)
{
    memset(&sim_cfg, 0, sizeof(sim_cfg));
    engine_defaults();
    profile_realistic();
    sim_cfg.seed = 12345;
    config_ok = true;
    if (spec) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", spec);
        char *save = NULL;
        for (char *item = strtok_r(buf, ",", &save); item; item = strtok_r(NULL, ",", &save))
            apply(item);
    }
    if (!config_ok) {
        memset(&sim_cfg, 0, sizeof(sim_cfg));
        engine_defaults();
        profile_realistic();
        sim_cfg.seed = 12345;
    }
    rng_state = sim_cfg.seed ? sim_cfg.seed : 1;
    return config_ok;
}

void sim_set_ignition(enum ignition state, uint32_t now)
{
    sim_cfg.ign_count = 1;
    sim_cfg.ign[0].at = now;
    sim_cfg.ign[0].state = state;
}
