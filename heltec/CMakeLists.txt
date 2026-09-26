cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flipper_heltec_over_ble)

# `meshcore_scan` (docs/PLAN.md's "MeshCore Scan Capability" design plan) only ever drives an
# SX1276 in plain LoRa receive mode -- every other modem family/protocol client RadioLib
# supports (CC1101, SX126x, SX128x, LR11x0/LR2021, LoRaWAN, RTTY/Morse/AFSK/AX.25/SSTV/Bell/
# APRS/FSK4/Pager) is dead code for this firmware, but the managed component's own
# CMakeLists.txt compiles every module unconditionally (see
# managed_components/jgromes__radiolib/CMakeLists.txt's unfiltered `file(GLOB_RECURSE ...)`).
# Excluding them via RadioLib's own documented RADIOLIB_EXCLUDE_* compile definitions
# (managed_components/jgromes__radiolib/src/BuildOpt.h) was required to fit this board's
# classic-ESP32 DRAM budget -- the unmodified default build overflowed `dram0_0_seg` by
# ~4.5 KB. Each excluded module's own .h/.cpp guard their entire content behind
# `#if !RADIOLIB_EXCLUDE_<NAME>`, so this only needs to reach the jgromes__radiolib component's
# own compilation (not every file that includes RadioLib.h) to shrink the linked binary.
idf_component_get_property(feb_radiolib_lib jgromes__radiolib COMPONENT_LIB)
target_compile_definitions(${feb_radiolib_lib} PRIVATE
    RADIOLIB_EXCLUDE_CC1101=1
    RADIOLIB_EXCLUDE_NRF24=1
    RADIOLIB_EXCLUDE_RF69=1
    RADIOLIB_EXCLUDE_SX1231=1
    RADIOLIB_EXCLUDE_SI443X=1
    RADIOLIB_EXCLUDE_RFM2X=1
    RADIOLIB_EXCLUDE_SX126X=1
    RADIOLIB_EXCLUDE_STM32WLX=1
    RADIOLIB_EXCLUDE_SX128X=1
    RADIOLIB_EXCLUDE_LR11X0=1
    RADIOLIB_EXCLUDE_LR2021=1
    RADIOLIB_EXCLUDE_LORAWAN=1
    RADIOLIB_EXCLUDE_AFSK=1
    RADIOLIB_EXCLUDE_AX25=1
    RADIOLIB_EXCLUDE_HELLSCHREIBER=1
    RADIOLIB_EXCLUDE_MORSE=1
    RADIOLIB_EXCLUDE_RTTY=1
    RADIOLIB_EXCLUDE_SSTV=1
    RADIOLIB_EXCLUDE_BELL=1
    RADIOLIB_EXCLUDE_APRS=1
    RADIOLIB_EXCLUDE_FSK4=1
    RADIOLIB_EXCLUDE_PAGER=1
)
