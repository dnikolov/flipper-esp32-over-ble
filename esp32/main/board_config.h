#ifndef FEB_BOARD_CONFIG_H
#define FEB_BOARD_CONFIG_H

/* Compile-time board constants for components/feb_app_core (docs/SOURCE_SPLIT.md 5.2).
   Resolved by the core through its PRIV_INCLUDE_DIRS, which esp32/CMakeLists.txt points at
   this directory via the FEB_APP_CORE_BOARD_DIR build property. */

/* docs/PLAN.md step 7: hand-maintained, opaque, per-firmware-target constants -- not
   build-injected, bumped by hand. See docs/CAPABILITIES.md. */
#define FEB_BOARD_MODEL "esp32-c6-devkit"
#define FEB_FIRMWARE_VERSION "0.1.0"
/* board_id is "<prefix>-<factory MAC hex>", see feb_compute_board_id(). */
#define FEB_BOARD_ID_PREFIX "esp32c6"
#define FEB_LOG_TAG "flipper_esp32_over_ble"
/* BOOT button (GPIO9) toggle + factory-reset gesture, glue in board_hooks.c. */
#define FEB_HAS_BOOT_BUTTON 1
/* 2.4 GHz-only radio: no `wifi_band` wardriving field / band hooks. */
#define FEB_WIFI_DUAL_BAND 0
/* docs/HARDENING_PLAN.md HP-21/BL18: C5-only free_heap/largest_free_block diagnostic log
   lines in feb_cap_scan.c, kept for an open investigation -- not ported here (SOURCE_SPLIT.md
   DR3, "kept" not "dropped"). */
#define FEB_DIAG_WIFI_HEAP_LOG 0

/* No Heltec-style BLE link-lifecycle hooks (feb_app_hooks_t.on_ble_state et al.): their
   members and call sites compile out entirely. */
#define FEB_HAS_LINK_HOOKS 0
/* No Phase 9 cluster worker (docs/CLUSTER.md) as an alternative Wi-Fi scan source. */
#define FEB_HAS_CLUSTER_WORKER 0

#endif
