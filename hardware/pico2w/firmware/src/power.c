#include "power.h"

#include <string.h>

#define NO_BATTERY_MV 6000   // below this there is no battery at the OBD port: running from USB

static struct power_settings settings = {
    .idle_s = 300, .cutoff_mv = 12000, .running_mv = 13200, .low_battery_s = 60, .stay_awake = false,
};
static struct power_status status;
static uint32_t low_since, booted_at;
static bool low, asked;

static bool within(uint32_t now, uint32_t then, uint32_t seconds)
{
    return then && (uint32_t)(now - then) < seconds * 1000u;
}

void power_init(enum power_wake wake, uint32_t now_ms)
{
    memset(&status, 0, sizeof(status));
    status.wake = wake;
    booted_at = now_ms ? now_ms : 1;
    low = asked = false;
}

struct power_settings *power_settings(void)
{
    return &settings;
}

void power_request_sleep(void)
{
    asked = true;
}

enum power_sleep_reason power_poll(const struct power_inputs *in)
{
    uint32_t now = in->now_ms;
    bool on_battery = in->vbat_mv >= NO_BATTERY_MV;
    bool running = on_battery && in->vbat_mv >= settings.running_mv;
    uint8_t h = 0;

    if (in->usb_host)
        h |= POWER_HOLD_USB;
    if (in->phone)
        h |= POWER_HOLD_PHONE;
    // Waking counts as activity: whatever woke it gets one idle period.
    if (within(now, in->user_ms, settings.idle_s) || within(now, booted_at, settings.idle_s))
        h |= POWER_HOLD_ACTIVITY;
    if (running)
        h |= POWER_HOLD_ENGINE;
    bool traffic = within(now, in->bus_ms, settings.idle_s);
    if (traffic)
        h |= POWER_HOLD_BUS;
    if (in->logging && (running || traffic))
        h |= POWER_HOLD_LOGGING;
    if (settings.stay_awake)
        h |= POWER_HOLD_STAY;
    status.holds = h;
    status.vbat_mv = in->vbat_mv;

    // Battery protection comes first: a low battery with the engine off sleeps whatever else is going
    // on, except on USB, where a person is at the bench.
    if (on_battery && !running && in->vbat_mv < settings.cutoff_mv && !in->usb_host) {
        if (!low) {
            low = true;
            low_since = now;
        }
        status.low_battery_for_s = (now - low_since) / 1000u;
        if (status.low_battery_for_s >= settings.low_battery_s)
            return status.sleeping = POWER_SLEEP_BATTERY;
    } else {
        low = false;
        status.low_battery_for_s = 0;
    }

    if (asked && !in->usb_host)
        return status.sleeping = POWER_SLEEP_ASKED;
    asked = false;

    // Without USB or a battery there is nothing to sleep from (and nothing to wake it); never sleep then.
    if (h || !on_battery) {
        status.sleep_in_s = 0;
        // the activity window is the one that runs out on its own: show when
        if (h == POWER_HOLD_ACTIVITY) {
            uint32_t last = in->user_ms && (int32_t)(in->user_ms - booted_at) > 0 ? in->user_ms : booted_at;
            uint32_t gone = (now - last) / 1000u;
            status.sleep_in_s = gone < settings.idle_s ? settings.idle_s - gone : 0;
        }
        return status.sleeping = POWER_SLEEP_NONE;
    }
    // Nothing holds it: every timed reason has already run out its idle period, so sleep now.
    return status.sleeping = POWER_SLEEP_IDLE;
}

void power_get_status(struct power_status *out)
{
    *out = status;
}

const char *power_wake_name(enum power_wake w)
{
    static const char *names[] = { "unknown", "power-on", "usb", "can", "engine", "button" };
    return (unsigned)w < sizeof(names) / sizeof(names[0]) ? names[w] : "unknown";
}

const char *power_sleep_name(enum power_sleep_reason r)
{
    static const char *names[] = { "awake", "idle", "battery low", "asked" };
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}
