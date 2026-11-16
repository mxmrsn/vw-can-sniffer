// The dongle's power hooks (platform.h): the battery voltage, whether a computer is on USB, why it
// woke, and switching itself off by putting the TJA1043 to sleep, which lets INH float and turns
// the bottom board's buck off (docs/architecture.md, "Power and sleep").
//
// Not yet run on hardware.
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pins.h"
#include "platform.h"
#include "power.h"
#include "tja1043.h"

#define NO_BATTERY_MV 6000

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static bool adc_ready, cali_ready;
static enum power_wake wake = POWER_WAKE_UNKNOWN;

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK)
        return;
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_ready = adc_oneshot_config_channel(adc, OBD_SENSE_ADC_CH, &ch) == ESP_OK;
    adc_cali_curve_fitting_config_t c = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    cali_ready = adc_cali_create_scheme_curve_fitting(&c, &cali) == ESP_OK;
}

uint16_t plat_battery_mv(void)
{
    // VBAT behind the input diode, through 1 M / 220 k; C11 holds the node for the ADC's sampling.
    if (!adc_ready)
        return 0;
    int sum = 0, raw, mv;
    for (int i = 0; i < 8; i++) {
        if (adc_oneshot_read(adc, OBD_SENSE_ADC_CH, &raw) != ESP_OK)
            return 0;
        if (cali_ready && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK)
            sum += mv;
        else
            sum += raw * 3100 / 4095;   // uncalibrated estimate
    }
    double v = (sum / 8.0) * OBD_SENSE_RATIO;
    return v > 65535 ? 65535 : (uint16_t)v;
}

bool plat_usb_host(void)
{
    return usb_serial_jtag_is_connected();
}

enum power_wake plat_wake_reason(void)
{
    return wake;
}

// Why it woke, read from the transceiver before CAN starts (call once, early).
//   Standby (where the pins' pull-downs left it): ERR_N shows the Wake flag.
//   Listen-only, coming from standby: ERR_N shows Pwon, set when the battery was connected.
//   Normal, before the controller sends anything: ERR_N shows the wake-up source flag, set
//   when the wake came through the WAKE pin (engine-running detector or button) rather than
//   the bus. TXD is held recessive meanwhile, so the transceiver cannot drive the bus.
void power_boot(void)
{
    adc_init();
    gpio_reset_pin(CAN_TX_PIN);
    gpio_set_direction(CAN_TX_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CAN_TX_PIN, 1);   // recessive
    tja1043_pins_init();

    uint16_t vbat = plat_battery_mv();
    tja1043_set_mode(TJA1043_STANDBY);
    bool woken = tja1043_flag();
    tja1043_set_mode(TJA1043_LISTEN_ONLY);
    bool power_on = tja1043_flag();
    bool local = false;
    if (!power_on && woken) {
        tja1043_set_mode(TJA1043_NORMAL);
        local = tja1043_flag();
        tja1043_set_mode(TJA1043_LISTEN_ONLY);
    }

    if (vbat < NO_BATTERY_MV)
        wake = POWER_WAKE_USB;
    else if (power_on)
        wake = POWER_WAKE_POWER_ON;
    else if (local)
        wake = vbat >= power_settings()->running_mv ? POWER_WAKE_ENGINE : POWER_WAKE_BUTTON;
    else if (woken)
        wake = POWER_WAKE_CAN;
    else
        wake = POWER_WAKE_UNKNOWN;
    gpio_reset_pin(CAN_TX_PIN);   // the CAN controller takes it from here
}

void plat_power_sleep(enum power_sleep_reason why)
{
    printf("power: sleeping (%s)\n", power_sleep_name(why));
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));   // let the console and the card finish
    tja1043_stop_controller();
    gpio_reset_pin(CAN_TX_PIN);
    gpio_set_direction(CAN_TX_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CAN_TX_PIN, 1);
    // Go-to-sleep: the transceiver enters Sleep once the mode has been held, INH floats, and the
    // buck's enable is pulled low: this processor's power goes away a moment later.
    tja1043_set_mode(TJA1043_GO_TO_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Still running: USB is powering the top board, or a wake request arrived while going to sleep
    // (the transceiver then refuses Sleep). Nap without the radio, then look again from the top.
    printf("power: still powered (USB, or woken while going to sleep); restarting\n");
    fflush(stdout);
    tja1043_set_mode(TJA1043_STANDBY);
    esp_sleep_enable_timer_wakeup(60ull * 1000000);
    esp_deep_sleep_start();
}
