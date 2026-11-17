# The application core, protocol, transport and logger are shared with the
# Pico 2 W board and are referenced where they live, so the repository holds
# exactly one copy of them. Only the files in this folder are board specific.
set(SHARED ${CMAKE_CURRENT_LIST_DIR}/../../../pico2w/firmware/src)
# ESP32-S3 drivers shared with the OBD dongle; each board supplies its own pins.h.
set(ESP ${CMAKE_CURRENT_LIST_DIR}/../../../esp32s3-common)

idf_component_register(
    SRCS "main.c"
         "can_port_twai.c"
         "${ESP}/leds.c"
         "${ESP}/storage_esp32.c"
         "${ESP}/twai_selftest.c"
         "${ESP}/ble_esp32.c"
         "${SHARED}/app.c"
         "${SHARED}/ble_link.c"
         "${SHARED}/frame.c"
         "${SHARED}/isotp.c"
         "${SHARED}/logger.c"
         "${SHARED}/proto.c"
         "${SHARED}/selftest.c"
         "${SHARED}/uds.c"
    INCLUDE_DIRS "." "${ESP}" "${SHARED}"
    REQUIRES driver esp_driver_gpio esp_driver_spi esp_driver_usb_serial_jtag esp_adc
             esp_timer fatfs sdmmc bt nvs_flash esp_hw_support)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
