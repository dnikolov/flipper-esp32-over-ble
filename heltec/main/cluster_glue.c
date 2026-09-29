#include <stdlib.h>

#include "driver/uart.h"

#include "feb_app_internal.h"
#include "board_hooks.h"
#include "cluster_link.h"

/* Phase 9 cluster worker glue (docs/CLUSTER.md): the UART link to an esp32/cluster_worker/ C6,
   and the feb_cluster_*() functions components/feb_app_core calls (FEB_HAS_CLUSTER_WORKER) to
   use it as an alternative Wi-Fi scan source for the manual wifi_scan capability and for
   wardriving's Wi-Fi source. Every cluster_link_spinlock critical section lives in this file;
   the core only reaches the lock-protected state through the functions below. */

/* Phase 9 cluster inter-board UART link (docs/CLUSTER.md, this board's own
   docs/hardware/heltec-wifi-lora-32-v2/README.md "Phase 9 cluster inter-board UART link"
   section, hardware-confirmed 2026-09-26): UART_NUM_1 is already location.c's GPS UART on
   this board, so this link uses UART_NUM_2 instead -- checked before picking a port, per
   this agent's own read discipline. TX=GPIO32 -> C6 RX/GPIO18, RX=GPIO33 <- C6 TX/GPIO19.
   Proxies only the wifi_scan capability's manual one-shot path to a present 2.4GHz worker
   (esp32/cluster_worker/) -- wardriving's Wi-Fi source stays local-radio-only, unchanged
   (a later, separate step per docs/CLUSTER.md's "Composite behaviors"). */
#define FEB_CLUSTER_UART_PORT UART_NUM_2
#define FEB_CLUSTER_UART_TX_GPIO 32
#define FEB_CLUSTER_UART_RX_GPIO 33
#define FEB_CLUSTER_UART_BAUD 115200u
#define FEB_CLUSTER_UART_RX_BUF_SIZE 2048u
#define FEB_CLUSTER_RX_CHUNK_SIZE 256u
/* Task-stack-local decoder/frame buffers (see cluster_link_rx_task()'s own comment) push this
   above the other lightweight tasks' 3072-byte precedent (location_task()'s FEB_GPS_TASK_STACK_SIZE,
   factory_reset_task, reconnect_task) -- feb_cluster_decoder_t + feb_cluster_frame_t alone are
   ~1KB, plus the chunk buffer and normal call-frame/switch-case overhead. */
#define FEB_CLUSTER_RX_TASK_STACK_SIZE 4096u
/* 3 missed 1000ms WORKER_HELLO beacons, per docs/CLUSTER.md's own stated presence rule. */
#define FEB_CLUSTER_HELLO_TIMEOUT_MS 3000u
/* Bounded wait for the worker's scan_batch_done after arming a manual one-shot scan.
   Unvalidated/borrowed, not derived from any Heltec- or C6-specific coexistence measurement
   (none exists yet) -- this is a UART-link wait bound, not a radio-coexistence bound, and a
   2.4GHz-only manual scan is the lower-risk one-shot shape this project's coexistence notes
   call out (see this agent's own scope notes) rather than a continuous concurrent capture.
   Picked generously above a typical ~1.5-2s default esp_wifi_scan_start() sweep across all
   14 2.4GHz channels plus UART transfer time for the result batch. */
#define FEB_CLUSTER_MANUAL_SCAN_TIMEOUT_MS 5000u

