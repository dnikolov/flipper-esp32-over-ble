# Wardriving Screen Redesign (Stopped/Running split, WiFi Swelling, country code, GPS speed)

**Status: implemented and hardware-verified 2026-09-21.** New standalone roadmap entry in
`docs/PLAN.md`'s "Roadmap phases" list, proceeding in parallel with Phase 4/6 (same override
pattern those two already carry) rather than waiting on either. `esp32/`, `heltec/`, and the
Flipper FAP (`fbt.cmd fap_flipper_esp32_over_ble`) all build clean; both host-native codec test
suites (`tests/esp32/build.ps1`, `tests/flipper/build.ps1`) pass in full (`tests/flipper`:
430/430); `tools/check_shared_headers.py` confirms no drift between `components/feb_protocol/`
and the Flipper's mirrored codec. Flashed to both physical boards; two real bugs were found
during hardware testing and fixed the same day — see "Hardware-testing fixes (2026-09-21)"
below. All reported symptoms (WiFi+BLE wardriving start, GPS screen fix/coordinates) confirmed
working after the fixes.

## Hardware-testing fixes (2026-09-21)

Both bugs below were the same failure class — a fixed-size buffer sized for the *pre-redesign*
worst case, not rechecked once this redesign's new fields grew that worst case past it — and
both were completely silent at the point of failure (no decode error, no malformed-wire
symptom): the encode call itself returned 0 and the caller's own `if (len == 0)` fallback path
fired, sending a well-formed but unhelpful `internal_error`/nothing at all rather than any
signal pointing at "buffer too small." This project has hit this exact class twice before (the
2026-09-07 `cmd_payload_buf` sizing bug, and 2026-09-13's `.bss` OOM) — worth treating "a shared
or capability-specific buffer's sizing comment" as something to actively re-verify, not just
trust, whenever a message shape it covers grows.

1. **WiFi+BLE wardriving silently failed to start** (WiFi-only worked). Root cause: the
   Flipper's shared `cmd_payload_buf`/`cmd_ciphertext_buf` (`FEB_CMD_PAYLOAD_MAX_LEN`, every
   capability's outgoing command envelope) stayed at 160 bytes — sized for the pre-`wifi_swelling`/
   `country` wardriving-start shape. The wardriving-arguments sub-buffer
   (`FEB_WARDRIVING_CMD_PAYLOAD_MAX_LEN`) was correctly bumped to 192 when those two fields were
   added, but nobody re-checked that the *outer* envelope buffer that arguments gets embedded
   into was still big enough once BLE's own fields (`ble_window_ms`/`ble_interval_ms`, plus a
   longer `sources` array for `ble_passive`) were added on top — WiFi-only's smaller arguments
   still fit under 160, masking the bug until BLE was added to the mix.
   `send_wardriving_start_command()`'s outer `feb_cbor_encode_command_payload()` call returned 0
   (`FURI_LOG_W`: `"wardriving start: payload encode failed"`), so nothing was ever sent to the
   ESP32 at all. Fixed: `FEB_CMD_PAYLOAD_MAX_LEN` 160 → 224 (`flipper/flipper_esp32_over_ble.c`).
2. **GPS screen never showed a fix or coordinates, even with a real fix.** Root cause: the
   ESP32's `handle_gps_command()` result buffer (`result_buf[128]`) wasn't bumped when
   `speed_e1_kmh` was added to the `gps` result. Real-world lat/lon/timestamp/altitude values
   need the full 5-byte CBOR uint encoding (not the 1-2 byte form small test values use), so the
   true worst case is ~133 bytes — over 128, and *not* just a rare edge case: it failed on every
   single reply once a real fix existed. `feb_cbor_encode_gps_result_payload()` returned 0, so
   the ESP32 sent `internal_error` back for every `gps` poll instead of a `status` reply — the
   Flipper side decoded that error correctly (`FURI_LOG_W`: `"runtime error received:
   code='internal_error'"`) but had no `PendingCommandKind` entry for `gps` (by design — see
   `post_gps_status()`'s own comment) to route it anywhere, so it was silently dropped with no
   `gps`-specific symptom to grep for. This is why a straightforward log search for "gps" showed
   nothing on either side. Fixed: `result_buf` 128 → 192 (`esp32/main/main.c`).

Diagnostic note for next time: both bugs were only found by capturing live serial/CLI logs from
both boards during physical reproduction (`esp32-monitoring`/`flipper-monitoring` agents) and
grepping broadly (not just for capability-name keywords like "gps" — the actual signal for bug
2 was the generic `"runtime error received: code=..."` line, which doesn't mention `gps`
anywhere). Static code review of the encode/decode/dispatch logic found nothing wrong in either
case, because nothing *was* wrong there — both were pure buffer-sizing gaps.

Reached via direct design discussion with the user, 2026-09-21. Supersedes
[docs/UI_REDESIGN.md](UI_REDESIGN.md)'s "Wardriving" menu-item subsection (that file's own
target-design text for Wardriving is now stale; see the pointer added there). Does **not**
touch `UI_REDESIGN.md`'s "Scan (new)" section — the separate wifi_scan/ble_scan picker screen
keeps its own still-backlogged five-mode redesign, untouched by this doc.

