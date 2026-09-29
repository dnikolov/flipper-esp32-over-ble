#ifndef FEB_BOARD_HOOKS_H
#define FEB_BOARD_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nimble/nimble_port.h"

#include "feb_app_core.h"

/* Cross-file glue between this board's main/ files (the core-facing contract is
   feb_app_core.h/feb_app_internal.h). */

/* board_hooks.c: PRG-button wardriving toggle + factory-reset events. Both run on the NimBLE
   host task, from feb_board_host_synced(), in the order host_synced() always used;
   feb_board_control_ready_clear() is the radio kill switch's shutdown path. */
void feb_board_button_events_init(void);
void feb_board_control_ready_set(void);
void feb_board_control_ready_clear(void);

/* main.c */
void feb_board_host_synced(void);
void feb_board_nimble_host_task(void *arg);
void feb_board_wifi_subsystem_init(void);
esp_err_t feb_board_wifi_subsystem_start(void);

/* killswitch_glue.c: radio_kill_switch_enabled accessors (written from app_main() and the
   touch task, read by feb_start_scan() on the NimBLE host task). */
bool feb_board_radio_permitted(void);
void feb_board_radio_enabled_set(bool enabled);

/* cluster_glue.c: Phase 9 cluster UART link; the two callouts are initialized by
   feb_board_host_synced() and stopped by the kill switch. */
extern struct ble_npl_callout feb_cluster_scan_done_co;
extern struct ble_npl_callout feb_cluster_scan_timeout_co;
void feb_cluster_link_init(void);
void feb_cluster_scan_done_cb(struct ble_npl_event *ev);
void feb_cluster_scan_timeout_cb(struct ble_npl_event *ev);

/* mesh_caps.c: meshcore_scan/meshtastic_scan capabilities and the mesh_log backlog drain. */
void feb_handle_meshcore_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
void feb_handle_meshtastic_command(uint16_t conn_handle, const feb_command_payload_t *cmd);
void feb_mesh_log_maybe_kick_send(uint16_t conn_handle);
void feb_mesh_log_send_next_batch(uint16_t conn_handle);
void feb_mesh_log_on_connect(void);
void feb_mesh_log_on_disconnect(void);

#endif
