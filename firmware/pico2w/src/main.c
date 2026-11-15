// VW diagnostic interface, bench firmware for the Pico 2 W board (PCB v0.2.0).
// A serial console over the Pico's USB port drives CAN, ISO-TP and UDS.
// Type "help" for the command list.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#include "../pins.h"
#include "can_port.h"
#include "isotp.h"
#include "uds.h"
#include "vw_ecus.h"

#define CAN_BITRATE 500000

static struct isotp_link link;
static struct uds_client uds;
static const struct ecu_address *target = &VW_ECUS[0];
static struct ecu_address custom_target = { "custom", 0, 0, "custom identifiers" };
static bool monitor;
static uint32_t led_off_at;

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return to_ms_since_boot(get_absolute_time());
}

static int send_frame(void *ctx, const struct can_frame *f)
{
    (void)ctx;
    return can_port_send(f);
}

static void print_frame(const struct can_frame *f)
{
    printf(f->ext ? "%08lX" : "%03lX", (unsigned long)f->id);
    printf(" [%u]", f->dlc);
    for (int i = 0; i < f->dlc; i++)
        printf(" %02X", f->data[i]);
    printf("\n");
}

// Move received frames into the transport layer and run its timers.
static void pump(void *ctx)
{
    (void)ctx;
    struct can_frame f;
    while (can_port_receive(&f)) {
        gpio_put(LED_CAN_PIN, 1);
        led_off_at = now_ms(NULL) + 20;
        if (monitor)
            print_frame(&f);
        isotp_on_frame(&link, &f);
    }
    isotp_poll(&link);
    if (led_off_at && (int32_t)(now_ms(NULL) - led_off_at) >= 0) {
        gpio_put(LED_CAN_PIN, 0);
        led_off_at = 0;
    }
}

static void select_target(const struct ecu_address *ecu)
{
    target = ecu;
    isotp_init(&link, ecu->request_id, ecu->response_id, false, send_frame, now_ms, NULL);
    uds_init(&uds, &link, pump, NULL);
    printf("target %s: request %03lX, response %03lX (%s)\n", ecu->name,
           (unsigned long)ecu->request_id, (unsigned long)ecu->response_id, ecu->description);
}

static void print_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        printf("%02X ", data[i]);
    printf("\n");
}

static void report(int rc)
{
    if (rc == UDS_ERR_NEGATIVE)
        printf("refused by ECU: NRC 0x%02X (%s)\n", uds.last_nrc, uds_nrc_str(uds.last_nrc));
    else if (rc == UDS_ERR_TRANSPORT)
        printf("transport error %d\n", uds.last_tp_error);
    else if (rc != UDS_OK)
        printf("failed: %s\n", uds_result_str(rc));
}

static bool can_transmit(void)
{
    if (can_port_is_silent()) {
        printf("listen-only: type \"silent off\" first to allow transmitting on the bus\n");
        return false;
    }
    return true;
}

static int parse_hex_bytes(char *text, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (char *tok = strtok(text, " "); tok; tok = strtok(NULL, " ")) {
        char *end;
        unsigned long v = strtoul(tok, &end, 16);
        if (*end || v > 0xFF || n >= cap)
            return -1;
        out[n++] = (uint8_t)v;
    }
    return (int)n;
}

static void cmd_help(void)
{
    printf("commands:\n"
           "  status              bus mode, counters, target ECU\n"
           "  silent on|off       listen-only on (default) or off\n"
           "  mon on|off          print every received CAN frame\n"
           "  ecus                list known ECU addresses\n"
           "  ecu <name>          select a target from the list\n"
           "  ecu <req> <resp>    select a target by hex CAN IDs\n"
           "  tp                  TesterPresent\n"
           "  session <hex>       DiagnosticSessionControl (01 default, 03 extended)\n"
           "  vin                 read DID F190\n"
           "  did <hex>           ReadDataByIdentifier\n"
           "  dtc [mask]          read fault codes (status mask, default FF)\n"
           "  raw <hex bytes>     send any UDS request and print the response\n");
}

static void cmd_status(void)
{
    struct can_stats s;
    can_port_get_stats(&s);
    printf("bus: %d bit/s, %s\n", CAN_BITRATE, can_port_is_silent() ? "listen-only" : "transmit enabled");
    printf("rx %lu (dropped %lu), tx %lu, errors %lu\n", (unsigned long)s.rx, (unsigned long)s.rx_dropped,
           (unsigned long)s.tx, (unsigned long)s.errors);
    printf("target %s: %03lX -> %03lX\n", target->name, (unsigned long)target->request_id,
           (unsigned long)target->response_id);
}

static void cmd_did(uint16_t did, bool as_text)
{
    static uint8_t data[ISOTP_MAX_PAYLOAD];
    size_t n;
    if (!can_transmit())
        return;
    int rc = uds_read_did(&uds, did, data, sizeof(data), &n);
    if (rc != UDS_OK) {
        report(rc);
        return;
    }
    printf("%04X: ", did);
    if (as_text) {
        for (size_t i = 0; i < n; i++)
            putchar(isprint(data[i]) ? data[i] : '.');
        printf("\n");
    } else {
        print_hex(data, n);
    }
}

