// VW diagnostic interface: console and command handling.
//
// Two ways to talk to it over the serial console:
//   - by hand: type "help"
//   - from a program: send "mode m". Echo and prompt are turned off and every
//     command's output ends with a line containing only "$". The "x" command is
//     the workhorse: it sends one UDS request and prints "+ <hex>" for a positive
//     response or "- <reason>" for anything else.
//   - from an app: send "mode b". The console answers "binary mode" and from
//     then on speaks only the packet protocol in proto.h, until it is told to exit.

#include "app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_link.h"
#include "can_port.h"
#include "isotp.h"
#include "logger.h"
#include "platform.h"
#include "proto.h"
#include "storage.h"
#include "uds.h"
#include "vw_ecus.h"

#define KEEPALIVE_PERIOD_MS 2000
#define SCAN_TIMEOUT_MS 300
#define CAN_BITRATE 500000
#define POLL_MAX 8
#define POLL_TIMEOUT_MS 150
#define AUTORUN_FILE "AUTORUN.TXT"

// Requests repeated on a timer, so a log holds live data even when the car's
// gateway passes nothing but diagnostic answers to the OBD port.
struct poll_entry {
    uint32_t request_id, response_id, period_ms, next_at;
    uint8_t len, data[APP_POLL_MAX_LEN];
};
static struct poll_entry polls[POLL_MAX];
static size_t poll_count, poll_next;

static struct isotp_link link;
static struct uds_client uds;
static struct ecu_address target = { "engine", 0x7E0, 0x7E8, "" };
static bool monitor, machine, keepalive, binary;
static struct proto_port usb_port, ble_port;
static uint32_t led_off_at, keepalive_at;
static uint32_t user_ms, bus_ms; // last request from a person, last CAN frame: for the power manager
static void power_check(void);
static void cmd_power(char *rest);
static void gnss_service(void);
static void cmd_gps(char *rest);
static void cmd_sync(char *rest);
static void cmd_selftest(char *rest);
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
    int rc = can_port_send(f);
    if (rc == 0)
        logger_can(f, plat_now_ms(), true);
    return rc;
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
        bus_ms = plat_now_ms();
        plat_led(PLAT_LED_CAN, true);
        led_off_at = plat_now_ms() + 20;
        logger_can(&f, plat_now_ms(), false);
        proto_send_can_event(&f, plat_now_ms());
        if (monitor && !binary)
            print_frame(&f);
        isotp_on_frame(&link, &f);
    }
    isotp_poll(&link);
    gnss_service();
    plat_ble_poll();
    if (logger_active()) {
        struct can_stats s;
        can_port_get_stats(&s);
        logger_poll(plat_now_ms(), s.rx_dropped);
        if (!logger_active())
            plat_led(PLAT_LED_WIFI, false); // the card failed or was pulled
    }
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

void app_set_silent(bool on)
{
    can_port_set_silent(on);
    plat_led(PLAT_LED_SILENT, can_port_is_silent());
}

void app_set_monitor(bool on)
{
    monitor = on;
}

void app_set_keepalive(bool on)
{
    keepalive = on;
    keepalive_at = plat_now_ms() + KEEPALIVE_PERIOD_MS;
}

uint8_t app_flags(void)
{
    return (uint8_t)((can_port_is_silent() ? PROTO_FLAG_SILENT : 0) | (monitor ? PROTO_FLAG_MONITOR : 0) |
                     (keepalive ? PROTO_FLAG_KEEPALIVE : 0) | (logger_active() ? PROTO_FLAG_LOGGING : 0));
}

bool app_set_logging(bool on)
{
    if (on) {
        struct can_stats s;
        can_port_get_stats(&s);
        logger_start(plat_now_ms(), CAN_BITRATE, s.rx_dropped);
    } else {
        logger_stop();
    }
    plat_led(PLAT_LED_WIFI, logger_active()); // no Wi-Fi yet, so this LED shows "logging"
    return logger_active() == on;
}

void app_set_time(uint32_t epoch)
{
    logger_set_time(epoch, plat_now_ms());
}

void app_log_mark(const char *text)
{
    logger_mark(text, plat_now_ms());
}

