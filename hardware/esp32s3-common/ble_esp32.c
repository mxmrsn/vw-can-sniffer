// Bluetooth LE on the ESP32-S3 (NimBLE): the Nordic UART Service carrying the
// binary packet protocol. Same service, same security and the same pairing
// rules as the Pico board's ble_pico.c; see hardware/pico2w/docs/link_protocol.md.
//
// NimBLE runs in its own task, so the phone keeps being served while the main
// loop waits for a slow control module. ble_link.c's rings are the only state
// shared with the main loop, exactly as on the Pico. Notifications are sent from
// a second task of our own so that plat_ble_kick() never blocks the main loop.
//
// Security: LE Secure Connections with a passkey, bonded, legacy pairing
// disabled in sdkconfig. The passkey is fixed per board (derived from the
// chip's eFuse MAC) and shown by the console's "ble" command. New phones can
// pair only for 2 minutes after power-up, after pressing the MODE button, or
// after "ble pair"; bonded phones reconnect any time.
//
// Not yet run on hardware.
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_link.h"
#include "leds.h"
#include "pins.h"
#include "platform.h"

#define PAIRING_WINDOW_MS 120000
#define TX_TASK_STACK 4096
#define TX_TASK_PRIORITY 6
#define NOTIFY_RETRY_MS 20
#define PIECE_MAX 512

void ble_store_config_init(void); // NimBLE's NVS-backed bond store

// Nordic UART Service, least significant byte first.
static const ble_uuid128_t nus_service = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                                                          0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t nus_rx = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                                                     0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t nus_tx = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                                                     0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static uint16_t con = BLE_HS_CONN_HANDLE_NONE;
static uint16_t tx_value_handle;
static uint8_t own_addr_type;
static volatile uint32_t pairing_until;
static TaskHandle_t tx_task_handle;
static bool notify_on;
// Only the notification task may move the transmit ring's read index, so a
// connect or disconnect asks it to throw the queue away rather than doing it
// from the host task.
static volatile bool tx_flush_wanted;
static bool button_was_down = true; // the button is active low, so released reads high

static int gap_event(struct ble_gap_event *event, void *arg);

static bool pairing_open(void)
{
    return (int32_t)(pairing_until - plat_now_ms()) > 0;
}

// A phone may only send or subscribe over a link that is encrypted and
// authenticated with a full-length key, which is what the Pico's GATT database
// demanded through WRITE_AUTHENTICATED_SC | ENCRYPTION_KEY_SIZE_16.
static bool link_secure(uint16_t handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(handle, &desc) != 0)
        return false;
    return desc.sec_state.encrypted && desc.sec_state.authenticated && desc.sec_state.key_size >= 16;
}

static int nus_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    if (!link_secure(conn_handle))
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    uint8_t buf[PIECE_MAX];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0)
        return BLE_ATT_ERR_UNLIKELY;
    ble_link_rx_push(buf, len);
    return 0;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_service.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // RX: the phone writes packets here
                .uuid = &nus_rx.u,
                .access_cb = nus_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC
                         | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                // TX: the interface sends packets as notifications
                .uuid = &nus_tx.u,
                .access_cb = nus_access,
                .val_handle = &tx_value_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN,
            },
            { 0 },
        },
    },
    { 0 },
};

// ---- runs in the notification task
static void tx_task(void *arg)
{
    (void)arg;
    static uint8_t piece[PIECE_MAX];
    static size_t piece_len;
    for (;;) {
        if (tx_flush_wanted) {
            tx_flush_wanted = false;
            piece_len = 0;
            ble_link_tx_discard();
        }
        if (!piece_len && !ble_link_tx_pending()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (con == BLE_HS_CONN_HANDLE_NONE || !ble_info.subscribed) {
            piece_len = 0;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NOTIFY_RETRY_MS));
            continue;
        }
        if (!piece_len) {
            size_t max = ble_info.mtu > 3 ? (size_t)ble_info.mtu - 3u : 20u;
            if (max > sizeof(piece))
                max = sizeof(piece);
            piece_len = ble_link_tx_take(piece, max);
            if (!piece_len)
                continue;
        }
        // The bytes are already out of the ring, so the same piece is retried
        // until it goes: dropping it would cut the packet stream in half.
        struct os_mbuf *om = ble_hs_mbuf_from_flat(piece, (uint16_t)piece_len);
        if (!om || ble_gatts_notify_custom(con, tx_value_handle, om) != 0) {
            vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_MS));
            continue;
        }
        piece_len = 0;
    }
}

