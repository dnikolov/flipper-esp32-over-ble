/* Shared contract, mirrored byte-for-byte in flipper/session.h. Changes here must be
   mirrored there and in docs/PROTOCOL.md#session-establishment, or the two firmwares
   diverge -- the AES-GCM AAD/proof mismatch class of bug from steps 3/5 applies equally
   here: both peers must independently derive byte-identical transcripts, proofs, session
   keys, AAD, and nonces, or every session fails authentication immediately.

   Scope (docs/PLAN.md step 6): the hello/hello_ack/client_auth wire shapes, the runtime
   transcript S builder, the S -> proof pipeline, the pairing_secret -> session_key
   derivation, and the AAD/nonce construction + AES-256-GCM encrypt/decrypt of protected
   records -- built on the primitives in pairing_crypto.h (HMAC-SHA-256, HKDF-SHA-256,
   feb_secure_zero, feb_consttime_equal), session_crypto.h (AES-256-GCM), and the generic
   CBOR/envelope primitives in cbor_codec.h. It does NOT own sequence-counter state, the
   30-second idle-connection timeout, rate-limiting failed auth attempts, or the
   reset-vs-runtime-auth boot decision -- those are implementation-specific per firmware and
   live in main.c / flipper_esp32_over_ble.c, the same split pairing.h used against main.c's
   own window-timer/BLE-state handling.

   The `unknown_board` error path is deliberately NOT part of this header: docs/PROTOCOL.md's
   "Runtime auth failure handling" specifies it uses the pairing-record wire envelope
   (feb_pairing_envelope_t in pairing.h) plus feb_error_payload_t (cbor_codec.h), not the
   session_id-bearing shapes here, since no session_id is trusted to exist yet when a peer
   has no stored record for the incoming board_id. Callers reuse those existing types
   directly; no new struct is needed for it.

   Stack-budget note for the Flipper implementation (docs/SESSION_MEMORY.md's step 3/5
   entries -- this bug class has recurred three times): every buffer this header's functions
   need internally (AAD encoding, transcript S encoding) is on the order of 150-200 bytes and
   reachable from profile_event_handler() on the 1280-byte BleEventWorker stack. Implement
   session.c's internals with file-scope `static` storage for these buffers from the start,
   not stack-local -- do not wait for a crash to discover this again. */
#ifndef FEB_SESSION_H
#define FEB_SESSION_H

#include <stdint.h>
#include <stddef.h>

#include "pairing.h"
#include "session_crypto.h"

#define FEB_SESSION_NONCE_FIELD_LEN 16u /* client_nonce / device_nonce in hello/hello_ack */
#define FEB_SESSION_PROOF_LEN 16u       /* hello_ack / client_auth "proof" */

/* Worst-case encoded length of transcript S = canonical CBOR of {version, board_id,
   session_id, client_nonce, device_nonce}: map header + 5 key/value pairs, board_id up to
   FEB_PAIRING_BOARD_ID_MAX_LEN (32) bytes -- computed worst case is 137 bytes; sized with
   margin for a caller-owned fixed buffer, no dynamic allocation. */
#define FEB_SESSION_MAX_TRANSCRIPT_LEN 160u

/* Worst-case encoded length of the AAD = canonical CBOR of {version, type, session_id,
   sequence, board_id}: `type` up to FEB_CBOR_MAX_TEXT_LEN (64) bytes (the longest protected
   record type, "capability_response", is far shorter, but the field itself is bounded by
   the general text limit), `sequence` a worst-case 9-byte uint64 header, board_id up to
   FEB_PAIRING_BOARD_ID_MAX_LEN (32) bytes -- computed worst case is 166 bytes; sized with
   margin. */
#define FEB_SESSION_MAX_AAD_LEN 200u

#define FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32 0x00u
#define FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER 0x01u

#define FEB_HELLO_TYPE "hello"
#define FEB_HELLO_ACK_TYPE "hello_ack"
#define FEB_CLIENT_AUTH_TYPE "client_auth"

/* ---- hello/hello_ack/client_auth payloads (docs/PROTOCOL.md#session-establishment) ----
   Wrapped in feb_unencrypted_record_t (cbor_codec.h), same as any other unencrypted-record
   payload; session_id is the ESP32-generated session_id for the connection being
   established, board_id identifies which stored secret to use. Field order below is the
   canonical order encoders must emit and decoders must enforce. */

