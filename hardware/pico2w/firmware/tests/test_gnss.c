// Host tests for the GPS input and time sync (gnss.c).   make -C hardware/pico2w/firmware/tests
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gnss.h"

static int failures, checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

#define MAX_EVENTS 64
static struct gnss_event events[MAX_EVENTS];
static int n_events;

static void on_event(const struct gnss_event *e, void *ctx)
{
    (void)ctx;
    if (n_events < MAX_EVENTS)
        events[n_events++] = *e;
}

static void feed(const char *text, uint64_t now_us)
{
    gnss_feed((const uint8_t *)text, strlen(text), now_us, on_event, NULL);
}

// A sentence with its checksum worked out, fed at now_us.
static void say(const char *body, uint64_t now_us)
{
    unsigned sum = 0;
    for (const char *p = body; *p; p++)
        sum ^= (unsigned char)*p;
    char line[160];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, sum);
    feed(line, now_us);
}

// RMC for a whole second, at 1994-03-23 12:35:19 + s.
static void rmc_at(int s, uint64_t now_us)
{
    char body[120];
    int t = 12 * 3600 + 35 * 60 + 19 + s;
    snprintf(body, sizeof(body), "GPRMC,%02d%02d%02d.00,A,4807.038,N,01131.000,E,0.0,0.0,230394,,,A",
             t / 3600, t / 60 % 60, t % 60);
    say(body, now_us);
}

static void reset(void)
{
    gnss_init();
    n_events = 0;
}

