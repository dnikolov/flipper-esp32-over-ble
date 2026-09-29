#include "app_internal.h"

#ifndef APP_UNITY_BUILD
#error "this module .c file is part of a unity build -- #include it from \n    flipper_esp32_over_ble.c, do not add it to application.fam sources directly \n    (APP_FN/APP_DATA in app_internal.h expand to 'static' and rely on being in \n    the same translation unit as every other module)"
#endif

/* app_shared_status_result: same single-in-flight BLE-thread reasoning as app_shared_ble_event just
   above, applied to the per-capability decode-scratch struct handle_gps_status() and
   handle_mesh_log_status() each declare for their own feb_cbor_decode_*_result_payload()
   call. A union rather than two single typed statics since these are different struct types
   -- each handler fully decodes into and drains its own member (posted onward as an AppEvent,
   or written to the mesh_log accumulator) before returning, and neither holds a pointer into
   it across a call boundary or into the other handler, so both can safely overlay the same
   storage. Sized to the larger member (mesh_log's, capped at
   FEB_MESH_LOG_MAX_RECORDS_PER_BATCH == 1 record per batch by wire-format contract, not gps's
   handful of scalar fields).

   wifi_scan/ble_scan/wardriving used to have their own members here too (up to 32 full
   records/APs/devices resident at once, ~2.8 KB combined -- H04's original "biggest remaining
   single win"), removed 2026-09-28 once their decoders gained a streaming, one-element-at-a-
   time entry point (feb_cbor_decode_wifi_scan_result_payload_stream()/_ble_scan_.../
   _wardriving_status_..., the cbor_*.h headers) that handle_wifi_scan_status()/
   handle_ble_scan_status()/handle_wardriving_status() now call instead -- see those handlers'
   own comments and wardriving_record_stream_cb()'s comment above handle_wardriving_status(). */

/* HP-08: every furi_message_queue_put() call in this file used to go unchecked, silently
   dropping events -- including AppEventSessionFatal/AppEventBtStatus -- whenever the 8-deep
   app.queue was transiently full. This wrapper counts and rate-limit-logs failures, and
   picks a real (bounded) timeout for callers that can afford to wait a little for the main
   thread to drain a slot: every caller here runs on its own dedicated FreeRTOS thread
   (BleEventWorker, the "Bt" service thread, GuiSrv) except gps_poll_timer_callback/
   publish_poll_timer_callback, which share the one FreeRTOS Timer Service task with every
   other furi_timer_alloc() callback in the whole firmware and must always pass timeout_ms=0
   -- blocking that thread would stall every other app's timers too. */
static uint32_t app_queue_put_failures;

APP_FN bool app_queue_put(FuriMessageQueue* queue, const AppEvent* event, uint32_t timeout_ms) {
    FuriStatus status =
        furi_message_queue_put(queue, event, timeout_ms ? furi_ms_to_ticks(timeout_ms) : 0);
    if(status != FuriStatusOk) {
        app_queue_put_failures++;
        if(app_queue_put_failures == 1 || (app_queue_put_failures % 50) == 0) {
            FURI_LOG_E(
                TAG,
                "app queue put failed: event type %d, status %d, total drops %lu",
                (int)event->type,
                (int)status,
                (unsigned long)app_queue_put_failures);
        }
        return false;
    }
    return true;
}

#define APP_QUEUE_PUT_TIMEOUT_MS 20u

APP_FN void post_pairing_phase(Esp32App* app, PairingPhase phase, const char* reason) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventPairingPhase;
    event->u.pairing.phase = phase;
    if(reason) {
        strncpy(event->u.pairing.reason, reason, sizeof(event->u.pairing.reason) - 1);
        event->u.pairing.reason[sizeof(event->u.pairing.reason) - 1] = '\0';
    } else {
        event->u.pairing.reason[0] = '\0';
    }
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

APP_FN void post_session_fatal(Esp32App* app) {
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventSessionFatal;
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* Caller must already hold app_protocol_mutex; does not bump app_protocol_generation itself (callers
   that need the bump -- i.e. every caller except protocol_reset_if_unchanged(), which bumps
   once for both resets together -- do it themselves). */
static void pairing_reset_state_locked(void) {
    app_pair_stage = PairStageNone;
    feb_secure_zero(app_pair_board_id, sizeof(app_pair_board_id));
    app_pair_board_id_len = 0;
    feb_secure_zero(app_pair_transcript, sizeof(app_pair_transcript));
    app_pair_transcript_len = 0;
    feb_secure_zero(app_pair_k_confirm, sizeof(app_pair_k_confirm));
    feb_secure_zero(app_pair_secret, sizeof(app_pair_secret));
}

APP_FN void pairing_reset_state(void) {
    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    pairing_reset_state_locked();
    app_protocol_generation++;
    furi_mutex_release(app_protocol_mutex);
}

static void send_pairing_wire_error(Esp32BleProfile* profile, feb_pairing_error_t err) {
    const char* code = feb_pairing_error_code_str(err);
    static feb_error_payload_t error_payload;
    error_payload = (feb_error_payload_t){
        .code = code,
        .code_len = strlen(code),
        .has_message = 0,
        .has_request_id = 0,
    };
    size_t payload_len =
        feb_cbor_encode_error_payload(app_pairing_payload_buf, sizeof(app_pairing_payload_buf), &error_payload);
    if(payload_len == 0) {
        return;
    }
    static feb_pairing_envelope_t envelope;
    envelope = (feb_pairing_envelope_t){
        .version = 2,
        .type = "error",
        .type_len = sizeof("error") - 1,
        .board_id = app_pair_board_id,
        .board_id_len = app_pair_board_id_len,
        .payload_span = app_pairing_payload_buf,
        .payload_span_len = payload_len,
    };
    size_t record_len =
        feb_cbor_encode_pairing_envelope(app_pairing_record_buf, sizeof(app_pairing_record_buf), &envelope);
    if(record_len == 0) {
        return;
    }
    send_pairing_record(profile, app_pairing_record_buf, record_len);
}

/* Ends the in-progress ceremony attempt: zeroizes ephemeral secrets, resets the LED to
   off, and reports the failure phase to the UI. `send_wire_error` is false for a raw
   decode/board_id failure where we cannot trust enough of the record to safely echo a
   board_id-bearing reply (docs/PLAN.md step 5 board_id validation gap). */
static void abort_pairing(Esp32BleProfile* profile, const char* reason, bool send_wire_error) {
    Esp32App* app = profile->app;
    if(send_wire_error && app_pair_board_id_len > 0) {
        send_pairing_wire_error(profile, FEB_PAIRING_ERR_FAILED);
    }
    pairing_reset_state();
    if(app->notifications) {
        /* sequence_reset_blue only touches the static RGB channel; an active
           sequence_blink_start_blue runs on the separate hardware LED-blink
           subsystem and keeps blinking until explicitly stopped. */
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_reset_blue);
    }
    post_pairing_phase(app, PairingPhaseFailed, reason);
    FURI_LOG_W(TAG, "Pairing aborted: %s", reason);
}