bool app_poll_add(uint32_t request_id, uint32_t response_id, uint32_t period_ms, const uint8_t *req, size_t len)
{
    if (poll_count >= POLL_MAX || len == 0 || len > APP_POLL_MAX_LEN || period_ms < 20 || period_ms > 3600000)
        return false;
    struct poll_entry *e = &polls[poll_count++];
    e->request_id = request_id;
    e->response_id = response_id;
    e->period_ms = period_ms;
    e->next_at = plat_now_ms();
    e->len = (uint8_t)len;
    memcpy(e->data, req, len);
    return true;
}

void app_poll_clear(void)
{
    poll_count = 0;
}

static bool usb_write(const uint8_t *data, size_t len, bool is_event)
{
    (void)is_event;
    plat_write(data, len);
    return true;
}

static bool ble_write(const uint8_t *data, size_t len, bool is_event)
{
    bool ok = ble_link_write(data, len, is_event);
    if (ok)
        plat_ble_kick();
    return ok;
}

void app_leave_binary(void)
{
    proto_detach(&usb_port);
    binary = false;
    machine = false;
    line_len = 0;
}

int app_uds_request(uint32_t request_id, uint32_t response_id, uint32_t request_timeout_ms,
                    const uint8_t *req, size_t req_len,
                    uint8_t *resp, size_t resp_cap, size_t *resp_len, int *detail)
{
    if (request_id != target.request_id || response_id != target.response_id)
        set_target("custom", request_id, response_id, "");
    uds.p2_ms = request_timeout_ms ? request_timeout_ms : timeout_ms;
    int rc = uds_request(&uds, req, req_len, resp, resp_cap, resp_len);
    uds.p2_ms = timeout_ms;
    *detail = rc == UDS_ERR_NEGATIVE ? uds.last_nrc : rc == UDS_ERR_TRANSPORT ? uds.last_tp_error : 0;
    return rc;
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
           "  mode m|h            machine mode (no echo, \"$\" ends each reply) or human\n"
           "  log on|off          record every CAN frame to the microSD card\n"
           "  log                 logging state and counters\n"
           "  log mark <text>     put a note in the log\n"
           "  log ls              files on the card\n"
           "  log get <f> <o> <n> read n bytes (up to 1024) of file f from offset o, as hex\n"
           "  time <unix seconds> tell the interface the time, for log files\n"
           "  poll <req> <resp> <ms> <hex bytes>   repeat a request on a timer\n"
           "  poll list | poll clear\n"
           "  ble                 Bluetooth state, name and pairing passkey\n"
           "  ble pair | forget   allow a new phone to pair for 2 minutes, or forget all phones\n"
           "  power               power state; power stay on|off, idle <s>, cutoff <mV>, sleep (dongle)\n"
           "  gps                 GPS fix, pulse lock and clock drift; gps baud <n> (dongle)\n"
           "  sync off|<hz>       camera sync output, 1-100 Hz, locked to the GPS pulse (dongle)\n"
           "  selftest [active] [force]   bring-up check of every part; active also drives the\n"
           "                      CAN bus (one loopback frame) and the sync output\n"
           "  mode b              binary packet protocol, for apps\n");
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
    printf("log %s, %u poll request(s)\n", logger_active() ? "on" : "off", (unsigned)poll_count);
}

static void print_file(const char *name, uint32_t size, void *ctx)
{
    (void)ctx;
    printf("%s %lu\n", name, (unsigned long)size);
}

// "log get NAME OFFSET LENGTH": "+ <file size> <hex bytes>", at most 1024 bytes per line.
static void log_get(char *args)
{
    static uint8_t buf[1024];
    char *name = strtok(args, " ");
    char *off = strtok(NULL, " ");
    char *len = strtok(NULL, " ");
    unsigned long offset = off ? strtoul(off, NULL, 10) : 0, want = len ? strtoul(len, NULL, 10) : sizeof(buf);
    if (!name) {
        printf("- usage: log get <file> <offset> <length, at most 1024>\n");
        return;
    }
    if (want == 0 || want > sizeof(buf))
        want = sizeof(buf);
    uint32_t size = 0;
    int n = logger_read_file(name, (uint32_t)offset, buf, want, &size);
    if (n == LOGGER_READ_BAD_NAME)
        printf("- bad file name\n");
    else if (n == LOGGER_READ_BUSY)
        printf("- busy: that file is being recorded; log off first\n");
    else if (n < 0)
        printf("- no card, or no such file\n");
    else {
        printf("+ %lu", (unsigned long)size);
        for (int i = 0; i < n; i++)
            printf(" %02X", buf[i]);
        printf("\n");
    }
}