## Why

Today's single Wardriving screen (`flipper/flipper_esp32_over_ble.c`'s `draw_wardriving_screen`
+ `WardrivingSourceMode`) couples WiFi/BLE source selection, WiFi cadence, and BLE active/
passive into one 6-value enum cycled with Left/Right, with no persistence across app restarts
and no control over WiFi per-channel scan dwell time at all. The user wants a real Stopped/
Running screen split, independently adjustable and persisted settings, and two genuinely new
capabilities: a WiFi scan-dwell ("swelling") control with a speed-triggered mode, and a WiFi
regulatory country-code toggle — plus GPS speed display, needed both for the UI and as the
input to speed-triggered swelling.

## Scope boundaries — explicitly out

- The "Scan" menu screen (`AppScreenScan`) and its own five-mode BLE-active/passive redesign:
  untouched.
- The manual `wifi_scan`/`ble_scan` one-shot capabilities' own `esp_wifi_scan_start()` call
  site (`esp32/main/main.c`, `handle_wifi_scan_command()`): untouched. Dwell time is scoped to
  wardriving's own WiFi scan calls only. Country code is a global radio-level ESP-IDF setting,
  though — once wardriving sets it, any later manual `wifi_scan` observes it too until changed
  again. This is inherent to how `esp_wifi_set_country_code()` works, not a gap in this design.
- Heltec (`heltec/main/main.c`) does not implement the `wardriving` capability today (only
  `wifi_scan`/`ble_scan` were ported per Phase 4). The shared-component protocol change below
  must keep `heltec/`'s build green; no new Heltec behavior is added by this doc.
- No GPS backfill, HDOP threshold, or altitude work — unrelated, already-backlogged items.
- `ble_passive` remains known-broken in current firmware (observer-only BLE scanning omits BLE
  devices entirely rather than doing a real passive scan — see `docs/PROTOCOL.md`). This design
  exposes the existing `ble_passive` wire source as a UI toggle as-is; it does not fix it.

## Wire protocol additions

### `wardriving` command: two new `arguments` fields

Appended to the existing field order — `action`, `sources`, `wifi_interval_ms`,
`ble_window_ms`, `ble_interval_ms`, **`wifi_swelling`, `country`** — additive only, matching
this protocol's established "append, never reorder" convention (e.g. how `gps`'s
`altitude_dm_offset` was appended rather than inserted).

| Field | Type | Meaning |
| --- | --- | --- |
| `wifi_swelling` | text string | `"normal"` \| `"aggressive"` \| `"speed_based"`. Required when `"wifi"` is in `sources`; must be absent otherwise. Any other value is `invalid_command`. |
| `country` | text string | `"BG"` \| `"RoW"`. Required when `"wifi"` is in `sources`; must be absent otherwise. Any other value is `invalid_command`. |

**Semantics, ESP32 side:**

- `country = "RoW"` → `esp_wifi_set_country_code("01", false)` — today's implicit default
  (world-safe mode: channels 1-11, `WIFI_COUNTRY_POLICY_AUTO`). No behavior change from today.
- `country = "BG"` → `esp_wifi_set_country_code("BG", false)` — channels 1-13, active scan
  across the whole range, no passive carve-out (matches the regulatory data already recorded in
  `docs/BACKLOG.md`'s country-code item: `country BG: DFS-ETSI, (2400 - 2483.5 @ 40), (100 mW)`,
  no active/passive distinction). Applied **once**, at wardriving start (a radio-global
  setting, not a per-scan-cycle parameter) — not re-applied on every WiFi re-arm.
- `wifi_swelling = "normal"` → today's behavior: `scan_time` left zeroed/default in the
  `wifi_scan_config_t` passed to `esp_wifi_scan_start()`.
- `wifi_swelling = "aggressive"` → `scan_time.active.min = scan_time.active.max = 85` (ms),
  applied to every channel the current `country` setting scans (1-11 for RoW, 1-13 for BG).