APP_FN void handle_pair_init(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope) {
    Esp32App* app = profile->app;
    /* board_id is already charset/length-validated by the caller (profile_event_handler)
       before dispatch, so it is copied in first -- a payload decode failure below still
       has a known-good board_id to echo back in a pairing_failed reply. */
    pairing_reset_state();
    memcpy(app_pair_board_id, envelope->board_id, envelope->board_id_len);
    app_pair_board_id[envelope->board_id_len] = '\0';
    app_pair_board_id_len = envelope->board_id_len;

    static feb_pair_init_payload_t init_payload;
    feb_cbor_status_t status = feb_cbor_decode_pair_init_payload(
        envelope->payload_span, envelope->payload_span_len, &init_payload);
    if(status != FEB_CBOR_OK) {
        abort_pairing(profile, "malformed pair_init", true);
        return;
    }

    static uint8_t flipper_private[FEB_X25519_KEY_LEN];
    static uint8_t flipper_public[FEB_X25519_KEY_LEN];
    static uint8_t client_nonce[FEB_PAIRING_NONCE_LEN];
    static uint8_t k_shared[FEB_PAIRING_KSHARED_LEN];

    furi_hal_random_fill_buf(flipper_private, sizeof(flipper_private));
    furi_hal_random_fill_buf(client_nonce, sizeof(client_nonce));
    feb_x25519_base(flipper_public, flipper_private);
    feb_x25519(k_shared, flipper_private, init_payload.esp32_public_key);
    feb_secure_zero(flipper_private, sizeof(flipper_private));

    if(feb_is_all_zero(k_shared, sizeof(k_shared))) {
        feb_secure_zero(k_shared, sizeof(k_shared));
        abort_pairing(profile, "invalid shared secret", true);
        return;
    }

    static feb_pairing_transcript_t transcript;
    memset(&transcript, 0, sizeof(transcript));
    transcript.version = 2;
    memcpy(transcript.service_uuid, FEB_PAIRING_SERVICE_UUID, FEB_PAIRING_SERVICE_UUID_LEN);
    transcript.board_id = app_pair_board_id;
    transcript.board_id_len = app_pair_board_id_len;
    memcpy(transcript.pairing_epoch, init_payload.pairing_epoch, FEB_PAIRING_EPOCH_LEN);
    memcpy(transcript.client_nonce, client_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(transcript.device_nonce, init_payload.device_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(transcript.esp32_public_key, init_payload.esp32_public_key, FEB_PAIRING_PUBKEY_LEN);
    memcpy(transcript.flipper_public_key, flipper_public, FEB_PAIRING_PUBKEY_LEN);

    app_pair_transcript_len =
        feb_pairing_encode_transcript(app_pair_transcript, sizeof(app_pair_transcript), &transcript);
    if(app_pair_transcript_len == 0) {
        feb_secure_zero(k_shared, sizeof(k_shared));
        abort_pairing(profile, "transcript encode failed", true);
        return;
    }

    feb_pairing_derive_kconfirm(k_shared, init_payload.pairing_epoch, app_pair_k_confirm);
    feb_pairing_derive_secret(
        k_shared,
        init_payload.pairing_epoch,
        client_nonce,
        init_payload.device_nonce,
        app_pair_board_id,
        app_pair_board_id_len,
        app_pair_secret);
    feb_secure_zero(k_shared, sizeof(k_shared));

    static uint8_t flipper_confirm[FEB_PAIRING_REPLY_CONFIRM_LEN];
    feb_pairing_flipper_confirm(app_pair_k_confirm, app_pair_transcript, app_pair_transcript_len, flipper_confirm);

    static feb_pair_reply_payload_t reply_payload;
    memcpy(reply_payload.client_nonce, client_nonce, FEB_PAIRING_NONCE_LEN);
    memcpy(reply_payload.flipper_public_key, flipper_public, FEB_PAIRING_PUBKEY_LEN);
    memcpy(reply_payload.confirmation, flipper_confirm, FEB_PAIRING_REPLY_CONFIRM_LEN);

    size_t payload_len = feb_cbor_encode_pair_reply_payload(
        app_pairing_payload_buf, sizeof(app_pairing_payload_buf), &reply_payload);
    if(payload_len == 0) {
        abort_pairing(profile, "pair_reply encode failed", true);
        return;
    }

    static feb_pairing_envelope_t reply_envelope;
    reply_envelope = (feb_pairing_envelope_t){
        .version = 2,
        .type = FEB_PAIR_REPLY_TYPE,
        .type_len = sizeof(FEB_PAIR_REPLY_TYPE) - 1,
        .board_id = app_pair_board_id,
        .board_id_len = app_pair_board_id_len,
        .payload_span = app_pairing_payload_buf,
        .payload_span_len = payload_len,
    };
    size_t record_len = feb_cbor_encode_pairing_envelope(
        app_pairing_record_buf, sizeof(app_pairing_record_buf), &reply_envelope);
    if(record_len == 0) {
        abort_pairing(profile, "pair_reply record encode failed", true);
        return;
    }

    if(!send_pairing_record(profile, app_pairing_record_buf, record_len)) {
        abort_pairing(profile, "pair_reply send failed", false);
        return;
    }

    app_pair_stage = PairStageInitReceived;
    post_pairing_phase(app, PairingPhaseConfirming, NULL);
}

APP_FN void handle_pair_confirm(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope) {
    if(app_pair_stage != PairStageInitReceived) {
        FURI_LOG_W(TAG, "Ignoring unexpected pair_confirm");
        return;
    }
    if(envelope->board_id_len != app_pair_board_id_len ||
       memcmp(envelope->board_id, app_pair_board_id, app_pair_board_id_len) != 0) {
        abort_pairing(profile, "board_id mismatch", true);
        return;
    }

    static feb_pair_confirm_payload_t confirm_payload;
    feb_cbor_status_t status = feb_cbor_decode_pair_confirm_payload(
        envelope->payload_span, envelope->payload_span_len, &confirm_payload);
    if(status != FEB_CBOR_OK) {
        abort_pairing(profile, "malformed pair_confirm", true);
        return;
    }

    static uint8_t expected[FEB_PAIRING_REPLY_CONFIRM_LEN];
    feb_pairing_esp32_confirm(app_pair_k_confirm, app_pair_transcript, app_pair_transcript_len, expected);
    bool match =
        feb_consttime_equal(expected, confirm_payload.confirmation, FEB_PAIRING_REPLY_CONFIRM_LEN) != 0;
    feb_secure_zero(expected, sizeof(expected));
    if(!match) {
        abort_pairing(profile, "confirm mismatch", true);
        return;
    }

    app_pair_stage = PairStageConfirmReceived;
    post_pairing_phase(profile->app, PairingPhaseSaving, NULL);
}

/* G08 (docs/archive/grok-4.6-findings-2026-09-11.md "### G08"): pairing_storage_save()'s
   temp-file/write/sync/rename dance used to run synchronously inside handle_pair_complete()
   below, on BleEventWorker (the thread that pumps BLE). This hand-off buffer is the only copy
   of the confirmed pairing secret outside app_pair_secret itself (which pairing_reset_state()
   zeroizes in place immediately after the copy below) -- guarded by app_protocol_mutex
   (already the general pairing/session cross-thread lock, see its own top-of-file comment),
   not the flat AppEvent union: AppEventPairingSaveRequest carries no payload precisely so this
   secret never has to sit inside the promiscuously-reused app_shared_ble_event. Zeroized on
   every path: by pairing_storage_save_pending() once the save attempt (success or failure) is
   done, and right here in handle_pair_complete() if the hand-off can't even be queued
   (fail-closed, matching this project's existing zeroize-on-every-path rule). */
static uint8_t pairing_save_pending_secret[FEB_PAIRING_SECRET_LEN];
static char pairing_save_pending_board_id[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
static size_t pairing_save_pending_board_id_len;
static bool pairing_save_pending_ready;

/* Runs on the main thread only: the AppEventPairingSaveRequest branch in
   flipper_esp32_over_ble.c's event loop, and that function's shutdown drain (a request
   queued right before the user exits must still be serviced, not silently discarded when the
   queue is freed). Reproduces handle_pair_complete()'s former synchronous save-then-decide-
   phase logic unchanged, just off BleEventWorker. A no-op if nothing is pending (defensive;
   this is only ever posted once per completed ceremony). */
APP_FN void pairing_storage_save_pending(Esp32App* app) {
    uint8_t secret[FEB_PAIRING_SECRET_LEN];
    char board_id[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
    size_t board_id_len;
    bool ready;

    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    ready = pairing_save_pending_ready;
    if(ready) {
        memcpy(secret, pairing_save_pending_secret, FEB_PAIRING_SECRET_LEN);
        memcpy(board_id, pairing_save_pending_board_id, sizeof(board_id));
        board_id_len = pairing_save_pending_board_id_len;
        pairing_save_pending_ready = false;
        feb_secure_zero(pairing_save_pending_secret, sizeof(pairing_save_pending_secret));
    }
    furi_mutex_release(app_protocol_mutex);
    if(!ready) {
        return;
    }

    bool saved = pairing_storage_save(app->storage, board_id, board_id_len, secret);
    feb_secure_zero(secret, sizeof(secret));

    if(!saved) {
        if(app->notifications) {
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_reset_blue);
        }
        post_pairing_phase(app, PairingPhaseFailed, "storage write failed");
        FURI_LOG_E(TAG, "Failed to persist pairing secret for board '%s'", board_id);
        return;
    }

    if(app->notifications) {
        /* Must stop the running blink sequence explicitly - it's a separate
           hardware LED-blink subsystem that a plain RGB message can't override. */
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_set_only_blue_255);
    }
    post_pairing_phase(app, PairingPhaseDone, NULL);
    FURI_LOG_I(TAG, "Paired with board '%s'", board_id);
}

APP_FN void handle_pair_complete(Esp32BleProfile* profile, const feb_pairing_envelope_t* envelope) {
    Esp32App* app = profile->app;
    if(app_pair_stage != PairStageConfirmReceived) {
        FURI_LOG_W(TAG, "Ignoring unexpected pair_complete");
        return;
    }
    if(envelope->board_id_len != app_pair_board_id_len ||
       memcmp(envelope->board_id, app_pair_board_id, app_pair_board_id_len) != 0) {
        abort_pairing(profile, "board_id mismatch", true);
        return;
    }

    static feb_pair_complete_payload_t complete_payload;
    feb_cbor_status_t status = feb_cbor_decode_pair_complete_payload(
        envelope->payload_span, envelope->payload_span_len, &complete_payload);
    if(status != FEB_CBOR_OK) {
        abort_pairing(profile, "malformed pair_complete", true);
        return;
    }

    static uint8_t expected[FEB_PAIRING_COMPLETE_TAG_LEN];
    feb_pairing_complete_tag(app_pair_k_confirm, app_pair_transcript, app_pair_transcript_len, expected);
    bool match =
        feb_consttime_equal(expected, complete_payload.confirmation, FEB_PAIRING_COMPLETE_TAG_LEN) != 0;
    feb_secure_zero(expected, sizeof(expected));
    if(!match) {
        abort_pairing(profile, "complete tag mismatch", true);
        return;
    }

    static char board_id_copy[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
    memcpy(board_id_copy, app_pair_board_id, app_pair_board_id_len);
    board_id_copy[app_pair_board_id_len] = '\0';

    /* G08: hand the actual SD write off to the main thread instead of blocking BleEventWorker
       on it -- see pairing_save_pending_secret's own declaration comment above. Must copy
       BEFORE pairing_reset_state() zeroizes app_pair_secret in place. */
    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    memcpy(pairing_save_pending_secret, app_pair_secret, FEB_PAIRING_SECRET_LEN);
    memcpy(pairing_save_pending_board_id, app_pair_board_id, app_pair_board_id_len);
    pairing_save_pending_board_id[app_pair_board_id_len] = '\0';
    pairing_save_pending_board_id_len = app_pair_board_id_len;
    pairing_save_pending_ready = true;
    furi_mutex_release(app_protocol_mutex);

    pairing_reset_state();

    AppEvent* save_event = &app_shared_ble_event;
    memset(save_event, 0, sizeof(*save_event));
    save_event->type = AppEventPairingSaveRequest;
    bool queued = app_queue_put(app->queue, save_event, APP_QUEUE_PUT_TIMEOUT_MS);
    if(!queued) {
        /* Fail closed: the main thread will never see this request, so there is no later
           point where pairing_save_pending_secret gets zeroized -- do it here instead. */
        furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
        feb_secure_zero(pairing_save_pending_secret, sizeof(pairing_save_pending_secret));
        pairing_save_pending_ready = false;
        furi_mutex_release(app_protocol_mutex);
        if(app->notifications) {
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_reset_blue);
        }
        post_pairing_phase(app, PairingPhaseFailed, "storage write failed");
        FURI_LOG_E(TAG, "Failed to queue pairing secret save for board '%s'", board_id_copy);
        return;
    }
    FURI_LOG_I(TAG, "Pair complete for board '%s'; save queued", board_id_copy);
}

/* ---- runtime session establishment (docs/PLAN.md step 6): hello/hello_ack/client_auth,
   single BLE connection, single-threaded BLE event dispatch (see profile_event_handler),
   so static storage for the one in-flight session attempt is safe and keeps these off the
   ~1280-byte BleEventWorker stack -- same rationale as the pairing-ceremony statics above.
   app_session_key is the one exception that survives past this handshake: it must stay alive
   for as long as the authenticated session does (step 7 will use it to
   encrypt/decrypt protected records), so it is only zeroized on disconnect/session end,
   never immediately after derivation. SessionStage itself is declared in app_internal.h
   (ble_transport.c's profile_event_handler also needs it). ---- */
/* docs/PLAN.md step 7: per-direction protected-record sequence counters, each beginning at
   1 for the session (docs/PROTOCOL.md#cryptographic-requirements). Not secret, so plain
   reset (not feb_secure_zero) is fine. */

/* docs/PROTOCOL.md: "must never wrap. A new BLE session is required before 2^24 - 1
   protected records are sent." feb_session_build_nonce() truncates sequence to its low
   24 bits and does not itself enforce this cap (session.h) -- both directions of a single
   session must stay strictly below this value or the AES-GCM nonce repeats. */
#define FEB_SESSION_SEQUENCE_MAX 0xFFFFFFu

/* feb_session_decrypt_record()'s plaintext output; its contract requires capacity >=
   FEB_CBOR_MAX_PAYLOAD (session.h) -- not shrunk to "today's actual capability_response
   size" on purpose, matching the project's own 256-vs-512 lesson (docs/PLAN.md step 3
   backlog) about payload buffers silently rejecting a legitimate larger record later. */
/* capability_query's own plaintext payload reuses app_pairing_payload_buf (256 bytes, well
   above the few real bytes an always-empty-map `requested`-omitted encoding needs,
   docs/PLAN.md step 7) rather than a dedicated array -- both are BLE-thread-only
   (capability_bootstrap() runs from handle_client_auth(), synchronously inside
   profile_event_handler, same as the pairing ceremony functions that also use
   app_pairing_payload_buf) and never overlap in time. Only the GCM ciphertext scratch still
   needs its own buffer (cannot alias the plaintext it's encrypting from). */
#define FEB_CAPABILITY_QUERY_PAYLOAD_MAX_LEN 16u
static uint8_t capability_query_ciphertext_buf[FEB_CAPABILITY_QUERY_PAYLOAD_MAX_LEN];

/* ---- wifi_scan capability (docs/PLAN.md's Wi-Fi scan capability follow-on step) ----
   Unlike every other outbound protected record in this file (sent synchronously from
   inside a BLE-thread callback, in direct response to an incoming record), the `command`
   that triggers a scan is sent from this app's own main thread, in direct response to a
   user OK-press on the results screen -- there is no incoming BLE event to key it off of.
   app_cmd_payload_buf/app_cmd_ciphertext_buf/app_cmd_record_buf below are kept separate from
   app_pairing_record_buf/capability_query_*_buf (which remain BLE-thread-only) to avoid any
   aliasing between the two independent senders, even though in practice they cannot run
   concurrently: the "Scan now" action is gated on app.capability_has_wifi_scan, which can
   only become true after capability_bootstrap()'s own send (if any, on the BLE thread) has
   already returned and its response has been processed -- see send_wifi_scan_command()'s
   own comment below for the full argument.

   One shared triple, not one set per capability: wifi_scan/ble_scan/gps/wardriving's command
   sends (send_wifi_scan_command/send_ble_scan_command/send_gps_command/
   send_wardriving_start_command/send_wardriving_status_query/send_wardriving_stop_command,
   all further below) run only on this app's own main thread, each a single synchronous
   encode-encrypt-send call with no state retained in these buffers across calls, and
   app_pending_command_kind's own one-command-in-flight convention (see its declaration below)
   already establishes only one of these can be in progress at a time -- the same
   single-in-flight reasoning this file already applies to app_shared_ble_event above, just on
   the main thread instead of BleEventWorker. Must fit the full *wrapped* command envelope
   (map(1) + "capability" key(1+10) + "wardriving" value(1+10) + "request_id" key(1+10) +
   uint value(1-9) + "arguments" key(1+9) + the arguments map embedded inline, not as a
   separate bstr) for every capability, not just each capability's own arguments sub-buffer
   -- wardriving's arguments alone (`FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN`, its own send_
   wardriving_start_command() comment further below) grew to ~150 bytes worst case
   (wifi_swelling/country added 2026-09-21, docs/WARDRIVING_REDESIGN.md) atop ~30-53 bytes of
   this envelope's own overhead == ~180-203 bytes total for a WiFi+BLE wardriving start,
   found (2026-09-21, hardware-verified) to exceed this buffer's previous 160u sizing:
   send_wardriving_start_command()'s outer feb_cbor_encode_command_payload() call silently
   returned 0 (out_cap exhausted) for the WiFi+BLE case specifically (WiFi-only's smaller
   arguments still fit, masking the bug until BLE was added to the mix) -- the same failure
   class, and the same "buffer sized for one capability's old worst case, not rechecked once
   another capability's own sub-buffer grew" root cause, as the original 2026-09-07 wifi_scan
   sizing bug this comment used to describe alone (send_wifi_scan_command() failed 100% of
   the time, OK-press never reached the ESP32, before that fix). Sized with real margin now,
   not shaved to the byte. */
#define FEB_CMD_PAYLOAD_MAX_LEN 224u


/* Solid-green-while-flushing / solid-blue-when-idle LED indicator for an active wardriving
   backlog flush (docs/PROTOCOL.md's backlog_remaining semantics) -- see
   handle_wardriving_status()'s "data" branch, further below, for both transition points.
   Declared here (rather than grouped with the other wardriving-status statics further down,
   next to wardriving_csv_write_failed) because session_reset_state(), which must clear it,
   is defined earlier in this file than that group. */

/* Caller must already hold app_protocol_mutex; see pairing_reset_state_locked()'s own comment
   for why this split exists and who bumps app_protocol_generation. */
static void session_reset_state_locked(void) {
    app_session_stage = SessionStageNone;
    feb_secure_zero(app_session_board_id, sizeof(app_session_board_id));
    app_session_board_id_len = 0;
    feb_secure_zero(app_session_id_bytes, sizeof(app_session_id_bytes));
    feb_secure_zero(app_session_client_nonce, sizeof(app_session_client_nonce));
    feb_secure_zero(app_session_device_nonce, sizeof(app_session_device_nonce));
    feb_secure_zero(app_session_transcript_buf, sizeof(app_session_transcript_buf));
    app_session_transcript_len = 0;
    feb_secure_zero(app_session_pairing_secret, sizeof(app_session_pairing_secret));
    feb_secure_zero(app_session_key, sizeof(app_session_key));
    app_session_seq_out = 0;
    app_session_seq_in = 0;
    app_wardriving_flush_led_active = false;
}

APP_FN void session_reset_state(void) {
    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    session_reset_state_locked();
    app_protocol_generation++;
    furi_mutex_release(app_protocol_mutex);
}

/* HP-07/G10: atomically resets pairing+session ceremony state only if `generation` (captured
   by bt_status_callback at the moment its BtStatus event was posted) still matches the
   current app_protocol_generation -- i.e. only if nothing on BleEventWorker has advanced the
   ceremony since. See app_protocol_mutex's own top-of-file comment for the full race argument.
   Also resets fragment reassembly (HP-24): a stale partial fragment from a previous
   connection must not survive into a new one, and gating it on the same check means it is
   never wiped out from under an in-flight reassembly either. Returns true if the reset was
   actually performed (informational only; no caller currently needs this). */
APP_FN bool protocol_reset_if_unchanged(uint32_t generation) {
    bool performed = false;
    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    if(generation == app_protocol_generation) {
        pairing_reset_state_locked();
        session_reset_state_locked();
        furi_mutex_acquire(app_reassembly_mutex, FuriWaitForever);
        feb_reassembly_reset(&app_reassembly);
        furi_mutex_release(app_reassembly_mutex);
        app_protocol_generation++;
        performed = true;
    }
    furi_mutex_release(app_protocol_mutex);
    return performed;
}

/* Bumps app_protocol_generation alone -- called as the very first action of a BleEventWorker
   protocol-state mutation (pairing/session envelope dispatch, protected-record decrypt),
   strictly before any of that mutation's own unlocked field writes begin. See app_protocol_mutex's
   top comment for why this ordering is what makes protocol_reset_if_unchanged() race-free
   without wrapping entire multi-return handler functions in the mutex. */
APP_FN void protocol_generation_bump(void) {
    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    app_protocol_generation++;
    furi_mutex_release(app_protocol_mutex);
}

/* docs/PROTOCOL.md's "Runtime auth failure handling": unknown_board replies use the
   pairing-record wire envelope (no session_id), since no session_id is trusted to exist
   yet -- deliberately distinct from feb_pairing_error_t (pairing.h's frozen contract),
   which has no unknown_board member; this is a session.h-domain error code, sent with the
   same envelope shape as a pairing-phase error rather than a new struct. Reuses this
   file's existing app_pairing_payload_buf/app_pairing_record_buf scratch buffers (already sized
   for this), matching send_pairing_wire_error()'s pattern. */
static void
    send_unknown_board_error(Esp32BleProfile* profile, const char* board_id, size_t board_id_len) {
    static feb_error_payload_t error_payload;
    error_payload = (feb_error_payload_t){
        .code = "unknown_board",
        .code_len = sizeof("unknown_board") - 1,
        .has_message = 0,
        .has_request_id = 0,
    };
    size_t payload_len =
        feb_cbor_encode_error_payload(app_pairing_payload_buf, sizeof(app_pairing_payload_buf), &error_payload);
    if(payload_len == 0) {
        return;
    }
    static feb_pairing_envelope_t envelope;
    envelope = (feb_pairing_envelope_t){
        .version = 2,
        .type = "error",
        .type_len = sizeof("error") - 1,
        .board_id = board_id,
        .board_id_len = board_id_len,
        .payload_span = app_pairing_payload_buf,
        .payload_span_len = payload_len,
    };
    size_t record_len =
        feb_cbor_encode_pairing_envelope(app_pairing_record_buf, sizeof(app_pairing_record_buf), &envelope);
    if(record_len == 0) {
        return;
    }
    send_pairing_record(profile, app_pairing_record_buf, record_len);
}

/* `hello` carries the ESP32's freshly-generated session_id + client_nonce and its stable
   board_id. A malformed hello payload is dropped silently (no reply) rather than answered
   with a wire error -- docs/PROTOCOL.md defines a reply only for the unknown_board case
   specifically; this project's established convention for anything else pre-authentication
   is to not hand an unauthenticated peer a reason, matching the client_auth-failure
   no-reply rule below. */
APP_FN void handle_hello(Esp32BleProfile* profile, const feb_unencrypted_record_t* envelope) {
    Esp32App* app = profile->app;
    if(!board_id_is_valid(envelope->board_id, envelope->board_id_len)) {
        FURI_LOG_W(TAG, "Rejected hello: invalid board_id");
        return;
    }

    session_reset_state();
    /* BL29 fix: session_reset_state() just cleared app_wardriving_flush_led_active (a fresh
       hello always supersedes whatever session/flush state existed before), but -- unlike
       every OTHER call site that clears this flag (BtStatusConnected/Advertising's own
       reset_scan_ui_state_keep_screen(), stop_service(), start_profile(), AppEventSessionFatal,
       and handle_client_auth()'s own success path) -- nothing here was pairing that flag clear
       with an actual physical-LED reset. If the ESP32 ever re-sends `hello` on a connection
       that was already authenticated with an active green backlog-flush LED, and this fresh
       hello then fails without the connection ever fully disconnecting (the two failure
       returns just below), the LED was left stuck green with no other path left to fix it --
       every one of this project's other reset paths is followed by a disconnect (which
       reset_scan_ui_state_impl() already handles) or, on success, handle_client_auth()'s own
       explicit blue-set. Matches those same two notification_message() calls; harmless/
       idempotent on the far more common case where the LED is already blue. */
    if(app->notifications) {
        notification_message(app->notifications, &sequence_blink_stop);
        notification_message(app->notifications, &sequence_set_only_blue_255);
    }
    memcpy(app_session_board_id, envelope->board_id, envelope->board_id_len);
    app_session_board_id[envelope->board_id_len] = '\0';
    app_session_board_id_len = envelope->board_id_len;
    memcpy(app_session_id_bytes, envelope->session_id, FEB_SESSION_ID_LEN);

    static feb_hello_payload_t hello_payload;
    feb_cbor_status_t status = feb_cbor_decode_hello_payload(
        envelope->payload_span, envelope->payload_span_len, &hello_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "Rejected hello: malformed payload");
        session_reset_state();
        return;
    }
    memcpy(app_session_client_nonce, hello_payload.client_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    if(!pairing_storage_load(app->storage, app_session_board_id, app_session_board_id_len, app_session_pairing_secret)) {
        FURI_LOG_W(TAG, "hello for unknown board '%s'", app_session_board_id);
        send_unknown_board_error(profile, app_session_board_id, app_session_board_id_len);
        session_reset_state();
        return;
    }

    furi_hal_random_fill_buf(app_session_device_nonce, sizeof(app_session_device_nonce));

    static feb_session_transcript_t transcript;
    memset(&transcript, 0, sizeof(transcript));
    transcript.version = 2;
    transcript.board_id = app_session_board_id;
    transcript.board_id_len = app_session_board_id_len;
    memcpy(transcript.session_id, app_session_id_bytes, FEB_SESSION_ID_LEN);
    memcpy(transcript.client_nonce, app_session_client_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(transcript.device_nonce, app_session_device_nonce, FEB_SESSION_NONCE_FIELD_LEN);

    app_session_transcript_len =
        feb_session_encode_transcript(app_session_transcript_buf, sizeof(app_session_transcript_buf), &transcript);
    if(app_session_transcript_len == 0) {
        FURI_LOG_W(TAG, "hello: transcript encode failed");
        session_reset_state();
        return;
    }

    static uint8_t flipper_proof[FEB_SESSION_PROOF_LEN];
    feb_session_flipper_proof(
        app_session_pairing_secret, app_session_transcript_buf, app_session_transcript_len, flipper_proof);

    static feb_hello_ack_payload_t ack_payload;
    memcpy(ack_payload.device_nonce, app_session_device_nonce, FEB_SESSION_NONCE_FIELD_LEN);
    memcpy(ack_payload.proof, flipper_proof, FEB_SESSION_PROOF_LEN);

    size_t payload_len =
        feb_cbor_encode_hello_ack_payload(app_pairing_payload_buf, sizeof(app_pairing_payload_buf), &ack_payload);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "hello_ack: payload encode failed");
        session_reset_state();
        return;
    }

    static feb_unencrypted_record_t ack_record;
    memset(&ack_record, 0, sizeof(ack_record));
    ack_record.version = 2;
    ack_record.type = FEB_HELLO_ACK_TYPE;
    ack_record.type_len = sizeof(FEB_HELLO_ACK_TYPE) - 1;
    memcpy(ack_record.session_id, app_session_id_bytes, FEB_SESSION_ID_LEN);
    ack_record.board_id = app_session_board_id;
    ack_record.board_id_len = app_session_board_id_len;
    ack_record.payload_span = app_pairing_payload_buf;
    ack_record.payload_span_len = payload_len;

    size_t record_len =
        feb_cbor_encode_unencrypted(app_pairing_record_buf, sizeof(app_pairing_record_buf), &ack_record);
    if(record_len == 0) {
        FURI_LOG_W(TAG, "hello_ack: record encode failed");
        session_reset_state();
        return;
    }

    if(!send_pairing_record(profile, app_pairing_record_buf, record_len)) {
        FURI_LOG_W(TAG, "hello_ack: send failed");
        session_reset_state();
        return;
    }

    app_session_stage = SessionStageHelloReceived;
    post_pairing_phase(app, PairingPhaseAuthenticating, NULL);
}

/* ---- board identity / capability registry (docs/PLAN.md step 7) ----
   Fires automatically the moment runtime auth succeeds (handle_client_auth() below), no UI
   gesture. `app_session_plaintext_buf`/`capability_query_ciphertext_buf` are declared with this
   file's other session statics above (capability_query's plaintext payload itself reuses
   app_pairing_payload_buf, see that declaration's comment); safe as static for the same
   single-in-flight-BLE-event-dispatch reason. */

static void format_capability_display(
    const feb_capability_response_payload_t* payload,
    char* board_out,
    size_t board_out_cap,
    char* features_out,
    size_t features_out_cap) {
    size_t board_len = payload->board_len;
    if(board_len > board_out_cap - 1) {
        board_len = board_out_cap - 1;
    }
    memcpy(board_out, payload->board, board_len);
    board_out[board_len] = '\0';

    size_t pos = 0;
    for(size_t i = 0; i < payload->feature_count && pos < features_out_cap - 1; i++) {
        if(i > 0 && pos < features_out_cap - 1) {
            features_out[pos++] = ',';
        }
        size_t copy_len = payload->feature_lens[i];
        if(copy_len > features_out_cap - 1 - pos) {
            copy_len = features_out_cap - 1 - pos;
        }
        memcpy(features_out + pos, payload->features[i], copy_len);
        pos += copy_len;
    }
    features_out[pos] = '\0';
}

static bool capability_has_feature(const feb_capability_response_payload_t* payload, const char* feature) {
    size_t feature_len = strlen(feature);
    for(size_t i = 0; i < payload->feature_count; i++) {
        if(payload->feature_lens[i] == feature_len &&
           memcmp(payload->features[i], feature, feature_len) == 0) {
            return true;
        }
    }
    return false;
}

static void post_capability_info(Esp32App* app, const feb_capability_response_payload_t* payload) {
    /* app_shared_ble_event -- see post_pairing_phase()'s comment above; same rationale and same
       reset-every-call requirement apply here. */
    AppEvent* event = &app_shared_ble_event;
    memset(event, 0, sizeof(*event));
    event->type = AppEventCapabilityInfo;
    format_capability_display(
        payload,
        event->u.capability.board,
        sizeof(event->u.capability.board),
        event->u.capability.features,
        sizeof(event->u.capability.features));
    event->u.capability.has_wifi_scan = capability_has_feature(payload, "wifi_scan");
    event->u.capability.has_ble_scan = capability_has_feature(payload, "ble_scan");
    event->u.capability.has_wardriving = capability_has_feature(payload, "wardriving");
    event->u.capability.has_gps = capability_has_feature(payload, "gps");
    event->u.capability.has_meshcore_scan = capability_has_feature(payload, "meshcore_scan");
    event->u.capability.has_mesh_log = capability_has_feature(payload, "mesh_log");
    app_queue_put(app->queue, event, APP_QUEUE_PUT_TIMEOUT_MS);
}

/* G08: capability_storage_save()'s temp-file write used to run synchronously in
   handle_capability_response() below, on BleEventWorker. `plaintext`/`plaintext_len` there
   alias app_session_plaintext_buf, valid only until the next BLE event dispatches -- deep-
   copied here into this dedicated hand-off buffer (not just the pointer, and not
   app_session_plaintext_buf itself, which the very next protected record could overwrite
   before the main thread reads this one). Not secret material, so no zeroize requirement --
   unlike pairing_save_pending_secret above, this only needs the ordinary copy-before-alias-
   changes rule. Guarded by app_protocol_mutex (same general protocol hand-off lock). This
   piggybacks on the pre-existing AppEventCapabilityInfo post rather than adding a second
   event: capability_storage_save_pending() is a no-op whenever pending_ready is false, which
   is exactly the case for capability_bootstrap()'s own cache-load path (that path posts the
   same event type to display an already-cached record, with nothing new to save). */
static uint8_t capability_save_pending_buf[FEB_CBOR_MAX_PAYLOAD];
static size_t capability_save_pending_len;
static char capability_save_pending_board_id[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
static size_t capability_save_pending_board_id_len;
static bool capability_save_pending_ready;

/* Runs on the main thread only: the AppEventCapabilityInfo branch in
   flipper_esp32_over_ble.c's event loop, and that function's shutdown drain. */
APP_FN void capability_storage_save_pending(Esp32App* app) {
    uint8_t buf[FEB_CBOR_MAX_PAYLOAD];
    size_t len = 0;
    char board_id[FEB_PAIRING_BOARD_ID_MAX_LEN + 1];
    size_t board_id_len = 0;
    bool ready;

    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    ready = capability_save_pending_ready;
    if(ready) {
        len = capability_save_pending_len;
        memcpy(buf, capability_save_pending_buf, len);
        memcpy(board_id, capability_save_pending_board_id, sizeof(board_id));
        board_id_len = capability_save_pending_board_id_len;
        capability_save_pending_ready = false;
    }
    furi_mutex_release(app_protocol_mutex);
    if(!ready) {
        return;
    }

    if(!capability_storage_save(app->storage, board_id, board_id_len, buf, len)) {
        FURI_LOG_E(TAG, "Failed to persist capability record for board '%s'", board_id);
    }
}

/* Decodes the just-received capability_response payload (docs/CAPABILITIES.md), queues its
   raw bytes for the main thread to persist (see capability_save_pending_buf's own comment
   above), and posts it for display. */
APP_FN void handle_capability_response(
    Esp32BleProfile* profile,
    const uint8_t* plaintext,
    size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_capability_response_payload_t response;
    feb_cbor_status_t status =
        feb_cbor_decode_capability_response_payload(plaintext, plaintext_len, &response);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "capability_response payload decode failed: %d; dropping", status);
        return;
    }

    if(plaintext_len <= sizeof(capability_save_pending_buf)) {
        furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
        memcpy(capability_save_pending_buf, plaintext, plaintext_len);
        capability_save_pending_len = plaintext_len;
        memcpy(capability_save_pending_board_id, app_session_board_id, app_session_board_id_len);
        capability_save_pending_board_id[app_session_board_id_len] = '\0';
        capability_save_pending_board_id_len = app_session_board_id_len;
        capability_save_pending_ready = true;
        furi_mutex_release(app_protocol_mutex);
    } else {
        /* Unreachable given every decoder's out_cap is bounded by FEB_CBOR_MAX_PAYLOAD, but
           fail closed rather than overrun the hand-off buffer if that ever changed. */
        FURI_LOG_E(
            TAG,
            "capability_response too large to hand off (%u bytes); not persisted",
            (unsigned)plaintext_len);
    }

    post_capability_info(app, &response);
    FURI_LOG_I(TAG, "capability_response queued for save, board '%s'", app_session_board_id);
}

