#ifndef FEB_BOARD_CONFIG_H
#define FEB_BOARD_CONFIG_H

/* Compile-time board constants for components/feb_app_core (docs/SOURCE_SPLIT.md 5.2).
   Resolved by the core through its PRIV_INCLUDE_DIRS, which esp32c5/CMakeLists.txt points at
   this directory via the FEB_APP_CORE_BOARD_DIR build property. */

/* docs/PLAN.md step 7: hand-maintained, opaque, per-firmware-target constants -- not
   build-injected, bumped by hand. See docs/CAPABILITIES.md. */
#define FEB_BOARD_MODEL "olimex-mod-esp32-c5"
#define FEB_FIRMWARE_VERSION "0.1.0"
/* board_id is "<prefix>-<factory MAC hex>", see feb_compute_board_id(). */
#define FEB_BOARD_ID_PREFIX "esp32c5"
#define FEB_LOG_TAG "flipper_esp32c5_over_ble"
/* docs/BACKLOG.md BL15: no onboard pushbutton on this board (docs/hardware/olimex-mod-esp32-c5/
   README.md) -- no boot-button/factory-reset glue, unlike the C6/Heltec. */
#define FEB_HAS_BOOT_BUTTON 0
/* Dual-band (2.4GHz+5GHz) Wi-Fi 6 radio -- the only board so far with the `wifi_band`
   wardriving field/hooks (docs/PROTOCOL.md's `wifi_band` row; wifi_band.c). */
#define FEB_WIFI_DUAL_BAND 1
/* docs/HARDENING_PLAN.md HP-21/BL18: free_heap/largest_free_block diagnostic log lines in
   feb_cap_scan.c around every esp_wifi_scan_start() this board makes, kept for an open
   heap-pressure investigation into dual-band scans (docs/SOURCE_SPLIT.md DR3 -- "kept", not a
   leftover to drop). Not ported to the C6/Heltec. */
#define FEB_DIAG_WIFI_HEAP_LOG 1

/* No Heltec-style BLE link-lifecycle hooks (feb_app_hooks_t.on_ble_state et al.): their
   members and call sites compile out entirely. */
#define FEB_HAS_LINK_HOOKS 0
/* No Phase 9 cluster worker (docs/CLUSTER.md) as an alternative Wi-Fi scan source. */
#define FEB_HAS_CLUSTER_WORKER 0

#endif
