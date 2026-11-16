// Shared declarations for the host simulator: scenario settings (from the VWSIM
// environment variable), a seeded random source, and the engine model.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../src/isotp.h"

enum ignition { IGN_OFF, IGN_ON, IGN_CRANK, IGN_RUN };

#define SIM_MAX_IGN_STEPS 8

struct sim_config {
    // timing
    int lat_min, lat_max;       // ms before a module answers (uniform)
    int s3_ms;                  // non-default session falls back after this long without a request (0 = never)
    uint8_t fc_bs, fc_stmin;    // flow control the modules ask for when a request spans several frames
    bool gateway_open;          // broadcast traffic visible at the OBD port
    bool trans_obd;             // transmission answers functional OBD requests too
    // ignition timeline: state from time `at` (ms after start)
    int ign_count;
    struct { uint32_t at; enum ignition state; } ign[SIM_MAX_IGN_STEPS];
    // engine
    int misfire_cyl;            // 0 = healthy, 1..4 = this cylinder misfires (low compression)
    bool history;               // carry previous drive cycles: stored code, freeze frame, misfire averages
    bool cold_start;            // coolant starts at ambient instead of warm
    bool drive;                 // follow a looping drive trace instead of idling
    // engine numbers that calibration from the car's logs sets (vwdiag calibrate)
    double idle_rpm, cold_idle_rpm;   // warm idle, and idle at 15 C coolant
    double warmup;                    // coolant warm-up rate, 1 = the model's own estimate
    double mis_cold, mis_idle, mis_light, mis_load;   // % of the weak cylinder's firings that misfire
    double trim_cold, trim_idle, trim_drive;          // fuel trim the weak cylinder costs, %
    double speed;                     // engine time runs this many times faster than real time
    // bus faults
    double drop_pct;            // frames the interface misses
    double error_pct;           // frames hit by an error frame (counted, retransmitted a little later)
    double delay_pct;           // frames held back by arbitration...
    int delay_ms;               // ...this long (other IDs overtake them)
    uint32_t busoff_at, busoff_ms; // the interface is bus-off for this window (0 = never)
    uint32_t seed;
};

extern struct sim_config sim_cfg;

// Apply a scenario: comma-separated profile names and key=value settings (see sim_config.c),
// on top of the realistic defaults. NULL or "" gives the defaults. Returns false, keeping the
// defaults, if a setting is not understood.
bool sim_config_set(const char *spec);
// Hold the ignition in one state from now on, overriding any timeline.
void sim_set_ignition(enum ignition state, uint32_t now);

// Deterministic random numbers from the configured seed.
uint32_t sim_rand(void);
double sim_rand01(void);
int sim_rand_range(int lo, int hi);   // inclusive

// ---- engine model (sim_engine.c)
struct engine_state {
    enum ignition ign;
    double rpm, speed_kmh, throttle_pct, load_pct, map_kpa, maf_gs, timing_deg;
    double coolant_c, intake_c, ambient_c, oil_c, baro_kpa, volts;
    double stft_pct, ltft_pct, lambda, lambda_cmd;
    bool closed_loop;
    uint32_t runtime_s;
    // misfire: counts this driving cycle, and the average of the last ten
    uint16_t mis_now[4], mis_avg[4];
};

void engine_step(uint32_t now_ms);   // advance the model to now
void engine_reset(void);             // start again (new scenario)
const struct engine_state *engine(void);
enum ignition sim_ignition(uint32_t now_ms);

// A fault code held by the engine module, with what the module recorded about it.
struct sim_dtc {
    uint8_t code[3];
    uint8_t status;
    bool has_snapshot;
    struct engine_state snap;   // the freeze frame
    uint8_t occurrences, aging;
};

#define SIM_MAX_DTC 8
// The engine module's fault memory (the model adds misfire codes as they happen).
struct sim_dtc *engine_dtcs(int *count);
void engine_clear_dtcs(void);

// ---- the car (car.c): control modules, engine and broadcast, on any CAN transport
typedef int (*car_tx_fn)(const struct can_frame *f);   // send one frame; 0 = accepted

struct car_stats {
    uint32_t received;   // frames from the bus
    uint32_t sent;       // frames the car sent
    uint32_t requests;   // complete diagnostic requests handled
};

bool car_init(const char *scenario, car_tx_fn tx);   // (re)start; false if the scenario was not understood
void car_on_frame(const struct can_frame *f);         // every frame from the bus
void car_poll(void);                                  // call often: answers, engine, broadcast
uint32_t car_time(void);                              // ms since car_init
void car_get_stats(struct car_stats *out);
