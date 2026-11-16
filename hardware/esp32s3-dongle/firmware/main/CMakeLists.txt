# The application core, protocol, transport, logger and power manager are the Pico board's
# (one copy in the repository); the ESP32-S3 drivers are shared with the XIAO board.
# Only the files in this folder are the dongle's own.
set(SHARED ${CMAKE_CURRENT_LIST_DIR}/../../../pico2w/firmware/src)
set(ESP ${CMAKE_CURRENT_LIST_DIR}/../../../esp32s3-common)

idf_component_register(
    SRCS "main.c"
         "can_port_tja1043.c"
         "power_esp32.c"
         "gnss_esp32.c"
         "${ESP}/leds.c"
         "${ESP}/storage_esp32.c"
         "${ESP}/ble_esp32.c"
         "${SHARED}/app.c"
         "${SHARED}/ble_link.c"
         "${SHARED}/frame.c"
         "${SHARED}/gnss.c"
         "${SHARED}/isotp.c"
         "${SHARED}/logger.c"
         "${SHARED}/power.c"
         "${SHARED}/proto.c"
         "${SHARED}/uds.c"
    INCLUDE_DIRS "." "${ESP}" "${SHARED}"
    REQUIRES driver esp_driver_gpio esp_driver_spi esp_driver_usb_serial_jtag esp_driver_uart esp_driver_gptimer esp_adc
             esp_timer fatfs sdmmc bt nvs_flash esp_hw_support)

# This board switches itself off, and has the GPS / sync connector: build the core with both.
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_POWER APP_GNSS)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