/* Sends capability_query (requested omitted, docs/PLAN.md step 7) only the first time
   runtime auth succeeds for a board with no locally cached capability record yet; loads and
   displays the cached record instead when one already exists. No retry/timeout of its own
   on send/decode failure -- a dropped or malformed response is simply retried on the next
   reconnect (docs/PLAN.md step 7 implementation-level decisions). */
APP_FN void capability_bootstrap(Esp32BleProfile* profile) {
    Esp32App* app = profile->app;
    if(capability_storage_exists(app->storage, app_session_board_id, app_session_board_id_len)) {
        size_t loaded_len = 0;
        if(capability_storage_load(
               app->storage,
               app_session_board_id,
               app_session_board_id_len,
               app_session_plaintext_buf,
               sizeof(app_session_plaintext_buf),
               &loaded_len)) {
            static feb_capability_response_payload_t cached;
            if(feb_cbor_decode_capability_response_payload(app_session_plaintext_buf, loaded_len, &cached) ==
               FEB_CBOR_OK) {
                post_capability_info(app, &cached);
            } else {
                FURI_LOG_W(TAG, "Cached capability file for '%s' is malformed", app_session_board_id);
            }
        }
        return;
    }

    static feb_capability_query_payload_t query_payload;
    query_payload.has_requested = 0;
    size_t payload_len = feb_cbor_encode_capability_query_payload(
        app_pairing_payload_buf, sizeof(app_pairing_payload_buf), &query_payload);
    if(payload_len == 0) {
        FURI_LOG_W(TAG, "capability_query: payload encode failed");
        return;
    }
    size_t record_len = session_send_encrypted_command(
        "capability_query",
        "capability_query",
        sizeof("capability_query") - 1,
        app_pairing_payload_buf,
        payload_len,
        capability_query_ciphertext_buf,
        sizeof(capability_query_ciphertext_buf),
        app_pairing_record_buf,
        sizeof(app_pairing_record_buf));
    if(record_len == 0) {
        return;
    }
    if(!send_pairing_record(profile, app_pairing_record_buf, record_len)) {
        FURI_LOG_W(TAG, "capability_query: send failed");
        return;
    }
    FURI_LOG_I(TAG, "capability_query sent for board '%s'", app_session_board_id);
}

