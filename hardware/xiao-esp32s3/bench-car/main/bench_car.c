// Bench car: the host simulator's car (control modules behind a gateway, the engine
// model, broadcast traffic) on a real CAN bus, through the ESP32-S3's CAN controller
// and the board's transceiver. Wire it to an interface board (CAN-H, CAN-L, GND; JP1
// bridged on both for termination) and test the interface on the desk as if it were
// plugged into the car.
//
// The console on the module's USB-C port picks the scenario (the same settings as the
// simulator's VWSIM, see sim/sim_config.c), holds the ignition in a state, and reports
// counters. The status LED shows the ignition: off dark, on amber, cranking red, running
// green; the CAN LED flashes with traffic.
//
// Not yet run on hardware.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/twai.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "leds.h"
#include "pins.h"
#include "platform.h"
#include "sim.h"

#define DEFAULT_SCENARIO "realistic,gateway=open"

static char scenario[256] = DEFAULT_SCENARIO;
static uint32_t tx_refused, frames_in, burst_left, burst_id = 0x123;
static bool monitor;
static uint32_t led_off_at;

uint32_t plat_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---- CAN: normal mode, the transceiver allowed to drive the bus
static void can_init(void)
{
    gpio_reset_pin(CAN_SILENT_PIN);
    gpio_set_direction(CAN_SILENT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CAN_SILENT_PIN, 0);   // a car's modules always acknowledge
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    g.rx_queue_len = 64;
    g.tx_queue_len = 64;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    ESP_ERROR_CHECK(twai_driver_install(&g, &t, &f));
    ESP_ERROR_CHECK(twai_start());
}

static void activity(void)
{
    leds_set(BOARD_LED_CAN, true);
    led_off_at = plat_now_ms() + 30;
}

static int car_send(const struct can_frame *f)
{
    twai_message_t m;
    memset(&m, 0, sizeof(m));
    m.extd = f->ext;
    m.identifier = f->id;
    m.data_length_code = f->dlc;
    memcpy(m.data, f->data, 8);
    if (twai_transmit(&m, pdMS_TO_TICKS(5)) != ESP_OK) {
        tx_refused++;
        return -1;
    }
    activity();
    return 0;
}

static void can_poll(void)
{
    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) {
        if (m.rtr)
            continue;
        struct can_frame f = { .id = m.identifier, .ext = m.extd, .dlc = m.data_length_code > 8 ? 8 : m.data_length_code };
        memcpy(f.data, m.data, 8);
        frames_in++;
        activity();
        if (monitor) {
            printf(f.ext ? "%08lX [%u]" : "%03lX [%u]", (unsigned long)f.id, f.dlc);
            for (int i = 0; i < f.dlc; i++)
                printf(" %02X", f.data[i]);
            printf("\n");
        }
        car_on_frame(&f);
    }
    // a burst of filler frames, for the interface's receive-overrun test
    while (burst_left) {
        struct can_frame f = { .id = burst_id, .dlc = 8 };
        memcpy(f.data, &burst_left, sizeof(burst_left));
        if (car_send(&f))
            break;
        burst_left--;
    }
    uint32_t alerts;
    if (twai_read_alerts(&alerts, 0) == ESP_OK && (alerts & TWAI_ALERT_BUS_OFF))
        twai_initiate_recovery();
}

// ---- console
static const char *ign_names[] = { "off", "on", "crank", "run" };

static void show_ignition(void)
{
    enum ignition s = sim_ignition(car_time());
    // the status LED: dark with the ignition off, amber on, red cranking, green running.
    // leds.c offers on/off per LED; "silent" shows amber when off and green when on,
    // so: running = green (on), anything else with the ignition on = amber (off).
    leds_set(BOARD_LED_SILENT, s == IGN_RUN);
    leds_set(BOARD_LED_LOG, s == IGN_CRANK);
    leds_set(BOARD_LED_BLE, s != IGN_OFF);
}