/* Snapshot scratch for a delegated wardriving flush only (feb_wifi_scan_done_cb()'s WARDRIVING
   branch, feb_wardriving_wifi_delegated case): cluster_link_rx_task() keeps appending
   scan_result frames into feb_wifi_scan_raw_records[]/feb_wifi_scan_raw_count continuously (no
   batch_done in FEB_CLUSTER_SCAN_MODE_CONTINUOUS to pause it at), so a flush snapshots
   whatever has accumulated into this separate buffer under cluster_link_spinlock (a short
   bounded memcpy, not a long-held critical section) instead of reading the live array
   directly while it may still be being written from the other task. The manual-scan path
   needs no such snapshot: its writer (cluster_link_rx_task) and the batch_done that stops
   it are the same task, so there is no concurrent writer left by the time the NimBLE host
   task reads feb_wifi_scan_raw_records for that case.
   Heap-allocated (FEB_WIFI_SCAN_RAW_MAX * sizeof(wifi_ap_record_t), ~5.4KB), not a static
   array like feb_wifi_scan_raw_records itself: a fixed-size second copy of that array living in
   .bss permanently overflowed this board's already-tight classic-ESP32 DRAM budget by ~5.5KB
   at build time (same tightness class as heltec/CMakeLists.txt's RadioLib-exclusion comment
   and cluster_link_rx_task's own stack-placement comment). Only allocated for the lifetime of
   a delegated wardriving run (feb_wardriving_start_internal()/feb_wardriving_stop_internal()), not
   permanently resident -- this board's heap has ample room by comparison, per those same
   comments. */
static wifi_ap_record_t *wardriving_cluster_flush_records;
/* HP-04: feb_wifi_scan_done_cb() (host task) sets busy under cluster_link_spinlock while it
   iterates the flush buffer; a release from another task (kill-switch) while busy only NULLs
   the pointer and sets free_pending, handing the free() to the reader when it finishes. */
static bool cluster_flush_busy;
static bool cluster_flush_free_pending;

/* Phase 9 cluster inter-board UART link state. cluster_worker_last_hello_ms is written only
   by cluster_link_rx_task() and read (without a lock) by feb_cluster_worker_is_present() -- a
   single-scalar timestamp update/read, same convention as feb_wifi_scan_in_progress above (no
   spinlock for a lone scalar in this file). cluster_scan_collecting is a two-way flag
   (cluster_link_rx_task() clears it on scan_batch_done, feb_cluster_scan_timeout_cb() clears it
   on the NimBLE host task if scan_batch_done never arrives) -- guarded by
   cluster_link_spinlock so exactly one of those two paths acts on any given manual scan,
   same shape as location.c's location_spinlock guarding its own multi-writer state. */
static portMUX_TYPE cluster_link_spinlock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t cluster_worker_last_hello_ms;
static bool cluster_scan_collecting; /* guarded by cluster_link_spinlock */
struct ble_npl_callout feb_cluster_scan_done_co;
struct ble_npl_callout feb_cluster_scan_timeout_co;

/* Phase 9 cluster inter-board UART link (docs/CLUSTER.md). Converts one worker-sourced
   feb_cluster_scan_result_t into the wifi_ap_record_t shape feb_wifi_scan_select_top32() already
   knows how to sort/convert -- only the fields wifi_scan_phy_str()/wifi_scan_auth_str()/the
   selection loop actually read are filled in (ssid, bssid, primary, rssi, authmode, phy_11*
   bits), everything else stays zeroed. `auth` is a direct cast: FEB_CLUSTER_AUTH_* 0..16
   mirror wifi_auth_mode_t's own numeric values exactly (cluster_link.h's own comment), and
   wifi_scan_auth_str()'s switch already defaults to "unknown" for anything else (including
   FEB_CLUSTER_AUTH_UNKNOWN=17), so no range check is needed here either. */
static void cluster_scan_result_to_ap_record(const feb_cluster_scan_result_t *in, wifi_ap_record_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->ssid, in->ssid, in->ssid_len);
    memcpy(out->bssid, in->bssid, FEB_CLUSTER_SCAN_RESULT_BSSID_LEN);
    out->primary = in->channel;
    out->rssi = in->rssi;
    out->authmode = (wifi_auth_mode_t)in->auth;
    switch ((feb_cluster_phy_t)in->phy) {
    case FEB_CLUSTER_PHY_11AX:
        out->phy_11ax = 1;
        break;
    case FEB_CLUSTER_PHY_11N:
        out->phy_11n = 1;
        break;
    case FEB_CLUSTER_PHY_11G:
        out->phy_11g = 1;
        break;
    case FEB_CLUSTER_PHY_11B:
    default:
        out->phy_11b = 1;
        break;
    }
}

