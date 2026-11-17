#include "can_port.h"

#include <stdio.h>
#include <string.h>

#include "can2040.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"

#include "../pins.h"

#define RX_QUEUE_SIZE 128 // power of two

static struct can2040 cbus;
static struct can_frame rx_queue[RX_QUEUE_SIZE];
static volatile uint32_t rx_head, rx_tail; // head written by the IRQ, tail by the main loop
static volatile struct can_stats stats;
static bool silent = true;

static void on_can2040_event(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg)
{
    (void)cd;
    if (notify == CAN2040_NOTIFY_RX) {
        if (msg->id & CAN2040_ID_RTR)
            return;
        uint32_t head = rx_head;
        if (head - rx_tail >= RX_QUEUE_SIZE) {
            stats.rx_dropped++;
            return;
        }
        struct can_frame *f = &rx_queue[head % RX_QUEUE_SIZE];
        f->ext = (msg->id & CAN2040_ID_EFF) != 0;
        f->id = msg->id & (f->ext ? 0x1FFFFFFF : 0x7FF);
        f->dlc = msg->dlc > 8 ? 8 : (uint8_t)msg->dlc;
        memcpy(f->data, msg->data, 8);
        rx_head = head + 1;
        stats.rx++;
    } else if (notify == CAN2040_NOTIFY_TX) {
        stats.tx++;
    } else if (notify & CAN2040_NOTIFY_ERROR) {
        stats.errors++;
    }
}

static void pio_irq_handler(void)
{
    can2040_pio_irq_handler(&cbus);
}

void can_port_init(uint32_t bitrate)
{
    gpio_init(CAN_SILENT_PIN);
    gpio_set_dir(CAN_SILENT_PIN, GPIO_OUT);
    can_port_set_silent(true);

    can2040_setup(&cbus, 0); // PIO0
    can2040_callback_config(&cbus, on_can2040_event);
    irq_set_exclusive_handler(PIO0_IRQ_0, pio_irq_handler);
    irq_set_priority(PIO0_IRQ_0, 1);
    irq_set_enabled(PIO0_IRQ_0, true);
    can2040_start(&cbus, clock_get_hz(clk_sys), bitrate, CAN_RX_PIN, CAN_TX_PIN);
}

void can_port_set_silent(bool on)
{
    silent = on;
    gpio_put(CAN_SILENT_PIN, on ? 1 : 0);
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
    struct can2040_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id = f->ext ? ((f->id & 0x1FFFFFFF) | CAN2040_ID_EFF) : (f->id & 0x7FF);
    msg.dlc = f->dlc;
    memcpy(msg.data, f->data, 8);
    return can2040_transmit(&cbus, &msg) < 0 ? -1 : 0;
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
    snprintf(detail, cap, "can2040 running %s, rx %lu tx %lu, %lu errors", silent ? "listen-only" : "transmit-enabled",
             (unsigned long)stats.rx, (unsigned long)stats.tx, (unsigned long)stats.errors);
    return SELFTEST_PASS;
}

enum selftest_result can_port_loopback(char *detail, size_t cap)
{
    // can2040 does not read its own frames back, and without another node no frame is acknowledged.
    snprintf(detail, cap, "not on this board: test against the bench car instead");
    return SELFTEST_SKIP;
}