/* ---- wifi_scan capability (docs/PLAN.md's Wi-Fi scan capability follow-on step) ---- */

/* Protected-record `error` (post-session-establishment shape, docs/PROTOCOL.md "Runtime
   auth failure handling" / message-payloads table) -- surfaced for wifi_scan's/ble_scan's
   `busy` response (docs/PROTOCOL.md's "Busy handling") and wardriving's `busy`/`not_running`/
   `invalid_command` responses (docs/PROTOCOL.md's "Busy/not-running handling"); any other code
   is logged and otherwise ignored. `error` carries no capability field, so
   app_pending_command_kind (see its own declaration comment above) picks which capability's
   in-flight command this reply belongs to, and is reset back to PendingCommandNone at the
   end of every branch below (also on the wifi_scan/ble_scan "complete" status, the
   wardriving "started"/"stopped" status, and reset_scan_ui_state()) so a stale value from an
   already-finished command can never be misattributed to a later, unrelated reply. A `busy`
   on a wardriving start means it was already running -- corrected here rather than left
   "unknown" (docs/LESSONS.md "UI must derive from real state"); symmetrically, `not_running`
   on a stop confirms it was already stopped. `internal_error` is a wardriving self-stop
   notice (docs/PROTOCOL.md: "an error record accompanies" a proactive `stopped` sent when
   "the engine self-stops for an internal reason") only when a wardriving start/stop is still
   the pending command -- the accompanying "stopped" status (handle_wardriving_status() above)
   always drives the actual run-state update regardless, so this branch only adds the reason
   message. If some other capability's command was pending instead, this reply is routed to
   that capability's own error surface rather than assumed to be about wardriving. */
