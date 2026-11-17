// The few things the application core needs from the platform it runs on.
// Implemented by src/main.c on the Pico and sim/main_host.c on a computer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum plat_led { PLAT_LED_CAN, PLAT_LED_WIFI, PLAT_LED_USB, PLAT_LED_SILENT };

uint32_t plat_now_ms(void);
int plat_getchar(void);                 // next console byte, or -1 if none waiting
void plat_led(enum plat_led led, bool on);
void plat_idle(void);                   // called while waiting; may sleep briefly
void plat_write(const uint8_t *data, size_t len); // raw bytes to the console, no newline translation

// Bluetooth LE (see ble_link.h). The Pico runs the stack in the background; the
// simulator emulates it over TCP. Both report their state in ble_info.
void plat_ble_poll(void);         // called often from the main loop
void plat_ble_kick(void);         // packets were queued: start sending them
bool plat_ble_open_pairing(void); // allow a new phone to pair for a while
bool plat_ble_forget(void);       // delete every bonded phone

// Bring-up self-test (selftest.h): the board adds an item "board" (its name and revision) and any
// checks of its own. flags: SELFTEST_ACTIVE, SELFTEST_FORCE.
struct selftest_report;
void plat_selftest(struct selftest_report *r, unsigned flags);

// Boards that switch themselves off (built with APP_POWER: the OBD dongle, and the simulator).
#include "power.h"
uint16_t plat_battery_mv(void);   // the OBD port's battery voltage; under ~6 V means none
bool plat_usb_host(void);         // a computer is attached to the USB port
enum power_wake plat_wake_reason(void);
void plat_power_sleep(enum power_sleep_reason why); // switch off; on the dongle it does not return
// Keep the power settings across sleeps (flash on the dongle). Load returns false if none were saved.
bool plat_settings_load(struct power_settings *s);
void plat_settings_save(const struct power_settings *s);

// Boards with the GPS / sync connector (built with APP_GNSS: the OBD dongle, and the simulator).
uint64_t plat_now_us(void);                      // the clock plat_now_ms() comes from, in microseconds
size_t plat_gnss_read(uint8_t *buf, size_t cap); // bytes from the GPS receiver, 0 if none waiting
bool plat_gnss_baud(uint32_t baud);              // the receiver's serial speed (default 9600)
bool plat_pps_take(uint64_t *edge_us);           // the next pulse edge, stamped when it happened
// The sync output for a camera or IMU rig: a square wave at hz (0 = off, 1 to 100). With GPS pulses
// coming in, each pulse restarts it, so a rising edge falls on every GPS second.
bool plat_sync_set(uint32_t hz);
bool plat_sync_take(uint64_t *edge_us);          // the next rising edge sent, stamped when it was sent
