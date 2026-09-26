# Cluster: wired C6 + C5 + Heltec, distributed scanning

**Status: frozen design, 2026-09-26, no code written against it yet.** This is Phase 9 — see
[PLAN.md](PLAN.md)'s "Phase 9: wired cluster" section for the step breakdown and "done when"
criteria. Reached via an iterative design conversation with the user (role assignment revised
twice during that conversation — the final split below is not the first one proposed, see
[PROJECT_HISTORY.md](PROJECT_HISTORY.md) for the reasoning trail if it's ever needed).

## Motivation

Every ESP32-family board in this project shares one combo radio between Wi-Fi, BLE, and (on the
C6/C5) 802.15.4. Every real coexistence bug this project has hit (G36, H01, BL16, BL18) is the
same failure shape: a foreground scan job starves a background real-time protocol (the BLE
connection to the Flipper) sharing that one radio. The cluster's goal is to eliminate that failure
shape structurally — one radio, one job, per board — rather than continuing to tune duty cycles
around it. Three secondary goals, all explicit from the user: minimize *inter-board* communication
overhead (a wired link, not a second RF layer), share one physical GPS module instead of wiring
three, and keep each board's job as small/dedicated as possible.

## Roles

| Board | Job | Radio(s) used | Talks to Flipper? |
| --- | --- | --- | --- |
| **Heltec** | Coordinator: holds the Flipper BLE link, `ble_scan`, `meshcore_scan`, GPS, wardriving aggregation/flash-log/CSV | Combo Wi-Fi/BT (BLE-central + `ble_scan`) + separate SPI SX1276 (LoRa) | Yes — the only board that does |
| **C6** | `wifi_scan`, 2.4GHz only | Combo radio, Wi-Fi-only, no BLE stack running | No |
| **C5** | `wifi_scan`, 5GHz only (with the existing fast/full DFS toggle) | Combo radio, Wi-Fi-only, no BLE stack running | No |

Why this split, not some other assignment of the same three jobs:

- **LoRa only exists on Heltec** (SX1276) — `meshcore_scan` has no other home. Forced, not chosen.
- **Only C5 has a native 5GHz radio** — giving it the 5GHz job (rather than splitting both bands
  across two boards arbitrarily) uses the one board actually built for it.
- **Splitting 2.4GHz and 5GHz across two separate boards (C6, C5) instead of both bands on one
  board is a real, deliberate win, not just tidiness**: on a single radio, a dual-band scan visits
  both channel sets *sequentially* — the 5GHz DFS channels' regulatory passive-dwell requirement
  stalls 2.4GHz collection too. Splitting them onto two independent radios means both bands are
  scanned *concurrently*, shortening the revisit interval for each band rather than summing them.
- **`ble_scan` goes on Heltec, not C6/C5, because it's the one already-proven-safe combination**:
  BLE-scan-while-BLE-connected is one radio doing one protocol (the existing merged-reconnect-scan
  mechanism already relies on exactly this interleaving), unlike Wi-Fi/BLE cross-protocol
  contention. Putting it anywhere else would just relocate a solved problem.
- **Net effect**: C6 and C5 each do exactly one job, full time, with literally nothing else ever
  competing for their radio. Heltec carries three jobs, but every pairing on it (BLE-central +
  `ble_scan` on one radio, LoRa on a separate radio) is one already validated as safe — no *new*
  contention is introduced anywhere.

A real side effect: since C6 and C5 never hold a BLE connection to anything in cluster mode, their
cluster-mode firmware needs none of the pairing/session/crypto layer (X25519, HKDF, AES-256-GCM,
persisted `pairing_secret`) — that machinery only matters on the board actually facing the
Flipper. Cluster-mode C6/C5 builds are structurally simpler than their standalone builds, not just
reconfigured.

## Physical wiring

**UART star topology, two independent point-to-point links** (confirmed over an I2C multi-drop
bus alternative): Heltec↔C6 and Heltec↔C5, each 3 wires (TX/RX/GND), no shared bus, no addressing,
no arbitration. This reuses a pattern the project already has real experience with (the GPS/NMEA
UART driver) rather than introducing I2C-slave-mode support that's never been used on any of
these targets.

**GPS is wired to Heltec only** (confirmed: single owner, not a TX fan-out to all three boards).
Forwarding a fix to C6/C5 over their UART links is possible if some future capability on those
boards ever needs one, but nothing in the current capability set does — wardriving aggregation
(the only consumer of GPS fixes) happens entirely on Heltec, since that's where every scan result
from every source converges before being logged. C6/C5 in cluster mode never need to know the
current fix.

**Exact GPIO pin assignments for both UART links are not decided here** — this needs the physical
boards in hand, matching this project's "confirm, don't assume" discipline for every other board's
pinout (see each board's `docs/hardware/*/README.md`). Deferred to Phase 9 step 1. Constraints to
respect when picking pins: avoid GPIO0/4/5/8/9/15 (strapping/JTAG, per `CLAUDE.md`), and avoid
UART peripherals already committed to GPS on Heltec's *standalone* build (GPIO17) — cluster mode
is a separate build variant so reuse is not automatically unsafe, but picking distinct pins avoids
having to reason about it board-by-board.

## Inter-board protocol (new — distinct from PROTOCOL.md)

This is a physically wired, co-located, non-RF link between boards the same person possesses
simultaneously — it does not need the BLE protocol's cryptographic protections (X25519 pairing,
AES-256-GCM sessions). This matches the project's already-accepted threat model: physical
possession of a device is already fully compromising (see [PLAN.md](PLAN.md)'s "Deferred: hardware
hardening" and [DECISIONS.md](DECISIONS.md)) — a wired lab/backpack link between boards the owner
already holds adds no new exposure a cryptographic layer would meaningfully close.

No third-party library — hand-rolled and host-tested, same convention as `framing.c`/the `cbor_*`
codecs (see `CLAUDE.md`'s conventions). **Frozen wire format, 2026-09-26** (this supersedes the
earlier loose sketch — implement exactly this, do not re-derive):

### Frame layout

```
byte 0-1: SOF        0xFE 0xED (resync marker, not covered by the CRC)
byte 2:   msg_type   uint8
byte 3-4: payload_len uint16, little-endian, max 512
byte 5..: payload    payload_len bytes, shape depends on msg_type (below)
last 2:   crc16      CCITT-FALSE (poly 0x1021, init 0xFFFF), over [msg_type, payload_len, payload]
                      — not over the SOF bytes, transmitted little-endian (crc_lo, crc_hi),
                      matching payload_len's byte order
```

Receiver is a byte-at-a-time state machine: scan for `0xFE 0xED`, then read `msg_type`, then
`payload_len` (reject/resync if it exceeds 512), then that many payload bytes, then the CRC. On
any validation failure (bad CRC, oversized length), drop back to scanning for the next SOF
**advancing only one byte at a time** from wherever the search resumes — never skip a
declared-but-untrusted frame length's worth of bytes, since a corrupted length field is exactly
the failure mode this must recover from.

### Message types

- **`0x01 WORKER_HELLO`** (worker → coordinator, sent unconditionally every 1000ms — doubles as
  both a link-up announcement and a heartbeat, so no separate status/heartbeat message type is
  needed). Payload: `band: u8` (`1` = 2.4GHz, `2` = 5GHz). The coordinator considers a worker
  present if it has received one within the last ~3000ms (3 missed beacons), absent otherwise —
  see "Open questions" for what absence should do.
- **`0x02 SCAN_CONFIG_SET`** (coordinator → worker, sent on every mode change: wardriving start,
  wardriving stop, or before/after a manual one-shot scan). Payload: `mode: u8` (`0` = idle, `1` =
  continuous/wardriving-style streaming, `2` = manual one-shot, armed for exactly one pass),
  `dwell_mode: u8` (`0`/`1`/`2`, mirrors today's single-board Normal/Aggressive/Speed-based
  swelling enum), `band_filter: u8` (only meaningful to the 5GHz worker: `0` = n/a, `1` =
  fast/non-DFS, `2` = full — mirrors today's `wifi_band` values, minus the `2.4ghz` option since
  that's simply which worker you're talking to now, not a per-worker setting).
- **`0x03 SCAN_RESULT`** (worker → coordinator, one per discovered AP, streamed as found in either
  `mode=1` or `mode=2`). Payload: `ssid_len: u8`, `ssid: bytes[ssid_len]` (max 32), `bssid: u8[6]`,
  `rssi: i8`, `channel: u8`, `phy: u8`, `auth: u8` — `phy`/`auth` use the same enumerations
  `components/feb_protocol/cbor_wifi_scan.h` already defines, so no new encoding is invented for
  values that cross back into the Flipper-facing wire format later.
- **`0x04 SCAN_BATCH_DONE`** (worker → coordinator, only in `mode=2`, marks the end of one manual
  scan pass). Payload: `count: u16` (how many `SCAN_RESULT` frames were sent for this pass) — the
  coordinator's bounded wait for a manual `wifi_scan` reply (see "Composite behaviors") ends on
  this message or its own timeout, whichever comes first.

Four message types total, not five — `worker_hello`/`worker_status` merged into one beacon, and
`scan_manual_request` folded into `scan_config_set`'s `mode` field rather than a separate type.

**Numeric enums, and where they come from:** `SCAN_RESULT`'s `auth` field reuses the pinned
ESP-IDF v5.5.2 `wifi_auth_mode_t` numeric values directly (PROTOCOL.md's `auth` string list is
already stated to match that enum's order). `phy` has no existing ESP-IDF or PROTOCOL.md numeric
source (the BLE wire only ever carries it as text) — `feb_cluster_link.h` assigns `0..3` matching
PROTOCOL.md's listed string order (`11b`/`11g`/`11n`/`11ax`). `dwell_mode`/`band_filter` are in the
same situation as `phy` (their BLE-facing analogues, `wifi_swelling`/`wifi_band`, are text-only) —
newly assigned in-header, ordered to match those existing string lists. **Not yet resolved**:
when the coordinator eventually needs to turn a worker's numeric `phy`/`dwell_mode`/`band_filter`
back into the text PROTOCOL.md expects for anything crossing the Flipper-facing wire, that
conversion table is new shared knowledge, currently living only in
`components/feb_cluster_link/cluster_link.h`'s comments — worth a cross-reference at minimum,
possibly promoting the string tables themselves into this header, when the coordinator dispatch
step actually needs to do that conversion. Flagged, not solved here.

## Composite behaviors

- **Manual `wifi_scan`** (Flipper-triggered, one-shot): coordinator sends `scan_manual_request` to
  both C6 and C5, waits (bounded timeout) for both `scan_manual_done`, merges both bands' results,
  truncates to the top 32 by RSSI (matching today's existing single-board cap), replies to the
  Flipper as one `status` — indistinguishable on the wire from today's single-board response.
- **`wardriving`**: at `start`, the coordinator forwards per-band config to C6/C5
  (`scan_config_set`); both stream `scan_result` continuously. The coordinator geotags each
  incoming hit with its own current GPS fix (unchanged fix-dependency rule — a hit arriving while
  the coordinator isn't reporting `state = "fix"` is discarded, exactly like today's single-board
  behavior), dedups, and appends to its own flash-backed circular log — now merging three logical
  input streams (its own `ble_scan` hits, plus two forwarded `wifi_scan` streams) instead of one.
  Backlog drain/CSV export to the Flipper is unchanged from the coordinator's point of view; it
  already owns that logic today.
- **`meshcore_scan`** is unaffected — stays exactly as it is today, Heltec-only.

## What doesn't change

- **`docs/PROTOCOL.md` needs zero changes.** The Flipper only ever pairs with one `board_id`
  (Heltec's), sees one merged `capability_response`, and issues the same `command`/`status` shapes
  it already does. Cluster orchestration is entirely invisible to the Flipper side — it has no
  concept of a cluster at all.
- Pairing ceremony, factory reset, LED indicators, session crypto: unchanged, all live on Heltec
  exactly as its existing standalone firmware already implements them.

## What's new

- The inter-board framing/message set above (this doc's own contract, not `PROTOCOL.md`'s).
- New cluster-mode firmware variants for C6 and C5, alongside their existing standalone builds —
  **standalone stays the default/unaffected build; cluster mode is a separate, opt-in
  configuration** (confirmed with the user: no regression to solo operation for either board).
- New coordinator-side dispatch/aggregation logic on Heltec's `main.c`: worker presence detection,
  config forwarding, manual-scan request/merge/reply, merged capability reporting.

## Open questions / deferred (not resolved by this design)

- **`SCAN_CONFIG_SET`'s `dwell_mode = speed_based` cannot actually work on a worker as specified**
  (found during C6 cluster-worker implementation, 2026-09-26). Speed-based dwell on the standalone
  boards reads a live on-board GPS fix; C6/C5 workers have no GPS (GPS is Heltec-only, see
  "Physical wiring" above) and the frame carries no speed/aggressive field for the coordinator to
  forward one. Current C6 worker firmware treats `speed_based` identically to `normal` as a
  stopgap. Needs a decision: extend `SCAN_CONFIG_SET` with a speed or aggressive-flag field the
  coordinator fills in from its own GPS fix, or accept speed-based dwell only ever applies to a
  scan source running directly on the coordinator.

- **Worker-absence behavior.** What the coordinator reports to the Flipper if a worker's UART link
  is down or a worker hasn't sent `worker_hello` yet — degrade gracefully to a smaller
  `capability_response` (only report `wifi_scan` for whichever band actually responded, or drop it
  entirely if neither has), versus reporting the full feature set and returning an explicit error
  per-request. This is a product decision, not an engineering one — flagged for the implementation
  step, not decided here.
- **Coexistence, unchanged risk carried forward**: Heltec's own combo-radio (BLE-central +
  `ble_scan`) is already proven safe; Heltec's LoRa radio running concurrently with that combo
  radio activity has never had its own coexistence sweep (`docs/BACKLOG.md` BL19) — this design
  neither introduces nor resolves that gap.
- **Whether cluster-mode C6/C5 need any persisted state at all.** Current expectation: no — a
  scan-only worker with no pairing/session has nothing to persist, so no factory-reset-equivalent
  gesture should be needed on either board in cluster mode. Confirm during implementation rather
  than assuming.
- **Worker presence re-detection**: recommend the coordinator always re-detects workers fresh at
  its own boot via `worker_hello` rather than persisting "a worker was present last time" — matches
  this project's existing preference for deriving stateless facts at boot instead of storing them
  (e.g. `board_id` itself is never written to NVS).

## Cross-references

- Board-specific pin/radio facts once wiring is decided: each board's own
  `docs/hardware/*/README.md`.
- Roadmap step breakdown and "done when" bars: [PLAN.md](PLAN.md)'s "Phase 9" section.
- Accepted physical-possession threat model this design's "no crypto on the wired link" choice
  relies on: [DECISIONS.md](DECISIONS.md), [PLAN.md](PLAN.md)'s "Deferred: hardware hardening".