APP_FN void
    handle_runtime_error(Esp32BleProfile* profile, const uint8_t* plaintext, size_t plaintext_len) {
    Esp32App* app = profile->app;
    static feb_error_payload_t error_payload;
    feb_cbor_status_t status = feb_cbor_decode_error_payload(plaintext, plaintext_len, &error_payload);
    if(status != FEB_CBOR_OK) {
        FURI_LOG_W(TAG, "runtime error payload decode failed: %d; dropping", status);
        return;
    }
    FURI_LOG_W(
        TAG, "runtime error received: code='%.*s'", (int)error_payload.code_len, error_payload.code);
    if(text_matches(error_payload.code, error_payload.code_len, "busy")) {
        if(app_pending_command_kind == PendingCommandBleScan) {
            post_ble_scan_error(app, "ESP32 busy, try again");
        } else if(app_pending_command_kind == PendingCommandWifiScan) {
            post_wifi_scan_error(app, "ESP32 busy, try again");
        } else if(app_pending_command_kind == PendingCommandWardrivingStart) {
            post_wardriving_run_state(app, true, false);
            post_wardriving_error(app, "Already running");
        }
        app_pending_command_kind = PendingCommandNone;
    } else if(text_matches(error_payload.code, error_payload.code_len, "not_running")) {
        if(app_pending_command_kind == PendingCommandWardrivingStop) {
            post_wardriving_run_state(app, false, false);
            post_wardriving_error(app, "Already stopped");
        }
        app_pending_command_kind = PendingCommandNone;
    } else if(text_matches(error_payload.code, error_payload.code_len, "invalid_command")) {
        if(app_pending_command_kind == PendingCommandWardrivingStart ||
           app_pending_command_kind == PendingCommandWardrivingStop) {
            post_wardriving_error(app, "Invalid command");
        }
        app_pending_command_kind = PendingCommandNone;
    } else if(text_matches(error_payload.code, error_payload.code_len, "internal_error")) {
        /* Only a wardriving start/stop still in flight makes this "internal_error" a
           self-stop notice for *this* pending command (docs/PROTOCOL.md's "stopped" state:
           self-stop always also sends its own "stopped" status, which already drives
           post_wardriving_run_state() regardless of app_pending_command_kind -- see
           handle_wardriving_status() above). Otherwise this reply belongs to whichever
           other capability was actually in flight (or none), so route it through that
           capability's own error surface instead of misattributing it to wardriving. */
        if(app_pending_command_kind == PendingCommandWardrivingStart ||
           app_pending_command_kind == PendingCommandWardrivingStop) {
            post_wardriving_run_state(app, false, false);
            post_wardriving_error(app, "Engine stopped (internal error)");
        } else if(app_pending_command_kind == PendingCommandWifiScan) {
            post_wifi_scan_error(app, "ESP32 internal error");
        } else if(app_pending_command_kind == PendingCommandBleScan) {
            post_ble_scan_error(app, "ESP32 internal error");
        } else {
            FURI_LOG_W(TAG, "internal_error with no matching pending command; ignoring");
        }
        app_pending_command_kind = PendingCommandNone;
    }
}


