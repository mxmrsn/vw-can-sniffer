// The self-test half of the ESP32-S3 CAN drivers (can_port_twai.c on the XIAO board,
// can_port_tja1043.c on the dongle): reading the controller's state, and one loopback frame.
//
// The loopback uses TWAI's no-acknowledge self-test mode: the controller sends a frame with the
// self-reception flag, the transceiver puts it on the wires, and the controller reads it back
// through the transceiver's RXD. A frame that comes back proves TXD, RXD, the transceiver's supply
// and its mode pins; nothing else on the bus has to answer. The caller installs the driver in that
// mode and sets the transceiver to drive the bus around this call.
//
// Not yet run on hardware.
#include "twai_selftest.h"

#include <stdio.h>
#include <string.h>

#include "driver/twai.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define LOOPBACK_ID 0x7FF
#define LOOPBACK_WAIT_US 100000

enum selftest_result twai_selftest_check(bool silent, char *detail, size_t cap)
{
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        snprintf(detail, cap, "the CAN controller is not running");
        return SELFTEST_FAIL;
    }
    if (st.state == TWAI_STATE_BUS_OFF || st.state == TWAI_STATE_RECOVERING) {
        snprintf(detail, cap, "bus off after %lu bus errors", (unsigned long)st.bus_error_count);
        return SELFTEST_FAIL;
    }
    if (st.state != TWAI_STATE_RUNNING) {
        snprintf(detail, cap, "the CAN controller is stopped");
        return SELFTEST_FAIL;
    }
    snprintf(detail, cap, "running %s, error counters tx %lu rx %lu, %lu bus errors",
             silent ? "listen-only" : "transmit-enabled", (unsigned long)st.tx_error_counter,
             (unsigned long)st.rx_error_counter, (unsigned long)st.bus_error_count);
    // 96 is where the controller raises its error warning
    return st.tx_error_counter >= 96 || st.rx_error_counter >= 96 ? SELFTEST_WARN : SELFTEST_PASS;
}

esp_err_t twai_selftest_install(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NO_ACK);
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_stop();
    twai_driver_uninstall();
    esp_err_t err = twai_driver_install(&g, &t, &f);
    return err == ESP_OK ? twai_start() : err;
}

enum selftest_result twai_selftest_loopback(char *detail, size_t cap)
{
    static const uint8_t pattern[8] = { 'S', 'E', 'L', 'F', 'T', 'E', 'S', 'T' };
    twai_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.identifier = LOOPBACK_ID;
    msg.self = 1;
    msg.data_length_code = 8;
    memcpy(msg.data, pattern, sizeof(pattern));
    if (twai_transmit(&msg, pdMS_TO_TICKS(50)) != ESP_OK) {
        snprintf(detail, cap, "the controller would not queue the frame");
        return SELFTEST_FAIL;
    }
    unsigned others = 0;
    int64_t until = esp_timer_get_time() + LOOPBACK_WAIT_US;
    while (esp_timer_get_time() < until) {
        twai_message_t back;
        if (twai_receive(&back, pdMS_TO_TICKS(10)) != ESP_OK)
            continue;
        if (back.identifier == LOOPBACK_ID && !back.extd && back.data_length_code == 8
            && !memcmp(back.data, pattern, sizeof(pattern))) {
            snprintf(detail, cap, "frame 7FF sent and read back through the transceiver%s",
                     others ? " (other traffic seen too)" : "");
            return SELFTEST_PASS;
        }
        others++;
    }
    twai_status_info_t st;
    twai_get_status_info(&st);
    snprintf(detail, cap, "nothing came back: tx error counter %lu, %lu bus errors, %u other frames",
             (unsigned long)st.tx_error_counter, (unsigned long)st.bus_error_count, others);
    return SELFTEST_FAIL;
}
