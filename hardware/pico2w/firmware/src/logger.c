#include "logger.h"

#include <string.h>

#include "gnss.h"
#include "storage.h"

#define BUFFER_SIZE 8192
#define FLUSH_AT 4096       // write once this much is waiting...
#define FLUSH_PERIOD_MS 250 // ...or this often when there is anything at all
#define SYNC_PERIOD_MS 2000 // at most this much is lost if power is pulled

static uint8_t buffer[BUFFER_SIZE];
static size_t buffered;
static struct logger_stats stats;
static uint32_t pending_lost, seen_dropped;
static uint32_t flushed_at, synced_at;
static bool unsynced;
static uint32_t epoch_base, epoch_base_ms; // 0 = time not known
static bool have_time;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Reserve room for a record and write its type and time. NULL if the buffer is full.
static uint8_t *record(uint8_t type, uint32_t time_ms, size_t payload_len)
{
    if (buffered + 5 + payload_len > BUFFER_SIZE)
        return NULL;
    uint8_t *p = buffer + buffered;
    p[0] = type;
    put_u32(p + 1, time_ms);
    buffered += 5 + payload_len;
    return p + 5;
}

static void fail(void)
{
    storage_close();
    stats.active = false;
    buffered = 0;
}

static void flush(uint32_t now_ms)
{
    if (buffered) {
        if (!storage_write(buffer, buffered)) {
            fail();
            return;
        }
        stats.bytes += (uint32_t)buffered;
        buffered = 0;
        unsynced = true;
    }
    flushed_at = now_ms;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t epoch_at(uint32_t now_ms)
{
    return have_time ? epoch_base + (now_ms - epoch_base_ms) / 1000 : 0;
}

bool logger_start(uint32_t now_ms, uint32_t bitrate, uint32_t rx_dropped_total)
{
    if (stats.active)
        return true;
    char name[sizeof(stats.name)];
    if (!storage_create_log(name, sizeof(name)))
        return false;
    memset(&stats, 0, sizeof(stats));
    memcpy(stats.name, name, sizeof(stats.name));
    stats.active = true;
    buffered = 0;
    pending_lost = 0;
    seen_dropped = rx_dropped_total;
    flushed_at = synced_at = now_ms;

    uint8_t *h = buffer;
    memset(h, 0, LOG_HEADER_LEN);
    memcpy(h, "VWLG", 4);
    h[4] = LOG_VERSION;
    h[5] = LOG_HEADER_LEN;
    put_u32(h + 8, bitrate);
    put_u32(h + 12, now_ms);
    put_u32(h + 16, epoch_at(now_ms));
    buffered = LOG_HEADER_LEN;
    flush(now_ms);
    if (stats.active && !storage_sync())
        fail();
    return stats.active;
}

void logger_stop(void)
{
    if (!stats.active)
        return;
    flush(flushed_at);
    if (stats.active) {
        storage_close();
        stats.active = false;
    }
}

bool logger_active(void)
{
    return stats.active;
}

void logger_can(const struct can_frame *f, uint32_t time_ms, bool sent)
{
    if (!stats.active)
        return;
    uint8_t dlc = f->dlc > 8 ? 8 : f->dlc;
    // Report earlier losses first, so the gap sits in the right place in the file.
    if (pending_lost) {
        uint8_t *p = record(LOG_REC_LOST, time_ms, 4);
        if (!p) {
            pending_lost++;
            stats.lost++;
            return;
        }
        put_u32(p, pending_lost);
        pending_lost = 0;
    }
    uint8_t *p = record(LOG_REC_CAN, time_ms, 5 + dlc);
    if (!p) {
        pending_lost++;
        stats.lost++;
        return;
    }
    put_u32(p, f->id);
    p[4] = (uint8_t)(dlc | (sent ? 0x40 : 0) | (f->ext ? 0x80 : 0));
    memcpy(p + 5, f->data, dlc);
    stats.records++;
}

void logger_mark(const char *text, uint32_t time_ms)
{
    if (!stats.active)
        return;
    size_t len = strlen(text);
    if (len > 200)
        len = 200;
    uint8_t *p = record(LOG_REC_MARK, time_ms, 1 + len);
    if (p) {
        p[0] = (uint8_t)len;
        memcpy(p + 1, text, len);
    }
}

void logger_pps(uint64_t edge_us, uint32_t utc)
{
    if (!stats.active)
        return;
    uint8_t *p = record(LOG_REC_PPS, (uint32_t)(edge_us / 1000), 12);
    if (p) {
        put_u64(p, edge_us);
        put_u32(p + 8, utc);
    }
}

void logger_fix(const struct gnss_fix *x, uint64_t read_us)
{
    if (!stats.active)
        return;
    uint8_t *p = record(LOG_REC_FIX, (uint32_t)(read_us / 1000), LOG_FIX_LEN);
    if (!p)
        return;
    put_u32(p, x->utc);
    put_u16(p + 4, x->utc_ms);
    p[6] = x->valid ? 1 : 0;
    p[7] = x->quality;
    p[8] = x->sats;
    put_u32(p + 9, (uint32_t)x->lat_e7);
    put_u32(p + 13, (uint32_t)x->lon_e7);
    put_u32(p + 17, (uint32_t)x->alt_cm);
    put_u16(p + 21, x->speed_cms);
    put_u16(p + 23, x->course_cdeg);
    put_u16(p + 25, x->hdop_c);
    put_u16(p + 27, 0); // reserved
}

void logger_sync(uint64_t edge_us, uint32_t count)
{
    if (!stats.active)
        return;
    uint8_t *p = record(LOG_REC_SYNC, (uint32_t)(edge_us / 1000), 12);
    if (p) {
        put_u64(p, edge_us);
        put_u32(p + 8, count);
    }
}

void logger_set_time(uint32_t epoch, uint32_t now_ms)
{
    epoch_base = epoch;
    epoch_base_ms = now_ms;
    have_time = true;
    if (stats.active) {
        uint8_t *p = record(LOG_REC_TIME, now_ms, 4);
        if (p)
            put_u32(p, epoch);
    }
}

void logger_poll(uint32_t now_ms, uint32_t rx_dropped_total)
{
    if (!stats.active)
        return;
    if (rx_dropped_total != seen_dropped) {
        uint32_t n = rx_dropped_total - seen_dropped;
        pending_lost += n;
        stats.lost += n;
        seen_dropped = rx_dropped_total;
    }
    if (buffered >= FLUSH_AT || (buffered && now_ms - flushed_at >= FLUSH_PERIOD_MS))
        flush(now_ms);
    if (stats.active && unsynced && now_ms - synced_at >= SYNC_PERIOD_MS) {
        if (!storage_sync()) {
            fail();
            return;
        }
        unsynced = false;
        synced_at = now_ms;
    }
}

void logger_get_stats(struct logger_stats *out)
{
    *out = stats;
}

// A plain 8.3 name in the top folder: no paths, nothing to escape.
static bool valid_name(const char *n)
{
    size_t base = 0, ext = 0;
    bool dot = false;
    for (; *n; n++) {
        char c = *n;
        if (c == '.' && !dot && base) {
            dot = true;
            continue;
        }
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
        if (dot ? ++ext > 3 : ++base > 8)
            return false;
    }
    return base > 0 && (!dot || ext > 0);
}

int logger_read_file(const char *name, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *size)
{
    if (!valid_name(name))
        return LOGGER_READ_BAD_NAME;
    if (stats.active) {
        // the file being recorded is still growing: stop logging first
        const char *a = name, *b = stats.name;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return LOGGER_READ_BUSY;
    }
    int n = storage_read_at(name, offset, buf, cap, size);
    return n < 0 ? LOGGER_READ_NO_CARD : n;
}
