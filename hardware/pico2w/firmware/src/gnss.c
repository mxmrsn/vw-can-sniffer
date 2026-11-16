#include "gnss.h"

#include <string.h>

#define LINE_MAX_LEN 100            // NMEA allows 82; some receivers run a little over
#define MAX_FIELDS 24
#define PRESENT_US 3000000ull       // "present" and "locked" mean within the last 3 s
#define PAIR_WINDOW_US 1000000ull   // a time sentence pairs with a pulse less than 1 s before it
#define PPS_GIVE_UP_US 1100000ull   // a pulse with no time sentence by now goes on unpaired
#define FIX_WAIT_US 500000ull       // an RMC waits this long for its GGA
#define DRIFT_LIMIT_PPB 1000000     // 1000 ppm: anything faster is a bad pulse, not a clock
#define MISMATCH_RESET 3            // this many disagreeing pairings in a row: start again
#define DRIFT_SPAN 64               // drift is measured over up to this many paired pulses: a pulse
                                    // stamp is good to a microsecond or two, so one second alone
                                    // would only give the rate to 1-2 ppm

static struct {
    char line[LINE_MAX_LEN + 1];
    size_t len;
    bool in_line;

    bool have_date;
    int32_t date_days;

    struct gnss_fix pending; // an RMC waiting for its GGA
    bool pending_rmc;
    uint32_t pending_tod;
    uint64_t pending_us;
    struct { bool have; uint32_t tod; uint8_t quality, sats; int32_t alt_cm; uint16_t hdop_c; } gga;

    bool pps_waiting;
    uint64_t pps_wait_us;
    bool paired_any;
    uint64_t pair_us;
    uint32_t pair_utc;
    int mismatch_run;
    struct { uint64_t us; uint32_t utc; } hist[DRIFT_SPAN]; // recent pairings, oldest at hist_first
    int hist_n, hist_first;
    int64_t drift_ppb;
    bool drift_valid;

    bool any_sentence;
    uint64_t sentence_us;
    struct gnss_status st;
} g;

void gnss_init(void)
{
    memset(&g, 0, sizeof(g));
}

