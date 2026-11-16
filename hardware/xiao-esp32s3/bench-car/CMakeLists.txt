# Bench car for the XIAO ESP32-S3 board: a second board that answers like the car.
#
#   . ~/esp/esp-idf/export.sh
#   idf.py set-target esp32s3 && idf.py build && idf.py -p <port> flash
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bench_car)
