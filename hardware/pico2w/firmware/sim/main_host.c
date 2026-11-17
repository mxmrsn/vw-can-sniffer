// Host build of the firmware: the same application core, run against the
// simulated bus in sim_bus.c, with the console on stdin/stdout.
//
//   make -C hardware/pico2w/firmware/sim && hardware/pico2w/firmware/sim/vw_diag_sim
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>

#include "../src/app.h"
#include "../src/can_port.h"
#include "../src/platform.h"
#include "sim.h"

uint32_t plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int plat_getchar(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 0)
        _exit(0); // console closed
    return n == 1 ? c : -1;
}

void plat_write(const uint8_t *data, size_t len)
{
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

void plat_led(enum plat_led led, bool on)
{
    (void)led;
    (void)on;
}

void plat_idle(void)
{
    usleep(200);
}

void plat_selftest(struct selftest_report *r, unsigned flags)
{
    (void)flags;
    selftest_add(r, "board", SELFTEST_PASS, "simulator (vw_diag_sim), standing in for the OBD dongle");
}

// ---- power: the simulated dongle sleeps like the real one. The battery voltage is the car's
// (the engine model's), VWDIAG_USB=0 means no computer on USB (the default is one: the console),
// and VWDIAG_WAKE names the wake reason (power-on, usb, can, engine, button).
uint16_t plat_battery_mv(void)
{
    return (uint16_t)(engine()->volts * 1000);
}

bool plat_usb_host(void)
{
    const char *v = getenv("VWDIAG_USB");
    return !v || strcmp(v, "0");
}

enum power_wake plat_wake_reason(void)
{
    const char *v = getenv("VWDIAG_WAKE");
    for (int w = 0; v && w <= POWER_WAKE_BUTTON; w++)
        if (!strcmp(v, power_wake_name((enum power_wake)w)))
            return (enum power_wake)w;
    return POWER_WAKE_POWER_ON;
}

// Settings kept in the file VWDIAG_SETTINGS names (none: kept in memory only, like a fresh board).
bool plat_settings_load(struct power_settings *s)
{
    const char *path = getenv("VWDIAG_SETTINGS");
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f)
        return false;
    unsigned long idle = 0;
    unsigned cutoff = 0, stay = 0;
    bool ok = fscanf(f, "idle %lu cutoff %u stay %u", &idle, &cutoff, &stay) == 3;
    fclose(f);
    if (ok) {
        s->idle_s = (uint32_t)idle;
        s->cutoff_mv = (uint16_t)cutoff;
        s->stay_awake = stay != 0;
    }
    return ok;
}

void plat_settings_save(const struct power_settings *s)
{
    const char *path = getenv("VWDIAG_SETTINGS");
    FILE *f = path && *path ? fopen(path, "w") : NULL;
    if (!f)
        return;
    fprintf(f, "idle %lu cutoff %u stay %u\n", (unsigned long)s->idle_s, s->cutoff_mv, s->stay_awake ? 1u : 0u);
    fclose(f);
}

void plat_power_sleep(enum power_sleep_reason why)
{
    // The dongle's power goes off; here the process ends, which a connected app sees as the link dropping.
    fprintf(stderr, "power: asleep (%s)\n", power_sleep_name(why));
    exit(3);
}

int main(void)
{
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    setvbuf(stdout, NULL, _IONBF, 0);
    can_port_init(500000);
    app_init();
    for (;;)
        app_poll();
}
