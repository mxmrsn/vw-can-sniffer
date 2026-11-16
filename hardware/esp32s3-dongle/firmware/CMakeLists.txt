# VW diagnostic interface firmware for the OBD dongle (v0.4.0): ESP32-S3-MINI-1 on the top board,
# TJA1043 and the switched supply on the bottom board.
#
#   . ~/esp/esp-idf/export.sh
#   idf.py set-target esp32s3 && idf.py build
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vw_diag_dongle)
