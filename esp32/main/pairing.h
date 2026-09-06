/* Shared contract, mirrored byte-for-byte in flipper/pairing.h. Changes here must be
   mirrored there and in docs/PROTOCOL.md#initial-pairing-records / docs/PAIRING.md, or
   the two firmwares diverge -- the AES-GCM AAD mismatch class of bug from step 3 applies
   equally here: both peers must independently derive byte-identical transcripts and
   confirmation tags, or every pairing attempt fails.

   Scope (docs/PLAN.md step 5): the pairing record wire shapes (envelope, per-type
   payloads), the transcript T builder, and the T -> K_shared -> K_confirm ->
   pairing_secret -> confirmation-tag derivation pipeline, built on the primitives in
   pairing_crypto.h and the generic CBOR primitives in cbor_codec.h. It does NOT own the
   reset-gated 120-second window timer, BLE scanning/connection, or persistent storage --
   those are implementation-specific per firmware and live in main.c /
   flipper_esp32_over_ble.c, the same split step 3's framing.c/cbor_codec.c used against
   main.c's own connection-state handling. board_id generation (MAC-derived, ESP32-only)
   is also implementation-specific, not part of this wire contract. */
#ifndef FEB_PAIRING_H
#define FEB_PAIRING_H

#include <stdint.h>
#include <stddef.h>

#include "pairing_crypto.h"
#include "cbor_codec.h"

#define FEB_PAIRING_EPOCH_LEN 16u
#define FEB_PAIRING_NONCE_LEN 16u                 /* client_nonce / device_nonce */
#define FEB_PAIRING_PUBKEY_LEN FEB_X25519_KEY_LEN  /* = 32 */
#define FEB_PAIRING_KSHARED_LEN FEB_X25519_KEY_LEN
#define FEB_PAIRING_KCONFIRM_LEN 32u
#define FEB_PAIRING_SECRET_LEN 32u
#define FEB_PAIRING_REPLY_CONFIRM_LEN 32u   /* pair_reply / pair_confirm "confirmation" */
#define FEB_PAIRING_COMPLETE_TAG_LEN 16u    /* pair_complete "confirmation" */
#define FEB_PAIRING_SERVICE_UUID_LEN 16u
#define FEB_PAIRING_BOARD_ID_MAX_LEN 32u    /* <= FEB_CBOR_MAX_TEXT_LEN (64) */
#define FEB_PAIRING_WINDOW_MS (120u * 1000u)

/* Worst-case encoded length of T (docs/PROTOCOL.md#initial-pairing-records): the golden
   vector's T (tests/vectors/vectors.h, FEB_VEC_PAIR_TRANSCRIPT, board_id 17 bytes) is
   262 bytes; the only field whose length varies is board_id, so the true worst case at
   FEB_PAIRING_BOARD_ID_MAX_LEN (32 bytes, needing a 2-byte length prefix instead of
   board_id's 1-byte prefix at 17 bytes) is 262 + (32 - 17) + 1 = 278 bytes. Sized with
   margin for a caller-owned fixed buffer -- no dynamic allocation. */
#define FEB_PAIRING_MAX_TRANSCRIPT_LEN 288u

/* v2 primary service UUID (docs/PROTOCOL.md), RFC 4122 big-endian byte order -- the
   value used inside transcript T. Each implementation must explicitly convert its BLE
   stack's native in-memory UUID representation to this order before building T; do not
   assume it already matches (see docs/PROTOCOL.md's note on the step 2 UUID-byte-order
   bug this mirrors). `static const` in a shared header is intentional: each translation
   unit that includes this file gets its own copy, with no linkage conflict in C. */
static const uint8_t FEB_PAIRING_SERVICE_UUID[FEB_PAIRING_SERVICE_UUID_LEN] = {
    0x9c, 0x3f, 0x7e, 0x6a, 0xf4, 0x03, 0x4c, 0x31, 0x9e, 0xa2, 0x58, 0xa7, 0xa2, 0x0f, 0xb8, 0x11
};

#define FEB_PAIR_INIT_TYPE "pair_init"
#define FEB_PAIR_REPLY_TYPE "pair_reply"
#define FEB_PAIR_CONFIRM_TYPE "pair_confirm"
#define FEB_PAIR_COMPLETE_TYPE "pair_complete"

