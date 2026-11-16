// Power management for a board that can switch itself off (the OBD dongle): decides when to
// sleep, why it stays awake, and protects the car's battery. Pure logic, no hardware, so the host
// tests and the simulator run exactly what the dongle runs.
//
// The dongle sleeps by putting its CAN transceiver to sleep, which switches its own supply off; it
// wakes on CAN activity, on the engine-running voltage detector, or on its button
// (hardware/esp32s3-dongle/docs/architecture.md, "Power and sleep").
//
// On this car the gateway probably keeps all traffic off the OBD port, so a silent bus does not
// mean the car is off. The dongle therefore stays awake for anything that says it is in use:
// a USB host, a connected phone, requests from a person (console or app), the engine-running
// voltage, recent bus traffic, logging while there is something to log, or the stay-awake setting.
// Its own poll-list requests do not count, or it would never sleep.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum power_wake {
    POWER_WAKE_UNKNOWN = 0,
    POWER_WAKE_POWER_ON = 1, // the battery was connected (plugged in, or the battery reconnected)
    POWER_WAKE_USB = 2,      // running from USB alone
    POWER_WAKE_CAN = 3,      // traffic on the OBD bus
    POWER_WAKE_ENGINE = 4,   // the engine-running voltage detector
    POWER_WAKE_BUTTON = 5,   // the button
};

// What keeps it awake right now (a bit mask).
enum {
    POWER_HOLD_USB = 1 << 0,
    POWER_HOLD_PHONE = 1 << 1,
    POWER_HOLD_ACTIVITY = 1 << 2, // a request from a person within the idle time
    POWER_HOLD_ENGINE = 1 << 3,   // battery voltage says the engine is running
    POWER_HOLD_BUS = 1 << 4,      // CAN frames within the idle time
    POWER_HOLD_LOGGING = 1 << 5,  // recording while there is something to record
    POWER_HOLD_STAY = 1 << 6,     // the stay-awake setting
};

enum power_sleep_reason {
    POWER_SLEEP_NONE = 0,
    POWER_SLEEP_IDLE = 1,    // nothing kept it awake for the idle time
    POWER_SLEEP_BATTERY = 2, // battery low with the engine off
    POWER_SLEEP_ASKED = 3,   // someone asked it to sleep
};

struct power_settings {
    uint32_t idle_s;        // how long without a reason before sleeping (default 300)
    uint16_t cutoff_mv;     // battery below this, engine off: sleep (default 12000)
    uint16_t running_mv;    // battery above this: the engine is running (default 13200)
    uint32_t low_battery_s; // how long the battery must stay low before sleeping (default 60)
    bool stay_awake;        // never sleep for idleness (battery protection still applies)
};

struct power_inputs {
    uint32_t now_ms;
    uint16_t vbat_mv;      // battery at the OBD port; under about 6 V means none (USB only)
    bool usb_host;         // a computer is attached to the USB port
    bool phone;            // a phone is connected over Bluetooth
    bool logging;
    uint32_t user_ms;      // last request from a person (console or app)
    uint32_t bus_ms;       // last CAN frame received
};

struct power_status {
    uint8_t holds;               // POWER_HOLD_* bits
    enum power_wake wake;
    uint16_t vbat_mv;
    uint32_t sleep_in_s;         // seconds until it would sleep if nothing changes (0 = not counting)
    uint32_t low_battery_for_s;  // how long the battery has been below the cutoff
    enum power_sleep_reason sleeping;
};

void power_init(enum power_wake wake, uint32_t now_ms);
struct power_settings *power_settings(void); // change them in place
// Take saved settings, keeping only values inside the ranges the commands allow. Returns false
// (and changes nothing) if they are not plausible.
bool power_settings_adopt(const struct power_settings *saved);
void power_request_sleep(void);

// Call regularly. Returns the reason to sleep now, or POWER_SLEEP_NONE to stay awake.
enum power_sleep_reason power_poll(const struct power_inputs *in);
void power_get_status(struct power_status *out);

const char *power_wake_name(enum power_wake w);
const char *power_sleep_name(enum power_sleep_reason r);