static void cmd_log(char *rest)
{
    struct logger_stats s;
    if (rest && !strcmp(rest, "on")) {
        if (!app_set_logging(true)) {
            printf("- no card, or the log file could not be created\n");
            return;
        }
    } else if (rest && !strcmp(rest, "off")) {
        app_set_logging(false);
    } else if (rest && !strncmp(rest, "mark ", 5)) {
        if (!logger_active()) {
            printf("- not logging\n");
            return;
        }
        app_log_mark(rest + 5);
    } else if (rest && !strcmp(rest, "ls")) {
        int n = storage_list(print_file, NULL);
        if (n < 0)
            printf("- no card\n");
        else
            printf("%d file(s)\n", n);
        return;
    } else if (rest && !strncmp(rest, "get ", 4)) {
        log_get(rest + 4);
        return;
    } else if (rest) {
        printf("- usage: log on|off, log mark <text>, log ls, log get <file> <offset> <length>, or log alone\n");
        return;
    }
    logger_get_stats(&s);
    printf("log %s %s frames %lu bytes %lu lost %lu\n", s.active ? "on" : "off", s.name[0] ? s.name : "-",
           (unsigned long)s.records, (unsigned long)s.bytes, (unsigned long)s.lost);
}

static void cmd_ble(char *rest)
{
    if (rest && !strcmp(rest, "pair")) {
        printf(plat_ble_open_pairing() ? "pairing open for 2 minutes\n" : "- Bluetooth is not available\n");
        return;
    }
    if (rest && !strcmp(rest, "forget")) {
        printf(plat_ble_forget() ? "all paired phones forgotten\n" : "- Bluetooth is not available\n");
        return;
    }
    if (rest) {
        printf("- usage: ble, ble pair, ble forget\n");
        return;
    }
    const struct ble_link_info *b = &ble_info;
    if (!b->available) {
        printf("ble not available\n");
        return;
    }
    printf("ble %s %s%s\n", b->name, b->address, b->emulated ? " (simulator: TCP stand-in, no pairing)" : "");
    if (!b->emulated)
        printf("passkey %06lu, pairing %s\n", (unsigned long)b->passkey, b->pairing_open ? "open" : "closed");
    printf("%s, mtu %u, connections %lu\n",
           b->subscribed ? "phone connected" : b->connected ? "phone connected, notifications off" : "no phone",
           b->mtu, (unsigned long)b->connections);
    printf("rx %lu tx %lu bytes, rx overflow %lu, packets refused %lu, events dropped %lu\n",
           (unsigned long)b->rx_bytes, (unsigned long)b->tx_bytes, (unsigned long)b->rx_overflow,
           (unsigned long)b->tx_refused, (unsigned long)ble_port.events_dropped);
}

