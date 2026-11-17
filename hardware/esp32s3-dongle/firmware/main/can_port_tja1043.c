// CAN bus access for the OBD dongle: the ESP32-S3's TWAI controller plus a TJA1043T on
// the bottom board. Same interface and semantics as the XIAO board's, including
// listen-only at power-up; the transceiver's modes replace the TJA1051's silent pin.
//
// Listen-only is enforced twice: the transceiver is in its own Listen-only mode (STB_N
// high, EN low), where it physically cannot drive the bus, and the controller is in
// listen-only mode so it does not acknowledge either. TWAI's mode is fixed when the driver
// is installed, so changing it means reinstalling the driver; that is why
// everything here goes through reconfigure().
//
// A task waits on the controller's alerts and moves received frames into the
// same lock-free ring the Pico used, so the main loop sees exactly one producer.
//
// Not yet run on hardware.
#include "can_port.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pins.h"
#include "twai_selftest.h"
#include "tja1043.h"

#define RX_QUEUE_SIZE 128 // power of two
#define DRIVER_RX_QUEUE 32
#define DRIVER_TX_QUEUE 16
#define ALERT_WAIT_MS 50
#define RX_TASK_STACK 3072
#define RX_TASK_PRIORITY 10

#define WATCHED_ALERTS                                                                      \
    (TWAI_ALERT_RX_DATA | TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_ARB_LOST \
     | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_RX_FIFO_OVERRUN          \
     | TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED)

static struct can_frame rx_queue[RX_QUEUE_SIZE];
static volatile uint32_t rx_head, rx_tail; // head written by the receive task, tail by the main loop
static volatile struct can_stats stats;
static bool silent = true;
static SemaphoreHandle_t driver_lock;

static esp_err_t reconfigure(bool listen_only)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN,
                                                          listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
    g.rx_queue_len = DRIVER_RX_QUEUE;
    g.tx_queue_len = DRIVER_TX_QUEUE;
    g.alerts_enabled = WATCHED_ALERTS;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    twai_stop();
    twai_driver_uninstall();
    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err != ESP_OK)
        return err;
    return twai_start();
}

static void push(const twai_message_t *msg)
{
    uint32_t head = rx_head;
    if (head - rx_tail >= RX_QUEUE_SIZE) {
        stats.rx_dropped++;
        return;
    }
    struct can_frame *f = &rx_queue[head % RX_QUEUE_SIZE];
    f->ext = msg->extd != 0;
    f->id = msg->identifier & (f->ext ? 0x1FFFFFFFu : 0x7FFu);
    f->dlc = msg->data_length_code > 8 ? 8 : (uint8_t)msg->data_length_code;
    memcpy(f->data, msg->data, 8);
    rx_head = head + 1;
    stats.rx++;
}

static void rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t alerts = 0;
        xSemaphoreTake(driver_lock, portMAX_DELAY);
        esp_err_t err = twai_read_alerts(&alerts, pdMS_TO_TICKS(ALERT_WAIT_MS));
        if (err == ESP_OK) {
            if (alerts & TWAI_ALERT_RX_DATA) {
                twai_message_t msg;
                while (twai_receive(&msg, 0) == ESP_OK) {
                    if (!msg.rtr)
                        push(&msg);
                }
            }
            if (alerts & TWAI_ALERT_TX_SUCCESS)
                stats.tx++;
            if (alerts & (TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_RX_FIFO_OVERRUN))
                stats.rx_dropped++;
            if (alerts & (TWAI_ALERT_TX_FAILED | TWAI_ALERT_ARB_LOST | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_BUS_OFF))
                stats.errors++;
            if (alerts & TWAI_ALERT_BUS_OFF)
                twai_initiate_recovery(); // a shorted or badly terminated bus should not need a power cycle
            if (alerts & TWAI_ALERT_BUS_RECOVERED)
                twai_start();
        }
        xSemaphoreGive(driver_lock);
        if (err != ESP_OK)
            vTaskDelay(1); // driver missing or being reinstalled: let the main loop have the lock
    }
}

