#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif

static const Service_UUID_t service_uuid = {.Service_UUID_128 =
                                                {0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31,
                                                 0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x11}};

typedef struct {
    const uint8_t* data;
    uint16_t len;
} NotifyFragment;

/* ble_gatt_characteristic_update()'s Fixed-data path always sends data.fixed.length bytes
   regardless of the source buffer's real size; a Callback characteristic is required here
   so each notification carries exactly the fragment's own length over the air.

   context == NULL is not a "sending with no data" case: it's ble_gatt_characteristic_init()
   itself (targets/f7/ble_glue/furi_ble/gatt.c) probing this characteristic's maximum size at
   registration time, via this same callback with data=NULL. Reporting anything less than
   PAYLOAD_MAX here registers the Notify characteristic's max attribute length too small,
   and aci_gatt_update_char_value() then rejects every real notify (BLE_STATUS_INVALID_PARAMS,
   0x92) whose fragment exceeds that registered max -- silently breaking hello_ack and every
   other outbound notification. (Regressed 2026-09-12 by an unverified "fix" for G20 that
   assumed this NULL-context path only mattered for real sends.) */
static bool notify_data_callback(const void* context, const uint8_t** data, uint16_t* data_len) {
    if(context == NULL) {
        if(data) *data = NULL;
        if(data_len) *data_len = PAYLOAD_MAX;
        return false;
    }
    const NotifyFragment* fragment = context;
    if(data) *data = fragment->data;
    if(data_len) *data_len = fragment->len;
    return false;
}