static void cmd_dtc(uint8_t mask)
{
    static uint8_t rec[ISOTP_MAX_PAYLOAD];
    size_t n;
    uint8_t avail = 0;
    if (!can_transmit())
        return;
    int rc = uds_read_dtcs(&uds, mask, &avail, rec, sizeof(rec), &n);
    if (rc != UDS_OK) {
        report(rc);
        return;
    }
    printf("%u fault code(s), availability mask %02X\n", (unsigned)(n / 4), avail);
    for (size_t i = 0; i + 4 <= n; i += 4)
        printf("  %02X%02X%02X  status %02X\n", rec[i], rec[i + 1], rec[i + 2], rec[i + 3]);
}

static void cmd_raw(char *args)
{
    static uint8_t req[256], resp[ISOTP_MAX_PAYLOAD];
    size_t n;
    int len = parse_hex_bytes(args, req, sizeof(req));
    if (len <= 0) {
        printf("usage: raw <hex bytes>, e.g. raw 22 F1 90\n");
        return;
    }
    if (!can_transmit())
        return;
    int rc = uds_request(&uds, req, (size_t)len, resp, sizeof(resp), &n);
    if (rc == UDS_OK)
        print_hex(resp, n);
    else
        report(rc);
}

static void handle_line(char *line)
{
    char *cmd = strtok(line, " ");
    char *rest = strtok(NULL, "");
    if (!cmd)
        return;

    if (!strcmp(cmd, "help")) {
        cmd_help();
    } else if (!strcmp(cmd, "status")) {
        cmd_status();
    } else if (!strcmp(cmd, "silent") && rest) {
        can_port_set_silent(strcmp(rest, "off") != 0);
        gpio_put(LED_SILENT_PIN, can_port_is_silent());
        printf(can_port_is_silent() ? "listen-only\n" : "transmit enabled\n");
    } else if (!strcmp(cmd, "mon") && rest) {
        monitor = !strcmp(rest, "on");
    } else if (!strcmp(cmd, "ecus")) {
        for (size_t i = 0; i < VW_ECU_COUNT; i++)
            printf("  %-8s %03lX -> %03lX  %s\n", VW_ECUS[i].name, (unsigned long)VW_ECUS[i].request_id,
                   (unsigned long)VW_ECUS[i].response_id, VW_ECUS[i].description);
    } else if (!strcmp(cmd, "ecu") && rest) {
        char *a = strtok(rest, " "), *b = strtok(NULL, " ");
        if (a && b) {
            custom_target.request_id = strtoul(a, NULL, 16) & 0x7FF;
            custom_target.response_id = strtoul(b, NULL, 16) & 0x7FF;
            select_target(&custom_target);
            return;
        }
        for (size_t i = 0; a && i < VW_ECU_COUNT; i++) {
            if (!strcmp(a, VW_ECUS[i].name)) {
                select_target(&VW_ECUS[i]);
                return;
            }
        }
        printf("unknown ECU; type \"ecus\" for the list\n");
    } else if (!strcmp(cmd, "tp")) {
        if (can_transmit()) {
            int rc = uds_tester_present(&uds);
            if (rc == UDS_OK)
                printf("ECU answered\n");
            else
                report(rc);
        }
    } else if (!strcmp(cmd, "session") && rest) {
        if (can_transmit()) {
            int rc = uds_session_control(&uds, (uint8_t)strtoul(rest, NULL, 16));
            if (rc == UDS_OK)
                printf("session active\n");
            else
                report(rc);
        }
    } else if (!strcmp(cmd, "vin")) {
        cmd_did(0xF190, true);
    } else if (!strcmp(cmd, "did") && rest) {
        cmd_did((uint16_t)strtoul(rest, NULL, 16), false);
    } else if (!strcmp(cmd, "dtc")) {
        cmd_dtc(rest ? (uint8_t)strtoul(rest, NULL, 16) : 0xFF);
    } else if (!strcmp(cmd, "raw") && rest) {
        cmd_raw(rest);
    } else {
        printf("unknown command; type \"help\"\n");
    }
}

int main(void)
{
    stdio_init_all();

    const uint led_pins[] = { LED_CAN_PIN, LED_WIFI_PIN, LED_USB_PIN, LED_SILENT_PIN };
    for (size_t i = 0; i < sizeof(led_pins) / sizeof(led_pins[0]); i++) {
        gpio_init(led_pins[i]);
        gpio_set_dir(led_pins[i], GPIO_OUT);
    }
    gpio_put(LED_SILENT_PIN, 1);
    gpio_put(LED_USB_PIN, 1);

    can_port_init(CAN_BITRATE);
    select_target(&VW_ECUS[0]);

    static char line[320];
    size_t len = 0;
    for (;;) {
        pump(NULL);
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT)
            continue;
        if (ch == '\r' || ch == '\n') {
            if (len) {
                putchar('\n');
                line[len] = 0;
                handle_line(line);
                len = 0;
                printf("> ");
            }
        } else if ((ch == 8 || ch == 127) && len) {
            len--;
            printf("\b \b");
        } else if (isprint(ch) && len < sizeof(line) - 1) {
            line[len++] = (char)ch;
            putchar(ch);
        }
    }
}
