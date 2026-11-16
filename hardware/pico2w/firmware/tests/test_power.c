// Host tests for the power manager: when the dongle sleeps, and what keeps it awake.
#include <stdio.h>
#include <string.h>

#include "../src/power.h"

static int failures, checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

#define S(x) ((uint32_t)(x) * 1000u)

static struct power_inputs parked(uint32_t now)
{
    struct power_inputs in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.vbat_mv = 12500;   // battery present, engine off
    return in;
}

int main(void)
{
    struct power_status st;
    struct power_inputs in;

    printf("woken and left alone: one idle period, then sleep\n");
    power_init(POWER_WAKE_CAN, S(1));
    in = parked(S(2));
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    power_get_status(&st);
    CHECK(st.holds == POWER_HOLD_ACTIVITY && st.wake == POWER_WAKE_CAN && st.sleep_in_s > 290);
    in = parked(S(300));
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in = parked(S(302));
    CHECK(power_poll(&in) == POWER_SLEEP_IDLE);

    printf("a person using it keeps it awake; their last request starts the idle time\n");
    power_init(POWER_WAKE_BUTTON, S(1));
    in = parked(S(250));
    in.user_ms = S(250);
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.now_ms = S(500);
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    power_get_status(&st);
    CHECK(st.sleep_in_s == 50);
    in.now_ms = S(551);
    CHECK(power_poll(&in) == POWER_SLEEP_IDLE);

    printf("phone, USB, engine running and stay-awake each hold it\n");
    power_init(POWER_WAKE_POWER_ON, S(1));
    in = parked(S(5000));
    in.phone = true;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.phone = false;
    in.usb_host = true;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.usb_host = false;
    in.vbat_mv = 14100;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    power_get_status(&st);
    CHECK(st.holds == POWER_HOLD_ENGINE);
    in.vbat_mv = 12500;
    power_settings()->stay_awake = true;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    power_settings()->stay_awake = false;
    CHECK(power_poll(&in) == POWER_SLEEP_IDLE);

    printf("its own polls do not hold it; bus traffic does, until it stops\n");
    power_init(POWER_WAKE_CAN, S(1));
    in = parked(S(1000));
    in.logging = true;       // logging with nothing to record: does not count
    CHECK(power_poll(&in) == POWER_SLEEP_IDLE);
    in.bus_ms = S(990);
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    power_get_status(&st);
    CHECK(st.holds == (POWER_HOLD_BUS | POWER_HOLD_LOGGING));
    in.now_ms = S(1291);
    CHECK(power_poll(&in) == POWER_SLEEP_IDLE);

    printf("low battery with the engine off sleeps after a minute, whatever else holds it\n");
    power_init(POWER_WAKE_CAN, S(1));
    in = parked(S(10));
    in.vbat_mv = 11800;
    in.phone = true;
    in.user_ms = S(10);
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.now_ms = S(69);
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    power_get_status(&st);
    CHECK(st.low_battery_for_s == 59);
    in.now_ms = S(71);
    CHECK(power_poll(&in) == POWER_SLEEP_BATTERY);
    printf("...but a dip that recovers starts the count again, and USB or a running engine never trip it\n");
    power_init(POWER_WAKE_CAN, S(1));
    in = parked(S(10));
    in.vbat_mv = 11800;
    in.phone = true;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.now_ms = S(50);
    in.vbat_mv = 12300;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.now_ms = S(60);
    in.vbat_mv = 11800;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.now_ms = S(100);
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.usb_host = true;
    in.now_ms = S(500);
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.usb_host = false;
    in.vbat_mv = 13900;
    in.now_ms = S(900);
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);

    printf("USB alone, no battery: never sleeps (nothing would wake it)\n");
    power_init(POWER_WAKE_USB, S(1));
    in = parked(S(100000));
    in.vbat_mv = 0;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);

    printf("asked to sleep: sleeps, except on USB\n");
    power_init(POWER_WAKE_CAN, S(1));
    in = parked(S(5));
    in.phone = true;
    power_request_sleep();
    CHECK(power_poll(&in) == POWER_SLEEP_ASKED);
    power_init(POWER_WAKE_CAN, S(1));
    in.usb_host = true;
    power_request_sleep();
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);
    in.usb_host = false;
    CHECK(power_poll(&in) == POWER_SLEEP_NONE);   // the request does not linger

    CHECK(!strcmp(power_wake_name(POWER_WAKE_ENGINE), "engine"));
    CHECK(!strcmp(power_sleep_name(POWER_SLEEP_BATTERY), "battery low"));

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
