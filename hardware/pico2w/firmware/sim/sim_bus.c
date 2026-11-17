// Host build of the bus: the simulated car (car.c) on a pretend CAN bus, with the
// fault injection a real bus and a real interface add (missed frames, error frames,
// arbitration delays, bus-off). Implements can_port.h, so the application core runs
// against it unchanged. The scenario comes from the VWSIM environment variable.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/can_port.h"
#include "../src/isotp.h"
#include "../src/platform.h"
#include "sim.h"

#define QUEUE_SIZE 512
#define HOLD_SIZE 64

static struct can_frame queue[QUEUE_SIZE];
static unsigned q_head, q_tail;
static struct { struct can_frame f; uint32_t due; } hold[HOLD_SIZE];   // frames held back by the fault layer
static int hold_count;
static bool silent = true, started;
static struct can_stats stats;
static uint32_t t0;

static uint32_t rel(uint32_t now)
{
    return now - t0;
}

static bool bus_off(uint32_t now)
{
    return sim_cfg.busoff_ms && rel(now) >= sim_cfg.busoff_at && rel(now) < sim_cfg.busoff_at + sim_cfg.busoff_ms;
}

static void enqueue(const struct can_frame *f)
{
    if (q_head - q_tail >= QUEUE_SIZE) {
        stats.rx_dropped++;
        return;
    }
    queue[q_head++ % QUEUE_SIZE] = *f;
}

static void hold_frame(const struct can_frame *f, uint32_t due)
{
    if (hold_count == HOLD_SIZE) {
        enqueue(f);
        return;
    }
    hold[hold_count].f = *f;
    hold[hold_count].due = due;
    hold_count++;
}

// Every frame on its way to the interface passes through here.
static int from_car(const struct can_frame *f)
{
    uint32_t now = plat_now_ms();
    if (bus_off(now))
        return 0;                       // the interface sees nothing while bus-off
    if (sim_cfg.drop_pct > 0 && sim_rand01() * 100 < sim_cfg.drop_pct) {
        stats.rx_dropped++;             // missed by the interface
        return 0;
    }
    // A frame of an ID already held back stays behind it: one sender never reorders its own frames.
    uint32_t after = 0;
    bool behind = false;
    for (int i = 0; i < hold_count; i++) {
        if (hold[i].f.id == f->id) {
            if (!behind || (int32_t)(hold[i].due - after) > 0)
                after = hold[i].due;
            behind = true;
        }
    }
    uint32_t due = behind ? after : now;
    if (sim_cfg.error_pct > 0 && sim_rand01() * 100 < sim_cfg.error_pct) {
        stats.errors++;                 // an error frame destroys it; the sender tries again
        due += 1;
    }
    if (sim_cfg.delay_pct > 0 && sim_rand01() * 100 < sim_cfg.delay_pct)
        due += (uint32_t)sim_cfg.delay_ms;   // lost arbitration to busier IDs for a while
    if (due != now || behind)
        hold_frame(f, due);
    else
        enqueue(f);
    return 0;
}

static void release_held(uint32_t now)
{
    // oldest first, so frames of one ID leave in the order they were sent
    for (int i = 0; i < hold_count;) {
        if ((int32_t)(now - hold[i].due) >= 0) {
            enqueue(&hold[i].f);
            memmove(&hold[i], &hold[i + 1], (size_t)(hold_count - i - 1) * sizeof(hold[0]));
            hold_count--;
        } else {
            i++;
        }
    }
}

static void step(void)
{
    uint32_t now = plat_now_ms();
    if (!started) {
        t0 = now;
        if (!car_init(getenv("VWSIM"), from_car)) {
            fprintf(stderr, "VWSIM: cannot use \"%s\"\n", getenv("VWSIM"));
            exit(2);
        }
        started = true;
    }
    car_poll();
    release_held(now);
}

void can_port_init(uint32_t bitrate)
{
    (void)bitrate;
}

void can_port_set_silent(bool on)
{
    silent = on;
}

bool can_port_is_silent(void)
{
    return silent;
}

bool can_port_receive(struct can_frame *out)
{
    step();
    if (q_tail == q_head)
        return false;
    *out = queue[q_tail++ % QUEUE_SIZE];
    stats.rx++;
    return true;
}

int can_port_send(const struct can_frame *f)
{
    if (silent)
        return -1;
    step();
    if (bus_off(plat_now_ms())) {
        stats.errors++;
        return -1;
    }
    stats.tx++;
    car_on_frame(f);
    return 0;
}

void can_port_get_stats(struct can_stats *out)
{
    *out = stats;
}

enum selftest_result can_port_check(char *detail, size_t cap)
{
    snprintf(detail, cap, "simulated bus, %s, rx %lu tx %lu, %lu errors", silent ? "listen-only" : "transmit-enabled",
             (unsigned long)stats.rx, (unsigned long)stats.tx, (unsigned long)stats.errors);
    return SELFTEST_PASS;
}

enum selftest_result can_port_loopback(char *detail, size_t cap)
{
    if (sim_cfg.fault_can) {
        snprintf(detail, cap, "nothing came back: tx error counter 128, 16 bus errors, 0 other frames");
        return SELFTEST_FAIL;
    }
    snprintf(detail, cap, "frame 7FF sent and read back through the transceiver");
    return SELFTEST_PASS;
}
