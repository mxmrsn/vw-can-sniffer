// VW diagnostic interface: console and command handling.
//
// Two ways to talk to it over the serial console:
//   - by hand: type "help"
//   - from a program: send "mode m". Echo and prompt are turned off and every
//     command's output ends with a line containing only "$". The "x" command is
//     the workhorse: it sends one UDS request and prints "+ <hex>" for a positive
//     response or "- <reason>" for anything else.

#include "app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can_port.h"
#include "isotp.h"
#include "platform.h"
#include "uds.h"
#include "vw_ecus.h"

#define KEEPALIVE_PERIOD_MS 2000
#define SCAN_TIMEOUT_MS 300

static struct isotp_link link;
static struct uds_client uds;
static struct ecu_address target = { "engine", 0x7E0, 0x7E8, "" };
static bool monitor, machine, keepalive;
static uint32_t led_off_at, keepalive_at;
static uint32_t timeout_ms = 1000;

static char line[700];
static size_t line_len;

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return plat_now_ms();
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

static void print_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        printf(i ? " %02X" : "%02X", data[i]);
    printf("\n");
}

// Move received frames into the transport layer and run its timers.
static void pump(void *ctx)
{
    (void)ctx;
    struct can_frame f;
    bool any = false;
    while (can_port_receive(&f)) {
        any = true;
        plat_led(PLAT_LED_CAN, true);
        led_off_at = plat_now_ms() + 20;
        if (monitor)
            print_frame(&f);
        isotp_on_frame(&link, &f);
    }
    isotp_poll(&link);
    if (led_off_at && (int32_t)(plat_now_ms() - led_off_at) >= 0) {
        plat_led(PLAT_LED_CAN, false);
        led_off_at = 0;
    }
    if (!any)
        plat_idle();
}

static void set_target(const char *name, uint32_t request_id, uint32_t response_id, const char *description)
{
    target.name = name;
    target.request_id = request_id;
    target.response_id = response_id;
    target.description = description;
    // Identifiers above 0x7FF are 29-bit.
    isotp_init(&link, request_id, response_id, request_id > 0x7FF, send_frame, now_ms, NULL);
    uds_init(&uds, &link, pump, NULL);
    uds.p2_ms = timeout_ms;
}

static void print_target(void)
{
    printf("target %s: request %03lX, response %03lX\n", target.name,
           (unsigned long)target.request_id, (unsigned long)target.response_id);
}

static void report(int rc)
{
    if (rc == UDS_ERR_NEGATIVE)
        printf("- nrc %02X %s\n", uds.last_nrc, uds_nrc_str(uds.last_nrc));
    else if (rc == UDS_ERR_TRANSPORT)
        printf("- transport %d\n", uds.last_tp_error);
    else if (rc == UDS_ERR_TIMEOUT)
        printf("- timeout\n");
    else if (rc != UDS_OK)
        printf("- error %s\n", uds_result_str(rc));
}

static bool can_transmit(void)
{
    if (can_port_is_silent()) {
        printf("- silent (type \"silent off\" to allow transmitting on the bus)\n");
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
           "  scan                TesterPresent to every known ECU\n"
           "  tp                  TesterPresent\n"
           "  session <hex>       DiagnosticSessionControl (01 default, 03 extended)\n"
           "  keepalive on|off    send TesterPresent every 2 s to hold a session\n"
           "  vin                 read DID F190\n"
           "  did <hex>           ReadDataByIdentifier\n"
           "  dtc [mask]          read fault codes (status mask, default FF)\n"
           "  raw <hex bytes>     send any UDS request to the target\n"
           "  timeout <ms>        response timeout (default 1000)\n"
           "  x <req> <resp> <hex bytes>   one request, machine-readable reply\n"
           "  mode m|h            machine mode (no echo, \"$\" ends each reply) or human\n");
}