typedef struct {
    uint8_t client_nonce[FEB_SESSION_NONCE_FIELD_LEN];
} feb_hello_payload_t;

size_t feb_cbor_encode_hello_payload(uint8_t *out, size_t out_cap, const feb_hello_payload_t *p);
feb_cbor_status_t feb_cbor_decode_hello_payload(const uint8_t *in, size_t in_len, feb_hello_payload_t *p);

typedef struct {
    uint8_t device_nonce[FEB_SESSION_NONCE_FIELD_LEN];
    uint8_t proof[FEB_SESSION_PROOF_LEN];
} feb_hello_ack_payload_t;

size_t feb_cbor_encode_hello_ack_payload(uint8_t *out, size_t out_cap, const feb_hello_ack_payload_t *p);
feb_cbor_status_t feb_cbor_decode_hello_ack_payload(const uint8_t *in, size_t in_len, feb_hello_ack_payload_t *p);

typedef struct {
    uint8_t proof[FEB_SESSION_PROOF_LEN];
} feb_client_auth_payload_t;

size_t feb_cbor_encode_client_auth_payload(uint8_t *out, size_t out_cap, const feb_client_auth_payload_t *p);
feb_cbor_status_t feb_cbor_decode_client_auth_payload(const uint8_t *in, size_t in_len, feb_client_auth_payload_t *p);

/* ---- Transcript S (docs/PROTOCOL.md#session-establishment) ----
   S = canonical CBOR of {version, board_id, session_id, client_nonce, device_nonce} --
   field order fixed, matching this struct's declaration order. */
typedef struct {
    uint32_t version;
    const char *board_id;
    size_t board_id_len;
    uint8_t session_id[FEB_SESSION_ID_LEN];
    uint8_t client_nonce[FEB_SESSION_NONCE_FIELD_LEN];
    uint8_t device_nonce[FEB_SESSION_NONCE_FIELD_LEN];
} feb_session_transcript_t;

/* Encodes S into `out` (capacity `out_cap`, >= FEB_SESSION_MAX_TRANSCRIPT_LEN is always
   sufficient). Returns bytes written, or 0 on failure (oversized output, or
   board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN). */
size_t feb_session_encode_transcript(uint8_t *out, size_t out_cap, const feb_session_transcript_t *s);

/* ---- Runtime proofs (docs/PROTOCOL.md#session-establishment) ----
   proof = first 16 bytes of HMAC-SHA-256(pairing_secret, label || S). `s`/`s_len` is S as
   produced by feb_session_encode_transcript(); the label is raw ASCII bytes concatenated
   directly before S, not itself CBOR-wrapped -- matching pairing.h's confirmation-tag
   pattern exactly. Compare with feb_consttime_equal() (pairing_crypto.h), never memcmp(). */
void feb_session_flipper_proof(const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN], const uint8_t *s, size_t s_len, uint8_t out[FEB_SESSION_PROOF_LEN]);   /* label "runtime-flipper" */
void feb_session_esp32_proof(const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN], const uint8_t *s, size_t s_len, uint8_t out[FEB_SESSION_PROOF_LEN]);     /* label "runtime-esp32" */

/* ---- Session key derivation (docs/PROTOCOL.md#cryptographic-requirements) ----
   session_key = HKDF-SHA-256(ikm=pairing_secret, salt=client_nonce||device_nonce,
   info="flipper-esp32-over-ble/v2/aes-256-gcm"||board_id||session_id, length=32) */
void feb_session_derive_key(
    const uint8_t pairing_secret[FEB_PAIRING_SECRET_LEN],
    const uint8_t client_nonce[FEB_SESSION_NONCE_FIELD_LEN],
    const uint8_t device_nonce[FEB_SESSION_NONCE_FIELD_LEN],
    const char *board_id, size_t board_id_len,
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    uint8_t out[FEB_SESSION_KEY_LEN]);

/* ---- AAD (docs/PROTOCOL.md#cryptographic-requirements) ----
   AAD = canonical CBOR of {version, type, session_id, sequence, board_id} -- field order
   fixed, matching this struct's declaration order. Authenticated but not encrypted. */
typedef struct {
    uint32_t version;
    const char *type;
    size_t type_len;
    uint8_t session_id[FEB_SESSION_ID_LEN];
    uint64_t sequence;
    const char *board_id;
    size_t board_id_len;
} feb_session_aad_t;

