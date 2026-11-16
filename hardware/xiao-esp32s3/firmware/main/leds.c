#include "leds.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

#include "pins.h"

#define LED_COUNT 4
#define LEVEL 24 // of 255: plenty indoors, and each LED stays well under its 60 mA maximum

static led_strip_handle_t strip;
static SemaphoreHandle_t lock;
static bool state[LED_COUNT] = { true, false, false, false }; // listen-only at power-up

static void show(void)
{
    // D1 is never dark: amber while listen-only, green once transmitting is allowed.
    if (state[BOARD_LED_SILENT])
        led_strip_set_pixel(strip, 0, LEVEL, LEVEL / 2, 0);
    else
        led_strip_set_pixel(strip, 0, 0, LEVEL, 0);
    led_strip_set_pixel(strip, 1, state[BOARD_LED_CAN] ? LEVEL : 0, state[BOARD_LED_CAN] ? LEVEL : 0, state[BOARD_LED_CAN] ? LEVEL : 0);
    led_strip_set_pixel(strip, 2, 0, 0, state[BOARD_LED_BLE] ? LEVEL : 0);
    led_strip_set_pixel(strip, 3, state[BOARD_LED_LOG] ? LEVEL : 0, 0, 0);
    led_strip_refresh(strip);
}

void leds_init(void)
{
    lock = xSemaphoreCreateMutex();
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_DATA_PIN,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&cfg, &rmt, &strip) != ESP_OK) {
        strip = NULL; // no indicators is better than no interface
        return;
    }
    show();
}

void leds_set(enum board_led which, bool on)
{
    if (!strip || state[which] == on)
        return; // only redraw on a change: the CAN LED is set on every frame
    xSemaphoreTake(lock, portMAX_DELAY);
    state[which] = on;
    show();
    xSemaphoreGive(lock);
}
