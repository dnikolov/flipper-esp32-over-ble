#ifndef FEB_WIFI_BAND_H
#define FEB_WIFI_BAND_H

#include <stdint.h>

#include "esp_wifi_types.h"

/* This board's feb_app_hooks_t implementations (docs/SOURCE_SPLIT.md 5.2/5.3 step 3):
   band apply (wifi_scan_cfg_ext) and band-mode set (wardriving_start_ext), ported unchanged
   from esp32c5/main/main.c's pre-split wardriving_apply_wifi_band()/
   wardriving_set_wifi_band_mode(). Defined here, wired into the `const feb_app_hooks_t` in
   main.c. */
void feb_board_wifi_band_apply_scan_cfg(wifi_scan_config_t *scan_cfg);
void feb_board_wifi_band_start_ext(uint8_t wifi_band);

#endif