static void cmd_status(void)
{
    struct car_stats s;
    twai_status_info_t t;
    car_get_stats(&s);
    twai_get_status_info(&t);
    const struct engine_state *e = engine();
    printf("scenario %s\n", scenario);
    printf("ignition %s, %lu s since start\n", ign_names[sim_ignition(car_time())], (unsigned long)(car_time() / 1000));
    printf("engine %.0f rpm, %.0f km/h, coolant %.0f C, stft %+.1f %%, ltft %+.1f %%\n", e->rpm, e->speed_kmh,
           e->coolant_c, e->stft_pct, e->ltft_pct);
    printf("misfires this cycle: %u %u %u %u\n", e->mis_now[0], e->mis_now[1], e->mis_now[2], e->mis_now[3]);
    printf("frames in %lu, out %lu, requests %lu, send refused %lu\n", (unsigned long)frames_in,
           (unsigned long)s.sent, (unsigned long)s.requests, (unsigned long)tx_refused);
    static const char *states[] = { "stopped", "running", "bus-off", "recovering" };
    printf("bus %s, tx errors %lu, rx errors %lu, bus errors %lu, tx failed %lu, arbitration lost %lu\n",
           t.state < 4 ? states[t.state] : "?", (unsigned long)t.tx_error_counter, (unsigned long)t.rx_error_counter,
           (unsigned long)t.bus_error_count, (unsigned long)t.tx_failed_count, (unsigned long)t.arb_lost_count);
}

static void cmd_help(void)
{
    printf("bench car: the simulator's car on a real CAN bus\n"
           "  status              scenario, ignition, engine, counters\n"
           "  scenario <spec>     restart as e.g. gti, lossy, realistic,gateway=open,drive=1\n"
           "  ign off|on|crank|run  hold the ignition in a state\n"
           "  mon on|off          print every frame received from the interface\n"
           "  burst <n> [hex id]  send n filler frames as fast as possible\n");
}

static void handle(char *line)
{
    char *cmd = strtok(line, " ");
    char *rest = strtok(NULL, "");
    if (!cmd)
        return;
    if (!strcmp(cmd, "help")) {
        cmd_help();
    } else if (!strcmp(cmd, "status")) {
        cmd_status();
    } else if (!strcmp(cmd, "scenario") && rest) {
        char spec[sizeof(scenario)];
        snprintf(spec, sizeof(spec), "%s", rest);
        if (car_init(spec, car_send)) {
            snprintf(scenario, sizeof(scenario), "%s", spec);
            printf("scenario %s\n", scenario);
        } else {
            car_init(scenario, car_send);
            printf("- cannot use \"%s\"; still %s\n", spec, scenario);
        }
    } else if (!strcmp(cmd, "ign") && rest) {
        for (int i = 0; i < 4; i++) {
            if (!strcmp(rest, ign_names[i])) {
                sim_set_ignition((enum ignition)i, car_time());
                printf("ignition %s\n", rest);
                return;
            }
        }
        printf("- usage: ign off|on|crank|run\n");
    } else if (!strcmp(cmd, "mon") && rest) {
        monitor = !strcmp(rest, "on");
    } else if (!strcmp(cmd, "burst") && rest) {
        char *n = strtok(rest, " ");
        char *id = strtok(NULL, " ");
        burst_left = (uint32_t)strtoul(n, NULL, 10);
        if (id)
            burst_id = (uint32_t)strtoul(id, NULL, 16) & 0x7FF;
        printf("sending %lu frames as %03lX\n", (unsigned long)burst_left, (unsigned long)burst_id);
    } else {
        printf("- unknown command; type help\n");
    }
}

static void console_poll(void)
{
    static char line[300];
    static size_t len;
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (len)
                handle(line);
            len = 0;
            printf("> ");
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
        }
    }
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t cfg = { .rx_buffer_size = 512, .tx_buffer_size = 2048 };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdout, NULL, _IONBF, 0);
    leds_init();
    can_init();
    car_init(scenario, car_send);
    printf("bench car, scenario %s; type help\n> ", scenario);
    for (;;) {
        can_poll();
        car_poll();
        console_poll();
        show_ignition();
        if (led_off_at && (int32_t)(plat_now_ms() - led_off_at) >= 0) {
            leds_set(BOARD_LED_CAN, false);
            led_off_at = 0;
        }
        vTaskDelay(1);
    }
}
