// The dongle's GPS / sync connector (platform.h, APP_GNSS): the receiver's UART, its pulse-per-second
// input, and the sync output for a camera or IMU rig. The decisions (parsing, pairing pulses with
// time, the drift) are in the shared src/gnss.c; this file only moves bytes and stamps edges.
//
// Everything is stamped with esp_timer_get_time(), the clock plat_now_ms() comes from.
//   Pulse: a GPIO interrupt on the rising edge; the stamp is taken first thing in the handler, so it
//     is late by the interrupt latency (a few microseconds), the same every time.
//   Sync output: a general-purpose timer at 1 MHz toggles the pin every half period. A pulse restarts
//     it with a rising edge, so with GPS each second begins with an edge on the GPS second.
//
// Not yet run on hardware.
#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_timer.h"

#include "pins.h"
#include "platform.h"

#define GPS_UART UART_NUM_1
#define GPS_RX_BUFFER 2048
#define DEFAULT_BAUD 9600
#define QUEUE_LEN 64   // power of two

// Single producer (an interrupt), single consumer (the main loop).
struct edge_queue {
    uint64_t at[QUEUE_LEN];
    atomic_uint head, tail;
};

static struct edge_queue pps_q, sync_q;
static bool uart_ready;
static gptimer_handle_t sync_timer;
static volatile uint32_t sync_half_us; // 0 = off
static volatile bool sync_level;

static IRAM_ATTR void push(struct edge_queue *q, uint64_t t)
{
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (h - atomic_load_explicit(&q->tail, memory_order_acquire) >= QUEUE_LEN)
        return; // full: the main loop has stalled; drop rather than overwrite
    q->at[h % QUEUE_LEN] = t;
    atomic_store_explicit(&q->head, h + 1, memory_order_release);
}

static bool pop(struct edge_queue *q, uint64_t *t)
{
    unsigned tl = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (tl == atomic_load_explicit(&q->head, memory_order_acquire))
        return false;
    *t = q->at[tl % QUEUE_LEN];
    atomic_store_explicit(&q->tail, tl + 1, memory_order_release);
    return true;
}

uint64_t plat_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static IRAM_ATTR bool on_sync_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *e, void *ctx)
{
    (void)timer;
    (void)e;
    (void)ctx;
    sync_level = !sync_level;
    gpio_set_level(SYNC_OUT_PIN, sync_level);
    if (sync_level)
        push(&sync_q, (uint64_t)esp_timer_get_time());
    return false;
}

static IRAM_ATTR void on_pps(void *arg)
{
    (void)arg;
    uint64_t t = (uint64_t)esp_timer_get_time();
    push(&pps_q, t);
    if (sync_half_us) {
        // Restart the sync output on the GPS second: a rising edge now, the next toggle half a period on.
        gptimer_set_raw_count(sync_timer, 0);
        if (!sync_level) {
            sync_level = true;
            gpio_set_level(SYNC_OUT_PIN, 1);
            push(&sync_q, t);
        }
    }
}

static void gnss_init_hw(void)
{
    static bool done;
    if (done)
        return;
    done = true;

    uart_config_t u = {
        .baud_rate = DEFAULT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_ready = uart_driver_install(GPS_UART, GPS_RX_BUFFER, 0, 0, NULL, 0) == ESP_OK &&
                 uart_param_config(GPS_UART, &u) == ESP_OK &&
                 uart_set_pin(GPS_UART, GPS_TXD_PIN, GPS_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK;

    // Pulse input: pulled down, so an empty connector reads no pulses rather than noise.
    gpio_config_t in = {
        .pin_bit_mask = 1ull << GPS_PPS_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&in);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);   // harmless if something installed it already
    gpio_isr_handler_add(GPS_PPS_PIN, on_pps, NULL);

    gpio_config_t out = { .pin_bit_mask = 1ull << SYNC_OUT_PIN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(SYNC_OUT_PIN, 0);

    gptimer_config_t t = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000 };
    if (gptimer_new_timer(&t, &sync_timer) == ESP_OK) {
        gptimer_event_callbacks_t cb = { .on_alarm = on_sync_alarm };
        gptimer_register_event_callbacks(sync_timer, &cb, NULL);
        gptimer_enable(sync_timer);
    }
}

size_t plat_gnss_read(uint8_t *buf, size_t cap)
{
    gnss_init_hw();
    if (!uart_ready)
        return 0;
    int n = uart_read_bytes(GPS_UART, buf, cap, 0);
    return n > 0 ? (size_t)n : 0;
}

bool plat_gnss_baud(uint32_t baud)
{
    gnss_init_hw();
    return uart_ready && uart_set_baudrate(GPS_UART, baud) == ESP_OK;
}

bool plat_pps_take(uint64_t *edge_us)
{
    gnss_init_hw();
    return pop(&pps_q, edge_us);
}

bool plat_sync_set(uint32_t hz)
{
    gnss_init_hw();
    if (!sync_timer)
        return false;
    gptimer_stop(sync_timer);
    sync_half_us = 0;
    sync_level = false;
    gpio_set_level(SYNC_OUT_PIN, 0);
    if (!hz)
        return true;
    uint32_t half = 500000 / hz;
    gptimer_alarm_config_t a = { .alarm_count = half, .reload_count = 0, .flags.auto_reload_on_alarm = true };
    if (gptimer_set_alarm_action(sync_timer, &a) != ESP_OK)
        return false;
    gptimer_set_raw_count(sync_timer, 0);
    sync_half_us = half;
    return gptimer_start(sync_timer) == ESP_OK;
}

bool plat_sync_take(uint64_t *edge_us)
{
    return pop(&sync_q, edge_us);
}
