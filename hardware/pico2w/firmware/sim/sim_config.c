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

static void bad(const char *what)
{
    fprintf(stderr, "VWSIM: cannot use \"%s\"\n", what);
    exit(2);
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
        if (!colon || sim_cfg.ign_count == SIM_MAX_IGN_STEPS)
            bad(v);
        enum ignition st = ign_name(v, (size_t)(colon - v));
        char *end;
        unsigned long at = strtoul(colon + 1, &end, 10);
        sim_cfg.ign[sim_cfg.ign_count].at = (uint32_t)at;
        sim_cfg.ign[sim_cfg.ign_count].state = st;
        sim_cfg.ign_count++;
        v = *end == '/' ? end + 1 : end;
        if (*end && *end != '/')
            bad(end);
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
        if (sscanf(v, "%u:%u", &sim_cfg.busoff_at, &sim_cfg.busoff_ms) != 2)
            bad(v);
    } else if (!strcmp(k, "seed")) {
        sim_cfg.seed = (uint32_t)strtoul(v, NULL, 0);
    } else {
        bad(k);
    }
}

void sim_config_load(void)
{
    memset(&sim_cfg, 0, sizeof(sim_cfg));
    profile_realistic();
    sim_cfg.seed = 12345;

    const char *env = getenv("VWSIM");
    if (env) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", env);
        for (char *item = strtok(buf, ","); item; item = strtok(NULL, ","))
            apply(item);
    }
    rng_state = sim_cfg.seed ? sim_cfg.seed : 1;
}
