# Pairing flow

Pairing is a BLE-only ceremony that establishes a long-term pairing secret in a trusted environment. The security boundary and decision rationale are recorded in [DECISIONS.md](DECISIONS.md). Runtime behavior is specified by [PROTOCOL.md](PROTOCOL.md).

## Reset-gated BLE onboarding

The ESP32-C6 is the BLE central and the Flipper firmware service is the BLE peripheral/GATT server. The Flipper starts its pairing advertisement only while the user is on the pairing screen; from step 6 onward it also starts advertising automatically whenever the app launches and at least one stored pairing record exists (see [PROTOCOL.md](PROTOCOL.md) "Session establishment" and [PLAN.md](PLAN.md) step 6), with no explicit action required to resume a known pairing.

1. On every ESP reset, the board first checks for a stored `pairing_secret`. If one exists, it attempts the runtime `hello`/`hello_ack`/`client_auth` flow instead of opening a pairing window (see [PROTOCOL.md](PROTOCOL.md) "Session establishment"). A pairing window opens only if no secret is stored yet, or if the runtime attempt fails specifically with `unknown_board` (the Flipper has no record for this board). A proof-verification failure does **not** open a window — see [PROTOCOL.md](PROTOCOL.md) "Runtime auth failure handling." When a window does open, the board generates a fresh 16-byte `pairing_epoch` for it.
2. During that window, the board scans for the Flipper v2 pairing service, connects, and sends `pair_init` with its stable `board_id`, fresh 16-byte `device_nonce`, and an ephemeral X25519 public key.
3. The user selects **Add ESP32 board**. The Flipper generates a fresh X25519 keypair and 16-byte `client_nonce`, then returns its public key in `pair_reply`.
4. Both peers derive `pairing_secret` from the X25519 shared secret and the complete transcript. They exchange transcript confirmations derived from that secret.
5. The ESP32 atomically stores the pairing record before sending `pair_complete`. The Flipper stores its record only after verifying `pair_complete`.
6. A successful pairing closes the window immediately. Until the next ESP reset, the board rejects pairing records with `pairing_disabled`.

The ESP32 accepts exactly one pairing attempt per reset window: whatever happens on that first connection — success, disconnect, expiry, malformed input, or an invalid confirmation — ends the window immediately, even if time remains on the 120-second clock. "Clears transient state" means the ephemeral per-attempt values (X25519 keypair, nonce) are zeroized and no partial pairing record is ever written — it does not mean the window stays open for a second try. A physical ESP reset (or `unknown_board`, per point 1 above) is required to open a new window.

**Explicit re-pairing (from step 6 onward).** A physical ESP reset by itself no longer forces re-pairing once a working `pairing_secret` is stored on both sides — it just resumes the runtime session (point 1 above). Re-pairing an already-paired board therefore requires a deliberate action on one side: the Flipper's local "unpair this board" action (deletes only the Flipper's record for that `board_id`; the next runtime `hello` for that board then gets `unknown_board`, and the ESP32 falls back to opening a window on its own — no ESP32-side action needed), or clearing the ESP32's own stored secret (e.g. the NVS-partition erase in [PLAN.md](PLAN.md) step 6, which never needs a Flipper-side action either, since the ESP32 then has nothing to attempt runtime auth with). A board with no stored secret at all (fresh or erased) always opens a window immediately on reset, as before — that case is unchanged. A factory reset or explicit unpair deletes the pairing record and associated metadata on its respective device.

## Security boundary

X25519 provides forward secrecy for the pairing exchange but does not authenticate a peer. This design protects later traffic from passive BLE capture, but an active attacker in radio range during initial pairing can impersonate or intercept a peer. Pair only in an environment where that risk is accepted.

## BLE session

After pairing:

- BLE is used for all runtime communication.
- AES-256-GCM encrypts CBOR payloads and authenticates each protected message (originally
  AES-128-GCM; revised during step 6 design since the Flipper's only exported raw-key
  AES-GCM primitive is hardcoded to a 256-bit key at the hardware level — see
  [PLAN.md](PLAN.md) step 6).
- The stored pairing secret derives a new session key for each BLE connection; it is never used as a direct AES key.
- Every protected message includes a direction-specific sequence number and is rejected if replayed or out of order.

## Device registration

The authenticated `capability_response` is the device registration record. Each ESP32 reports:

- `board`: board model
- `board_id`: stable per-device identifier in the authenticated envelope
- `features`: supported capability identifiers
- `firmware`: firmware version

The Flipper app uses this data to render a capability-aware interface.
