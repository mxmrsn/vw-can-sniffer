// A small engine model, so the live values the simulator reports move together
// the way a real engine's do: rpm, load, airflow, manifold pressure, timing and
// fuel trims follow a throttle trace, the coolant warms up, and a cylinder with low
// compression misfires most at idle and when cold, which pushes the fuel trims
// rich. Rough figures for a 2.0 l turbo four, not a calibration.
#include <math.h>
#include <string.h>

#include "sim.h"

#define STEP_MS 10
#define WARM_C 90.0

static struct engine_state st;
static uint32_t last_t, run_since, trace_from;   // trace_from: when the drive trace starts
static bool inited, was_running, first;
static double mis_acc[4];        // fractional misfires carried between steps
static double phase;             // fuel-trim oscillation
static double rpm_smooth;        // the engine's speed; st.rpm adds the unevenness a sensor sees
static struct sim_dtc dtcs[SIM_MAX_DTC];
static int dtc_count;

enum ignition sim_ignition(uint32_t t)
{
    enum ignition s = IGN_RUN;
    for (int i = 0; i < sim_cfg.ign_count; i++)
        if (t >= sim_cfg.ign[i].at)
            s = sim_cfg.ign[i].state;
    return s;
}

const struct engine_state *engine(void)
{
    return &st;
}

struct sim_dtc *engine_dtcs(int *count)
{
    *count = dtc_count;
    return dtcs;
}

static struct sim_dtc *add_dtc(uint8_t a, uint8_t b, uint8_t c, uint8_t status)
{
    for (int i = 0; i < dtc_count; i++)
        if (dtcs[i].code[0] == a && dtcs[i].code[1] == b && dtcs[i].code[2] == c)
            return &dtcs[i];
    if (dtc_count == SIM_MAX_DTC)
        return NULL;
    struct sim_dtc *d = &dtcs[dtc_count++];
    memset(d, 0, sizeof(*d));
    d->code[0] = a; d->code[1] = b; d->code[2] = c;
    d->status = status;
    d->occurrences = 1;
    return d;
}

void engine_clear_dtcs(void)
{
    // Clearing codes also resets the OBD misfire monitor; fuel-trim adaptation is kept.
    dtc_count = 0;
    memset(st.mis_now, 0, sizeof(st.mis_now));
    memset(st.mis_avg, 0, sizeof(st.mis_avg));
}

static void init(void)
{
    memset(&st, 0, sizeof(st));
    st.ambient_c = sim_cfg.cold_start ? 12 : 17;
    st.coolant_c = sim_cfg.cold_start ? st.ambient_c : WARM_C;
    st.oil_c = sim_cfg.cold_start ? st.ambient_c : 96;
    st.intake_c = st.ambient_c + 6;
    st.baro_kpa = 100;
    st.volts = 12.5;
    st.lambda = st.lambda_cmd = 1.0;

    // What any car's fault memory might hold: the codes earlier versions of the simulator carried.
    add_dtc(0x02, 0x99, 0x00, 0x09);   // P0299 boost too low: failed now, confirmed
    add_dtc(0x13, 0x0A, 0x00, 0x28);   // P130A: confirmed, failed since clear

    if (sim_cfg.history && sim_cfg.misfire_cyl) {
        // Earlier drives: a confirmed misfire code with its freeze frame, averages in mode 06,
        // and a long-term trim that has already moved to cover the weak cylinder.
        int c = sim_cfg.misfire_cyl - 1;
        st.mis_avg[c] = 47;
        st.ltft_pct = 3.1;
        struct sim_dtc *d = add_dtc(0x03, (uint8_t)(0x00 + sim_cfg.misfire_cyl), 0x00, 0xA8); // P030x, lamp, confirmed
        if (d) {
            d->has_snapshot = true;
            d->occurrences = 6;
            d->aging = 0;
            struct engine_state *s = &d->snap;
            s->ign = IGN_RUN;
            s->rpm = 812; s->speed_kmh = 0; s->load_pct = 22.0; s->map_kpa = 33; s->maf_gs = 3.9;
            s->coolant_c = 41; s->intake_c = 18; s->timing_deg = 6.0; s->stft_pct = 7.8; s->ltft_pct = 3.1;
            s->runtime_s = 95; s->volts = 14.2; s->lambda = 1.0;
        }
    }
}

static double idle_target(void)
{
    // EA888 Gen 3 warm idle is around 650-720 rpm; a cold start idles fast and settles as it warms
    if (st.coolant_c >= 75)
        return 700;
    double t = 1100 - (st.coolant_c - 15) * (400.0 / 60.0);
    return t < 700 ? 700 : t > 1100 ? 1100 : t;
}