static const BleGattCharacteristicParams characteristics[CharacteristicCount] = {
    [CharacteristicWrite] =
        {.name = "Write",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = PAYLOAD_MAX,
         .uuid.Char_UUID_128 = {0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31, 0x9e, 0xa2,
                                0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x12},
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [CharacteristicNotify] =
        {.name = "Notify",
         .data_prop_type = FlipperGattCharacteristicDataCallback,
         .data.callback.fn = notify_data_callback,
         .data.callback.context = NULL,
         .uuid.Char_UUID_128 = {0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31, 0x9e, 0xa2,
                                0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x13},
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
         .is_variable = CHAR_VALUE_LEN_VARIABLE}};

/* feb_reassembly_feed() runs synchronously on the BleEventWorker thread (see
   profile_event_handler); the periodic timeout check below runs on the separate Furi
   timer-service thread, so `app_reassembly` needs real cross-thread protection here — unlike
   every other BleEventWorker-only static in this file, which stays safe under the
   single-threaded-BLE-dispatch assumption alone. Both critical sections are tiny (no
   crypto, no nested calls), so a plain blocking mutex acquire is fine on the 1280-byte
   BleEventWorker stack. */

/* HP-06/HP-07/G10: serializes (a) every feb_session_encrypt_record()/feb_session_decrypt_record()
   call (encrypt runs on the main thread, e.g. send_gps_command() from AppEventGpsPollTick,
   wardriving start/stop, scans; decrypt runs on BleEventWorker for unsolicited pushes -- the
   two were previously able to interleave on the shared GCM nonce/AAD/tag scratch in session.c,
   risking nonce reuse) and (b) every reset of the pairing/session ceremony statics
   (pairing_reset_state()/session_reset_state()), so a main-thread reset can never tear a
   ceremony BleEventWorker is mid-way through advancing.

   Design (connection-generation counter, not "move every reset onto BleEventWorker" -- see
   docs/HARDENING_PLAN.md HP-07 for why either is acceptable): `app_protocol_generation` is bumped,
   under this mutex, as the very first action of every protocol-state mutation on BleEventWorker
   (pairing/session envelope dispatch below, and the protected-record decrypt block), strictly
   before any of that mutation's own (unlocked, BleEventWorker-only) field writes begin. A
   BtStatusConnected/Advertising event (posted from the separate "Bt" service thread, see
   bt_status_callback) carries the generation value that was current at post time. The main
   loop never resets pairing/session state directly; it calls protocol_reset_if_unchanged()
   with that carried value, which atomically (a) compares it against the current generation
   and (b) performs the reset only if unchanged -- i.e. only if nothing on BleEventWorker has
   touched protocol state since this now-possibly-stale event was queued. This closes the race
   without needing to wrap entire multi-return ceremony functions: either the compare-and-reset
   commits before BleEventWorker's own bump (safe -- the ceremony proceeds on a freshly-reset
   session/pairing state), or the bump commits first (any reset attempt against that now-stale
   generation is rejected, so it can never run concurrently with the unlocked field writes that
   follow the bump). UI-only fields (pairing_phase, connection_lost, LED, scan-UI resets) are
   deliberately NOT gated by this check -- they always run, matching "leave the main thread
   UI-only" for everything except the actual crypto-state reset.

   Deadlock avoidance: every critical section under this mutex is a handful of memcpy/zero
   calls or a single feb_session_encrypt_record()/feb_session_decrypt_record() call -- never a
   BLE send (ble_gatt_characteristic_update() via send_pairing_record()/emit_fragment() always
   runs after this mutex has already been released) and never a call back into
   pairing_reset_state()/session_reset_state() while already holding it (their `_locked`
   variants are used instead wherever the caller already holds the lock). app_reassembly_mutex may
   be acquired while this mutex is held (protocol_reset_if_unchanged()) but never the reverse,
   so there is a single fixed lock order and no cycle. */

/* Guards wifi_scan_aps/ble_scan_devices/mesh_log_display_nodes and the wardriving CSV/mesh-log
   export file handles -- see its point of use further down (wardriving_csv_file's own comment)
   for the full cross-thread argument. Declared here, ahead of the wifi_scan/ble_scan capability
   section, so handle_wifi_scan_status()/handle_ble_scan_status() can take it too (HP-08). */
static uint8_t outgoing_message_id;

/* `gps` poll timer (docs/PLAN.md "Real GPS driver...", decision 5: poll only while a screen
   that needs live status is open -- today, only the Wardriving screen; see
   gps_poll_timer_callback()'s own comment). Allocated/freed alongside
   app_reassembly_timeout_timer; started/stopped on Wardriving-screen entry/exit rather than at
   profile start/stop, since the whole point is "only while that screen is open", not "only
   while connected" (a poll tick with no active session is just a harmless no-op send
   attempt -- see send_gps_command()'s own profile/pairing_phase guard). */

/* Publish-result poll timer (docs/WARDRIVING_PUBLISH.md) -- allocated/freed alongside
   app_gps_poll_timer above; started only while AppScreenPublish is showing "waiting for
   publish..." and stopped the moment a result is read, a timeout is hit, or the user backs
   out. Independent of any BLE session, unlike app_gps_poll_timer. */


/* ---- pairing ceremony state: BleEventWorker's own dispatch of these fields is
   single-threaded/non-reentrant (see profile_event_handler), which is what makes static
   storage safe here at all (keeps these off the ~1280-byte BleEventWorker stack,
   docs/SESSION_MEMORY.md's stack-overflow root cause) -- but the main thread's
   BtStatusConnected/Advertising handling also calls pairing_reset_state() on these same
   statics (HP-07), a real cross-thread access app_protocol_mutex above now serializes.
   PairStage itself is declared in app_internal.h (session_flow.c's handle_pair_* functions
   also need it). ---- */



/* G12 fix: the real negotiated ATT MTU, once known -- see profile_event_handler's
   ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE case. Starts at (and is reset back to on disconnect)
   FEB_DEFAULT_ATT_MTU, matching the ESP32's own negotiated_att_mtu reset-on-connect
   convention, so a stale large value from a previous connection can never carry over. */
static uint16_t negotiated_att_mtu = FEB_DEFAULT_ATT_MTU;

static void emit_fragment(const uint8_t* fragment, size_t fragment_len, void* ctx) {
    Esp32BleProfile* profile = ctx;
    NotifyFragment notify_fragment = {.data = fragment, .len = (uint16_t)fragment_len};
    ble_gatt_characteristic_update(
        profile->service_handle, &profile->characteristics[CharacteristicNotify], &notify_fragment);
}

APP_FN bool send_pairing_record(Esp32BleProfile* profile, const uint8_t* record, size_t record_len) {
    /* G12 fix: use whichever is smaller of the real negotiated MTU and this Notify
       characteristic's own fixed 64-byte cap (FEB_NOTIFY_CHAR_EFFECTIVE_MTU) -- never the raw
       negotiated MTU alone (see that define's own comment). Before MTU negotiation completes,
       negotiated_att_mtu is still FEB_DEFAULT_ATT_MTU (23), so early pairing-phase records
       naturally get the same conservative fragmentation as before this fix. */
    uint16_t effective_mtu = (negotiated_att_mtu < FEB_NOTIFY_CHAR_EFFECTIVE_MTU) ?
                             negotiated_att_mtu :
                             FEB_NOTIFY_CHAR_EFFECTIVE_MTU;
    size_t capacity = feb_fragment_capacity(effective_mtu);
    if(capacity == 0) {
        return false;
    }
    uint8_t message_id = outgoing_message_id++;
    uint8_t fragment_count =
        feb_fragment_record(record, record_len, capacity, message_id, emit_fragment, profile);
    return fragment_count != 0;
}

/* Runs on the Furi timer-service thread (not BleEventWorker) — see app_reassembly_mutex's
   comment above for why this needs the mutex. A stalled sequence is silently reclaimed
   (drop and continue): keep the BLE connection open, send no response, per PROTOCOL.md's
   malformed-fragment/message policy (docs/PLAN.md step 3). */
APP_FN void reassembly_timeout_timer_callback(void* context) {
    UNUSED(context);
    furi_mutex_acquire(app_reassembly_mutex, FuriWaitForever);
    feb_frame_status_t status = feb_reassembly_check_timeout(&app_reassembly, furi_get_tick());
    furi_mutex_release(app_reassembly_mutex);
    if(status == FEB_FRAME_TIMEOUT) {
        FURI_LOG_W(TAG, "Fragment reassembly timed out, buffer reclaimed");
    }
}

/* gps_poll_timer_callback and publish_poll_timer_callback both
   run on the Furi timer-service thread -- FreeRTOS's single Timer Service task, the one every
   furi_timer_alloc(..., FuriTimerTypePeriodic, ...) callback in the whole firmware shares
   (targets/f7/inc/FreeRTOSConfig.h's configTIMER_TASK_STACK_DEPTH, 256 words/1024 bytes).
   That task processes one expired-timer callback at a time from its own command queue, so
   these callbacks can never actually be concurrent with *each other* -- only with
   BleEventWorker/Bt/GuiSrv, i.e. with app_shared_ble_event/the Bt-thread event/input_callback's
   own event, none of which this pair touches. They therefore safely share one static
   AppEvent, timer_service_event, below -- same single-in-flight rationale as
   app_shared_ble_event, just scoped to this one other thread instead. AppEvent is 104 bytes
   since the tagged-union rework (it was 576 -- roughly half this thread's entire 1024-byte
   budget for one frame); kept static anyway, matching every other post_*()/callback's
   static-buffer convention in this file (docs/LESSONS.md's BleEventWorker entry generalizes
   the rule to any tight system thread) -- 104 resident bytes is not worth reopening the
   stack-budget question on a 1024-byte thread for. Explicit reset at the top of each callback because a static initializer only
   runs once at load time, not per call. */
static AppEvent timer_service_event;

/* Must NOT touch app_session_seq_out/the shared app_cmd_payload_buf/app_cmd_ciphertext_buf/
   app_cmd_record_buf itself (those are the main thread's alone to write, per
   send_wifi_scan_command()'s own cross-thread-safety argument). It only posts
   AppEventGpsPollTick; the main loop's own handler for that event is what actually calls
   send_gps_command(). */
APP_FN void gps_poll_timer_callback(void* context) {
    Esp32App* app = context;
    memset(&timer_service_event, 0, sizeof(timer_service_event));
    timer_service_event.type = AppEventGpsPollTick;
    /* timeout_ms must stay 0: this runs on the FreeRTOS Timer Service task, shared by every
       furi_timer_alloc() callback in the whole firmware (HP-08) -- a dropped tick just means
       the next one retries a moment later, which is far cheaper than stalling every other
       app's timers too. */
    app_queue_put(app->queue, &timer_service_event, 0);
}

APP_FN void publish_poll_timer_callback(void* context) {
    Esp32App* app = context;
    memset(&timer_service_event, 0, sizeof(timer_service_event));
    timer_service_event.type = AppEventPublishPollTick;
    /* Same Timer Service thread, same timeout_ms=0 reasoning as gps_poll_timer_callback(). */
    app_queue_put(app->queue, &timer_service_event, 0);
}

/* Profile-teardown sequence shared by stop_service() (full app-exit/BT-unavailable path) and
   publish_start() (temporary pause for the duration of a publish transfer). Deliberately
   does not touch pairing/session state, scan UI, or notifications -- those resets belong to
   whichever caller actually needs them (stop_service() applies them itself). */
APP_FN void stop_ble_profile(Esp32App* app) {
    furi_hal_bt_stop_advertising();
    bt_disconnect(app->bt);
    /* bt_disconnect() only closes the RPC session and stops advertising -- it does not itself
       drop an already-established GATT link; that only happens inside
       bt_profile_restore_default()'s own furi_hal_bt_reinit()/hci_reset(). Without a settle
       delay here, a still-live, busy connection (wardriving mid-backlog-drain is the
       confirmed real-world trigger) can still be dispatching an inbound BLE event to
       profile_event_handler on BleEventWorker at the exact moment profile_stop() -- called
       from the Bt service thread inside that same reinit -- unregisters the handler and
       frees profile: a cross-thread teardown race (event_dispatcher.c's handler list has no
       locking against concurrent register/unregister vs. dispatch). Matches the pinned
       firmware's own hid_app (applications/system/hid_app/hid.c), which inserts this same
       200ms wait in the same spot for the same class of reason. */
    furi_delay_ms(200);
    if(app->profile) {
        furi_check(bt_profile_restore_default(app->bt));
        app->profile = NULL;
    }
}

/* `client_auth` carries the ESP32's proof over the same transcript S computed in
   handle_hello(). A proof mismatch is a real anomaly (desync/corruption/impersonation),
   not the "never paired" case -- per docs/PROTOCOL.md, it gets no reply at all (matching
   the existing no-proactive-bt_disconnect()-from-inside-profile_event_handler
   reentrancy-safety pattern; rely on the peer disconnecting) and must NOT be reported as
   unknown_board, which would let an attacker force repeated pairing-window openings by
   corrupting proofs in transit. A malformed client_auth payload is folded into the same
   no-reply bucket for the same reason: this layer must not give an unauthenticated peer a
   way to distinguish "malformed" from "wrong proof". */
static void handle_client_auth(Esp32BleProfile* profile, const feb_unencrypted_record_t* envelope) {
    if(app_session_stage != SessionStageHelloReceived) {
        FURI_LOG_W(TAG, "Ignoring unexpected client_auth");
        return;
    }
    if(envelope->board_id_len != app_session_board_id_len ||
       memcmp(envelope->board_id, app_session_board_id, app_session_board_id_len) != 0 ||
       memcmp(envelope->session_id, app_session_id_bytes, FEB_SESSION_ID_LEN) != 0) {
        FURI_LOG_W(TAG, "Ignoring client_auth: board_id/session_id mismatch");
        return;
    }

    static feb_client_auth_payload_t auth_payload;
    feb_cbor_status_t status = feb_cbor_decode_client_auth_payload(
        envelope->payload_span, envelope->payload_span_len, &auth_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "client_auth: malformed payload, dropping (no reply)");
        return;
    }

    static uint8_t expected_proof[FEB_SESSION_PROOF_LEN];
    feb_session_esp32_proof(
        app_session_pairing_secret, app_session_transcript_buf, app_session_transcript_len, expected_proof);
    bool match = feb_consttime_equal(expected_proof, auth_payload.proof, FEB_SESSION_PROOF_LEN) != 0;
    feb_secure_zero(expected_proof, sizeof(expected_proof));
    if(!match) {
        FURI_LOG_W(TAG, "client_auth: proof verification failed (no reply, per PROTOCOL.md)");
        post_pairing_phase(profile->app, PairingPhaseFailed, "proof verification failed");
        session_reset_state();
        post_session_fatal(profile->app);
        return;
    }

    feb_session_derive_key(
        app_session_pairing_secret,
        app_session_client_nonce,
        app_session_device_nonce,
        app_session_board_id,
        app_session_board_id_len,
        app_session_id_bytes,
        app_session_key);
    /* pairing_secret's in-memory copy has served its purpose (proof + key derivation);
       app_session_key itself is intentionally left alive -- see this section's top comment. */
    feb_secure_zero(app_session_pairing_secret, sizeof(app_session_pairing_secret));

    app_session_stage = SessionStageActive;
    app_session_seq_out = 1;
    app_session_seq_in = 1;
    if(profile->app->notifications) {
        /* Must stop the running blink sequence explicitly - it's a separate
           hardware LED-blink subsystem that a plain RGB message can't override. */
        notification_message(profile->app->notifications, &sequence_blink_stop);
        notification_message(profile->app->notifications, &sequence_set_only_blue_255);
    }
    post_pairing_phase(profile->app, PairingPhaseSessionActive, NULL);
    FURI_LOG_I(TAG, "Runtime session authenticated for board '%s'", app_session_board_id);
    capability_bootstrap(profile);
    if(profile->app->capability_has_wardriving && !profile->app->wardriving_running_known) {
        send_wardriving_status_query(profile->app);
    }
}

