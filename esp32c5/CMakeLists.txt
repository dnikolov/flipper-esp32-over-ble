cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# components/feb_app_core resolves board_config.h (and status_led.h/location.h) from here.
idf_build_set_property(FEB_APP_CORE_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/main")

project(flipper_esp32c5_over_ble)