int main(void)
{
    struct gnss_status st;

    printf("calendar\n");
    CHECK(gnss_days_from_civil(1970, 1, 1) == 0);
    CHECK(gnss_days_from_civil(2000, 3, 1) == 11017);
    CHECK(gnss_days_from_civil(2026, 11, 16) == 20773);

    printf("the textbook RMC and GGA, merged into one fix\n");
    reset();
    feed("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n", 1000);
    CHECK(n_events == 0);   // waits for its GGA
    feed("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n", 2000);
    CHECK(n_events == 1 && events[0].kind == GNSS_EVT_FIX && events[0].us == 2000);
    struct gnss_fix *x = &events[0].fix;
    CHECK(x->utc == 764426119 && x->utc_ms == 0 && x->valid);
    CHECK(x->lat_e7 == 481173000 && x->lon_e7 == 115166667);
    CHECK(x->speed_cms == 1152 && x->course_cdeg == 8440);
    CHECK(x->quality == 1 && x->sats == 8 && x->hdop_c == 90 && x->alt_cm == 54540);
    gnss_get_status(&st, 3000);
    CHECK(st.present && st.fix && st.sentences == 2 && st.bad_sentences == 0 && !st.pps_locked);

    printf("south and west are negative; a sentence split across reads\n");
    reset();
    feed("$GPRMC,000000.50,A,3351.4370,S,15112.5580,W,1.0,270.0,010126,,,A*", 0);
    CHECK(n_events == 0);
    feed("5E\r\n", 10);
    gnss_get_status(&st, 20);
    CHECK(st.sentences == 1);
    gnss_poll(600000, on_event, NULL); // no GGA came: the fix goes on alone
    CHECK(n_events == 1 && events[0].fix.lat_e7 == -338572833 && events[0].fix.lon_e7 == -1512093000);
    CHECK(events[0].fix.utc == (uint32_t)gnss_days_from_civil(2026, 1, 1) * 86400 && events[0].fix.utc_ms == 500);
    CHECK(events[0].fix.quality == 1 && events[0].fix.sats == 0);

    printf("bad checksums, damaged and overlong lines are refused\n");
    reset();
    feed("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6B\r\n", 0);
    feed("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,\r\n", 0);
    feed("$GPRMC,1235$GPGGA,1\r\n", 0);
    char longline[200];
    memset(longline, 'A', sizeof(longline));
    longline[0] = '$';
    longline[sizeof(longline) - 2] = '\r';
    longline[sizeof(longline) - 1] = '\n';
    gnss_feed((const uint8_t *)longline, sizeof(longline), 0, on_event, NULL);
    gnss_get_status(&st, 0);
    CHECK(st.sentences == 0 && st.bad_sentences == 5 && n_events == 0);
    feed("garbage before $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n", 0);
    gnss_get_status(&st, 0);
    CHECK(st.sentences == 1);

    printf("pulses paired with time, and the local clock's drift measured\n");
    reset();
    // The local clock runs 50 ppm fast: a GPS second lasts 1000050 local microseconds.
    uint64_t edge = 0;
    for (int s = 0; s < 20; s++) {
        edge = 5000000 + (uint64_t)s * 1000050;
        gnss_pps(edge, on_event, NULL);
        rmc_at(s, edge + 80000);
        gnss_poll(edge + 700000, on_event, NULL);
    }
    int pps = 0, fixes = 0;
    for (int i = 0; i < n_events; i++) {
        if (events[i].kind == GNSS_EVT_PPS) {
            CHECK(events[i].utc == 764426119u + (uint32_t)pps);
            CHECK(events[i].us == 5000000 + (uint64_t)pps * 1000050);
            pps++;
        } else {
            fixes++;
        }
    }
    CHECK(pps == 20 && fixes == 20);
    gnss_get_status(&st, edge + 900000);
    CHECK(st.pps_locked && st.paired == 20 && st.pulses == 20 && st.drift_valid);
    CHECK(abs(st.drift_ppb - 50000) <= 50);
    uint64_t utc_us = 0;
    CHECK(gnss_local_to_utc(edge + 500025, &utc_us));
    CHECK(utc_us == (764426119ull + 19) * 1000000 + 500000);   // half a true second after the last pulse
    gnss_get_status(&st, edge + 4000000);
    CHECK(!st.pps_locked && !st.present);

    printf("pulse jitter averages out over the baseline\n");
    reset();
    for (int s = 0; s < 90; s++) {
        uint64_t e = 5000000 + (uint64_t)s * 1000010 + (s % 3 == 0 ? 3 : 0);   // 10 ppm, 3 us of jitter
        gnss_pps(e, on_event, NULL);
        rmc_at(s, e + 80000);
    }
    gnss_get_status(&st, 5000000 + 89ull * 1000010 + 100000);
    CHECK(st.drift_valid && abs(st.drift_ppb - 10000) <= 50);

    printf("a pulse with no time sentence goes on unpaired\n");
    reset();
    for (int s = 0; s < 20; s++) {
        edge = 5000000 + (uint64_t)s * 1000050;
        gnss_pps(edge, on_event, NULL);
        rmc_at(s, edge + 80000);
        gnss_poll(edge + 700000, on_event, NULL);
    }
    n_events = 0;
    edge += 1000050;
    gnss_pps(edge, on_event, NULL);
    gnss_poll(edge + 1000000, on_event, NULL);
    CHECK(n_events == 0);
    gnss_poll(edge + 1200000, on_event, NULL);
    CHECK(n_events == 1 && events[0].kind == GNSS_EVT_PPS && events[0].utc == 0 && events[0].us == edge);
    // ...and so does one followed straight away by the next pulse
    n_events = 0;
    gnss_pps(edge + 1000050, on_event, NULL);
    gnss_pps(edge + 2000100, on_event, NULL);
    CHECK(n_events == 1 && events[0].utc == 0 && events[0].us == edge + 1000050);

    printf("a time sentence that does not follow from the lock is refused, three in a row re-lock\n");
    // The pulse waiting is GPS second 22 (edge + 2000100). Say second 40 instead, three times.
    uint64_t p = edge + 2000100;
    n_events = 0;
    rmc_at(40, p + 80000);
    gnss_get_status(&st, p + 90000);
    CHECK(st.mismatched == 1 && st.paired == 20);
    for (int k = 1; k <= 3; k++) {
        p += 1000050;
        gnss_pps(p, on_event, NULL);
        rmc_at(40 + k, p + 80000);
    }
    gnss_get_status(&st, p + 90000);
    CHECK(st.mismatched == 3 && st.paired == 21 && st.pps_us == p && st.pps_utc == 764426119u + 43);
    CHECK(!st.drift_valid);   // started again: the drift is measured afresh

    printf("a sentence long after the pulse, or a fraction of a second, does not pair\n");
    reset();
    gnss_pps(1000000, on_event, NULL);
    rmc_at(0, 2100000);
    say("GPRMC,123519.10,A,4807.038,N,01131.000,E,0.0,0.0,230394,,,A", 1100000);
    gnss_get_status(&st, 2100000);
    CHECK(st.paired == 0);

    printf("ZDA gives the date and pairs too\n");
    reset();
    gnss_pps(1000000, on_event, NULL);
    say("GPZDA,235959.00,31,12,2025,00,00", 1050000);
    gnss_get_status(&st, 1060000);
    CHECK(st.paired == 1 && st.pps_utc == (uint32_t)gnss_days_from_civil(2026, 1, 1) * 86400 - 1);

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