int32_t gnss_days_from_civil(int32_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

// ---- field parsing

static bool digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

static unsigned num2(const char *s)
{
    return (unsigned)(s[0] - '0') * 10 + (unsigned)(s[1] - '0');
}

// A decimal number scaled by 10^decimals (extra decimals are dropped). False if empty or not a number.
static bool parse_fixed(const char *s, int decimals, int64_t *out)
{
    bool neg = false, any = false;
    int64_t v = 0;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    for (; *s >= '0' && *s <= '9'; s++, any = true)
        v = v * 10 + (*s - '0');
    int got = 0;
    if (*s == '.')
        for (s++; *s >= '0' && *s <= '9'; s++, any = true)
            if (got < decimals) {
                v = v * 10 + (*s - '0');
                got++;
            }
    if (!any || *s)
        return false;
    for (; got < decimals; got++)
        v *= 10;
    *out = neg ? -v : v;
    return true;
}

// hhmmss[.sss] to milliseconds of the day.
static bool parse_tod(const char *s, uint32_t *ms)
{
    if (strlen(s) < 6 || !digits(s, 6))
        return false;
    unsigned h = num2(s), m = num2(s + 2), sec = num2(s + 4);
    if (h > 23 || m > 59 || sec > 60)
        return false;
    uint32_t frac = 0;
    if (s[6] == '.') {
        int64_t f;
        char buf[8] = "0";
        strncpy(buf + 1, s + 6, sizeof(buf) - 2);
        if (!parse_fixed(buf, 3, &f))
            return false;
        frac = (uint32_t)f;
    } else if (s[6]) {
        return false;
    }
    *ms = ((h * 60 + m) * 60 + sec) * 1000 + frac;
    return true;
}

// (d)ddmm.mmmm plus a hemisphere letter to degrees x 10^7.
static bool parse_angle(const char *s, const char *hemi, int32_t *out)
{
    int64_t v; // minutes-and-degrees number, scaled by 10^7
    if (!parse_fixed(s, 7, &v) || v < 0 || !hemi[0] || hemi[1])
        return false;
    int64_t whole = v / 10000000; // dddmm
    int64_t deg = whole / 100;
    int64_t min_e7 = v - deg * 100 * 10000000;
    if (min_e7 >= 60ll * 10000000)
        return false;
    int64_t e7 = deg * 10000000 + (min_e7 + 30) / 60;
    if (*hemi == 'S' || *hemi == 'W')
        e7 = -e7;
    else if (*hemi != 'N' && *hemi != 'E')
        return false;
    *out = (int32_t)e7;
    return true;
}

static uint16_t clamp16(int64_t v)
{
    return v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : (uint16_t)v;
}

// ---- events

static void emit_fix(uint64_t us, gnss_cb cb, void *ctx)
{
    g.st.last = g.pending;
    g.st.fix = g.pending.valid;
    g.pending_rmc = false;
    if (cb) {
        struct gnss_event e = { .kind = GNSS_EVT_FIX, .us = us, .fix = g.pending };
        cb(&e, ctx);
    }
}

static void emit_pps(uint64_t edge_us, uint32_t utc, gnss_cb cb, void *ctx)
{
    if (cb) {
        struct gnss_event e = { .kind = GNSS_EVT_PPS, .us = edge_us, .utc = utc };
        cb(&e, ctx);
    }
}

static void merge_gga(void)
{
    g.pending.quality = g.gga.quality;
    g.pending.sats = g.gga.sats;
    g.pending.alt_cm = g.gga.alt_cm;
    g.pending.hdop_c = g.gga.hdop_c;
}

// A time sentence for a whole second, read at now_us: pair it with the pulse waiting for it.
static void on_whole_second(uint32_t utc, uint64_t now_us, gnss_cb cb, void *ctx)
{
    if (!g.pps_waiting || now_us < g.pps_wait_us || now_us - g.pps_wait_us >= PAIR_WINDOW_US)
        return;
    uint64_t edge = g.pps_wait_us;
    if (g.paired_any && g.mismatch_run < MISMATCH_RESET) {
        // The second must follow from the last pairing; otherwise the sentence belongs to another pulse.
        uint32_t expected = g.pair_utc + (uint32_t)((edge - g.pair_us + 500000) / 1000000);
        if (utc != expected) {
            g.st.mismatched++;
            g.mismatch_run++;
            return;
        }
    }
    if (g.mismatch_run >= MISMATCH_RESET) { // three in a row: the old pairing was wrong; start again
        g.paired_any = false;
        g.drift_valid = false;
        g.hist_n = 0;
    }
    g.mismatch_run = 0;
    if (g.hist_n && utc <= g.hist[(g.hist_first + g.hist_n - 1) % DRIFT_SPAN].utc)
        g.hist_n = 0; // time went backwards: measure afresh
    if (g.hist_n == DRIFT_SPAN) {
        g.hist_first = (g.hist_first + 1) % DRIFT_SPAN;
        g.hist_n--;
    }
    int last = (g.hist_first + g.hist_n) % DRIFT_SPAN;
    g.hist[last].us = edge;
    g.hist[last].utc = utc;
    g.hist_n++;
    if (g.hist_n >= 2) {
        int64_t n = utc - g.hist[g.hist_first].utc;
        int64_t ppb = ((int64_t)(edge - g.hist[g.hist_first].us) - n * 1000000) * 1000 / n;
        if (ppb > -DRIFT_LIMIT_PPB && ppb < DRIFT_LIMIT_PPB) {
            g.drift_ppb = ppb;
            g.drift_valid = true;
        }
    }
    g.paired_any = true;
    g.pair_us = edge;
    g.pair_utc = utc;
    g.pps_waiting = false;
    g.st.paired++;
    emit_pps(edge, utc, cb, ctx);
}

static void on_rmc(char **f, int n, uint64_t now_us, gnss_cb cb, void *ctx)
{
    uint32_t tod;
    if (n < 10 || !parse_tod(f[1], &tod))
        return;
    if (strlen(f[9]) == 6 && digits(f[9], 6)) {
        unsigned yy = num2(f[9] + 4);
        g.date_days = gnss_days_from_civil((int32_t)(yy < 80 ? 2000 + yy : 1900 + yy), num2(f[9] + 2), num2(f[9]));
        g.have_date = true;
    }
    if (g.pending_rmc)
        emit_fix(g.pending_us, cb, ctx); // its GGA never came
    struct gnss_fix *x = &g.pending;
    memset(x, 0, sizeof(*x));
    x->utc = g.have_date ? (uint32_t)(g.date_days * 86400 + (int32_t)(tod / 1000)) : 0;
    x->utc_ms = (uint16_t)(tod % 1000);
    x->valid = f[2][0] == 'A';
    parse_angle(f[3], f[4], &x->lat_e7);
    parse_angle(f[5], f[6], &x->lon_e7);
    int64_t v;
    if (parse_fixed(f[7], 3, &v)) // knots x 1000 -> cm/s
        x->speed_cms = clamp16(v * 514444 / 10000000);
    if (parse_fixed(f[8], 3, &v))
        x->course_cdeg = clamp16(v / 10);
    x->quality = x->valid ? 1 : 0;
    g.pending_rmc = true;
    g.pending_tod = tod;
    g.pending_us = now_us;
    if (g.gga.have && g.gga.tod == tod) {
        merge_gga();
        emit_fix(now_us, cb, ctx);
    }
    if (x->utc && tod % 1000 == 0)
        on_whole_second(x->utc, now_us, cb, ctx);
}

static void on_gga(char **f, int n, uint64_t now_us, gnss_cb cb, void *ctx)
{
    uint32_t tod;
    int64_t v;
    if (n < 10 || !parse_tod(f[1], &tod))
        return;
    g.gga.have = true;
    g.gga.tod = tod;
    g.gga.quality = parse_fixed(f[6], 0, &v) ? (uint8_t)v : 0;
    g.gga.sats = parse_fixed(f[7], 0, &v) ? (uint8_t)v : 0;
    g.gga.hdop_c = parse_fixed(f[8], 2, &v) ? clamp16(v) : 0;
    g.gga.alt_cm = parse_fixed(f[9], 2, &v) ? (int32_t)v : 0;
    if (g.pending_rmc && g.pending_tod == tod) {
        merge_gga();
        emit_fix(now_us, cb, ctx);
    }
}

static void on_zda(char **f, int n, uint64_t now_us, gnss_cb cb, void *ctx)
{
    uint32_t tod;
    if (n < 5 || !parse_tod(f[1], &tod) || strlen(f[2]) != 2 || strlen(f[3]) != 2 || strlen(f[4]) != 4 ||
        !digits(f[2], 2) || !digits(f[3], 2) || !digits(f[4], 4))
        return;
    int32_t year = (int32_t)(num2(f[4]) * 100 + num2(f[4] + 2));
    g.date_days = gnss_days_from_civil(year, num2(f[3]), num2(f[2]));
    g.have_date = true;
    if (tod % 1000 == 0)
        on_whole_second((uint32_t)(g.date_days * 86400 + (int32_t)(tod / 1000)), now_us, cb, ctx);
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void on_line(uint64_t now_us, gnss_cb cb, void *ctx)
{
    // $<body>*hh
    char *star = strrchr(g.line, '*');
    if (!star || hexval(star[1]) < 0 || hexval(star[2]) < 0 || star[3] != 0) {
        g.st.bad_sentences++;
        return;
    }
    uint8_t sum = 0;
    for (char *p = g.line; p < star; p++)
        sum ^= (uint8_t)*p;
    if (sum != (uint8_t)(hexval(star[1]) << 4 | hexval(star[2]))) {
        g.st.bad_sentences++;
        return;
    }
    *star = 0;
    char *f[MAX_FIELDS];
    int n = 0;
    for (char *p = g.line; n < MAX_FIELDS;) {
        f[n++] = p;
        p = strchr(p, ',');
        if (!p)
            break;
        *p++ = 0;
    }
    g.st.sentences++;
    g.any_sentence = true;
    g.sentence_us = now_us;
    if (strlen(f[0]) != 5)
        return;
    const char *type = f[0] + 2;
    for (int i = n; i < MAX_FIELDS; i++)
        f[i] = (char *)"";   // missing trailing fields read as empty
    if (!strcmp(type, "RMC"))
        on_rmc(f, n, now_us, cb, ctx);
    else if (!strcmp(type, "GGA"))
        on_gga(f, n, now_us, cb, ctx);
    else if (!strcmp(type, "ZDA"))
        on_zda(f, n, now_us, cb, ctx);
}

void gnss_feed(const uint8_t *data, size_t len, uint64_t now_us, gnss_cb cb, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '$') {
            if (g.in_line)
                g.st.bad_sentences++; // cut short
            g.in_line = true;
            g.len = 0;
        } else if (!g.in_line) {
            continue;
        } else if (c == '\r' || c == '\n') {
            g.line[g.len] = 0;
            g.in_line = false;
            on_line(now_us, cb, ctx);
        } else if (g.len >= LINE_MAX_LEN || c < 0x20 || c > 0x7E) {
            g.in_line = false;
            g.st.bad_sentences++;
        } else {
            g.line[g.len++] = c;
        }
    }
}

