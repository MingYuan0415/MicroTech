# SPDX-License-Identifier: Apache-2.0
#
# Standalone ESP32-C6 coprocessor firmware for the ESP32-P4 board. This project
# is intentionally outside the root build: it targets a different chip, links
# the esp_hosted CP role, and is flashed over the C6 UART exposed on J6.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(microtech_c6_cp)
