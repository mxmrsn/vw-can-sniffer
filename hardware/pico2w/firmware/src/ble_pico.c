// Bluetooth LE on the Pico 2 W (CYW43439 + BTstack): the Nordic UART Service
// carrying the binary packet protocol. See hardware/pico2w/docs/link_protocol.md.
//
// BTstack runs in the background (cyw43_arch "threadsafe background" mode, from
// an interrupt), so the phone keeps being served while the main loop waits for a
// slow control module. ble_link.c's rings are the only shared state.
//
// Security: LE Secure Connections with a passkey, bonded. The passkey is fixed
// per board (derived from the flash chip's unique ID) and shown by the console's
// "ble" command. New phones can pair only for 2 minutes after power-up, after
// pressing the MODE button, or after "ble pair"; bonded phones reconnect any time.
//
// Not yet run on hardware.
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "ble/gatt-service/nordic_spp_service_server.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "../pins.h"
#include "ble_link.h"
#include "platform.h"
#include "vw_diag.h" // generated from vw_diag.gatt

#define PAIRING_WINDOW_MS 120000

static btstack_packet_callback_registration_t hci_registration, sm_registration;
static btstack_context_callback_registration_t send_request;
static hci_con_handle_t con = HCI_CON_HANDLE_INVALID;
static volatile bool send_pending;
static volatile uint32_t pairing_until;
static bool button_was_down;

static uint8_t adv_data[] = {
    2, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    // Nordic UART Service UUID, least significant byte first
    17, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
};
static uint8_t scan_response[2 + sizeof(ble_info.name)];

static bool pairing_open(void)
{
    return (int32_t)(pairing_until - to_ms_since_boot(get_absolute_time())) > 0;
}

// ---- runs in the Bluetooth context
static void send_next(void *context)
{
    (void)context;
    uint8_t piece[512];
    size_t max = ble_info.mtu > 3 ? ble_info.mtu - 3u : 20u;
    if (max > sizeof(piece))
        max = sizeof(piece);
    if (con == HCI_CON_HANDLE_INVALID || !ble_info.subscribed) {
        send_pending = false;
        return;
    }
    size_t n = ble_link_tx_take(piece, max);
    if (n)
        nordic_spp_service_server_send(con, piece, (uint16_t)n);
    if (ble_link_tx_pending())
        nordic_spp_service_server_request_can_send_now(&send_request, con);
    else
        send_pending = false;
}

static void start_sending(void)
{
    if (!send_pending && con != HCI_CON_HANDLE_INVALID && ble_info.subscribed && ble_link_tx_pending()) {
        send_pending = true;
        nordic_spp_service_server_request_can_send_now(&send_request, con);
    }
}

static void spp_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    if (type == RFCOMM_DATA_PACKET) { // a write to the RX characteristic
        ble_link_rx_push(packet, size);
        return;
    }
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META)
        return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
    case GATTSERVICE_SUBEVENT_SPP_SERVICE_CONNECTED:
        ble_info.subscribed = true;
        start_sending();
        break;
    case GATTSERVICE_SUBEVENT_SPP_SERVICE_DISCONNECTED:
        ble_info.subscribed = false;
        break;
    default:
        break;
    }
}

static void hci_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET)
        return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            bd_addr_t addr;
            gap_local_bd_addr(addr);
            snprintf(ble_info.address, sizeof(ble_info.address), "%s", bd_addr_to_str(addr));
            snprintf(ble_info.name, sizeof(ble_info.name), "VW-Diag-%02X%02X", addr[4], addr[5]);
            size_t len = strlen(ble_info.name);
            scan_response[0] = (uint8_t)(len + 1);
            scan_response[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
            memcpy(scan_response + 2, ble_info.name, len);
            gap_scan_response_set_data((uint8_t)(len + 2), scan_response);
            ble_info.available = true;
        }
        break;
    case ATT_EVENT_CONNECTED:
        con = att_event_connected_get_handle(packet);
        ble_info.connected = true;
        ble_info.mtu = 23;
        ble_info.connections++;
        ble_link_tx_discard();
        ble_link_new_session();
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        break;
    case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
        ble_info.mtu = att_event_mtu_exchange_complete_get_MTU(packet);
        break;
    case ATT_EVENT_DISCONNECTED:
        con = HCI_CON_HANDLE_INVALID;
        ble_info.connected = ble_info.subscribed = false;
        send_pending = false;
        ble_link_tx_discard();
        ble_link_new_session();
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        break;
    default:
        break;
    }
}

static void sm_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET)
        return;
    if (hci_event_packet_get_type(packet) == SM_EVENT_PAIRING_STARTED && !pairing_open()) {
        // Not in a pairing window: refuse new phones outright.
        gap_disconnect(sm_event_pairing_started_get_handle(packet));
    }
}

// ---- main loop side
static uint32_t board_passkey(void)
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint32_t h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < sizeof(id.id); i++)
        h = (h ^ id.id[i]) * 16777619u;
    return h % 1000000u;
}

void ble_pico_init(void)
{
    gpio_init(BTN_MODE_PIN);
    gpio_set_dir(BTN_MODE_PIN, GPIO_IN); // external pull-down, pressed = high
    if (cyw43_arch_init())
        return; // no radio: ble_info.available stays false
    pairing_until = to_ms_since_boot(get_absolute_time()) + PAIRING_WINDOW_MS;
    ble_info.passkey = board_passkey();

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_MITM_PROTECTION | SM_AUTHREQ_BONDING);
    sm_set_secure_connections_only_mode(true);
    sm_use_fixed_passkey_in_display_role(ble_info.passkey);

    att_server_init(profile_data, NULL, NULL);
    nordic_spp_service_server_init(spp_handler);
    send_request.callback = send_next;

    hci_registration.callback = hci_handler;
    hci_add_event_handler(&hci_registration);
    att_server_register_packet_handler(hci_handler);
    sm_registration.callback = sm_handler;
    sm_add_event_handler(&sm_registration);

    bd_addr_t none = { 0 };
    gap_advertisements_set_params(160, 320, 0 /* connectable undirected */, 0, none, 0x07, 0x00); // 100-200 ms
    gap_advertisements_set_data(sizeof(adv_data), adv_data);
    gap_advertisements_enable(1);
    hci_power_control(HCI_POWER_ON);
}

void plat_ble_poll(void)
{
    bool down = gpio_get(BTN_MODE_PIN);
    if (down && !button_was_down)
        plat_ble_open_pairing();
    button_was_down = down;
    ble_info.pairing_open = ble_info.available && pairing_open();
}

void plat_ble_kick(void)
{
    if (send_pending || !ble_info.subscribed)
        return;
    async_context_t *ctx = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(ctx);
    start_sending();
    async_context_release_lock(ctx);
}

bool plat_ble_open_pairing(void)
{
    if (!ble_info.available)
        return false;
    pairing_until = to_ms_since_boot(get_absolute_time()) + PAIRING_WINDOW_MS;
    return true;
}

bool plat_ble_forget(void)
{
    if (!ble_info.available)
        return false;
    async_context_t *ctx = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(ctx);
    if (con != HCI_CON_HANDLE_INVALID)
        gap_disconnect(con);
    for (int i = 0; i < le_device_db_max_count(); i++)
        le_device_db_remove(i);
    async_context_release_lock(ctx);
    return true;
}
