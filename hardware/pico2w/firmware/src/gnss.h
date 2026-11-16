// GPS receiver input and time sync for the dongle's GPS / sync connector: NMEA sentences from an
// external receiver, its pulse-per-second (PPS) edge, and the local clock measured against GPS
// time. Pure logic, no hardware, so the host tests and the simulator run what the dongle runs.
//
// Time model: the platform stamps every PPS edge (and every sync-out edge) with the local
// microsecond clock, the same clock the millisecond log stamps come from (ms = us / 1000). Each
// pulse marks the start of a GPS second; the NMEA time sentence for that second arrives a little
// after it. Pairing the two gives (local us, UTC second) points: the absolute reference for the
// whole log, and, from one pulse to the next, how fast the local clock runs (its drift).
//
// Sentences understood (any talker: GP, GN, GL, GA, GB): RMC (time, date, position, speed,
// course), GGA (fix quality, satellites, HDOP, altitude) and ZDA (time and date). The receiver
// must send RMC or ZDA for time; checksums are required.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gnss_fix {
    uint32_t utc;         // Unix seconds of the fix (0 = the date is not known yet)
    uint16_t utc_ms;      // milliseconds within that second
    bool valid;           // RMC status A
    uint8_t quality;      // GGA: 0 none, 1 GPS, 2 differential, 4 RTK fixed, 5 RTK float, 6 estimated
    uint8_t sats;         // satellites used (GGA)
    int32_t lat_e7;       // degrees x 10^7, north positive
    int32_t lon_e7;       // degrees x 10^7, east positive
    int32_t alt_cm;       // above mean sea level (GGA)
    uint16_t speed_cms;   // over ground
    uint16_t course_cdeg; // over ground, 0.01 degree from true north
    uint16_t hdop_c;      // horizontal dilution of precision x 100 (GGA; 0 = not given)
};

enum gnss_event_kind { GNSS_EVT_FIX = 1, GNSS_EVT_PPS = 2 };

struct gnss_event {
    enum gnss_event_kind kind;
    uint64_t us;          // local time: the pulse edge, or when the fix's sentences were read
    uint32_t utc;         // PPS: the UTC second the pulse marks (0 = could not be paired)
    struct gnss_fix fix;  // FIX only
};

typedef void (*gnss_cb)(const struct gnss_event *e, void *ctx);

struct gnss_status {
    bool present;         // valid sentences within the last 3 s
    bool fix;             // the latest fix is valid
    bool pps_locked;      // a pulse paired with GPS time within the last 3 s
    bool drift_valid;
    int32_t drift_ppb;    // local clock rate against GPS, parts per billion: positive = local runs fast
    struct gnss_fix last; // the latest fix
    uint64_t pps_us;      // the latest paired pulse...
    uint32_t pps_utc;     // ...and the second it marks
    uint32_t sentences, bad_sentences, pulses, paired, mismatched;
};

void gnss_init(void);
// Bytes from the receiver, read at local time now_us. Calls cb for each complete fix.
void gnss_feed(const uint8_t *data, size_t len, uint64_t now_us, gnss_cb cb, void *ctx);
// A pulse edge at local time edge_us (as stamped by the platform when it happened).
void gnss_pps(uint64_t edge_us, gnss_cb cb, void *ctx);
// Call often: hands on pulses that found no time sentence, and fixes still waiting for a GGA.
void gnss_poll(uint64_t now_us, gnss_cb cb, void *ctx);
void gnss_get_status(struct gnss_status *out, uint64_t now_us);

// Local microseconds to UTC (Unix microseconds), from the latest pulse pair and the measured drift.
// False until a pulse has been paired.
bool gnss_local_to_utc(uint64_t local_us, uint64_t *utc_us);

// Days since 1970-01-01 of a calendar date (proleptic Gregorian). Exposed for the tests.
int32_t gnss_days_from_civil(int32_t y, unsigned m, unsigned d);