static void cmd_status(void)
{
    struct can_stats s;
    can_port_get_stats(&s);
    printf("bus %s\n", can_port_is_silent() ? "listen-only" : "transmit-enabled");
    printf("rx %lu dropped %lu tx %lu errors %lu\n", (unsigned long)s.rx, (unsigned long)s.rx_dropped,
           (unsigned long)s.tx, (unsigned long)s.errors);
    print_target();
    printf("timeout %lu ms, keepalive %s, monitor %s\n", (unsigned long)timeout_ms,
           keepalive ? "on" : "off", monitor ? "on" : "off");
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
        printf("  %02X%02X%02X status %02X\n", rec[i], rec[i + 1], rec[i + 2], rec[i + 3]);
}

static void do_request(char *hex)
{
    static uint8_t req[300], resp[ISOTP_MAX_PAYLOAD];
    size_t n;
    int len = hex ? parse_hex_bytes(hex, req, sizeof(req)) : -1;
    if (len <= 0) {
        printf("- usage: hex bytes, e.g. 22 F1 90\n");
        return;
    }
    if (!can_transmit())
        return;
    int rc = uds_request(&uds, req, (size_t)len, resp, sizeof(resp), &n);
    if (rc == UDS_OK) {
        printf("+ ");
        print_hex(resp, n);
    } else {
        report(rc);
    }
}

static void cmd_x(char *args)
{
    char *a = args ? strtok(args, " ") : NULL;
    char *b = a ? strtok(NULL, " ") : NULL;
    char *hex = b ? strtok(NULL, "") : NULL;
    if (!hex) {
        printf("- usage: x <req id> <resp id> <hex bytes>\n");
        return;
    }
    uint32_t req = strtoul(a, NULL, 16), resp = strtoul(b, NULL, 16);
    if (req != target.request_id || resp != target.response_id)
        set_target("custom", req, resp, "");
    do_request(hex);
}

static void cmd_scan(void)
{
    if (!can_transmit())
        return;
    struct ecu_address saved = target;
    uint32_t saved_timeout = timeout_ms;
    timeout_ms = SCAN_TIMEOUT_MS;
    for (size_t i = 0; i < VW_ECU_COUNT; i++) {
        set_target(VW_ECUS[i].name, VW_ECUS[i].request_id, VW_ECUS[i].response_id, VW_ECUS[i].description);
        int rc = uds_tester_present(&uds);
        printf("%-8s %03lX %03lX %s\n", VW_ECUS[i].name, (unsigned long)VW_ECUS[i].request_id,
               (unsigned long)VW_ECUS[i].response_id,
               rc == UDS_OK ? "present" : rc == UDS_ERR_NEGATIVE ? "present (refused TesterPresent)" : "no answer");
    }
    timeout_ms = saved_timeout;
    set_target(saved.name, saved.request_id, saved.response_id, saved.description);
}

