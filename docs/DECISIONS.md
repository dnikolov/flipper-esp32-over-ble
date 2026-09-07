# Architecture Decisions

## BLE pairing bootstrap

**Decision:** Pairing uses an unauthenticated ephemeral X25519 key exchange over BLE with no pre-provisioned pairing credential.

**Context:** Initial pairing occurs only in a trusted environment. Physical reset of an ESP32-C6 with no stored `pairing_secret` authorizes a new pairing relationship. Once a board has a stored secret, a reset alone no longer re-opens pairing (from step 6 onward it attempts runtime auth instead, per [PAIRING.md](PAIRING.md)); replacing an existing relationship then requires a deliberate action — the Flipper's local unpair, or clearing the ESP32's own stored secret — rather than physical reset by itself. This revises the pairing-bootstrap decision as it stood through step 5, made explicit during step 6's design review (see [PLAN.md](PLAN.md) step 6 and `docs/PROJECT_HISTORY.md`'s step 6 entry).

**Protocol:** An ESP32-C6 reset opens a 120-second pairing window. During this window, the ESP32-C6 connects to the Flipper pairing service. Both peers generate a fresh X25519 ephemeral keypair and a fresh 16-byte nonce, derive a 32-byte `pairing_secret` from the shared secret using HKDF-SHA-256, confirm the complete transcript with keys derived from that shared secret, and persist the pairing secret only after confirmation succeeds.

**Consequences:** This design protects subsequent BLE traffic from passive capture and gives each successful pairing a distinct high-entropy secret. It does not protect against an active attacker who can participate in or intercept the initial pairing exchange. Pair only where that radio threat is accepted. The ESP32 accepts one pairing attempt per reset window and closes the window immediately after a successful pairing.

## Runtime protection

**Decision:** Runtime traffic uses fresh AES-256-GCM session keys derived from `pairing_secret` with HKDF-SHA-256 and fresh per-connection nonces. (Originally AES-128-GCM; revised during step 6 design — the Flipper's only exported raw-key AES-GCM primitive is hardcoded to a 256-bit key at the hardware level, see [PLAN.md](PLAN.md) step 6.)

**Consequences:** The initial X25519 private keys, ECDH shared secret, transcript-confirmation keys, and session keys are transient. Only `pairing_secret` persists across connections.

## Flipper delivery model

**Decision:** Deliver the Flipper component as a standalone FAP, specifically a standalone external FAP that temporarily owns a custom BLE peripheral/GATT-server profile. Custom/in-tree firmware is optional hardening, not a transport prerequisite.

**Consequences:** The ESP32-C6 remains the BLE central. The FAP must restore the default Bluetooth profile on exit and stores `pairing_secret` in app-owned persistent storage, which does not protect against local SD-card, debug, or modified-firmware access. The detailed assessment is in [STANDALONE_FAP.md](STANDALONE_FAP.md).

## Bluetooth profile lifecycle

**Decision:** Accept the Flipper's single-active-profile model for this standalone FAP. While active, the FAP replaces the current Bluetooth profile with its custom pairing and runtime GATT profile.

**Consequences:** This is normal Flipper BLE application behavior. Concurrent Bluetooth serial, HID, and other BLE-app operation is not supported while the FAP profile is active. It must explicitly stop advertising, disconnect the ESP32-C6, release GATT state, and restore the default Bluetooth profile on normal exit and every recoverable failure path.