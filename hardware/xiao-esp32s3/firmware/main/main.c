// VW diagnostic interface, firmware entry point for the XIAO ESP32-S3 board
// (PCB v0.3.0). All behaviour lives in app.c; this file only provides the
// platform hooks. The startup order matches the Pico board's main.c.
//
// Not yet run on hardware.

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app.h"
#include "can_port.h"
#include "pins.h"
#include "platform.h"

#define CAN_BITRATE 500000
#define CONSOLE_RX_BUFFER 1024
#define CONSOLE_TX_BUFFER 2048

void ble_esp32_init(void); // ble_esp32.c

// The board has two LEDs for platform.h's four indicators. Logging and a
// connected phone share the status LED, so each keeps its own bit and the LED
// shows whether either is on.
static bool status_bits[2];

void board_status_led(int source, bool on)
{
    status_bits[source] = on;
    gpio_set_level(LED_STATUS_PIN, status_bits[0] || status_bits[1]);
}

uint32_t plat_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int plat_getchar(void)
{
    uint8_t byte;
    return usb_serial_jtag_read_bytes(&byte, 1, 0) == 1 ? byte : -1;
}

void plat_write(const uint8_t *data, size_t len)
{
    // Straight to the driver: the console's VFS layer turns \n into \r\n, which
    // would corrupt the binary protocol. printf() still goes through the VFS,
    // so flush it first to keep the two streams in order.
    fflush(stdout);
    for (size_t sent = 0; sent < len;) {
        int n = usb_serial_jtag_write_bytes(data + sent, len - sent, pdMS_TO_TICKS(100));
        if (n <= 0)
            break; // nothing is reading the port; dropping is better than blocking the bus
        sent += (size_t)n;
    }
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
}

void plat_led(enum plat_led led, bool on)
{
    switch (led) {
    case PLAT_LED_CAN:
        gpio_set_level(LED_CAN_PIN, on);
        break;
    case PLAT_LED_WIFI: // app.c uses this one for "logging"
        board_status_led(0, on);
        break;
    case PLAT_LED_SILENT:
        gpio_set_level(LED_USER_PIN, !on); // the module's LED is active low
        break;
    case PLAT_LED_USB:
        break; // no pad left for it; the module's USB-C is lit by the host anyway
    }
}

void plat_idle(void)
{
    // FreeRTOS: the main loop has to let the idle task run, or its watchdog fires.
    vTaskDelay(1);
}

static void console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = CONSOLE_RX_BUFFER,
        .tx_buffer_size = CONSOLE_TX_BUFFER,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdout, NULL, _IONBF, 0);
}

static void leds_init(void)
{
    const int pins[] = { LED_CAN_PIN, LED_STATUS_PIN, LED_USER_PIN };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_reset_pin(pins[i]); // GPIO43 is UART0 TX after reset
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], pins[i] == LED_USER_PIN ? 1 : 0);
    }
}

void app_main(void)
{
    console_init();
    leds_init();

    // NimBLE keeps its bonding keys here.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ble_esp32_init();
    can_port_init(CAN_BITRATE);
    app_init();
    for (;;)
        app_poll();
}