/* Outcome codes for a pairing attempt (docs/PAIRING.md, docs/PROTOCOL.md). Report to the
   peer as a pairing-phase `error` (see below), never exposing which specific check
   failed for FEB_PAIRING_ERR_FAILED (docs/PROTOCOL.md: "do not expose the cause"). */
typedef enum {
    FEB_PAIRING_ERR_DISABLED = 0, /* -> error.code "pairing_disabled": outside an open window */
    FEB_PAIRING_ERR_EXPIRED,      /* -> error.code "pairing_expired": window elapsed */
    FEB_PAIRING_ERR_FAILED,       /* -> error.code "pairing_failed": bad pubkey/shared-secret/confirmation */
} feb_pairing_error_t;

/* Single source of truth for the error.code string per feb_pairing_error_t, so both
   firmwares can't drift on spelling. */
const char *feb_pairing_error_code_str(feb_pairing_error_t err);

/* ---- Pairing record wire envelope (docs/PROTOCOL.md#pairing-record-wire-envelope) ----
   {version, type, board_id, payload} -- the general unencrypted-record shape minus
   session_id, which does not exist until the runtime `hello` flow begins. Every
   pair_init/pair_reply/pair_confirm/pair_complete and every pairing-phase `error` uses
   this envelope; a decoder distinguishes it from the runtime session_id-bearing envelope
   (feb_unencrypted_record_t in cbor_codec.h) by connection phase, not record content. */
typedef struct {
    uint32_t version;
    const char *type;
    size_t type_len;
    const char *board_id;
    size_t board_id_len;
    const uint8_t *payload_span; /* raw CBOR bytes of the payload map, opaque at this layer */
    size_t payload_span_len;
} feb_pairing_envelope_t;

/* Encodes into `out` (capacity `out_cap`); returns bytes written, or 0 on failure
   (oversized output, or board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN). */
size_t feb_cbor_encode_pairing_envelope(uint8_t *out, size_t out_cap, const feb_pairing_envelope_t *record);

/* Decodes a full reassembled record buffer into `record`, whose text/payload_span
   pointers alias `in` (valid only as long as `in` does). Returns FEB_CBOR_OK or a
   feb_cbor_status_t failure. Rejects board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN with
   FEB_CBOR_ERR_TOO_LARGE. */
feb_cbor_status_t feb_cbor_decode_pairing_envelope(const uint8_t *in, size_t in_len, feb_pairing_envelope_t *record);

/* ---- Per-type payloads (docs/PROTOCOL.md's pairing records table) ----
   Field order below is the canonical order encoders must emit and decoders must
   enforce, matching docs/PROTOCOL.md#canonical-cbor-encoding-definition. Pairing-phase
   `error` has no dedicated struct here -- it reuses feb_error_payload_t and
   feb_cbor_encode_error_payload()/feb_cbor_decode_error_payload() from cbor_codec.h,
   wrapped in feb_pairing_envelope_t instead of feb_unencrypted_record_t. */

typedef struct {
    uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN];
    uint8_t device_nonce[FEB_PAIRING_NONCE_LEN];
    uint8_t esp32_public_key[FEB_PAIRING_PUBKEY_LEN];
} feb_pair_init_payload_t;

size_t feb_cbor_encode_pair_init_payload(uint8_t *out, size_t out_cap, const feb_pair_init_payload_t *p);
feb_cbor_status_t feb_cbor_decode_pair_init_payload(const uint8_t *in, size_t in_len, feb_pair_init_payload_t *p);

typedef struct {
    uint8_t client_nonce[FEB_PAIRING_NONCE_LEN];
    uint8_t flipper_public_key[FEB_PAIRING_PUBKEY_LEN];
    uint8_t confirmation[FEB_PAIRING_REPLY_CONFIRM_LEN];
} feb_pair_reply_payload_t;

size_t feb_cbor_encode_pair_reply_payload(uint8_t *out, size_t out_cap, const feb_pair_reply_payload_t *p);
feb_cbor_status_t feb_cbor_decode_pair_reply_payload(const uint8_t *in, size_t in_len, feb_pair_reply_payload_t *p);

typedef struct {
    uint8_t confirmation[FEB_PAIRING_REPLY_CONFIRM_LEN];
} feb_pair_confirm_payload_t;

size_t feb_cbor_encode_pair_confirm_payload(uint8_t *out, size_t out_cap, const feb_pair_confirm_payload_t *p);
feb_cbor_status_t feb_cbor_decode_pair_confirm_payload(const uint8_t *in, size_t in_len, feb_pair_confirm_payload_t *p);