static BleEventAckStatus profile_event_handler(void* event, void* context) {
    Esp32BleProfile* profile = context;
    hci_event_pckt* event_packet = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);
    evt_blecore_aci* ble_event = (evt_blecore_aci*)event_packet->data;

    if(event_packet->evt == HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE &&
       ble_event->ecode == ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) {
        aci_gatt_attribute_modified_event_rp0* modified =
            (aci_gatt_attribute_modified_event_rp0*)ble_event->data;
        if(modified->Attr_Handle == profile->characteristics[CharacteristicWrite].handle + 1) {
            const uint8_t* out_record = NULL;
            size_t out_len = 0;
            furi_mutex_acquire(app_reassembly_mutex, FuriWaitForever);
            feb_frame_status_t status = feb_reassembly_feed(
                &app_reassembly,
                modified->Attr_Data,
                modified->Attr_Data_Length,
                furi_get_tick(),
                &out_record,
                &out_len);
            furi_mutex_release(app_reassembly_mutex);

            if(status == FEB_FRAME_OK) {
                return BleEventAckFlowEnable;
            } else if(status == FEB_FRAME_MESSAGE_COMPLETE) {
                /* Two outer envelope shapes share this one wire: the 4-field pairing
                   envelope (pair_init/pair_confirm/pair_complete/pairing-phase error) and
                   the 5-field session_id-bearing envelope (hello/client_auth/runtime
                   error). Which one a connecting peer sends depends on whether it already
                   holds a working pairing_secret -- this Flipper can't know that in
                   advance, so it peeks the map's own field count (a read-only header
                   parse, no further decoding) to route to the matching decoder, rather
                   than guessing or trying both. docs/PROTOCOL.md's "distinguish by
                   connection phase, not record content" instruction is about the
                   session-vs-pairing `error` ambiguity specifically (same `type` string,
                   two possible envelopes) -- it does not forbid this structural routing
                   step, which exists precisely because no phase state can predict which
                   envelope shape a not-yet-authenticated peer will use first. */
                feb_cbor_status_t peek_status = FEB_CBOR_OK;
                size_t field_count = 0;
                size_t peek_len = feb_cbor_decode_map_header(out_record, out_len, &field_count, &peek_status);
                if(peek_len == 0) {
                    FURI_LOG_W(TAG, "Rejected record: cbor status %d", peek_status);
                    return BleEventAckFlowEnable;
                }
                if(field_count == 4) {
                    static feb_pairing_envelope_t envelope;
                    feb_cbor_status_t decode_status =
                        feb_cbor_decode_pairing_envelope(out_record, out_len, &envelope);
                    if(decode_status != FEB_CBOR_OK) {
                        FURI_LOG_W(TAG, "Rejected pairing record: cbor status %d", decode_status);
                        return BleEventAckFlowEnable;
                    }
                    if(envelope.version != 2 ||
                       !board_id_is_valid(envelope.board_id, envelope.board_id_len)) {
                        FURI_LOG_W(TAG, "Rejected pairing record: bad version or board_id");
                        return BleEventAckFlowEnable;
                    }
                    /* HP-07/G10: marks protocol state as touched before any of the pair_*
                       field writes below begin -- see protocol_generation_bump()'s own
                       comment for why this ordering (not wrapping the whole handler) is
                       what makes protocol_reset_if_unchanged() race-free. */
                    protocol_generation_bump();
                    if(text_matches(envelope.type, envelope.type_len, FEB_PAIR_INIT_TYPE)) {
                        handle_pair_init(profile, &envelope);
                    } else if(text_matches(envelope.type, envelope.type_len, FEB_PAIR_CONFIRM_TYPE)) {
                        handle_pair_confirm(profile, &envelope);
                    } else if(text_matches(envelope.type, envelope.type_len, FEB_PAIR_COMPLETE_TYPE)) {
                        handle_pair_complete(profile, &envelope);
                    } else {
                        FURI_LOG_W(
                            TAG,
                            "Ignoring pairing record with unexpected type '%.*s'",
                            (int)envelope.type_len,
                            envelope.type);
                    }
                } else if(field_count == 5) {
                    static feb_unencrypted_record_t session_envelope;
                    feb_cbor_status_t decode_status =
                        feb_cbor_decode_unencrypted(out_record, out_len, &session_envelope);
                    if(decode_status != FEB_CBOR_OK) {
                        FURI_LOG_W(TAG, "Rejected session record: cbor status %d", decode_status);
                        return BleEventAckFlowEnable;
                    }
                    if(session_envelope.version != 2) {
                        FURI_LOG_W(TAG, "Rejected session record: bad version");
                        return BleEventAckFlowEnable;
                    }
                    /* HP-07/G10: see the pairing-envelope branch's identical comment above. */
                    protocol_generation_bump();
                    if(text_matches(session_envelope.type, session_envelope.type_len, FEB_HELLO_TYPE)) {
                        handle_hello(profile, &session_envelope);
                    } else if(text_matches(
                                  session_envelope.type, session_envelope.type_len, FEB_CLIENT_AUTH_TYPE)) {
                        handle_client_auth(profile, &session_envelope);
                    } else {
                        FURI_LOG_W(
                            TAG,
                            "Ignoring session record with unexpected type '%.*s'",
                            (int)session_envelope.type_len,
                            session_envelope.type);
                    }
                } else if(field_count == 7) {
                    /* docs/PLAN.md step 7: protected (AES-256-GCM) record, e.g.
                       capability_response. Per docs/PROTOCOL.md, decode/decrypt failure or
                       a session/board_id/sequence mismatch is fatal: dropped silently (no
                       reply) and the connection is closed. This function still can't call
                       bt_disconnect() directly (BLE-thread reentrancy hazard, see this
                       file's existing no-proactive-bt_disconnect() rationale above), so it
                       posts AppEventSessionFatal instead; the main thread's event loop
                       calls bt_disconnect() on receipt. An unrecognized type is not fatal
                       and is still just dropped below. */
                    /* HP-06/HP-07/G10: the whole decrypt-and-validate sequence runs under
                       app_protocol_mutex -- this is the same mutex every encrypt call site (main
                       thread) takes around feb_session_encrypt_record(), so the two can never
                       interleave on session.c's shared GCM nonce/AAD/tag scratch (HP-06), and
                       bumping app_protocol_generation as the first action makes any subsequent
                       stale-event reset attempt (protocol_reset_if_unchanged()) a no-op for
                       this connection. Released before send_pairing_record()-style BLE sends
                       or the capability/status handlers below run -- none of that is reached
                       from inside this critical section. */
                    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
                    app_protocol_generation++;
                    if(app_session_stage != SessionStageActive) {
                        furi_mutex_release(app_protocol_mutex);
                        FURI_LOG_W(TAG, "Ignoring protected record: no active session");
                        return BleEventAckFlowEnable;
                    }
                    static feb_session_decrypted_record_t decrypted;
                    feb_cbor_status_t decode_status = feb_session_decrypt_record(
                        app_session_key,
                        out_record,
                        out_len,
                        FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER,
                        app_session_plaintext_buf,
                        sizeof(app_session_plaintext_buf),
                        &decrypted);
                    if(decode_status != FEB_CBOR_OK) {
                        furi_mutex_release(app_protocol_mutex);
                        FURI_LOG_W(
                            TAG,
                            "Protected record decode/decrypt failed: %d; dropping (no reply)",
                            decode_status);
                        session_reset_state();
                        post_session_fatal(profile->app);
                        return BleEventAckFlowEnable;
                    }
                    if(decrypted.version != 2 ||
                       memcmp(decrypted.session_id, app_session_id_bytes, FEB_SESSION_ID_LEN) != 0 ||
                       decrypted.board_id_len != app_session_board_id_len ||
                       memcmp(decrypted.board_id, app_session_board_id, app_session_board_id_len) != 0 ||
                       decrypted.sequence != app_session_seq_in ||
                       decrypted.sequence >= FEB_SESSION_SEQUENCE_MAX) {
                        furi_mutex_release(app_protocol_mutex);
                        FURI_LOG_W(TAG, "Protected record session/sequence mismatch; dropping (no reply)");
                        session_reset_state();
                        post_session_fatal(profile->app);
                        return BleEventAckFlowEnable;
                    }
                    app_session_seq_in++;
                    furi_mutex_release(app_protocol_mutex);

                    if(text_matches(decrypted.type, decrypted.type_len, "capability_response")) {
                        handle_capability_response(profile, decrypted.plaintext, decrypted.plaintext_len);
                    } else if(text_matches(decrypted.type, decrypted.type_len, "status")) {
                        /* `status` carries no capability field either (docs/PROTOCOL.md);
                           wardriving's own states ("started"/"data"/"stopped") are never
                           ambiguous with wifi_scan/ble_scan's ("partial"/"complete") by text
                           alone, so a cheap state-only peek is enough to route correctly --
                           including a wardriving status(state="data", request_id=0)
                           unsolicited backlog-drain record, since nothing here (or inside
                           handle_wardriving_status()) ever inspects request_id at all. Same
                           for mesh_log's own status(state="mesh_data", request_id=0) --
                           "mesh_data" is deliberately distinct from wardriving's "data"
                           (cbor_mesh_log.h's own top comment: every capability's status.state
                           value(s) must be globally unique, not merely unique within that one
                           capability). This peek's own decode failure is logged and dropped
                           here rather than silently falling through to a wifi_scan/ble_scan
                           handler that would just fail the exact same decode again. */
                        static feb_status_payload_t status_peek;
                        feb_cbor_status_t peek_status = feb_cbor_decode_status_payload(
                            decrypted.plaintext, decrypted.plaintext_len, &status_peek);
                        if(peek_status != FEB_CBOR_OK) {
                            FURI_LOG_W(TAG, "status payload decode failed: %d; dropping", peek_status);
                        } else if(
                            text_matches(status_peek.state, status_peek.state_len, "started") ||
                            text_matches(status_peek.state, status_peek.state_len, "data") ||
                            text_matches(status_peek.state, status_peek.state_len, "stopped")) {
                            handle_wardriving_status(profile, decrypted.plaintext, decrypted.plaintext_len);
                        } else if(
                            text_matches(status_peek.state, status_peek.state_len, "no_signal") ||
                            text_matches(status_peek.state, status_peek.state_len, "acquiring") ||
                            text_matches(status_peek.state, status_peek.state_len, "fix")) {
                            handle_gps_status(profile, decrypted.plaintext, decrypted.plaintext_len);
                        } else if(
                            text_matches(status_peek.state, status_peek.state_len, "mesh_data")) {
                            handle_mesh_log_status(profile, decrypted.plaintext, decrypted.plaintext_len);
                        } else if(app_pending_command_kind == PendingCommandBleScan) {
                            handle_ble_scan_status(profile, decrypted.plaintext, decrypted.plaintext_len);
                        } else {
                            handle_wifi_scan_status(profile, decrypted.plaintext, decrypted.plaintext_len);
                        }
                    } else if(text_matches(decrypted.type, decrypted.type_len, "error")) {
                        handle_runtime_error(profile, decrypted.plaintext, decrypted.plaintext_len);
                    } else {
                        FURI_LOG_W(
                            TAG,
                            "Ignoring unrecognized protected record type '%.*s'",
                            (int)decrypted.type_len,
                            decrypted.type);
                    }
                    return BleEventAckFlowEnable;
                } else {
                    FURI_LOG_W(TAG, "Rejected record: unexpected field count %u", (unsigned)field_count);
                }
                return BleEventAckFlowEnable;
            } else {
                FURI_LOG_W(TAG, "Fragment reassembly rejected: status %d", status);
                return BleEventAckFlowEnable;
            }
        }
    }

    /* G12 fix: this dispatcher is invoked for every raw BLE event before Furi's own internal
       GAP handler gets a chance (furi_ble/event_dispatcher.c's ble_event_dispatcher_process_event()
       only falls through to ble_event_app_notification() -- gap.c's own handler -- once every
       registered service handler, including this one, has returned BleEventNotAck). That means
       this same ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE event that gap.c logs internally
       (GapEventTypeUpdateMTU, never exposed to app code via any public API) is also visible
       right here, letting the app learn the real negotiated MTU without needing one. Returns
       BleEventNotAck (not handled/consumed) so gap.c's own internal handling of this same event
       still runs afterward, unchanged. */
    if(event_packet->evt == HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE &&
       ble_event->ecode == ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE) {
        aci_att_exchange_mtu_resp_event_rp0* mtu_resp =
            (aci_att_exchange_mtu_resp_event_rp0*)ble_event->data;
        negotiated_att_mtu = mtu_resp->Server_RX_MTU;
        FURI_LOG_I(TAG, "negotiated ATT MTU: %u", (unsigned)negotiated_att_mtu);
    }

    return BleEventNotAck;
}

