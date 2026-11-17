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
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "nvs_flash.h"

#include "app.h"
#include "leds.h"
#include "can_port.h"
#include "pins.h"
#include "platform.h"

#define CAN_BITRATE 500000
#define CONSOLE_RX_BUFFER 1024
#define CONSOLE_TX_BUFFER 2048

void ble_esp32_init(void); // ble_esp32.c

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
        leds_set(BOARD_LED_CAN, on);
        break;
    case PLAT_LED_WIFI: // app.c uses this one for "logging"
        leds_set(BOARD_LED_LOG, on);
        break;
    case PLAT_LED_SILENT:
        leds_set(BOARD_LED_SILENT, on);
        break;
    case PLAT_LED_USB:
        break; // the module's USB-C needs no indicator
    }
}

void plat_idle(void)
{
    // FreeRTOS: the main loop has to let the idle task run, or its watchdog fires.
    vTaskDelay(1);
}

// OBD +12V through 100 k / 22 k: reads about 0 V on USB alone.
#define OBD_SENSE_RATIO ((100.0 + 22.0) / 22.0)

static int obd_sense_mv(void)
{
    static adc_oneshot_unit_handle_t adc;
    static adc_cali_handle_t cali;
    static bool ready, cali_ready;
    if (!ready) {
        adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
        adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK || adc_oneshot_config_channel(adc, OBD_SENSE_ADC_CH, &ch) != ESP_OK)
            return -1;
        adc_cali_curve_fitting_config_t c = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                              .bitwidth = ADC_BITWIDTH_DEFAULT };
        cali_ready = adc_cali_create_scheme_curve_fitting(&c, &cali) == ESP_OK;
        ready = true;
    }
    int raw, mv, sum = 0;
    for (int i = 0; i < 8; i++) {
        if (adc_oneshot_read(adc, OBD_SENSE_ADC_CH, &raw) != ESP_OK)
            return -1;
        if (!cali_ready || adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK)
            mv = raw * 3100 / 4095;
        sum += mv;
    }
    return (int)(sum / 8 * OBD_SENSE_RATIO);
}

void plat_selftest(struct selftest_report *r, unsigned flags)
{
    (void)flags;
    selftest_add(r, "board", SELFTEST_PASS, "XIAO ESP32-S3 carrier v0.3.0");
    int mv = obd_sense_mv();
    if (mv < 0)
        selftest_add(r, "supply", SELFTEST_FAIL, "the ADC on GPIO9 would not read");
    else if (mv < 6000)
        selftest_add(r, "supply", SELFTEST_SKIP, "%d mV at the OBD port: no battery, running from USB", mv);
    else if (mv < 11800)
        selftest_add(r, "supply", SELFTEST_WARN, "%d mV at the OBD port: battery low, or the divider reads low", mv);
    else if (mv <= 15500)
        selftest_add(r, "supply", SELFTEST_PASS, "%d mV at the OBD port", mv);
    else
        selftest_add(r, "supply", SELFTEST_FAIL, "%d mV at the OBD port: above any charging voltage", mv);
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