typedef struct {
    uint8_t confirmation[FEB_PAIRING_COMPLETE_TAG_LEN];
} feb_pair_complete_payload_t;

size_t feb_cbor_encode_pair_complete_payload(uint8_t *out, size_t out_cap, const feb_pair_complete_payload_t *p);
feb_cbor_status_t feb_cbor_decode_pair_complete_payload(const uint8_t *in, size_t in_len, feb_pair_complete_payload_t *p);

/* ---- Transcript T (docs/PROTOCOL.md#initial-pairing-records) ----
   T = canonical CBOR of {version, service_uuid, board_id, pairing_epoch, client_nonce,
   device_nonce, esp32_public_key, flipper_public_key} -- field order fixed, matching
   this struct's declaration order. */
typedef struct {
    uint32_t version;
    uint8_t service_uuid[FEB_PAIRING_SERVICE_UUID_LEN];
    const char *board_id;
    size_t board_id_len;
    uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN];
    uint8_t client_nonce[FEB_PAIRING_NONCE_LEN];
    uint8_t device_nonce[FEB_PAIRING_NONCE_LEN];
    uint8_t esp32_public_key[FEB_PAIRING_PUBKEY_LEN];
    uint8_t flipper_public_key[FEB_PAIRING_PUBKEY_LEN];
} feb_pairing_transcript_t;

/* Encodes T into `out` (capacity `out_cap`, >= FEB_PAIRING_MAX_TRANSCRIPT_LEN is always
   sufficient). Returns bytes written, or 0 on failure (oversized output, or
   board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN). */
size_t feb_pairing_encode_transcript(uint8_t *out, size_t out_cap, const feb_pairing_transcript_t *t);

/* ---- Key derivation pipeline (docs/PROTOCOL.md#initial-pairing-records) ----
   `k_shared` is the X25519 ECDH output (feb_x25519() from pairing_crypto.h) computed by
   the caller; this module treats it as an opaque 32-byte input and does not itself
   reject an all-zero value -- callers must call feb_is_all_zero() on it first, per
   docs/PAIRING.md's "Security boundary". */

/* K_confirm = HKDF-SHA-256(ikm=k_shared, salt=pairing_epoch,
   info="flipper-esp32-over-ble/v2/pair-confirm", length=32) */
void feb_pairing_derive_kconfirm(
    const uint8_t k_shared[FEB_PAIRING_KSHARED_LEN],
    const uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN],
    uint8_t out[FEB_PAIRING_KCONFIRM_LEN]);

/* pairing_secret = HKDF-SHA-256(ikm=k_shared, salt=pairing_epoch||client_nonce||device_nonce,
   info="flipper-esp32-over-ble/v2/x25519-pairing-secret"||board_id, length=32) */
void feb_pairing_derive_secret(
    const uint8_t k_shared[FEB_PAIRING_KSHARED_LEN],
    const uint8_t pairing_epoch[FEB_PAIRING_EPOCH_LEN],
    const uint8_t client_nonce[FEB_PAIRING_NONCE_LEN],
    const uint8_t device_nonce[FEB_PAIRING_NONCE_LEN],
    const char *board_id, size_t board_id_len,
    uint8_t out[FEB_PAIRING_SECRET_LEN]);

/* Confirmation/completion tags: HMAC-SHA-256(k_confirm, label || T), truncated to the
   field's length. `t`/`t_len` is T as produced by feb_pairing_encode_transcript(); the
   label is raw ASCII bytes concatenated directly before T, not itself CBOR-wrapped. */
void feb_pairing_flipper_confirm(const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN], const uint8_t *t, size_t t_len, uint8_t out[FEB_PAIRING_REPLY_CONFIRM_LEN]);   /* label "flipper-confirm" */
void feb_pairing_esp32_confirm(const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN], const uint8_t *t, size_t t_len, uint8_t out[FEB_PAIRING_REPLY_CONFIRM_LEN]);    /* label "esp32-confirm" */
void feb_pairing_complete_tag(const uint8_t k_confirm[FEB_PAIRING_KCONFIRM_LEN], const uint8_t *t, size_t t_len, uint8_t out[FEB_PAIRING_COMPLETE_TAG_LEN]);      /* label "complete" */

#endif /* FEB_PAIRING_H */
