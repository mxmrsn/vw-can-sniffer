// VW diagnostic interface, firmware entry point for the Pico 2 W board (PCB v0.2.0).
// All behaviour lives in app.c; this file only provides the platform hooks.

#include <stdio.h>

#include "pico/stdlib.h"

#include "../pins.h"
#include "app.h"
#include "can_port.h"
#include "platform.h"

#define CAN_BITRATE 500000

void ble_pico_init(void); // ble_pico.c

static const uint led_pins[] = {
    [PLAT_LED_CAN] = LED_CAN_PIN,
    [PLAT_LED_WIFI] = LED_WIFI_PIN,
    [PLAT_LED_USB] = LED_USB_PIN,
    [PLAT_LED_SILENT] = LED_SILENT_PIN,
};

uint32_t plat_now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

int plat_getchar(void)
{
    int ch = getchar_timeout_us(0);
    return ch == PICO_ERROR_TIMEOUT ? -1 : ch;
}

void plat_write(const uint8_t *data, size_t len)
{
    stdio_put_string((const char *)data, (int)len, false, false);
    stdio_flush();
}

void plat_led(enum plat_led led, bool on)
{
    gpio_put(led_pins[led], on);
}

void plat_idle(void)
{
}

void plat_selftest(struct selftest_report *r, unsigned flags)
{
    (void)flags;
    selftest_add(r, "board", SELFTEST_PASS, "Pico 2 W board v0.2.0");
}

int main(void)
{
    stdio_init_all();
    for (size_t i = 0; i < sizeof(led_pins) / sizeof(led_pins[0]); i++) {
        gpio_init(led_pins[i]);
        gpio_set_dir(led_pins[i], GPIO_OUT);
    }
    ble_pico_init();
    can_port_init(CAN_BITRATE);
    app_init();
    for (;;)
        app_poll();
}
