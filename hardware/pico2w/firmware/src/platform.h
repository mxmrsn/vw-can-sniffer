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

// Boards that switch themselves off (built with APP_POWER: the OBD dongle, and the simulator).
#include "power.h"
uint16_t plat_battery_mv(void);   // the OBD port's battery voltage; under ~6 V means none
bool plat_usb_host(void);         // a computer is attached to the USB port
enum power_wake plat_wake_reason(void);
void plat_power_sleep(enum power_sleep_reason why); // switch off; on the dongle it does not return
// Keep the power settings across sleeps (flash on the dongle). Load returns false if none were saved.
bool plat_settings_load(struct power_settings *s);
void plat_settings_save(const struct power_settings *s);
