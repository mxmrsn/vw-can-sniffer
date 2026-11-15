#include "ble_link.h"

#include <string.h>

struct ble_link_info ble_info;

// Free-running indices; producer owns `in`, consumer owns `out`.
struct ring {
    uint32_t in, out;
};

static uint8_t rx_buf[BLE_RX_BUFFER], tx_buf[BLE_TX_BUFFER];
static struct ring rx, tx;
static uint32_t session, session_seen, session_mark;

static uint32_t load(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void copy_in(uint8_t *buf, uint32_t size, uint32_t at, const uint8_t *data, size_t len)
{
    uint32_t pos = at & (size - 1), first = size - pos;
    if (first > len)
        first = (uint32_t)len;
    memcpy(buf + pos, data, first);
    memcpy(buf, data + first, len - first);
}

static void copy_out(const uint8_t *buf, uint32_t size, uint32_t at, uint8_t *out, size_t len)
{
    uint32_t pos = at & (size - 1), first = size - pos;
    if (first > len)
        first = (uint32_t)len;
    memcpy(out, buf + pos, first);
    memcpy(out + first, buf, len - first);
}

// ---- Bluetooth stack side
size_t ble_link_rx_push(const uint8_t *data, size_t len)
{
    uint32_t in = rx.in, room = BLE_RX_BUFFER - (in - load(&rx.out));
    size_t n = len < room ? len : room;
    copy_in(rx_buf, BLE_RX_BUFFER, in, data, n);
    store(&rx.in, in + (uint32_t)n);
    ble_info.rx_bytes += (uint32_t)n;
    ble_info.rx_overflow += (uint32_t)(len - n);
    return n;
}

size_t ble_link_tx_pending(void)
{
    return load(&tx.in) - tx.out;
}

size_t ble_link_tx_take(uint8_t *out, size_t max)
{
    uint32_t o = tx.out, avail = load(&tx.in) - o;
    size_t n = avail < max ? avail : max;
    copy_out(tx_buf, BLE_TX_BUFFER, o, out, n);
    store(&tx.out, o + (uint32_t)n);
    ble_info.tx_bytes += (uint32_t)n;
    return n;
}

void ble_link_tx_discard(void)
{
    store(&tx.out, load(&tx.in));
}

void ble_link_new_session(void)
{
    // Everything received so far belongs to the previous connection. Mark the
    // boundary rather than moving `out`, which only the main loop may touch.
    store(&session_mark, rx.in);
    __atomic_add_fetch(&session, 1, __ATOMIC_RELEASE);
}

// ---- main loop side
bool ble_link_write(const uint8_t *packet, size_t len, bool is_event)
{
    uint32_t in = tx.in, queued = in - load(&tx.out), room = BLE_TX_BUFFER - queued;
    // An event waits its turn behind everything already queued, so letting events
    // fill the buffer would push replies minutes into the future on a slow link.
    if (is_event && queued + len > BLE_TX_EVENT_BACKLOG) {
        ble_info.tx_refused++;
        return false;
    }
    if (!ble_info.subscribed || len > room) {
        ble_info.tx_refused++;
        return false;
    }
    copy_in(tx_buf, BLE_TX_BUFFER, in, packet, len);
    store(&tx.in, in + (uint32_t)len);
    return true;
}

bool ble_link_rx_pop(uint8_t *byte)
{
    uint32_t o = rx.out;
    if (load(&rx.in) == o)
        return false;
    *byte = rx_buf[o & (BLE_RX_BUFFER - 1)];
    store(&rx.out, o + 1);
    return true;
}

bool ble_link_session_changed(void)
{
    uint32_t now = __atomic_load_n(&session, __ATOMIC_ACQUIRE);
    if (now == session_seen)
        return false;
    session_seen = now;
    uint32_t mark = load(&session_mark);
    if ((int32_t)(mark - rx.out) > 0)
        store(&rx.out, mark); // drop what the previous connection left, but nothing newer
    return true;
}