- `wifi_swelling = "speed_based"` → starts at normal dwell. On each WiFi re-arm
  (`wardriving_wifi_interval_cb()`), the ESP32 reads its own last-parsed GPS speed
  (`location_get_fix()`'s new `speed_e1_kmh` field, itself derived from `RMC` — see below) and
  switches the *next* scan's dwell to the aggressive 85ms values once speed ≥ 10 km/h, back to
  normal once speed drops below 8 km/h. This 2 km/h hysteresis band exists purely to avoid
  flapping the scan config right at the threshold; not user-configurable in v1. No fix / speed
  unknown → stays at normal dwell. No wire round-trip is needed for this mode after `start` —
  the ESP32 already owns its own GPS reading, so there's nothing for the Flipper to tell it.
- Applied only in wardriving's own scan-start call sites (`wardriving_start_internal()` and
  `wardriving_wifi_interval_cb()` in `esp32/main/main.c`) — `handle_wifi_scan_command()`'s
  manual-scan call site is untouched, per "Scope boundaries" above.

### `gps` capability result: one new field

Appended after `altitude_dm_offset` (same rule as above):

| Field | Type | Meaning |
| --- | --- | --- |
| `speed_e1_kmh` | unsigned integer | Ground speed in km/h, scaled ×10 and truncated (same scaling convention as `hdop_e1`) — e.g. `12.3` km/h encodes as `123`. Derived from the most recent valid `RMC` sentence's speed-over-ground field (knots) × 1.852. Present only on `state = "fix"`, same gating as the rest of `result`. |

`RMC`'s speed field is not parsed anywhere in this codebase today (`nmea_parse_rmc()` reads
time/status/date only) — this is new parsing, not a wire-only change.

### Codec/struct changes

Both `components/feb_protocol/cbor_wardriving.h`/`.c` (used by `esp32/`, and transitively by
`heltec/` which must keep building) and the Flipper's mirrored copy
(`flipper/cbor_wardriving.h`/`.c`) get the same two new fields added to
`feb_wardriving_command_payload_t`, same text-span pattern already used for `action`/`sources`,
with a `has_wifi_swelling`/`has_country` presence flag pair (mirroring how
`has_wifi_interval_ms` already works) — action-dependent presence validation
("required iff wifi in sources") stays a caller concern in `main.c`/
`flipper_esp32_over_ble.c`, matching this codec's existing documented split (see
`cbor_wardriving.h`'s own top-of-file comment).

`components/feb_protocol/cbor_gps.h`/`.c` and its Flipper mirror get `speed_e1_kmh` added to
the `gps` result struct/encode/decode path, same pattern as `altitude_dm_offset`.

`tests/vectors/vectors.h` and both sides' host-native codec tests need new vectors for the two
wardriving-start fields and the new gps-result field. Do not hand-edit the generated vectors
file directly — locate whatever generator produced it first.

## Flipper UI

### Home menu

- `HomeMenuPublish` moves to immediately after `HomeMenuWardriving` in menu order (currently
  last). `HomeMenuWardriving` is already index 0 — unchanged.
- **Force-jump to Wardriving on connect:** the moment a session transitions to active with the
  board advertising `wardriving`, the Home cursor is forced to `HomeMenuWardriving`
  unconditionally, even if the user was sitting on Settings/About/Publish/Legacy at that
  moment. This is in addition to, not a replacement for, the existing "clamp to first visible
  item if the current selection stops being visible" safety net.

### Wardriving: two screens instead of one

`AppScreenWardriving` is replaced by `AppScreenWardrivingStopped` and
`AppScreenWardrivingRunning`. Navigating in from Home always lands on whichever matches the
Flipper's current knowledge of run state (`wardriving_running_known && wardriving_running`);
the existing start/stop-ack handling flips between them exactly as it already flips the
single screen's displayed state today.

**Running screen** — unchanged content from today's running-state rendering: state text,
"Recs: N Backlog: M" or "Recs: N Live" line, GPS fix suffix, and the existing single
"Last WiFi: ..." / "Last BLE: ..." lines (last-seen only, not a history — confirmed with the
user, no new scrollable multi-row list). Footer action is "Stop" (already effectively true
today); OK sends the stop command as now.

**Stopped screen** — new scrollable settings list. Up/Down moves the highlighted row;
Left/Right changes that row's value; OK triggers start (the existing `gps_delayed`-derived
"start" vs. "start (delayed)" footer label logic is unchanged). Each row is capability-gated
the same way today's single Source line is gated by `both_sources_advertised`:

| Row | Values | Shown when |
| --- | --- | --- |
| Mode | WiFi+BLE / WiFi / BLE | board advertises both `wifi_scan` and `ble_scan` (fixed to the one supported source, no row, otherwise) |
| WiFi Swelling | Normal / Aggressive / Speed-based | board advertises `wifi_scan` |
| WiFi Cooldown | 5s / 2s / 0s | board advertises `wifi_scan` — reuses the existing `wifi_interval_ms` values (5000/2000/0) already hardcoded in today's enum; no new wire work for this row |
| BLE Mode | Active / Passive | board advertises `ble_scan` (Passive is the known-broken `ble_passive` source, exposed as-is per "Scope boundaries" above) |
| Country | BG / RoW | board advertises `wifi_scan` |

