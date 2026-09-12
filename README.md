# flipper-esp32-over-ble

Pairs a Flipper Zero with an ESP32-C6 over BLE using a trusted-environment X25519 key exchange,
then exposes board capabilities through an authenticated CBOR protocol. Two independent firmware
targets (`esp32/`, `flipper/`) implementing one shared wire contract. See
[CLAUDE.md](CLAUDE.md) for the project map and pinned baselines, and
[docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) for current status.

## Pairing model

1. User chooses **Add ESP32 board** in the Flipper app.
2. The board enters a short (120-second), reset-gated BLE pairing window.
3. The app and board exchange ephemeral X25519 public keys and derive shared pairing material
   over BLE.
4. The app and board confirm the pairing transcript before storing the pairing material.
5. The pairing material is stored on both ends (app-owned storage on the Flipper, NVS on the
   ESP32 — see [docs/DECISIONS.md](docs/DECISIONS.md) for the accepted threat-model tradeoffs).
6. The app continues communication over BLE using authenticated, encrypted CBOR messages.

Full step-by-step ceremony: [docs/PAIRING.md](docs/PAIRING.md).

## Capability model

Each connected board advertises a capability set through an authenticated registry. Shipped
today: `wifi_scan`, `ble_scan`, and the composite `wardriving` (autonomous Wi-Fi/BLE capture with
flash-backed logging and WiGLE CSV export). Planned for later phases: `gpio_control`, passive
`zigbee`/`thread` recon, and a second board target (Heltec WiFi LoRa 32 V2) adding display/LoRa
capabilities. See [docs/CAPABILITIES.md](docs/CAPABILITIES.md) for the current registry format
and full list, and [docs/PLAN.md](docs/PLAN.md) for the roadmap.

The capability list is dynamic and depends on the board model and attached peripherals — the
Flipper renders only what a connected board actually reports.

## Security model

- BLE pairing is an unauthenticated ephemeral X25519 key exchange in a trusted environment,
  establishing a long-term `pairing_secret` — see [docs/DECISIONS.md](docs/DECISIONS.md) for why,
  and its accepted tradeoffs.
- Runtime BLE traffic is AES-256-GCM over canonical CBOR, using a fresh per-session key derived
  from `pairing_secret` via HKDF-SHA-256.
- Every protected message carries a direction-specific sequence number and is rejected if
  replayed, out of order, or unauthenticated.
- Physical possession of either paired device is accepted as fully compromising that device's
  stored secrets for the current phase — see [docs/PROTOCOL.md](docs/PROTOCOL.md)'s
  "Implementation security requirements."

Full wire contract: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Project layout

- `esp32/`: ESP-IDF firmware for the ESP32-C6 (BLE central/GATT client)
- `flipper/`: standalone external FAP for the Flipper Zero (BLE peripheral/GATT server)
- `tests/`: host-native codec/protocol tests shared between both firmwares
- `docs/`: protocol contract, pairing ceremony, capability registry, roadmap, and project history

## Implementation roadmap

The ordered implementation phases and acceptance criteria are in [docs/PLAN.md](docs/PLAN.md).
The open backlog (defects, deferred decisions, cost/efficiency work) is centralized in
[docs/BACKLOG.md](docs/BACKLOG.md).
