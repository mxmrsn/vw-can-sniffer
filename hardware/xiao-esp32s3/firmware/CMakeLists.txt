# VW diagnostic interface firmware for the XIAO ESP32-S3 board (PCB v0.3.0).
#
#   . ~/esp/esp-idf/export.sh
#   idf.py set-target esp32s3 && idf.py build
#   -> build/vw_diag.bin, flash with idf.py -p <port> flash
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vw_diag)
