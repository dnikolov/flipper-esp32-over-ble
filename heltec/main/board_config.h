#ifndef FEB_BOARD_CONFIG_H
#define FEB_BOARD_CONFIG_H

/* Compile-time board constants for components/feb_app_core (docs/SOURCE_SPLIT.md 5.2).
   Resolved by the core through its PRIV_INCLUDE_DIRS, which heltec/CMakeLists.txt points at
   this directory via the FEB_APP_CORE_BOARD_DIR build property. */

/* docs/PLAN.md step 7 naming convention, this board's own hand-maintained constants. */
#define FEB_BOARD_MODEL "heltec-wifi-lora-32-v2"
#define FEB_FIRMWARE_VERSION "0.1.0"
/* board_id is "<prefix>-<factory MAC hex>", see feb_compute_board_id(); confirmed distinct
   from the C6's "esp32c6-" prefix (docs/PLAN.md Phase 4 step 4). */
#define FEB_BOARD_ID_PREFIX "heltec"
#define FEB_LOG_TAG "flipper_heltec_over_ble"
/* PRG/BOOT button (GPIO0) toggle + factory-reset gesture, glue in board_hooks.c. */
#define FEB_HAS_BOOT_BUTTON 1
/* 2.4 GHz-only radio: no `wifi_band` wardriving field / band hooks. */
#define FEB_WIFI_DUAL_BAND 0
/* docs/HARDENING_PLAN.md HP-21/BL18: C5-only diagnostic, not ported here. */
#define FEB_DIAG_WIFI_HEAP_LOG 0
/* feb_app_hooks_t link-lifecycle hooks: SSD1306 OLED BLE line (on_ble_state), touch-pad radio
   kill switch (radio_permitted), mesh_log drain state (on_connect/on_disconnect/
   on_authenticated). See main.c's feb_board_hooks. */
#define FEB_HAS_LINK_HOOKS 1
/* Phase 9 (docs/CLUSTER.md): a UART-attached 2.4GHz cluster worker (esp32/cluster_worker/) as
   an alternative Wi-Fi scan source; the feb_cluster_*() functions live in cluster_glue.c. */
#define FEB_HAS_CLUSTER_WORKER 1

#endif