APP_FN size_t session_send_encrypted_command(
    const char* log_label,
    const char* type,
    size_t type_len,
    const uint8_t* payload,
    size_t payload_len,
    uint8_t* ciphertext_buf,
    size_t ciphertext_cap,
    uint8_t* record_buf,
    size_t record_cap) {
    /* HP-06/G10: protocol_mutex serializes this against every other
       feb_session_encrypt_record()/feb_session_decrypt_record() call site -- released before
       send_pairing_record()'s BLE send at each call site, never held across it. Consolidated
       2026-09-29 (TP-15 source split, decision S4): this block used to be copied verbatim into
       all six send_*_command functions plus capability_bootstrap(); same lock scope, same
       error paths, same zeroization (none of the inputs here are secret key material beyond
       app_session_key itself, which this function never copies). */
    furi_mutex_acquire(app_protocol_mutex, FuriWaitForever);
    if(app_session_seq_out >= FEB_SESSION_SEQUENCE_MAX) {
        furi_mutex_release(app_protocol_mutex);
        FURI_LOG_W(TAG, "%s: session sequence at cap; reconnect required", log_label);
        return 0;
    }
    size_t record_len = feb_session_encrypt_record(
        app_session_key,
        2,
        type,
        type_len,
        app_session_id_bytes,
        app_session_board_id,
        app_session_board_id_len,
        FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32,
        app_session_seq_out,
        payload,
        payload_len,
        ciphertext_buf,
        ciphertext_cap,
        record_buf,
        record_cap);
    if(record_len == 0) {
        furi_mutex_release(app_protocol_mutex);
        FURI_LOG_W(TAG, "%s: record encode failed", log_label);
        return 0;
    }
    app_session_seq_out++;
    furi_mutex_release(app_protocol_mutex);
    return record_len;
}