// ---- runs in the NimBLE host task
static void advertise(void)
{
    struct ble_hs_adv_fields fields, scan_response;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&nus_service;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0)
        return;

    memset(&scan_response, 0, sizeof(scan_response));
    scan_response.name = (const uint8_t *)ble_info.name;
    scan_response.name_len = (uint8_t)strlen(ble_info.name);
    scan_response.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&scan_response) != 0)
        return;

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 160; // 100 ms
    params.itvl_max = 320; // 200 ms
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static void connection_lost(void)
{
    con = BLE_HS_CONN_HANDLE_NONE;
    ble_info.connected = ble_info.subscribed = notify_on = false;
    tx_flush_wanted = true;
    plat_ble_kick();
    ble_link_new_session();
    leds_set(BOARD_LED_BLE, false);
    advertise();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            break;
        }
        con = event->connect.conn_handle;
        ble_info.connected = true;
        ble_info.subscribed = notify_on = false;
        ble_info.mtu = 23;
        ble_info.connections++;
        tx_flush_wanted = true;
        plat_ble_kick();
        ble_link_new_session();
        leds_set(BOARD_LED_BLE, true);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection_lost();
        break;
    case BLE_GAP_EVENT_MTU:
        ble_info.mtu = event->mtu.value;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_value_handle) {
            notify_on = event->subscribe.cur_notify;
            ble_info.subscribed = notify_on && link_secure(event->subscribe.conn_handle);
            plat_ble_kick();
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        // A phone may subscribe before it pairs, so the link only becomes
        // usable here, once the keys are in place.
        ble_info.subscribed = notify_on && link_secure(event->enc_change.conn_handle);
        plat_ble_kick();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (!pairing_open()) {
            // Not in a pairing window: refuse new phones outright.
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = ble_info.passkey };
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The phone wants new keys. Only inside a pairing window, and the old
        // bond has to go first or NimBLE refuses to store the new one.
        struct ble_gap_conn_desc desc;
        if (!pairing_open() || ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) != 0)
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static uint32_t board_passkey(void)
{
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    uint32_t h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < sizeof(mac); i++)
        h = (h ^ mac[i]) * 16777619u;
    return h % 1000000u;
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_addr_type) != 0)
        return;
    uint8_t addr[6];
    if (ble_hs_id_copy_addr(own_addr_type, addr, NULL) != 0)
        return;
    // NimBLE keeps addresses least significant byte first.
    snprintf(ble_info.address, sizeof(ble_info.address), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    snprintf(ble_info.name, sizeof(ble_info.name), "VW-Diag-%02X%02X", addr[1], addr[0]);
    ble_svc_gap_device_name_set(ble_info.name);
    ble_info.available = true;
    advertise();
}

static void on_reset(int reason)
{
    (void)reason;
    con = BLE_HS_CONN_HANDLE_NONE;
    ble_info.connected = ble_info.subscribed = false;
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---- main loop side
void ble_esp32_init(void)
{
    gpio_reset_pin(BTN_MODE_PIN);
    gpio_set_direction(BTN_MODE_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_MODE_PIN, BTN_MODE_PULL); // per board: see its pins.h

    if (nimble_port_init() != ESP_OK)
        return; // no radio: ble_info.available stays false
    pairing_until = plat_now_ms() + PAIRING_WINDOW_MS;
    ble_info.passkey = board_passkey();

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0)
        return;
    ble_store_config_init();

    xTaskCreate(tx_task, "ble_tx", TX_TASK_STACK, NULL, TX_TASK_PRIORITY, &tx_task_handle);
    nimble_port_freertos_init(host_task);
}

void plat_ble_poll(void)
{
    bool down = gpio_get_level(BTN_MODE_PIN) == BTN_MODE_PRESSED;
    if (down && !button_was_down)
        plat_ble_open_pairing();
    button_was_down = down;
    ble_info.pairing_open = ble_info.available && pairing_open();
}

void plat_ble_kick(void)
{
    if (tx_task_handle)
        xTaskNotifyGive(tx_task_handle);
}

bool plat_ble_open_pairing(void)
{
    if (!ble_info.available)
        return false;
    pairing_until = plat_now_ms() + PAIRING_WINDOW_MS;
    return true;
}

bool plat_ble_forget(void)
{
    if (!ble_info.available)
        return false;
    if (con != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(con, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
    return true;
}