/* Runs on its own dedicated task, sized (FEB_CLUSTER_RX_TASK_STACK_SIZE) with enough headroom
   for its own locals -- unlike the BLE-callback-path tasks this project's recurring
   BleEventWorker/task-stack-overflow bug class has hit (see docs/LESSONS.md), this task's
   stack is a fresh heap allocation this call sizes explicitly, not a fixed shared budget
   another subsystem also depends on, so `decoder`/`frame` (feb_cluster_frame_t alone is >500
   bytes: FEB_CLUSTER_MAX_PAYLOAD=512 payload plus header) are deliberately task-stack-local
   here rather than file-scope `static` -- this board's classic-ESP32 DRAM/.bss budget is
   already tight (see heltec/CMakeLists.txt's RadioLib-exclusion comment) and two 512-byte-payload
   structs living there permanently, for a link that's absent unless a C6 worker is physically
   wired up, was measured to overflow it by ~4.5KB at build time; the heap this task's own stack
   comes from has ample room by comparison. Only a single in-flight frame is ever needed (one
   dedicated task, one byte-at-a-time decode, no batching) -- `feb_cluster_decoder_feed_byte()`
   is called directly instead of the `_feed()` multi-frame convenience wrapper. */
static void cluster_link_rx_task(void *arg)
{
    feb_cluster_decoder_t decoder;
    uint8_t chunk[FEB_CLUSTER_RX_CHUNK_SIZE];
    feb_cluster_frame_t frame;

    (void)arg;
    feb_cluster_decoder_init(&decoder);

    for (;;) {
        int read = uart_read_bytes(FEB_CLUSTER_UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        int i;

        if (read < 0) {
            read = 0;
        }
        /* Past the last input byte, keep polling: a resync can leave further complete frames
           buffered in the decoder (HP-30). */
        for (i = 0;; i++) {
            feb_cluster_decode_result_t result =
                (i < read) ? feb_cluster_decoder_feed_byte(&decoder, chunk[i], &frame)
                           : feb_cluster_decoder_poll(&decoder, &frame);

            if (result != FEB_CLUSTER_DECODE_FRAME_READY) {
                if (i >= read) {
                    break;
                }
                continue;
            }
            switch (frame.msg_type) {
            case (uint8_t)FEB_CLUSTER_MSG_WORKER_HELLO: {
                feb_cluster_worker_hello_t hello;

                if (feb_cluster_decode_worker_hello(&frame, &hello)) {
                    cluster_worker_last_hello_ms = (uint32_t)(esp_timer_get_time() / 1000);
                }
                break;
            }
            case (uint8_t)FEB_CLUSTER_MSG_SCAN_RESULT: {
                feb_cluster_scan_result_t scan_result;
                int decoded = feb_cluster_decode_scan_result(&frame, &scan_result);

                /* Guards the append itself (not just the collecting check) so this can't
                   race a delegated-wardriving flush's snapshot-and-reset of
                   feb_wifi_scan_raw_count on the NimBLE host task (feb_wifi_scan_done_cb()) --
                   unlike the manual-scan case, that flush has no batch_done to hand off on,
                   so the two tasks are genuinely concurrent here. */
                portENTER_CRITICAL(&cluster_link_spinlock);
                if (cluster_scan_collecting && decoded && feb_wifi_scan_raw_count < FEB_WIFI_SCAN_RAW_MAX) {
                    cluster_scan_result_to_ap_record(&scan_result, &feb_wifi_scan_raw_records[feb_wifi_scan_raw_count]);
                    feb_wifi_scan_raw_count++;
                }
                portEXIT_CRITICAL(&cluster_link_spinlock);
                break;
            }
            case (uint8_t)FEB_CLUSTER_MSG_SCAN_BATCH_DONE: {
                feb_cluster_scan_batch_done_t done;
                bool was_collecting;

                if (!feb_cluster_decode_scan_batch_done(&frame, &done)) {
                    break;
                }
                portENTER_CRITICAL(&cluster_link_spinlock);
                was_collecting = cluster_scan_collecting;
                cluster_scan_collecting = false;
                portEXIT_CRITICAL(&cluster_link_spinlock);
                if (was_collecting) {
                    /* Handoff to the NimBLE host task, same shape as feb_wifi_scan_done_co's own
                       sys_evt-task-to-host-task handoff for the local-scan path. */
                    ble_npl_callout_stop(&feb_cluster_scan_timeout_co);
                    ble_npl_callout_reset(&feb_cluster_scan_done_co, 0);
                }
                break;
            }
            default:
                break;
            }
        }
    }
}

/* Present if a WORKER_HELLO arrived within the last FEB_CLUSTER_HELLO_TIMEOUT_MS (3 missed
   1000ms beacons), per docs/CLUSTER.md. Checked only at the moment a wifi_scan command needs
   to decide which path to take (not polled continuously). Unsigned subtraction handles
   esp_timer_get_time()'s ms-truncated wraparound the same way this codebase's other
   millisecond-deadline checks do. */
bool feb_cluster_worker_is_present(void)
{
    uint32_t last_hello_ms = cluster_worker_last_hello_ms;
    uint32_t now_ms;

    if (last_hello_ms == 0) {
        return false;
    }
    now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return (uint32_t)(now_ms - last_hello_ms) <= FEB_CLUSTER_HELLO_TIMEOUT_MS;
}

static void cluster_link_send_scan_config(uint8_t mode, uint8_t dwell_mode, uint16_t interval_ms)
{
    feb_cluster_scan_config_t cfg;
    uint8_t frame[FEB_CLUSTER_MAX_FRAME_SIZE];
    size_t frame_len;

    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = mode;
    cfg.dwell_mode = dwell_mode;
    cfg.band_filter = (uint8_t)FEB_CLUSTER_BAND_FILTER_NA; /* only meaningful to a 5GHz worker */
    cfg.interval_ms = interval_ms; /* meaningless outside FEB_CLUSTER_SCAN_MODE_CONTINUOUS,
                                       per cluster_link.h's own comment -- callers outside
                                       the wardriving-delegation path just pass 0. */

    frame_len = feb_cluster_encode_scan_config_set(frame, sizeof(frame), &cfg);
    if (frame_len == 0) {
        ESP_LOGE(TAG, "cluster_link: scan_config_set encode failed");
        return;
    }
    uart_write_bytes(FEB_CLUSTER_UART_PORT, frame, frame_len);
}

/* If uart_driver_install()/uart_param_config()/uart_set_pin() fail, cluster_worker_last_hello_ms
   simply stays 0 forever and feb_cluster_worker_is_present() always reports absent -- wifi_scan
   falls back to the local-radio path unconditionally, so there is no separate "link enabled"
   flag to track. */
void feb_cluster_link_init(void)
{
    uart_config_t cfg = {
        .baud_rate = (int)FEB_CLUSTER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err;

    err = uart_driver_install(FEB_CLUSTER_UART_PORT, (int)FEB_CLUSTER_UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cluster_link: uart_driver_install failed: %s; worker proxying disabled",
                 esp_err_to_name(err));
        return;
    }
    err = uart_param_config(FEB_CLUSTER_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cluster_link: uart_param_config failed: %s; worker proxying disabled",
                 esp_err_to_name(err));
        return;
    }
    err = uart_set_pin(FEB_CLUSTER_UART_PORT, FEB_CLUSTER_UART_TX_GPIO, FEB_CLUSTER_UART_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cluster_link: uart_set_pin failed: %s; worker proxying disabled",
                 esp_err_to_name(err));
        return;
    }

    if (xTaskCreate(cluster_link_rx_task, "cluster_rx", FEB_CLUSTER_RX_TASK_STACK_SIZE, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cluster_link: failed to start rx task; worker proxying disabled");
    }
}