// ---- the transceiver's modes (NXP TJA1043 datasheet, table of operating modes)
void tja1043_pins_init(void)
{
    gpio_reset_pin(CAN_STB_N_PIN);
    gpio_reset_pin(CAN_EN_PIN);
    gpio_set_direction(CAN_STB_N_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(CAN_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(CAN_ERR_N_PIN);
    gpio_set_direction(CAN_ERR_N_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CAN_ERR_N_PIN, GPIO_PULLUP_ONLY);
}

void tja1043_set_mode(enum tja1043_mode m)
{
    // STB_N first when leaving standby or sleep, last when entering them, so the pins never
    // pass through Normal on the way between two non-transmitting modes.
    bool stb = m == TJA1043_NORMAL || m == TJA1043_LISTEN_ONLY;
    bool en = m == TJA1043_NORMAL || m == TJA1043_GO_TO_SLEEP;
    if (stb) {
        gpio_set_level(CAN_EN_PIN, en);
        gpio_set_level(CAN_STB_N_PIN, 1);
    } else {
        gpio_set_level(CAN_STB_N_PIN, 0);
        gpio_set_level(CAN_EN_PIN, en);
    }
    esp_rom_delay_us(10);   // ERR_N settles within 8 us of a mode change
}

bool tja1043_flag(void)
{
    return gpio_get_level(CAN_ERR_N_PIN) == 0;   // active low: low = the flag for this mode is set
}

void tja1043_stop_controller(void)
{
    xSemaphoreTake(driver_lock, portMAX_DELAY);
    twai_stop();
    twai_driver_uninstall();
    xSemaphoreGive(driver_lock);
}

void can_port_init(uint32_t bitrate)
{
    // The bit timing comes from a macro, so only one rate is offered here.
    configASSERT(bitrate == 500000);
    tja1043_pins_init();
    tja1043_set_mode(TJA1043_LISTEN_ONLY);

    driver_lock = xSemaphoreCreateMutex();
    silent = true;
    reconfigure(true);
    xTaskCreate(rx_task, "can_rx", RX_TASK_STACK, NULL, RX_TASK_PRIORITY, NULL);
}

void can_port_set_silent(bool on)
{
    if (on == silent)
        return;
    // The transceiver goes listen-only before reconfiguring, and to Normal only once the
    // controller is transmit-capable, so there is never a moment where one of the two allows
    // driving the bus and the other does not.
    tja1043_set_mode(TJA1043_LISTEN_ONLY);
    xSemaphoreTake(driver_lock, portMAX_DELAY);
    bool ok = reconfigure(on) == ESP_OK;
    xSemaphoreGive(driver_lock);
    silent = ok ? on : true;
    tja1043_set_mode(silent ? TJA1043_LISTEN_ONLY : TJA1043_NORMAL);
}

bool can_port_is_silent(void)
{
    return silent;
}

bool can_port_receive(struct can_frame *out)
{
    uint32_t tail = rx_tail;
    if (tail == rx_head)
        return false;
    *out = rx_queue[tail % RX_QUEUE_SIZE];
    rx_tail = tail + 1;
    return true;
}

int can_port_send(const struct can_frame *f)
{
    if (silent)
        return -1;
    twai_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.extd = f->ext;
    msg.identifier = f->id & (f->ext ? 0x1FFFFFFFu : 0x7FFu);
    msg.data_length_code = f->dlc;
    memcpy(msg.data, f->data, 8);
    return twai_transmit(&msg, 0) == ESP_OK ? 0 : -1;
}

void can_port_get_stats(struct can_stats *out)
{
    out->rx = stats.rx;
    out->rx_dropped = stats.rx_dropped;
    out->tx = stats.tx;
    out->errors = stats.errors;
}

enum selftest_result can_port_check(char *detail, size_t cap)
{
    enum selftest_result res = twai_selftest_check(silent, detail, cap);
    if (tja1043_flag() && res == SELFTEST_PASS) {
        // In listen-only and normal mode ERR_N shows the transceiver's failure flags (TJA1043
        // datasheet, ERR_N table): TXD held dominant, over-temperature, or a bus wiring fault.
        snprintf(detail, cap, "controller running, but ERR_N is low: a transceiver failure flag is set");
        res = SELFTEST_WARN;
    }
    return res;
}

enum selftest_result can_port_loopback(char *detail, size_t cap)
{
    bool was_silent = silent;
    enum selftest_result res;
    tja1043_set_mode(TJA1043_LISTEN_ONLY);
    xSemaphoreTake(driver_lock, portMAX_DELAY);
    if (twai_selftest_install() != ESP_OK) {
        snprintf(detail, cap, "the controller would not start in self-test mode");
        res = SELFTEST_FAIL;
    } else {
        tja1043_set_mode(TJA1043_NORMAL); // the transceiver may drive the bus for this one frame
        res = twai_selftest_loopback(detail, cap);
        tja1043_set_mode(TJA1043_LISTEN_ONLY);
    }
    bool ok = reconfigure(was_silent) == ESP_OK;
    xSemaphoreGive(driver_lock);
    silent = ok ? was_silent : true;
    tja1043_set_mode(silent ? TJA1043_LISTEN_ONLY : TJA1043_NORMAL);
    return res;
}
