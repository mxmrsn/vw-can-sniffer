# The simulated car is the host simulator's own code, compiled from where it lives,
# so the bench car and the simulator are the same car. The LEDs and pin map come
# from the interface firmware next door: the bench car runs on the same board.
set(SIM ${CMAKE_CURRENT_LIST_DIR}/../../../pico2w/firmware/sim)
set(SHARED ${CMAKE_CURRENT_LIST_DIR}/../../../pico2w/firmware/src)
set(BOARD ${CMAKE_CURRENT_LIST_DIR}/../../firmware/main)
set(ESP ${CMAKE_CURRENT_LIST_DIR}/../../../esp32s3-common)

idf_component_register(
    SRCS "bench_car.c"
         "${ESP}/leds.c"
         "${SIM}/car.c"
         "${SIM}/sim_config.c"
         "${SIM}/sim_engine.c"
         "${SHARED}/isotp.c"
    INCLUDE_DIRS "." "${BOARD}" "${ESP}" "${SHARED}" "${SIM}"
    REQUIRES driver esp_driver_gpio esp_driver_usb_serial_jtag esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