/* Runs on the NimBLE host task (feb_cluster_scan_done_co's queue), reached when
   cluster_link_rx_task() decoded a scan_batch_done for the manual scan it armed. Mirrors
   feb_wifi_scan_done_cb()'s non-wardriving branch: check for an authenticated connection, then
   reuse the same top-32 selection and batch-send path the local-scan source uses. */
void feb_cluster_scan_done_cb(struct ble_npl_event *ev)
{
    (void)ev;

    if (feb_connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        feb_runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        ESP_LOGW(TAG, "wifi_scan (cluster-worker-sourced) completed with no authenticated "
                      "connection; discarding %u result(s)", (unsigned)feb_wifi_scan_raw_count);
        feb_wifi_scan_in_progress = false;
        return;
    }
    feb_wifi_scan_select_top32(feb_wifi_scan_raw_count);
    feb_wifi_scan_send_next_batch(feb_connection_handle);
}

/* Runs on the NimBLE host task. Fires if the worker never sent scan_batch_done within
   FEB_CLUSTER_MANUAL_SCAN_TIMEOUT_MS of being armed -- per this task's own spec, falls back to
   exactly today's local-radio scan path rather than reporting whatever partial worker results
   arrived. Guarded against racing cluster_link_rx_task()'s own scan_batch_done handling via
   cluster_link_spinlock: whichever of the two actually observes cluster_scan_collecting==true
   is the one that acts; the other is a no-op. */