static void cmd_poll(char *rest)
{
    static uint8_t req[APP_POLL_MAX_LEN];
    if (rest && !strcmp(rest, "clear")) {
        app_poll_clear();
        printf("poll list cleared\n");
        return;
    }
    if (rest && !strcmp(rest, "list")) {
        for (size_t i = 0; i < poll_count; i++) {
            printf("%03lX %03lX every %lu ms: ", (unsigned long)polls[i].request_id,
                   (unsigned long)polls[i].response_id, (unsigned long)polls[i].period_ms);
            print_hex(polls[i].data, polls[i].len);
        }
        printf("%u of %u poll requests in use\n", (unsigned)poll_count, POLL_MAX);
        return;
    }
    char *a = rest ? strtok(rest, " ") : NULL;
    char *b = a ? strtok(NULL, " ") : NULL;
    char *c = b ? strtok(NULL, " ") : NULL;
    char *hex = c ? strtok(NULL, "") : NULL;
    int len = hex ? parse_hex_bytes(hex, req, sizeof(req)) : -1;
    if (len <= 0 || !app_poll_add(strtoul(a, NULL, 16), strtoul(b, NULL, 16), strtoul(c, NULL, 10), req, (size_t)len)) {
        printf("- usage: poll <req id> <resp id> <period ms, 20 or more> <1-%d hex bytes>; at most %d entries\n",
               APP_POLL_MAX_LEN, POLL_MAX);
        return;
    }
    printf("poll %u added\n", (unsigned)poll_count);
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
    } else if (!strcmp(cmd, "mode") && rest && rest[0] == 'b') {
        proto_port_init(&usb_port, usb_write);
        proto_attach(&usb_port);
        binary = true;
        printf("binary mode\n");
    } else if (!strcmp(cmd, "mode") && rest) {
        machine = rest[0] == 'm';
        printf(machine ? "machine mode\n" : "human mode\n");
    } else if (!strcmp(cmd, "silent") && rest) {
        app_set_silent(strcmp(rest, "off") != 0);
        printf(can_port_is_silent() ? "listen-only\n" : "transmit-enabled\n");
    } else if (!strcmp(cmd, "mon") && rest) {
        app_set_monitor(!strcmp(rest, "on"));
        printf(monitor ? "monitor on\n" : "monitor off\n");
    } else if (!strcmp(cmd, "keepalive") && rest) {
        app_set_keepalive(!strcmp(rest, "on"));
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
    } else if (!strcmp(cmd, "ble")) {
        cmd_ble(rest);
    } else if (!strcmp(cmd, "power")) {
        cmd_power(rest);
    } else if (!strcmp(cmd, "gps")) {
        cmd_gps(rest);
    } else if (!strcmp(cmd, "sync")) {
        cmd_sync(rest);
    } else if (!strcmp(cmd, "log")) {
        cmd_log(rest);
    } else if (!strcmp(cmd, "poll")) {
        cmd_poll(rest);
    } else if (!strcmp(cmd, "selftest")) {
        cmd_selftest(rest);
    } else if (!strcmp(cmd, "time") && rest) {
        app_set_time((uint32_t)strtoul(rest, NULL, 10));
        printf("time set\n");
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

// Commands in AUTORUN.TXT on the card run at power-up, one per line, as if typed.
// That is how the board logs on its own: for example "silent off", a few "poll"
// lines and "log on". Lines starting with # are comments.
static void run_boot_script(void)
{
    static char script[2048];
    int n = storage_read_file(AUTORUN_FILE, script, sizeof(script) - 1);
    if (n <= 0)
        return;
    script[n] = 0;
    char *next = script;
    while (next) {
        char *text = next;
        next = strchr(text, '\n');
        if (next)
            *next++ = 0;
        size_t len = strlen(text);
        while (len && (text[len - 1] == '\r' || text[len - 1] == ' '))
            text[--len] = 0;
        if (!len || text[0] == '#' || !strncmp(text, "mode", 4))
            continue;
        printf("autorun: %s\n", text);
        handle_line(text);
    }
}

// ---- power (boards built with APP_POWER)
#ifdef APP_POWER
static void power_check(void)
{
    static uint32_t next;
    static bool started;
    uint32_t now = plat_now_ms();
    if (started && (int32_t)(now - next) < 0)
        return;
    started = true;
    next = now + 500;
    struct power_inputs in = {
        .now_ms = now, .vbat_mv = plat_battery_mv(), .usb_host = plat_usb_host(), .phone = ble_info.connected,
        .logging = logger_active(), .user_ms = user_ms, .bus_ms = bus_ms,
    };
    enum power_sleep_reason why = power_poll(&in);
    if (why == POWER_SLEEP_NONE)
        return;
    // Close the log properly and leave the bus alone before the power goes.
    app_poll_clear();
    if (logger_active())
        app_set_logging(false);
    app_set_silent(true);
    if (!binary)
        printf("power: going to sleep (%s)\n", power_sleep_name(why));
    fflush(stdout);
    plat_power_sleep(why);
}

bool app_power_get(struct power_status *st, struct power_settings *set)
{
    power_get_status(st);
    *set = *power_settings();
    return true;
}

bool app_power_set(uint8_t op, uint32_t value)
{
    struct power_settings *s = power_settings();
    switch (op) {
    case 1: s->stay_awake = value != 0; break;
    case 2: if (value < 30 || value > 86400) return false; s->idle_s = value; break;
    case 3: if (value < 10500 || value > 13000) return false; s->cutoff_mv = (uint16_t)value; break;
    case 4: power_request_sleep(); return true;
    default: return false;
    }
    plat_settings_save(s);   // kept across sleeps
    return true;
}

static void cmd_power(char *rest)
{
    struct power_status st;
    struct power_settings set;
    if (rest) {
        char *what = strtok(rest, " ");
        char *val = strtok(NULL, " ");
        bool ok = false;
        if (!strcmp(what, "stay") && val)
            ok = app_power_set(1, !strcmp(val, "on"));
        else if (!strcmp(what, "idle") && val)
            ok = app_power_set(2, (uint32_t)strtoul(val, NULL, 10));
        else if (!strcmp(what, "cutoff") && val)
            ok = app_power_set(3, (uint32_t)strtoul(val, NULL, 10));
        else if (!strcmp(what, "sleep"))
            ok = app_power_set(4, 0);
        if (!ok) {
            printf("- usage: power, power stay on|off, power idle <30-86400 s>, power cutoff <10500-13000 mV>, power sleep\n");
            return;
        }
    }
    app_power_get(&st, &set);
    printf("power: woke by %s, battery %u mV, %s\n", power_wake_name(st.wake), st.vbat_mv,
           st.holds ? "awake" : "nothing keeps it awake");
    static const char *holds[] = { "usb", "phone", "activity", "engine", "bus", "logging", "stay" };
    printf("held by:");
    for (int i = 0; i < 7; i++)
        if (st.holds & (1 << i))
            printf(" %s", holds[i]);
    printf("%s\n", st.holds ? "" : " nothing");
    if (st.sleep_in_s)
        printf("sleeps in %lu s without activity\n", (unsigned long)st.sleep_in_s);
    if (st.low_battery_for_s)
        printf("battery low for %lu s (sleeps after %lu s)\n", (unsigned long)st.low_battery_for_s, (unsigned long)set.low_battery_s);
    printf("settings: idle %lu s, cutoff %u mV, engine running above %u mV, stay awake %s\n", (unsigned long)set.idle_s,
           set.cutoff_mv, set.running_mv, set.stay_awake ? "on" : "off");
}
#else
static void power_check(void)
{
}

bool app_power_get(struct power_status *st, struct power_settings *set)
{
    (void)st;
    (void)set;
    return false;
}

bool app_power_set(uint8_t op, uint32_t value)
{
    (void)op;
    (void)value;
    return false;
}

static void cmd_power(char *rest)
{
    (void)rest;
    printf("- this board has no power management\n");
}
#endif

// ---- GPS and sync output (boards built with APP_GNSS)
#ifdef APP_GNSS
static uint32_t sync_hz, sync_edges;
static bool time_from_gps;

static void on_gnss(const struct gnss_event *e, void *ctx)
{
    (void)ctx;
    if (e->kind == GNSS_EVT_FIX) {
        logger_fix(&e->fix, e->us);
        return;
    }
    logger_pps(e->us, e->utc);
    // The first paired pulse sets the log clock: better than anything a phone can say.
    if (e->utc && !time_from_gps) {
        logger_set_time(e->utc, (uint32_t)(e->us / 1000));
        time_from_gps = true;
    }
}

static void gnss_service(void)
{
    static uint8_t buf[256];
    uint64_t edge;
    // Pulses first: a time sentence read now belongs to a pulse that came before it.
    while (plat_pps_take(&edge))
        gnss_pps(edge, on_gnss, NULL);
    for (size_t n; (n = plat_gnss_read(buf, sizeof(buf))) > 0;)
        gnss_feed(buf, n, plat_now_us(), on_gnss, NULL);
    gnss_poll(plat_now_us(), on_gnss, NULL);
    while (plat_sync_take(&edge))
        logger_sync(edge, ++sync_edges);
}

bool app_gnss_get(struct app_gnss *out)
{
    gnss_get_status(&out->gps, plat_now_us());
    out->sync_hz = sync_hz;
    out->sync_edges = sync_edges;
    out->time_from_gps = time_from_gps;
    return true;
}

bool app_sync_set(uint32_t hz)
{
    if (hz > 100 || !plat_sync_set(hz))
        return false;
    sync_hz = hz;
    return true;
}

static void print_degrees(int32_t e7, char pos, char neg)
{
    uint32_t a = e7 < 0 ? (uint32_t)-(int64_t)e7 : (uint32_t)e7;
    printf("%lu.%07lu %c", (unsigned long)(a / 10000000), (unsigned long)(a % 10000000), e7 < 0 ? neg : pos);
}

static void cmd_gps(char *rest)
{
    if (rest) {
        char *what = strtok(rest, " ");
        char *val = strtok(NULL, " ");
        unsigned long baud = val ? strtoul(val, NULL, 10) : 0;
        if (strcmp(what, "baud") || baud < 4800 || baud > 921600 || !plat_gnss_baud((uint32_t)baud)) {
            printf("- usage: gps, gps baud <4800-921600>\n");
            return;
        }
        printf("gps baud %lu\n", baud);
        return;
    }
    struct app_gnss a;
    app_gnss_get(&a);
    const struct gnss_status *g = &a.gps;
    const struct gnss_fix *x = &g->last;
    printf("gps %s, %s, pulse %s\n", g->present ? "receiver present" : "no receiver",
           g->fix ? "fix" : "no fix", g->pps_locked ? "locked" : "not locked");
    if (g->present) {
        printf("fix: quality %u, %u satellites, hdop %u.%02u\n", x->quality, x->sats, x->hdop_c / 100, x->hdop_c % 100);
        printf("position: ");
        print_degrees(x->lat_e7, 'N', 'S');
        printf(", ");
        print_degrees(x->lon_e7, 'E', 'W');
        printf(", altitude %ld.%02u m\n", (long)(x->alt_cm / 100), (unsigned)((x->alt_cm < 0 ? -x->alt_cm : x->alt_cm) % 100));
        printf("speed %u.%02u m/s, course %u.%02u deg\n", x->speed_cms / 100, x->speed_cms % 100,
               x->course_cdeg / 100, x->course_cdeg % 100);
        printf("utc %lu.%03u\n", (unsigned long)x->utc, x->utc_ms);
    }
    if (g->drift_valid)
        printf("clock drift %ld ppb (local clock against GPS)\n", (long)g->drift_ppb);
    printf("log clock %s\n", a.time_from_gps ? "from GPS" : "not from GPS");
    printf("sentences %lu bad %lu pulses %lu paired %lu mismatched %lu\n", (unsigned long)g->sentences,
           (unsigned long)g->bad_sentences, (unsigned long)g->pulses, (unsigned long)g->paired,
           (unsigned long)g->mismatched);
    printf("sync %lu Hz, %lu edges\n", (unsigned long)a.sync_hz, (unsigned long)a.sync_edges);
}

static void cmd_sync(char *rest)
{
    if (!rest) {
        printf("sync %lu Hz, %lu edges\n", (unsigned long)sync_hz, (unsigned long)sync_edges);
        return;
    }
    char *end;
    unsigned long hz = !strcmp(rest, "off") ? 0 : strtoul(rest, &end, 10);
    if ((strcmp(rest, "off") && (*end || hz == 0)) || !app_sync_set((uint32_t)hz)) {
        printf("- usage: sync off, or sync <1-100 Hz>\n");
        return;
    }
    printf(hz ? "sync %lu Hz\n" : "sync off\n", hz);
}
#else
static void gnss_service(void)
{
}

bool app_gnss_get(struct app_gnss *out)
{
    (void)out;
    return false;
}

bool app_sync_set(uint32_t hz)
{
    (void)hz;
    return false;
}

static void cmd_gps(char *rest)
{
    (void)rest;
    printf("- this board has no GPS connector\n");
}

static void cmd_sync(char *rest)
{
    (void)rest;
    printf("- this board has no sync output\n");
}
#endif

void app_init(void)
{
#ifdef APP_GNSS
    gnss_init();
#endif
#ifdef APP_POWER
    power_init(plat_wake_reason(), plat_now_ms());
    struct power_settings saved = *power_settings();
    if (plat_settings_load(&saved))
        power_settings_adopt(&saved);
#endif
    plat_led(PLAT_LED_SILENT, true);
    plat_led(PLAT_LED_USB, true);
    set_target(VW_ECUS[0].name, VW_ECUS[0].request_id, VW_ECUS[0].response_id, VW_ECUS[0].description);
    proto_port_init(&ble_port, ble_write);
    proto_attach(&ble_port);
    run_boot_script();
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

    // One poll request per pass, so the console stays responsive between them.
    for (size_t i = 0; i < poll_count && !can_port_is_silent(); i++) {
        struct poll_entry *e = &polls[(poll_next + i) % poll_count];
        if ((int32_t)(plat_now_ms() - e->next_at) < 0)
            continue;
        static uint8_t resp[ISOTP_MAX_PAYLOAD];
        size_t n;
        int detail;
        app_uds_request(e->request_id, e->response_id, POLL_TIMEOUT_MS, e->data, e->len, resp, sizeof(resp), &n, &detail);
        e->next_at = plat_now_ms() + e->period_ms;
        poll_next = (poll_next + i + 1) % poll_count;
        break;
    }

    // Bluetooth: the link carries packets from its first byte, no text mode.
    if (ble_link_session_changed())
        proto_port_init(&ble_port, ble_write);
    for (uint8_t b; ble_link_rx_pop(&b);) {
        user_ms = plat_now_ms();
        proto_rx_byte(&ble_port, b);
    }

    power_check();

    int ch;
    while ((ch = plat_getchar()) >= 0) {
        user_ms = plat_now_ms();
        if (binary) {
            proto_rx_byte(&usb_port, (uint8_t)ch);
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (!line_len)
                continue;
            if (!machine)
                putchar('\n');
            line[line_len] = 0;
            line_len = 0;
            handle_line(line);
            if (!binary)
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

// ---------------------------------------------------------------- bring-up self-test (selftest.h)
#define LOOPBACK_QUIET_MS 2000 // no frames for this long before the loopback may send
#define NO_BATTERY_MV 6000

static void wait_pumping(uint32_t ms)
{
    uint32_t until = plat_now_ms() + ms;
    while ((int32_t)(plat_now_ms() - until) < 0)
        pump(NULL);
}

static void selftest_leds(struct selftest_report *r)
{
    static const enum plat_led order[] = { PLAT_LED_SILENT, PLAT_LED_CAN, PLAT_LED_WIFI, PLAT_LED_USB };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        plat_led(order[i], false);
    wait_pumping(250);
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        plat_led(order[i], true);
        wait_pumping(250);
        plat_led(order[i], false);
    }
    plat_led(PLAT_LED_SILENT, can_port_is_silent());
    plat_led(PLAT_LED_WIFI, logger_active());
    plat_led(PLAT_LED_USB, true);
    selftest_add(r, "leds", SELFTEST_LOOK, "all off, then status, CAN, log, USB lit one at a time for 0.25 s");
}

static void selftest_can(struct selftest_report *r, unsigned flags)
{
    char d[SELFTEST_DETAIL_LEN];
    enum selftest_result res = can_port_check(d, sizeof(d));
    selftest_add(r, "can", res, "%s", d);
    if (!(flags & SELFTEST_ACTIVE)) {
        selftest_add(r, "can_loop", SELFTEST_SKIP, "not asked for: selftest active (sends one frame)");
        return;
    }
    if (!(flags & SELFTEST_FORCE)) {
        if (bus_ms && plat_now_ms() - bus_ms < LOOPBACK_QUIET_MS) {
            selftest_add(r, "can_loop", SELFTEST_SKIP, "frames on the bus: on a car? On a bench, add force");
            return;
        }
#ifdef APP_POWER
        uint16_t mv = plat_battery_mv();
        if (mv >= NO_BATTERY_MV) {
            selftest_add(r, "can_loop", SELFTEST_SKIP, "%u mV at the OBD port: on a car? On a bench supply, add force",
                         (unsigned)mv);
            return;
        }
#endif
    }
    res = can_port_loopback(d, sizeof(d));
    plat_led(PLAT_LED_SILENT, can_port_is_silent());
    selftest_add(r, "can_loop", res, "%s", d);
}

static void selftest_card(struct selftest_report *r)
{
    char d[64];
    if (logger_active()) {
        struct logger_stats ls;
        logger_get_stats(&ls);
        selftest_add(r, "card", SELFTEST_PASS, "recording %s, %lu bytes so far (not written to while logging)",
                     ls.name, (unsigned long)ls.bytes);
        return;
    }
    uint32_t t0 = plat_now_ms();
    int res = storage_check(d, sizeof(d));
    selftest_add(r, "card", (enum selftest_result)res, "%s (%lu ms)", d, (unsigned long)(plat_now_ms() - t0));
}

static void selftest_ble(struct selftest_report *r)
{
    if (!ble_info.available)
        selftest_add(r, "bluetooth", SELFTEST_FAIL, "no Bluetooth controller running");
    else
        selftest_add(r, "bluetooth", SELFTEST_PASS, "%s as %s%s", ble_info.connected ? "connected" : "advertising",
                     ble_info.name, ble_info.emulated ? " (emulated)" : "");
}

#ifdef APP_POWER
static void selftest_supply(struct selftest_report *r)
{
    unsigned mv = plat_battery_mv();
    if (mv < NO_BATTERY_MV)
        selftest_add(r, "supply", SELFTEST_SKIP, "%u mV at the OBD port: no battery, running from USB", mv);
    else if (mv < 11800)
        selftest_add(r, "supply", SELFTEST_WARN, "%u mV at the OBD port: battery low, or the divider reads low", mv);
    else if (mv <= 15500)
        selftest_add(r, "supply", SELFTEST_PASS, "%u mV at the OBD port", mv);
    else
        selftest_add(r, "supply", SELFTEST_FAIL, "%u mV at the OBD port: above any charging voltage", mv);
}
#endif

#ifdef APP_GNSS
static void selftest_gnss(struct selftest_report *r, unsigned flags)
{
    struct gnss_status g;
    gnss_get_status(&g, plat_now_us());
    if (!g.sentences && !g.bad_sentences)
        selftest_add(r, "gps", SELFTEST_SKIP, "nothing readable from the GPS connector (none fitted?)");
    else if (g.bad_sentences > g.sentences)
        selftest_add(r, "gps", SELFTEST_FAIL, "%lu of %lu sentences unreadable", (unsigned long)g.bad_sentences,
                     (unsigned long)(g.sentences + g.bad_sentences));
    else
        selftest_add(r, "gps", SELFTEST_PASS, "%lu sentences, %s, %u satellites", (unsigned long)g.sentences,
                     g.fix ? "fix" : "no fix yet", (unsigned)g.last.sats);
    if (g.pulses && g.paired)
        selftest_add(r, "pps", SELFTEST_PASS, "%lu pulses, %lu paired with GPS time", (unsigned long)g.pulses,
                     (unsigned long)g.paired);
    else if (g.pulses)
        selftest_add(r, "pps", SELFTEST_WARN, "%lu pulses, none paired with a time sentence yet", (unsigned long)g.pulses);
    else if (g.fix)
        selftest_add(r, "pps", SELFTEST_FAIL, "a fix but no pulses");
    else
        selftest_add(r, "pps", SELFTEST_SKIP, "no pulses yet; most receivers pulse only once they have a fix");

    if (!(flags & SELFTEST_ACTIVE)) {
        selftest_add(r, "sync", SELFTEST_SKIP, "not asked for: selftest active (pulses the sync output)");
        return;
    }
    uint32_t before_hz = sync_hz, before = sync_edges;
    if (!app_sync_set(10)) {
        selftest_add(r, "sync", SELFTEST_FAIL, "the sync timer would not start");
        return;
    }
    wait_pumping(350);
    uint32_t n = sync_edges - before;
    app_sync_set(before_hz);
    selftest_add(r, "sync", n >= 2 ? SELFTEST_PASS : SELFTEST_FAIL,
                 "%lu rising edges in 0.35 s at 10 Hz; confirm on the pin with a scope or an LED", (unsigned long)n);
}
#endif

void app_selftest(struct selftest_report *r, unsigned flags)
{
    memset(r, 0, sizeof(*r));
    plat_selftest(r, flags);
    selftest_add(r, "firmware", SELFTEST_PASS, "%s, protocol %d", PROTO_NAME, PROTO_VERSION);
    selftest_can(r, flags);
    selftest_card(r);
    selftest_ble(r);
#ifdef APP_POWER
    selftest_supply(r);
#endif
#ifdef APP_GNSS
    selftest_gnss(r, flags);
#endif
    selftest_leds(r);
}

static void cmd_selftest(char *rest)
{
    static struct selftest_report rep;
    unsigned flags = 0;
    for (char *w = rest ? strtok(rest, " ") : NULL; w; w = strtok(NULL, " ")) {
        if (!strcmp(w, "active"))
            flags |= SELFTEST_ACTIVE;
        else if (!strcmp(w, "force"))
            flags |= SELFTEST_FORCE;
        else {
            printf("- selftest [active] [force]\n");
            return;
        }
    }
    app_selftest(&rep, flags);
    for (size_t i = 0; i < rep.count; i++)
        printf("%-4s %-9s %s\n", selftest_result_name((enum selftest_result)rep.items[i].result), rep.items[i].id,
               rep.items[i].detail);
}