/* Encodes into `out` (capacity `out_cap`, >= FEB_SESSION_MAX_AAD_LEN is always sufficient).
   Returns bytes written, or 0 on failure (oversized output, or a field exceeding its own
   type/board_id length limit). */
size_t feb_session_encode_aad(uint8_t *out, size_t out_cap, const feb_session_aad_t *aad);

/* ---- Nonce (docs/PROTOCOL.md#cryptographic-requirements) ----
   nonce = session_id[0..7] || direction || sequence[0..2] (sequence encoded big-endian,
   using only its low 24 bits -- callers must never let a real sequence counter reach
   2^24 - 1, per docs/PROTOCOL.md; this function does not itself enforce that). `direction`
   is FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32 or FEB_SESSION_DIRECTION_ESP32_TO_FLIPPER --
   the direction the record actually travelled, not the local role. */
void feb_session_build_nonce(
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    uint8_t direction,
    uint64_t sequence,
    uint8_t out[FEB_SESSION_NONCE_LEN]);

/* ---- Protected-record encrypt/decrypt ----
   High-level helpers wrapping cbor_codec.h's feb_protected_record_t around this header's
   AAD/nonce construction and session_crypto.h's AES-256-GCM primitives. Callers own every
   buffer (no dynamic allocation, no internal buffer sized off attacker-controlled input). */

/* Encrypts `payload` (`payload_len` bytes, its own canonical CBOR encoding, <=
   FEB_CBOR_MAX_PAYLOAD) and encodes the full protected-record CBOR envelope into `out`
   (capacity `out_cap`). `ciphertext_scratch` (capacity `ciphertext_scratch_cap`, must be >=
   payload_len) receives the intermediate ciphertext bytes before feb_cbor_encode_protected()
   copies them into `out`. `direction` is the direction this record is being sent in.
   Returns bytes written to `out`, or 0 on failure (oversized payload/output, or
   ciphertext_scratch_cap < payload_len). */
size_t feb_session_encrypt_record(
    const uint8_t session_key[FEB_SESSION_KEY_LEN],
    uint32_t version,
    const char *type, size_t type_len,
    const uint8_t session_id[FEB_SESSION_ID_LEN],
    const char *board_id, size_t board_id_len,
    uint8_t direction,
    uint64_t sequence,
    const uint8_t *payload, size_t payload_len,
    uint8_t *ciphertext_scratch, size_t ciphertext_scratch_cap,
    uint8_t *out, size_t out_cap);

/* Decoded, decrypted protected record. `type`/`board_id` alias `in` (valid only as long as
   `in` does); `plaintext` aliases the caller-supplied `plaintext_out` buffer. */
typedef struct {
    uint32_t version;
    const char *type;
    size_t type_len;
    uint8_t session_id[FEB_SESSION_ID_LEN];
    const char *board_id;
    size_t board_id_len;
    uint64_t sequence;
    const uint8_t *plaintext;
    size_t plaintext_len;
} feb_session_decrypted_record_t;

/* Decodes a full reassembled protected-record buffer, reconstructs the AAD and nonce from
   its own decoded fields, and decrypts `ciphertext` into `plaintext_out` (capacity
   `plaintext_out_cap`, must be >= FEB_CBOR_MAX_PAYLOAD). `direction` is the direction this
   record actually travelled (the sender's direction, not the local role -- e.g. the ESP32
   passes FEB_SESSION_DIRECTION_FLIPPER_TO_ESP32 when decoding a record it received).
   Returns FEB_CBOR_OK with `record` fully populated on success; a feb_cbor_status_t decode
   failure for malformed CBOR; or FEB_CBOR_ERR_AUTH_FAILED if the GCM tag does not verify.
   Every failure is fatal per docs/PROTOCOL.md ("discard the record and close the BLE
   connection without replying") -- this function does not itself close the connection,
   only reports the failure. Does NOT check `sequence` continuity, `board_id` against the
   caller's active session, or `session_id` against the caller's expected value -- those are
   connection-state checks owned by main.c/flipper_esp32_over_ble.c, matching how framing.c
   never owned message_id continuity policy beyond wraparound-safe reset. */
feb_cbor_status_t feb_session_decrypt_record(
    const uint8_t session_key[FEB_SESSION_KEY_LEN],
    const uint8_t *in, size_t in_len,
    uint8_t direction,
    uint8_t *plaintext_out, size_t plaintext_out_cap,
    feb_session_decrypted_record_t *record);

#endif /* FEB_SESSION_H */