void feb_cluster_scan_timeout_cb(struct ble_npl_event *ev)
{
    bool was_collecting;

    (void)ev;

    portENTER_CRITICAL(&cluster_link_spinlock);
    was_collecting = cluster_scan_collecting;
    cluster_scan_collecting = false;
    portEXIT_CRITICAL(&cluster_link_spinlock);

    if (!was_collecting) {
        return;
    }

    ESP_LOGW(TAG, "wifi_scan: cluster worker did not send scan_batch_done within %u ms; "
                  "falling back to local-radio scan", (unsigned)FEB_CLUSTER_MANUAL_SCAN_TIMEOUT_MS);
    feb_wifi_scan_raw_count = 0;
    if (feb_connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        feb_runtime_auth_state != RUNTIME_AUTH_STATE_AUTHENTICATED) {
        feb_wifi_scan_in_progress = false;
        return;
    }
    feb_wifi_scan_start_local(feb_connection_handle);
}

/* Shared teardown for every wardriving stop path. Local-radio behavior ported unchanged
   from esp32/main/main.c -- see that file's fuller comment. Phase 9 addition (2026-09-26):
   if the Wi-Fi source was delegated to a cluster worker, tell it to go idle instead of
   stopping a local scan that was never running -- mirrors the manual wifi_scan path's own
   disconnect-handling teardown.

   NOT host-task-only (correcting this comment's prior claim, HARDENING_PLAN.md HP-04): every
   other call site (feb_wardriving_self_stop(), wardriving_button_toggle_cb(),
   feb_handle_wardriving_command()'s stop path) does run on the NimBLE host task, but
   feb_radio_kill_switch_toggle()'s disable branch (radio_killswitch.c's touch task) calls this
   directly too, by BL27's own deliberate design (no DRAM budget for a host-task handoff). The
   free()/NULL of wardriving_cluster_flush_records below used to run unlocked, racing
   feb_wifi_scan_done_cb() (host task) reading that same pointer and iterating the buffer it points
   to outside cluster_link_spinlock -- a real UAF, not just a raced plain flag (see this
   function's own audit note below and radio_killswitch.c's/feb_radio_kill_switch_toggle()'s
   comments, both corrected in the same pass: this is a real counter-example to "only bool/
   uint32 globals are raced here"). Fixed by moving the pointer read+NULL under the lock and
   only calling free() on a local copy after releasing it (HP-04's preferred 0-DRAM-cost fix).
   This narrows the window to the pointer/snapshot handoff itself; it does not add a "busy"
   wait for feb_wifi_scan_done_cb()'s own (potentially slow, flash-writing) iteration of the
   buffer contents; cluster_flush_records_release() closes that remaining window with the
   cluster_flush_busy/cluster_flush_free_pending handoff (2 B .bss). */
