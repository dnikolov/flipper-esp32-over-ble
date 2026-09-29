# Shared application core (docs/SOURCE_SPLIT.md 5.2). Only a project that opts in by setting the
# FEB_APP_CORE_BOARD_DIR build property (the directory holding its board_config.h, status_led.h
# and location.h) registers this component; every other project that lists ../components in
# EXTRA_COMPONENT_DIRS returns before idf_component_register(), so the component (and its
# REQUIRES) never enters that project's build or link line at all.
idf_build_get_property(feb_app_core_board_dir FEB_APP_CORE_BOARD_DIR)
if(NOT feb_app_core_board_dir)
    return()
endif()

idf_component_register(SRCS "feb_link.c" "feb_central.c" "feb_cap_scan.c" "feb_cap_gps.c"
                            "feb_cap_wardriving.c" "feb_cap_wardriving_cmd.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS ${feb_app_core_board_dir}
                       REQUIRES feb_protocol feb_wardriving bt esp_timer nvs_flash esp_hw_support
                                esp_wifi esp_netif esp_event)

# The core calls back into the board's main component (feb_features[], feb_handle_command(),
# feb_status_led_*(), location_*()); make that dependency explicit so link order never relies
# on main.c happening to pull those objects in first.
idf_component_get_property(feb_app_core_main_lib main COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} PUBLIC ${feb_app_core_main_lib})