void gnss_pps(uint64_t edge_us, gnss_cb cb, void *ctx)
{
    g.st.pulses++;
    if (g.pps_waiting)
        emit_pps(g.pps_wait_us, 0, cb, ctx); // the one before never found its time sentence
    g.pps_waiting = true;
    g.pps_wait_us = edge_us;
}

void gnss_poll(uint64_t now_us, gnss_cb cb, void *ctx)
{
    if (g.pps_waiting && now_us >= g.pps_wait_us && now_us - g.pps_wait_us > PPS_GIVE_UP_US) {
        g.pps_waiting = false;
        emit_pps(g.pps_wait_us, 0, cb, ctx);
    }
    if (g.pending_rmc && now_us >= g.pending_us && now_us - g.pending_us > FIX_WAIT_US)
        emit_fix(g.pending_us, cb, ctx);
}

void gnss_get_status(struct gnss_status *out, uint64_t now_us)
{
    g.st.present = g.any_sentence && now_us - g.sentence_us < PRESENT_US;
    g.st.pps_locked = g.paired_any && now_us - g.pair_us < PRESENT_US;
    g.st.drift_valid = g.drift_valid;
    g.st.drift_ppb = (int32_t)g.drift_ppb;
    g.st.pps_us = g.pair_us;
    g.st.pps_utc = g.pair_utc;
    if (!g.st.present)
        g.st.fix = false;
    *out = g.st;
}

bool gnss_local_to_utc(uint64_t local_us, uint64_t *utc_us)
{
    if (!g.paired_any)
        return false;
    int64_t dt = (int64_t)(local_us - g.pair_us);
    if (g.drift_valid) // dt / (1 + drift): the local clock's microseconds to true ones
        dt -= dt * g.drift_ppb / (1000000000 + g.drift_ppb);
    *utc_us = (uint64_t)g.pair_utc * 1000000 + (uint64_t)dt;
    return true;
}
