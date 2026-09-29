#include "wifi_band.h"

#include "esp_log.h"
#include "esp_wifi.h"

/* This board's own `wifi_band` state (docs/PROTOCOL.md's `wifi_band` row) -- this chip's
   native dual-band (2.4GHz+5GHz) Wi-Fi 6 radio is the only reason this field has any
   observable effect here; the C6/Heltec decode-and-ignore it (components/feb_app_core's
   feb_cap_wardriving_cmd.c, FEB_WIFI_DUAL_BAND-gated). Unlike country code/band_mode
   themselves (both persistent radio-level settings that stay in effect until next changed),
   WARDRIVING_BAND_5GHZ_FAST's non-DFS channel restriction is expressed via a
   wifi_scan_config_t.channel_bitmap that must be resupplied on every esp_wifi_scan_start()
   call, so it must be remembered in software here (read back by
   feb_board_wifi_band_apply_scan_cfg() on every call site the core's feb_app_hooks->
   wifi_scan_cfg_ext reaches: manual wifi_scan, wardriving start, wardriving re-arm).

   Values match the wire order components/feb_app_core/feb_cap_wardriving_cmd.c parses:
   0 = "2.4ghz", 1 = "5ghz_fast", 2 = "5ghz_full". Boot default (5GHZ_FULL) matches this
   board's pre-existing (2026-09-25 "Scope reversal") behavior -- band_mode WIFI_BAND_MODE_AUTO
   set once in main.c's start_wifi_subsystem(), no channel_bitmap restriction -- so a manual
   wifi_scan run before wardriving ever starts sees the same dual-band full-sweep behavior it
   already did before this field existed. */
static const char *TAG = "flipper_esp32c5_over_ble";

typedef enum {
    WIFI_BAND_2_4GHZ = 0,
    WIFI_BAND_5GHZ_FAST = 1,
    WIFI_BAND_5GHZ_FULL = 2,
} board_wifi_band_t;

static board_wifi_band_t board_wifi_band = WIFI_BAND_5GHZ_FULL;

/* feb_app_hooks_t.wifi_scan_cfg_ext -- sets scan_cfg->channel_bitmap per the currently-stored
   Wi-Fi band mode -- only WIFI_BAND_5GHZ_FAST needs a per-call channel_bitmap;
   WIFI_BAND_2_4GHZ/WIFI_BAND_5GHZ_FULL rely entirely on the radio-level band_mode already set
   by esp_wifi_set_band_mode() (2G_ONLY / AUTO respectively) and leave channel_bitmap zeroed
   for a normal full sweep of whatever band(s) that mode allows. `channel` must stay 0 for
   channel_bitmap to take effect at all (confirmed against esp_wifi_types_generic.h's
   wifi_scan_config_t.channel_bitmap comment and esp-idf/examples/wifi/scan/main/scan.c's
   array_2_channel_bitmap() usage) -- scan_cfg is always freshly memset(0) by every caller
   the core hands this hook, so that's already satisfied. The non-DFS 5GHz set (36/40/44/48
   UNII-1-low, 149/153/157/161/165 UNII-3) matches docs/PROTOCOL.md's `wifi_band` row exactly
   -- see wifi_5g_channel_bit_t in esp_wifi_types_generic.h for the channel->bit mapping. */
void feb_board_wifi_band_apply_scan_cfg(wifi_scan_config_t *scan_cfg)
{
    if (board_wifi_band != WIFI_BAND_5GHZ_FAST) {
        return;
    }
    scan_cfg->channel = 0;
    scan_cfg->channel_bitmap.ghz_2_channels =
        (uint16_t)(WIFI_CHANNEL_1 | WIFI_CHANNEL_2 | WIFI_CHANNEL_3 | WIFI_CHANNEL_4 |
                   WIFI_CHANNEL_5 | WIFI_CHANNEL_6 | WIFI_CHANNEL_7 | WIFI_CHANNEL_8 |
                   WIFI_CHANNEL_9 | WIFI_CHANNEL_10 | WIFI_CHANNEL_11 | WIFI_CHANNEL_12 |
                   WIFI_CHANNEL_13 | WIFI_CHANNEL_14);
    scan_cfg->channel_bitmap.ghz_5_channels =
        (uint32_t)(WIFI_CHANNEL_36 | WIFI_CHANNEL_40 | WIFI_CHANNEL_44 | WIFI_CHANNEL_48 |
                   WIFI_CHANNEL_149 | WIFI_CHANNEL_153 | WIFI_CHANNEL_157 | WIFI_CHANNEL_161 |
                   WIFI_CHANNEL_165);
}

/* feb_app_hooks_t.wardriving_start_ext -- sets the radio-level band mode for the given
   wifi_band selection -- persistent until next changed, same "global radio setting" semantics
   as esp_wifi_set_country_code() right before this hook's call site in the core. WIFI_BAND_
   5GHZ_FAST also needs WIFI_BAND_MODE_AUTO (both bands enabled) since its restriction to
   non-DFS channels is expressed via feb_board_wifi_band_apply_scan_cfg()'s per-call
   channel_bitmap, not via band_mode itself. */
void feb_board_wifi_band_start_ext(uint8_t wifi_band)
{
    wifi_band_mode_t mode = (wifi_band == WIFI_BAND_2_4GHZ) ? WIFI_BAND_MODE_2G_ONLY : WIFI_BAND_MODE_AUTO;
    esp_err_t err = esp_wifi_set_band_mode(mode);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wardriving: esp_wifi_set_band_mode(%d) failed: %s", (int)mode, esp_err_to_name(err));
    }
    board_wifi_band = (board_wifi_band_t)wifi_band;
}