// Driving trace, 70 s long: idle, pull away, cruise, coast down, idle.
static void trace(double t, double *throttle, double *speed_target, bool *fuel_cut)
{
    double c = fmod(t, 70.0);
    *fuel_cut = false;
    if (c < 15) { *throttle = 0; *speed_target = 0; }
    else if (c < 27) { *throttle = 35; *speed_target = 60; }
    else if (c < 47) { *throttle = 14; *speed_target = 60; }
    else if (c < 57) { *throttle = 0; *speed_target = 0; *fuel_cut = st.speed_kmh > 8; }
    else { *throttle = 0; *speed_target = 0; }
}

static double gear_ratio(double kmh)
{
    // rpm per km/h in the gear a DSG would pick
    return kmh < 15 ? 105 : kmh < 30 ? 62 : kmh < 45 ? 42 : kmh < 60 ? 32 : 26;
}

static void misfire_step(double dt)
{
    if (!sim_cfg.misfire_cyl || st.rpm < 400)
        return;
    int c = sim_cfg.misfire_cyl - 1;
    double rate;                  // share of that cylinder's combustion events that misfire
    if (st.coolant_c < 60)
        rate = 0.06;
    else if (st.load_pct < 30 && st.speed_kmh < 2)
        rate = 0.025;             // warm idle
    else if (st.load_pct < 45)
        rate = 0.006;
    else
        rate = 0.002;             // under load the cylinder pressure is high enough to fire
    double events = st.rpm / 120.0 * dt;
    mis_acc[c] += events * rate * (0.6 + 0.8 * sim_rand01());
    int n = (int)mis_acc[c];
    mis_acc[c] -= n;
    if (n && st.mis_now[c] < 65535 - n)
        st.mis_now[c] = (uint16_t)(st.mis_now[c] + n);

    // The module flags the cylinder once enough misfires pile up in this driving cycle.
    if (st.mis_now[c] >= 150) {
        struct sim_dtc *d = add_dtc(0x03, (uint8_t)sim_cfg.misfire_cyl, 0x00, 0);
        if (d && !(d->status & 0x01)) {
            d->status |= 0x01 | 0x02 | 0x04 | 0x20;   // failed now, this cycle, pending, since clear
            d->occurrences++;
            d->aging = 0;
            if (!d->has_snapshot) {
                d->has_snapshot = true;
                d->snap = st;
            }
        }
    }
}

static void run_step(double t_run, double dt)
{
    double throttle = 0, speed_target = 0;
    bool fuel_cut = false;
    if (sim_cfg.drive)
        trace((last_t - trace_from) / 1000.0, &throttle, &speed_target, &fuel_cut);
    st.throttle_pct = throttle;

    // vehicle speed eases toward the trace
    double accel = speed_target > st.speed_kmh ? 5.0 : -6.0;
    st.speed_kmh += accel * dt;
    if ((accel > 0 && st.speed_kmh > speed_target) || (accel < 0 && st.speed_kmh < speed_target))
        st.speed_kmh = speed_target;

    double target = idle_target();
    if (st.speed_kmh > 1) {
        double r = st.speed_kmh * gear_ratio(st.speed_kmh) + throttle * 12;
        target = r > target ? r : target;
    }
    double jitter = (sim_cfg.misfire_cyl && st.speed_kmh < 2) ? 35 : 8;
    rpm_smooth += (target - rpm_smooth) * (dt / 0.3);
    st.rpm = rpm_smooth + (sim_rand01() - 0.5) * 2 * jitter;

    st.map_kpa = fuel_cut ? 22 : 31 + throttle * 2.2;
    st.load_pct = st.map_kpa / st.baro_kpa * 80;
    if (st.load_pct > 100)
        st.load_pct = 100;
    st.maf_gs = fuel_cut ? 1.5 : st.rpm / 120.0 * 2.0 * (st.map_kpa / 101.0) * 1.2 * 0.85;
    st.timing_deg = (st.speed_kmh < 1 && throttle < 1) ? 6 + (sim_rand01() - 0.5) * 4 : 34 - st.load_pct * 0.25;
    if (t_run < 40 && st.coolant_c < 50 && throttle < 1)
        st.timing_deg = -3 + (sim_rand01() - 0.5);   // catalyst heating after a cold start: spark retarded

    // warm-up: the thermostat holds the coolant at 90 C
    if (st.coolant_c < WARM_C) {
        st.coolant_c += (0.08 + 0.00008 * st.rpm + 0.004 * st.load_pct) * dt;
        if (st.coolant_c > WARM_C)
            st.coolant_c = WARM_C;
    }
    st.oil_c += (st.coolant_c + 6 - st.oil_c) * 0.01 * dt;
    st.intake_c = st.ambient_c + 6 + (st.speed_kmh < 5 ? 4 : 0);
    st.volts = 14.15 + (sim_rand01() - 0.5) * 0.1;

    // fuel control: open loop when cold-started or hard on the throttle
    st.closed_loop = !fuel_cut && throttle < 60 && (st.coolant_c > 20 && t_run > 25);
    st.lambda_cmd = st.closed_loop ? 1.0 : (st.coolant_c < 40 ? 0.92 : 0.85);
    phase += dt * 2 * M_PI * 1.2;
    if (st.closed_loop) {
        // A cylinder that pumps less air sends unburned oxygen to the sensor; the module adds fuel.
        double bias = 0;
        if (sim_cfg.misfire_cyl)
            bias = st.speed_kmh < 2 ? (st.coolant_c < 60 ? 9.0 : 6.5) : 2.5;
        st.ltft_pct += (bias * 0.6 - st.ltft_pct) * dt / 120.0;
        st.stft_pct = bias - st.ltft_pct + sin(phase) * 2.5;
        st.lambda = 1.0 + sin(phase + 1.0) * 0.02 + (sim_cfg.misfire_cyl && st.speed_kmh < 2 ? 0.01 : 0);
    } else {
        st.stft_pct = 0;
        st.lambda = fuel_cut ? 1.99 : st.lambda_cmd + (sim_rand01() - 0.5) * 0.01;
    }
    misfire_step(dt);
    st.runtime_s = (uint32_t)t_run;
}

