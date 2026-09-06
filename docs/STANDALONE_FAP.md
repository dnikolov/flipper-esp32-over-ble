# Standalone Flipper FAP Feasibility

This assessment is pinned to the locally cached official Flipper firmware source at `docs/references/flipper-firmware/upstream`. Its exact commit, origin, and retrieval date are recorded in `docs/references/flipper-firmware/REVISION.txt`.

## Decision

A fully standalone external FAP is feasible for this project with the existing protocol roles:

- Flipper: BLE peripheral/GATT server.
- ESP32-C6: BLE central and GATT client.

A standalone FAP cannot make the Flipper a BLE central/GATT client using the cached public ABI. The current protocol correctly selects the feasible direction.

## Standalone architecture

The FAP owns its Bluetooth profile only while the app is active. It creates the v2 service and its write/notify characteristics, receives ESP32 writes through its service callback, and emits notifications to the ESP32-C6. The C6 scans, connects, discovers the characteristics, subscribes to notifications, and drives the pairing and runtime protocol.

The FAP bundles a reviewed, bounded implementation of X25519, SHA-256, HMAC-SHA-256, HKDF-SHA-256, and canonical CBOR. It uses the exported Flipper AES-GCM and random-number APIs for AES-256-GCM runtime protection and random key material — the runtime protocol was revised from AES-128-GCM to AES-256-GCM during step 6 design once this same GCM primitive was found to be hardcoded to a 256-bit key at the hardware level (see `docs/PLAN.md` step 6).

## Exported capabilities

The cached ABI manifest is `upstream/targets/f7/api_symbols.csv`. At the cached revision it exports the APIs needed for:

- Custom Bluetooth profile ownership: `bt_profile_start`, `bt_profile_restore_default`, and `furi_hal_bt_change_app`.
- GATT server creation and service event routing: `ble_gatt_service_add`, `ble_gatt_characteristic_init`, `ble_gatt_characteristic_update`, and `ble_event_dispatcher_register_svc_handler`.
- Runtime AES-GCM: `furi_hal_crypto_gcm_encrypt_and_tag` and `furi_hal_crypto_gcm_decrypt_and_verify`.
- Random bytes: `furi_hal_random_fill_buf`.
- Persistent app storage: storage open, write, sync, close, and rename APIs.

The ABI does not export a complete X25519, SHA-256, HMAC-SHA-256, HKDF-SHA-256, or BLE central/GATT-client API. A FAP must not link against non-exported mbedTLS headers merely because they appear in the source tree.

## Constraints and mitigations

### Global BLE profile

Bluetooth profile ownership is global. Starting the FAP profile can disconnect active Bluetooth clients and interrupts normal Bluetooth serial/HID behavior. The FAP must stop advertising, close the connection, remove its service state, and restore the default profile on every exit and failure path.

Treat profile acquisition, advertising, connected, pairing, authenticated, and cleanup as explicit application states. Test transition behavior with an existing Bluetooth connection before promising coexistence.

### Pairing-secret storage

An external FAP can store a versioned pairing record in app-owned storage under its app-owned data path using a temporary file, exact write verification, `storage_file_sync()`, close, and rename. This is sufficient for functional persistence.

It is not a hardware-backed secret vault. The standalone design protects runtime BLE traffic from passive radio capture, but a person with local SD-card, debug, or modified-firmware access may recover the pairing record. This is acceptable only if the project threat model permits app-controlled persistent storage. Do not embed a storage-encryption key in the FAP; it does not meaningfully improve this threat model.

### Cryptographic implementation

Use a small reviewed library rather than implementing curve arithmetic or hashes from scratch. Constrain all decode buffers and zeroize X25519 private keys, shared secrets, confirmation keys, session keys, and plaintext buffers on success and failure. Pin the FAP build to a tested firmware API version and rerun ABI checks when updating the target firmware.

## Local evidence

- Custom profile contract: `upstream/targets/f7/ble_glue/furi_ble/profile_interface.h`.
- GATT server API: `upstream/targets/f7/ble_glue/furi_ble/gatt.h`.
- Bluetooth HAL and global profile handling: `upstream/targets/f7/furi_hal/furi_hal_bt.c`.
- Peripheral implementation pattern: `upstream/targets/f7/ble_glue/profiles/serial_profile.c` and `upstream/targets/f7/ble_glue/services/serial_service.c`.
- AES-GCM and random APIs: `upstream/targets/furi_hal_include/furi_hal_crypto.h` and `upstream/targets/furi_hal_include/furi_hal_random.h`.
- FAP storage API: `upstream/applications/services/storage/storage.h`.

## Implementation proof steps

1. Build a minimal FAP that starts a profile, advertises the v2 UUID, and restores the default profile on exit.
2. Add one writable GATT characteristic and one notification characteristic; prove ESP32-C6 central discovery, write delivery, subscription, notification delivery, and cleanup after disconnect.
3. Bundle the crypto and CBOR libraries; validate X25519, HKDF, HMAC, AES-GCM, and CBOR vectors before integrating the pairing state machine.
4. Implement the reset-gated X25519 pairing flow from [PAIRING.md](PAIRING.md), then persistence through atomic app-owned record replacement.
5. Build against the pinned firmware and repeat on the physical Flipper. Do not claim Bluetooth coexistence until it is tested.

## Conclusion

Custom/in-tree Flipper firmware is not required for BLE transport ownership in this project. It remains the stronger option only when firmware-owned protected secret storage or coexistence with system Bluetooth profiles is mandatory.