static FuriHalBleProfileBase* profile_start(FuriHalBleProfileParams params) {
    feb_reassembly_reset(&app_reassembly);
    pairing_reset_state();
    session_reset_state();
    /* G12 fix: reset back to the conservative default for a fresh profile activation, matching
       the ESP32's own negotiated_att_mtu reset-on-connect -- a stale large value from a
       previous connection must never carry over and be assumed valid before the new
       connection's own MTU exchange completes. */
    negotiated_att_mtu = FEB_DEFAULT_ATT_MTU;
    Esp32BleProfile* profile = malloc(sizeof(Esp32BleProfile));
    furi_check(profile);
    profile->base.config = &app_profile_callbacks;
    profile->app = params;
    profile->event_handler = ble_event_dispatcher_register_svc_handler(profile_event_handler, profile);
    if(!ble_gatt_service_add(UUID_TYPE_128, &service_uuid, PRIMARY_SERVICE, 6, &profile->service_handle)) {
        ble_event_dispatcher_unregister_svc_handler(profile->event_handler);
        free(profile);
        return NULL;
    }
    FURI_LOG_I(TAG, "GATT service registered: handle=%u", profile->service_handle);
    for(uint8_t index = 0; index < CharacteristicCount; index++) {
        ble_gatt_characteristic_init(
            profile->service_handle, &characteristics[index], &profile->characteristics[index]);
        FURI_LOG_I(
            TAG,
            "GATT characteristic '%s' registered: handle=%u descriptor_handle=%u",
            characteristics[index].name,
            profile->characteristics[index].handle,
            profile->characteristics[index].descriptor_handle);
    }
    return &profile->base;
}

