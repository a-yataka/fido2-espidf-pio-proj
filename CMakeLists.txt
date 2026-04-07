cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# https://kazulog.fun/dev/esp32-platformio-arduino-espidf/
list(APPEND EXTRA_COMPONENT_DIRS managed_components)

project(fido2-espidf-pio-proj)