static void cluster_flush_records_release(void)
{
    wifi_ap_record_t *to_free;

    portENTER_CRITICAL(&cluster_link_spinlock);
    cluster_scan_collecting = false;
    to_free = wardriving_cluster_flush_records;
    wardriving_cluster_flush_records = NULL;
    if (cluster_flush_busy) {
        cluster_flush_free_pending = true;
        to_free = NULL;
    }
    portEXIT_CRITICAL(&cluster_link_spinlock);
    free(to_free);
}

/* Manual wifi_scan (feb_handle_wifi_scan_command()): delegates to a present 2.4GHz cluster worker
   when one is currently announcing itself via WORKER_HELLO; false = the caller falls back to
   the local-radio scan. */
bool feb_cluster_wifi_scan_delegate(uint64_t request_id)
{
    if (!feb_cluster_worker_is_present()) {
        return false;
    }
    feb_wifi_scan_raw_count = 0;
    portENTER_CRITICAL(&cluster_link_spinlock);
    cluster_scan_collecting = true;
    portEXIT_CRITICAL(&cluster_link_spinlock);
    cluster_link_send_scan_config((uint8_t)FEB_CLUSTER_SCAN_MODE_MANUAL,
                                  (uint8_t)FEB_CLUSTER_DWELL_NORMAL, 0); /* wifi_scan has
                                      no dwell-mode/interval argument of its own
                                      (docs/PROTOCOL.md); a manual pass is always exactly
                                      one sweep. */
    ble_npl_callout_reset(&feb_cluster_scan_timeout_co,
                          ble_npl_time_ms_to_ticks32(FEB_CLUSTER_MANUAL_SCAN_TIMEOUT_MS));
    ESP_LOGI(TAG, "wifi_scan: 2.4GHz cluster worker present, delegating (request_id=%llu)",
             (unsigned long long)request_id);
    return true;
}

/* Disconnect (feb_gap_event()): Phase 9 cluster-link worker-sourced scan in flight -- unlike the
   local-scan path, no esp_wifi_scan_stop()-triggered WIFI_EVENT_SCAN_DONE will ever arrive to
   clear feb_wifi_scan_in_progress uniformly (the radio scan is running on the worker board, not
   this one) -- clear it directly and tell the worker to go idle so it doesn't keep streaming
   scan_result frames for a request nothing is listening for anymore. False = no worker scan was
   being collected (the caller stops a local scan instead). */
bool feb_cluster_wifi_scan_cancel(void)
{
    bool cluster_was_collecting;

    portENTER_CRITICAL(&cluster_link_spinlock);
    cluster_was_collecting = cluster_scan_collecting;
    cluster_scan_collecting = false;
    portEXIT_CRITICAL(&cluster_link_spinlock);

    if (!cluster_was_collecting) {
        return false;
    }
    ble_npl_callout_stop(&feb_cluster_scan_timeout_co);
    cluster_link_send_scan_config((uint8_t)FEB_CLUSTER_SCAN_MODE_IDLE,
                                  (uint8_t)FEB_CLUSTER_DWELL_NORMAL, 0);
    feb_wifi_scan_in_progress = false;
    ESP_LOGI(TAG, "wifi_scan (cluster-worker-sourced) was in progress at disconnect; "
                  "stopping it (pending results will be discarded)");
    return true;
}