static void handle_line(char *text)
{
    char *cmd = strtok(text, " ");
    char *rest = strtok(NULL, "");
    if (!cmd)
        return;

    if (!strcmp(cmd, "help")) {
        cmd_help();
    } else if (!strcmp(cmd, "status")) {
        cmd_status();
    } else if (!strcmp(cmd, "mode") && rest) {
        machine = rest[0] == 'm';
        printf(machine ? "machine mode\n" : "human mode\n");
    } else if (!strcmp(cmd, "silent") && rest) {
        can_port_set_silent(strcmp(rest, "off") != 0);
        plat_led(PLAT_LED_SILENT, can_port_is_silent());
        printf(can_port_is_silent() ? "listen-only\n" : "transmit-enabled\n");
    } else if (!strcmp(cmd, "mon") && rest) {
        monitor = !strcmp(rest, "on");
        printf(monitor ? "monitor on\n" : "monitor off\n");
    } else if (!strcmp(cmd, "keepalive") && rest) {
        keepalive = !strcmp(rest, "on");
        keepalive_at = plat_now_ms() + KEEPALIVE_PERIOD_MS;
        printf(keepalive ? "keepalive on\n" : "keepalive off\n");
    } else if (!strcmp(cmd, "timeout") && rest) {
        unsigned long v = strtoul(rest, NULL, 10);
        if (v >= 10 && v <= 60000) {
            timeout_ms = (uint32_t)v;
            uds.p2_ms = timeout_ms;
            printf("timeout %lu ms\n", v);
        } else {
            printf("- timeout must be 10..60000 ms\n");
        }
    } else if (!strcmp(cmd, "ecus")) {
        for (size_t i = 0; i < VW_ECU_COUNT; i++)
            printf("%-8s %03lX %03lX %s\n", VW_ECUS[i].name, (unsigned long)VW_ECUS[i].request_id,
                   (unsigned long)VW_ECUS[i].response_id, VW_ECUS[i].description);
    } else if (!strcmp(cmd, "ecu") && rest) {
        char *a = strtok(rest, " "), *b = strtok(NULL, " ");
        if (a && b) {
            set_target("custom", strtoul(a, NULL, 16), strtoul(b, NULL, 16), "");
            print_target();
            return;
        }
        for (size_t i = 0; a && i < VW_ECU_COUNT; i++) {
            if (!strcmp(a, VW_ECUS[i].name)) {
                set_target(VW_ECUS[i].name, VW_ECUS[i].request_id, VW_ECUS[i].response_id, VW_ECUS[i].description);
                print_target();
                return;
            }
        }
        printf("- unknown ECU; type \"ecus\" for the list\n");
    } else if (!strcmp(cmd, "scan")) {
        cmd_scan();
    } else if (!strcmp(cmd, "tp")) {
        if (can_transmit()) {
            int rc = uds_tester_present(&uds);
            if (rc == UDS_OK)
                printf("+ ECU answered\n");
            else
                report(rc);
        }
    } else if (!strcmp(cmd, "session") && rest) {
        if (can_transmit()) {
            int rc = uds_session_control(&uds, (uint8_t)strtoul(rest, NULL, 16));
            if (rc == UDS_OK)
                printf("+ session active\n");
            else
                report(rc);
        }
    } else if (!strcmp(cmd, "vin")) {
        cmd_did(0xF190, true);
    } else if (!strcmp(cmd, "did") && rest) {
        cmd_did((uint16_t)strtoul(rest, NULL, 16), false);
    } else if (!strcmp(cmd, "dtc")) {
        cmd_dtc(rest ? (uint8_t)strtoul(rest, NULL, 16) : 0xFF);
    } else if (!strcmp(cmd, "raw")) {
        do_request(rest);
    } else if (!strcmp(cmd, "x")) {
        cmd_x(rest);
    } else {
        printf("- unknown command; type \"help\"\n");
    }
}

static void prompt(void)
{
    if (machine)
        printf("$\n");
    else
        printf("> ");
    fflush(stdout);
}

void app_init(void)
{
    plat_led(PLAT_LED_SILENT, true);
    plat_led(PLAT_LED_USB, true);
    set_target(VW_ECUS[0].name, VW_ECUS[0].request_id, VW_ECUS[0].response_id, VW_ECUS[0].description);
}

void app_poll(void)
{
    pump(NULL);

    if (keepalive && !can_port_is_silent() && (int32_t)(plat_now_ms() - keepalive_at) >= 0) {
        // TesterPresent with "suppress positive response": nothing comes back.
        static const uint8_t tp[2] = { UDS_SID_TESTER_PRESENT, 0x80 };
        if (!isotp_tx_busy(&link))
            isotp_send(&link, tp, sizeof(tp));
        keepalive_at = plat_now_ms() + KEEPALIVE_PERIOD_MS;
    }

    int ch;
    while ((ch = plat_getchar()) >= 0) {
        if (ch == '\r' || ch == '\n') {
            if (!line_len)
                continue;
            if (!machine)
                putchar('\n');
            line[line_len] = 0;
            line_len = 0;
            handle_line(line);
            prompt();
        } else if ((ch == 8 || ch == 127) && line_len) {
            line_len--;
            if (!machine)
                printf("\b \b");
        } else if (isprint(ch) && line_len < sizeof(line) - 1) {
            line[line_len++] = (char)ch;
            if (!machine)
                putchar(ch);
        }
        fflush(stdout);
    }
}