These five rows are capability-gated only (shown whenever the board supports the relevant
radio), not re-gated by whichever `Mode` value happens to be currently selected — e.g. the
WiFi Swelling row stays visible even while `Mode = BLE` is selected. Simpler, and avoids rows
appearing/disappearing as the user cycles `Mode` itself.

`WardrivingSourceMode`'s single coupled 6-value enum is removed, replaced by five independent
fields on `Esp32App` (mode, swelling, cooldown, ble_mode, country). `send_wardriving_start_command()`
is rewritten to build `sources`, `wifi_interval_ms`, `wifi_swelling`, `country`, and the
unchanged `ble_window_ms`/`ble_interval_ms` (100/500 defaults, not exposed in this UI) from
these five fields.

### GPS screen

New Speed row: `speed_e1_kmh / 10.0`, one decimal, km/h — `--` when not `state = "fix"`, same
pattern as the existing coordinate/time rows.

### Persistence

New flat `key=value` file, `wardriving_settings.txt`, at the app data root (same plumbing-file
convention as `wdgwars_credentials.txt`/`wardriving_publish_result.txt` — no JSON decoder in
this codebase). Fields: `mode=`, `wifi_swelling=`, `wifi_cooldown_ms=`, `ble_mode=`,
`country=`. Loaded once at app init (same Storage-API read pattern already used for the
capability cache); rewritten immediately on every row-value change. **Global, not scoped per
paired board** — a deliberate simplification for v1; revisit if per-board scan tuning turns out
to matter in practice.

## Decisions made during design, with rationale

1. **Country code bundled into this same pass**, not deferred — the user's explicit call,
   overriding this doc's own initial draft question about scoping it out. Both dwell-time and
   country code touch the same `wifi_scan_config_t` setup, so building them together avoids a
   second pass through the same code paths.
2. **Speed-based swelling decides on-device (ESP32), not on the Flipper.** The ESP32 already
   owns the GPS reading; round-tripping speed to the Flipper and back for a policy decision the
   ESP32 could make locally would add wire chatter for no benefit. The Flipper only ever sends
   `wifi_swelling = "speed_based"` once, at `start`.
3. **Running screen keeps single last-seen lines, not a history list** — matches today's
   behavior exactly; a multi-row scrollable capture history remains a distinct, unscoped
   backlog item (`docs/BACKLOG.md`'s "Consolidating/grouping wardriving records" note).
4. **Settings rows are capability-gated only, not mode-gated** — chosen for UI stability
   (rows don't reflow as `Mode` is cycled) over strict "only show what's currently relevant."
5. **Persisted settings are global, not per-board** — simplest v1 shape; flagged as a
   deliberate simplification, not a considered-and-rejected alternative.
6. **Home cursor force-jumps to Wardriving on connect** — the user's explicit call, on top of
   the existing clamp-to-first-visible-item safety net that was otherwise judged sufficient.

## Open items

- `ble_passive`'s underlying firmware bug (omits BLE devices instead of passive-scanning them)
  stays open, tracked wherever `docs/BACKLOG.md`/`docs/PROTOCOL.md` already track it — exposing
  it as a UI toggle here does not change its status.
- Per-board settings persistence (item 5 above) — not scoped, revisit if requested.
- Hysteresis band width (2 km/h) is a first guess, not validated against real driving data —
  revisit once hardware-tested.
- **ESP32-side `wifi_swelling`/`country` are not persisted across the button-toggle or
  boot-autostart wardriving-start paths** (`wardriving_start_internal()`'s other three call
  sites) — only a `start` command sent by the Flipper carries them. Button-toggle and
  boot-autostart always run at Normal/RoW regardless of whatever the Flipper's own persisted
  `wardriving_settings.txt` last had selected. `wardriving_persist.h`'s NVS blob was
  deliberately not extended for this pass (out of scope — the Flipper-side file is this
  redesign's persistence story). Real gap if per-board ESP32-side persistence of these two
  settings is ever wanted for the autostart case specifically.
- **`esp_wifi_set_country_code()` failure is treated as non-fatal** — wardriving start still
  proceeds (world-safe default remains usable) rather than aborting with an error. Not specified
  either way by this doc originally; an implementation judgment call, not revisited since it's a
  reasonable default.
- **GPS screen's Speed value was fit into the existing Alt row's placeholder** (which already
  reserved `"Speed: --"` text) as `"Alt: %.1fm  Spd: %.1f km/h"` rather than a new, separate row
  — the screen's fixed 4-row layout had no space left. Functionally equivalent to what this doc
  originally specified; flagged in case a dedicated row is wanted later (would need a small
  layout pass).