static void profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == &app_profile_callbacks);
    Esp32BleProfile* profile = (Esp32BleProfile*)base;
    ble_event_dispatcher_unregister_svc_handler(profile->event_handler);
    for(uint8_t index = 0; index < CharacteristicCount; index++) {
        ble_gatt_characteristic_delete(profile->service_handle, &profile->characteristics[index]);
    }
    ble_gatt_service_delete(profile->service_handle);
    free(profile);
}

static const GapConfig profile_gap_config = {
    .adv_service = {.UUID_Type = UUID_TYPE_128, .Service_UUID_128 =
                        {0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31, 0x9e, 0xa2, 0x58, 0xa7,
                         0xa2, 0x0f, 0xb8, 0x11}},
    .appearance_char = 0x0000,
    .bonding_mode = false,
    .pairing_method = GapPairingNone,
    .conn_param = {.conn_int_min = 6, .conn_int_max = 36, .slave_latency = 0, .supervisor_timeout = 0}};

static void profile_get_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    memcpy(config, &profile_gap_config, sizeof(profile_gap_config));
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->adv_name[0] = '\0';
    config->adv_name[1] = '\0';
}

APP_DATA const FuriHalBleProfileTemplate app_profile_callbacks = {
    .start = profile_start, .stop = profile_stop, .get_gap_config = profile_get_gap_config};