static void end_of_cycle(void)
{
    // Ignition off: this driving cycle's misfire counts fold into the ten-cycle average,
    // and a code that did not fail this time ages.
    for (int c = 0; c < 4; c++) {
        st.mis_avg[c] = (uint16_t)((st.mis_avg[c] * 9u + st.mis_now[c]) / 10u);
        st.mis_now[c] = 0;
    }
    for (int i = 0; i < dtc_count; i++) {
        struct sim_dtc *d = &dtcs[i];
        if (d->status & 0x02) {
            if (d->status & 0x04)
                d->status |= 0x08;          // failed again: confirmed
        } else if (d->aging < 255) {
            d->aging++;
        }
        d->status &= (uint8_t)~(0x01 | 0x02);
    }
}

void engine_step(uint32_t t)
{
    if (!inited) {
        init();
        inited = true;
        last_t = t - STEP_MS;   // take one step now, so the first reading is already a running engine
        first = true;
    }
    while ((int32_t)(t - last_t) >= STEP_MS) {
        last_t += STEP_MS;
        enum ignition ign = sim_ignition(last_t);
        double dt = STEP_MS / 1000.0;
        bool was_first = first;
        first = false;
        if (ign == IGN_RUN && !was_running) {
            trace_from = last_t;
            if (was_first && !sim_cfg.cold_start) {
                // The engine was already warm and idling when the tools connected.
                run_since = last_t - 600000;
                rpm_smooth = idle_target();
            } else {
                run_since = last_t;      // a fresh start: a brief flare above idle
                rpm_smooth = 1200;
            }
        }
        if (ign != IGN_RUN && was_running)
            end_of_cycle();
        was_running = ign == IGN_RUN;
        st.ign = ign;
        if (ign == IGN_RUN) {
            run_step((last_t - run_since) / 1000.0, dt);
            continue;
        }
        // Not running: everything at rest, the battery carries the load.
        rpm_smooth = 0;
        st.rpm = ign == IGN_CRANK ? 200 + sim_rand01() * 40 : 0;
        st.speed_kmh = st.throttle_pct = st.timing_deg = 0;
        st.map_kpa = st.baro_kpa;
        st.load_pct = 0;
        st.maf_gs = ign == IGN_CRANK ? 1.2 : 0;
        st.volts = ign == IGN_CRANK ? 10.2 : ign == IGN_ON ? 12.3 : 12.5;
        st.closed_loop = false;
        st.stft_pct = 0;
        st.lambda = st.lambda_cmd = 1.0;
        st.runtime_s = 0;
        if (st.coolant_c > st.ambient_c)
            st.coolant_c -= 0.01 * dt;
    }
}
