// A simulated GPS receiver on the dongle's GPS / sync connector, for the host build: NMEA at 10 Hz
// (RMC and GGA), a pulse at every GPS second, and the sync output's edges. Off unless VWDIAG_GPS=1.
//
// The computer's clock stands in for the dongle's, and the simulated GPS time runs against it with
// a known error: VWDIAG_GPS_DRIFT ppm (default 25: the local clock runs 25 ppm fast), so the drift
// the firmware measures can be checked. The car drives east from a fixed point at the engine
// model's road speed. Sentences come out 60 ms after the second or tenth they describe.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/platform.h"
#include "sim.h"

#define RATE_HZ 10
#define LATENCY_US 60000
#define START_LAT 52.4227
#define START_LON 10.7865
#define EARTH_M 6371000.0

static bool started, enabled;
static uint64_t t0;            // local time of GPS second 0
static double scale;           // local microseconds per true microsecond
static uint32_t utc0;
static uint64_t next_epoch;    // next NMEA epoch (tenths of a second since t0)
static uint64_t next_pulse;    // next pulse (seconds since t0)
static double lat = START_LAT, lon = START_LON;

static char nmea[8192];
static size_t nmea_len;
#define PPS_MAX 16
static uint64_t pps[PPS_MAX];
static size_t pps_n;

static uint32_t sync_hz;
static uint64_t sync_next, sync_base; // next edge index, and the local time edge 0 belongs to
#define SYNC_MAX 256
static uint64_t sync_q[SYNC_MAX];
static size_t sync_n;

uint64_t plat_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void start(void)
{
    if (started)
        return;
    started = true;
    const char *v = getenv("VWDIAG_GPS");
    enabled = v && strcmp(v, "0");
    const char *d = getenv("VWDIAG_GPS_DRIFT");
    scale = 1.0 + (d ? atof(d) : 25.0) * 1e-6;
    t0 = plat_now_us();
    utc0 = (uint32_t)time(NULL);
    next_epoch = 1;
    next_pulse = 1;
}

static uint64_t local_at(double true_us)
{
    return t0 + (uint64_t)(true_us * scale + 0.5);
}

static void put_sentence(const char *body)
{
    unsigned sum = 0;
    for (const char *p = body; *p; p++)
        sum ^= (unsigned char)*p;
    char line[128];
    int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, sum);
    if (n > 0 && nmea_len + (size_t)n <= sizeof(nmea)) {
        memcpy(nmea + nmea_len, line, (size_t)n);
        nmea_len += (size_t)n;
    }
}

static void angle(char *out, size_t cap, double deg, int deg_digits)
{
    double a = fabs(deg);
    int whole = (int)a;
    snprintf(out, cap, "%0*d%08.5f", deg_digits, whole, (a - whole) * 60.0);
}

static void emit_epoch(uint64_t e)
{
    // Move the car on by a tenth of a second at the engine model's road speed.
    double v = engine()->speed_kmh / 3.6;
    lon += v / RATE_HZ / (EARTH_M * cos(lat * M_PI / 180.0)) * 180.0 / M_PI;

    time_t t = (time_t)(utc0 + e / RATE_HZ);
    struct tm tm;
    gmtime_r(&t, &tm);
    char tod[16], date[8], la[16], lo[16], body[120];
    snprintf(tod, sizeof(tod), "%02d%02d%02d.%02u", tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned)(e % RATE_HZ) * 10);
    snprintf(date, sizeof(date), "%02d%02d%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    angle(la, sizeof(la), lat, 2);
    angle(lo, sizeof(lo), lon, 3);
    snprintf(body, sizeof(body), "GNRMC,%s,A,%s,N,%s,E,%.3f,90.00,%s,,,A", tod, la, lo, v / 0.514444, date);
    put_sentence(body);
    snprintf(body, sizeof(body), "GNGGA,%s,%s,N,%s,E,1,12,0.80,85.30,M,46.00,M,,", tod, la, lo);
    put_sentence(body);
}

static void advance(void)
{
    start();
    if (!enabled)
        return;
    uint64_t now = plat_now_us();
    while (local_at(next_pulse * 1e6) <= now) {
        if (pps_n < PPS_MAX)
            pps[pps_n++] = local_at(next_pulse * 1e6);
        next_pulse++;
    }
    while (local_at(next_epoch * (1e6 / RATE_HZ) + LATENCY_US) <= now)
        emit_epoch(next_epoch++);
}

static void advance_sync(void)
{
    if (!sync_hz)
        return;
    uint64_t now = plat_now_us();
    for (;;) {
        // With GPS, edges fall on GPS seconds and their tenths (the pulse restarts the output);
        // without, the output runs from the local clock.
        uint64_t at = enabled ? local_at(sync_next * (1e6 / sync_hz)) : sync_base + sync_next * 1000000 / sync_hz;
        if (at > now)
            break;
        if (sync_n < SYNC_MAX)
            sync_q[sync_n++] = at;
        sync_next++;
    }
}

size_t plat_gnss_read(uint8_t *buf, size_t cap)
{
    advance();
    size_t n = nmea_len < cap ? nmea_len : cap;
    memcpy(buf, nmea, n);
    memmove(nmea, nmea + n, nmea_len - n);
    nmea_len -= n;
    return n;
}

bool plat_gnss_baud(uint32_t baud)
{
    (void)baud;
    return true;
}

bool plat_pps_take(uint64_t *edge_us)
{
    advance();
    if (!pps_n)
        return false;
    *edge_us = pps[0];
    memmove(pps, pps + 1, (--pps_n) * sizeof(pps[0]));
    return true;
}

bool plat_sync_set(uint32_t hz)
{
    start();
    sync_hz = hz;
    sync_n = 0;
    if (hz) {
        uint64_t now = plat_now_us();
        sync_base = now;
        // the first edge still to come
        sync_next = enabled ? (uint64_t)(((double)(now - t0) / scale) * hz / 1e6) + 1 : 1;
    }
    return true;
}

bool plat_sync_take(uint64_t *edge_us)
{
    advance_sync();
    if (!sync_n)
        return false;
    *edge_us = sync_q[0];
    memmove(sync_q, sync_q + 1, (--sync_n) * sizeof(sync_q[0]));
    return true;
}