APP_FN void bt_status_callback(BtStatus status, void* context) {
    Esp32App* app = context;
    FURI_LOG_I(TAG, "Bluetooth status: %d", status);
    /* static, not stack-local (found during the 2026-09-12 GPS-driver stack audit, measured
       via -fstack-usage): this callback runs on the "Bt" service's own thread
       (applications/services/bt/application.fam: stack_size=1024), a budget of the same
       order as BleEventWorker's, not this app's own. A stack-local AppEvent here measured
       480 bytes at that audit and had grown to 576 by the time capabilities stopped being
       added -- essentially half that thread's entire stack for one frame, before counting
       the Bt service's own dispatch call chain on top. The tagged-union rework (see
       AppEvent's own declaration) brought it to 104 bytes, which no longer forces the
       issue, but this stays static: matches every post_*() function's static
       convention elsewhere in this file, but deliberately its own instance rather than
       app_shared_ble_event (see that declaration's comment) -- the Bt thread can run
       concurrently with BleEventWorker/Timer/GuiSrv. Explicit reset below because a static
       initializer only runs once at load time, not per call. */
    static AppEvent event;
    memset(&event, 0, sizeof(event));
    event.type = AppEventBtStatus;
    event.u.bt_status.status = status;
    /* Snapshot app_protocol_generation now, under app_protocol_mutex -- the main loop compares this
       against the current value before ever resetting pairing/session state for this event
       (HP-07/G10, protocol_reset_if_unchanged()). */
    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    event.u.bt_status.generation = app_protocol_generation;
    furi_mutex_release(app_protocol_mutex);
    app_queue_put(app->queue, &event, APP_QUEUE_PUT_TIMEOUT_MS);
}