/* Delegated wardriving start (feb_wardriving_start_internal()). dwell_mode is
   wardriving_swelling_mode_t's numeric value, a direct 1:1 cast to feb_cluster_dwell_mode_t
   (wardriving_swelling_mode_t's comment at its own definition). No country field exists on
   this wire message (docs/CLUSTER.md) -- regulatory country is a local-radio-only concept
   here, not forwarded to the worker. */
void feb_cluster_wardriving_begin(wifi_ap_record_t *flush_buf, uint8_t dwell_mode, uint16_t interval_ms)
{
    portENTER_CRITICAL(&cluster_link_spinlock);
    wardriving_cluster_flush_records = flush_buf;
    cluster_scan_collecting = true;
    portEXIT_CRITICAL(&cluster_link_spinlock);
    cluster_link_send_scan_config((uint8_t)FEB_CLUSTER_SCAN_MODE_CONTINUOUS, dwell_mode, interval_ms);
}

/* Delegated wardriving stop (feb_wardriving_stop_internal() and feb_wardriving_start_internal()'s
   BLE-failure rollback). */
void feb_cluster_wardriving_end(void)
{
    cluster_flush_records_release();
    /* uart_write_bytes() holds the UART driver's own internal TX mutex for the whole
       call, so this can't corrupt/interleave bytes with a concurrent host-task send on
       the same port -- but a scan_config_set from this call site (touch task) can still
       land in either order relative to one this same session's host task might be
       sending at the same time (e.g. a fresh `start` reusing this UART right after a
       stop), which is a soft protocol-level race, not a memory-safety one. Accepted,
       same tradeoff class as this function's own comment above. */
    cluster_link_send_scan_config((uint8_t)FEB_CLUSTER_SCAN_MODE_IDLE,
                                  (uint8_t)FEB_CLUSTER_DWELL_NORMAL, 0);
}

/* Delegated wardriving flush (feb_wifi_scan_done_cb()), HARDENING_PLAN.md HP-04: take the
   flush-buffer pointer and mark it busy under the lock that guards it; returns whether it is
   held (then feb_cluster_flush_finish() must follow once the caller is done reading it). */
bool feb_cluster_flush_snapshot(wifi_ap_record_t **raw_out, uint16_t *raw_count_out)
{
    wifi_ap_record_t *raw;
    uint16_t raw_count;
    bool flush_held = false;

    portENTER_CRITICAL(&cluster_link_spinlock);
    raw = wardriving_cluster_flush_records;
    raw_count = feb_wifi_scan_raw_count;
    if (raw != NULL) {
        cluster_flush_busy = true;
        flush_held = true;
    }
    if (raw != NULL && raw_count > 0) {
        memcpy(raw, feb_wifi_scan_raw_records,
               (size_t)raw_count * sizeof(feb_wifi_scan_raw_records[0]));
    } else {
        raw_count = 0;
    }
    feb_wifi_scan_raw_count = 0;
    portEXIT_CRITICAL(&cluster_link_spinlock);
    *raw_out = raw;
    *raw_count_out = raw_count;
    return flush_held;
}

/* Must run before feb_wardriving_self_stop() in the same flush, which releases the buffer on
   the same task. */
void feb_cluster_flush_finish(wifi_ap_record_t *raw)
{
    wifi_ap_record_t *to_free = NULL;

    portENTER_CRITICAL(&cluster_link_spinlock);
    cluster_flush_busy = false;
    if (cluster_flush_free_pending) {
        cluster_flush_free_pending = false;
        to_free = raw;
    }
    portEXIT_CRITICAL(&cluster_link_spinlock);
    free(to_free);
}
