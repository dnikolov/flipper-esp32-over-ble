# Capability registry

Connected ESP32 boards advertise an authenticated capability registry using the `capability_response` message defined in [PROTOCOL.md](PROTOCOL.md). Capability strings are stable lowercase ASCII identifiers using underscores. A board only advertises a capability it actually has the hardware for — see the phased roadmap in [PLAN.md](PLAN.md) step 7 for when each capability ships.

## Capabilities

- **`wifi_scan`** — Wi-Fi network scan. Each result reports the discovered AP's supported PHY generation (802.11b/g/n/ax) as a field. The ESP32-C6 has one 2.4GHz radio only (no 5GHz radio), so there is no separate "Wi-Fi 6 scan" mode — PHY generation is a per-AP property surfaced in the scan result, not a distinct scan operation.
- **`ble_scan`** — passive BLE advertisement scan, reporting nearby BLE devices.
- **`wardriving`** — composite capability. Advertised only when GPS hardware and at least one of `wifi_scan`/`ble_scan` are present on the board. A `command` to start it takes `sources` (`["wifi"]`, `["ble"]`, or both) and a per-source scan interval in its `arguments`; requesting a source the board does not have is rejected with `invalid_command`. Interval bounds and defaults come from the radio-coexistence validation in [PLAN.md](PLAN.md) step 4, not a guessed value. Capture runs autonomously once started — it continues across BLE disconnects and does not require an active Flipper connection to keep buffering. Connecting drains the buffered backlog first, then streams live results. Results captured before GPS achieves a fix are discarded (a Flipper-settable backfill-to-first-fix option is backlogged, not implemented). Data is transmitted as a compact binary record over BLE; the Flipper converts to WiGLE CSV only when writing to its SD card, as one timestamped file per flush session.
- **`gpio_control`** (later phase) — generic GPIO control. Reserves strapping/JTAG pins (GPIO0, 4, 5, 8, 9, 15) from generic control actions.
- **`zigbee`, `thread`** (later phase) — first as passive recon/sniffing capabilities matching the `wifi_scan`/`ble_scan` pattern (no joining or commissioning); active stack participation (joining as an end-device / Thread node) is a separately-scoped, much larger later effort.
- Display and LoRa capabilities are planned for a second board (Heltec WiFi LoRa 32 V2 — a different chip family from the C6) in a later phase; see `docs/BASELINES.md`.

## Example capability record

```json
{
  "board": "esp32-c6-devkit",
  "board_id": "unique-board-id",
  "features": [
    "wifi_scan",
    "ble_scan",
    "wardriving"
  ],
  "firmware": "1.0.0"
}
```

The `board_id` travels in the authenticated protocol envelope; it is shown here to clarify the complete logical registry. `board_id` is derived from the chip's factory-programmed MAC address, avoiding collisions across multiple boards without a separate provisioning step. The Flipper app loads available actions from this registry instead of assuming every board behaves identically, and stores a separate pairing record per `board_id` — multiple boards may be paired to one Flipper at a time (see [PLAN.md](PLAN.md) "Multi-board pairing"). A board must reject a `command` for a capability absent from its advertised `features` list with `unsupported_capability`.

## Storage and persistence

The Flipper persists a board's capability registry in its own file per `board_id` (separate from the pairing-secret file). The capability record is queried exactly once — the first time runtime auth succeeds for that `board_id` AND no locally persisted capability file exists for it yet. The result is cached and never automatically re-queried on subsequent reconnects, even though this differs from the project's usual "distrust and re-verify every session" pattern used for authentication. This is a deliberate exception because a capability list is non-sensitive cached metadata, not a security credential. Nothing automatically invalidates or refreshes a persisted capability record; the only way to force a fresh query is a full unpair + re-pair. Unpairing a board deletes both its pairing record and its capability file together, as one operation — no orphaned capability file is left behind. The `board` and `firmware` fields are treated as opaque by the Flipper (no enum, no allowlist, no validation against known values) — they are hand-maintained constant strings per firmware build, not derived or validated values.
