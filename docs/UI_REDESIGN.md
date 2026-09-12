# Flipper FAP UI redesign

**Status: design-only, 2026-09-12. No code has been written against this design, and it has
no assigned phase/step in [PLAN.md](PLAN.md) yet** — placement was explicitly deferred rather
than decided (see "Open items" below). Do not start implementation from this doc without
first resolving that placement, per [CLAUDE.md](../CLAUDE.md)'s "don't implement roadmap
steps out of order" convention.

This replaces today's flat, button-shortcut Main screen with a menu-driven Home screen.
Reached via a grill-me design session with the user; decisions and rationale are recorded
below rather than left in chat, per this project's documentation conventions.

## Current-state baseline (as of 2026-09-12, for contrast)

- `AppScreen` enum has exactly 4 screens: `AppScreenMain`, `AppScreenWifiScanResults`,
  `AppScreenBleScanResults`, `AppScreenWardriving` (`flipper/flipper_esp32_over_ble.c`).
- Single `ViewPort`/`AppEvent`-queue architecture — no `ViewDispatcher`/scene manager.
- On the Main screen, Left/Right/Up are hardcoded to `wifi_scan`/`ble_scan`/Wardriving
  respectively; nothing is capability-list-driven at the menu level.
- `wifi_scan`/`ble_scan` are one-shot, manually-triggered scans. `wardriving` is continuous,
  interval-based, dual-source (Wi-Fi+BLE) capture with flash-log + WiGLE CSV persistence.
- BLE active scanning is hardcoded on (`passive=0`) at all four `ble_gap_disc()` call sites in
  `esp32/main/main.c` (confirmed by code search 2026-09-12) — there is no active/passive
  runtime toggle anywhere today.
- GPS is a fixed-coordinate stub behind a swappable `location_get_fix()` interface. No real
  GPS hardware is wired to the board. The ESP32 has no RTC; the Flipper's own clock is the
  only real wall-clock source in the whole system.

## Target design

### Navigation model

- **Home is one screen**, not a transition between two screens: a compact session-indicator
  strip (reuses today's existing status text — waiting/authenticating/failed/session-active,
  plus the capability line) with a menu list below it, in the same screen.
- **Full replacement of direct button shortcuts.** Up/Down navigate the menu, OK selects.
  Left/Right/Up no longer do anything capability-specific on Home.
- **Settings and About are always present** in the menu regardless of connection state.
- **Wardriving, Scan, and GPS appear only once a session is active AND the connected board's
  capability registry supports them** — hidden entirely (not grayed) when unsupported. Scan's
  own mode list further narrows to what the board supports (e.g. a WiFi-only board only ever
  shows the WiFi-only mode).
- **Reconnect/disconnect while inside any submenu or results screen: stay put.** Show a
  "connection lost" message on whatever screen is active; do not force navigation back to
  Home. Matches existing Wardriving-screen behavior (results/scroll position already survive
  a reconnect today).
- No menu-level pre-emption of the Scan/Wardriving radio-sharing conflict: if both want the
  same source concurrently, today's existing `busy` rejection at the command layer surfaces
  as-is. Chosen deliberately as the simplest option for v1 over graying out conflicting menu
  items (which would also need BL02 — wardriving status-on-reconnect — fixed first).

### Menu items

#### Wardriving

Unchanged from today's screen: source toggle (Wi-Fi/BLE/both, shown only while stopped and
only when the board supports both), Start/Stop, live record count, backlog-drain count, Back.

New: a GPS fix/no-fix icon. Since GPS is a stub, this always honestly shows "no fix
(simulated)" — never claims a real fix — until real GPS hardware lands.

#### Scan (new)

A live-view-only sibling of Wardriving: reuses Wardriving's capture engine and wire commands,
but does **not** persist to the flash log or CSV — display-only, current-session-only. This is
deliberately *not* a new/separate capability and *not* a rename of `wardriving` — it's the
same engine minus persistence.

Five modes, cycled via Left/Right:

1. BLE-active + Wi-Fi
2. BLE-passive + Wi-Fi
3. Wi-Fi-only
4. BLE-active-only
5. BLE-passive-only

Start/Stop control, same as Wardriving.

**Prerequisite not yet built:** a runtime BLE active/passive toggle. Active scanning is
hardcoded on in every `ble_gap_disc()` call site on the ESP32 today, for both `ble_scan` and
`wardriving`'s capture engine — there is no way currently to request passive scanning. Modes
2 and 5 above cannot be built until this toggle exists on both the wire protocol and firmware.
See "Open items" below.

("Long polling"/"short polling" from the original sketch was a misnomer, corrected during
design: the real distinction is BLE active vs. passive scan type (whether a scan-response
carrying the device name is solicited), not a window/interval duty-cycle preset. No
"long/short poll" concept or terminology exists anywhere in the codebase or docs today.)

#### GPS (new)

Built now despite no real GPS hardware, with the stub made explicit rather than hidden or
omitted:

- **Coordinates** — the fixed stub value, clearly labeled as simulated/no real fix.
- **Current time** — explicitly the Flipper's own clock (the ESP32 has no RTC), labeled as
  such, not presented as GPS-derived.
- **Speed** — shown as `--`, not a misleading `0.0`. Structurally meaningless without two
  distinct real fixes over time, which a fixed-coordinate stub can never produce.

#### Settings (placeholder)

No content specified yet — deliberately deferred rather than scoped in this pass. Candidates
surfaced during design discussion, not committed to:

- Scan-mode persistence across app launches.
- The BLE active/passive toggle's UI home (may belong inside the Scan screen instead of
  Settings — undecided).
- Wardriving interval configuration (`wifi_interval_ms`/`ble_window_ms`/`ble_interval_ms`) —
  the app still has no form/settings-entry widget anywhere.
- Board/pairing management (unpair, switch between multiple paired boards, disconnect current
  board) — all open backlog items with no UI home today.

#### About (placeholder)

No content specified yet. Obvious future candidates: firmware version, `board_id`, protocol
version — all already available to the app via the existing capability/pairing records.

## Decisions made during design, with rationale

1. **Sequencing:** design now, implementation later. Not yet placed in `PLAN.md`'s phase
   roadmap — Phase 3 still has open P0/P1 items (see [BACKLOG.md](BACKLOG.md)) and pending
   hardware-acceptance criteria, and `CLAUDE.md`'s conventions forbid working roadmap steps
   out of order.
2. **Scan vs. Wardriving:** Scan is Wardriving's capture engine minus persistence, not a new
   capability and not a rename of Wardriving — chosen as the smallest addition with the
   clearest mental model ("Scan" = look now, "Wardriving" = record a drive).
3. **BLE mode semantics:** confirmed by reading `esp32/main/main.c` — active vs. passive scan
   type, not a poll-duration preset.
4. **Passive BLE scanning stays in scope for v1's design** (not deferred to a later redesign
   pass), with its runtime toggle logged as a backlog prerequisite rather than blocking the
   design itself.
5. **GPS screen is built now**, with honest stub labeling, rather than hidden or replaced with
   a placeholder message — reuses the same "no fix (simulated)" signal as Wardriving's new
   icon instead of inventing two different stub conventions.
6. **Settings and About are placeholders** — deliberately not scoped in this pass.
7. **Capability-gating:** hide (not disable) unsupported menu items. Wardriving/Scan/GPS also
   require an active session to appear at all; Settings/About do not.
8. **Full replacement of direct button shortcuts** by the menu — mixing "sometimes a raw
   button does a thing, sometimes you need a list" was rejected as confusing.
9. **Menu grows/shrinks with connection+capability state** rather than being a separate screen
   navigated into — Settings/About are the only always-present items.
10. **On disconnect while inside a submenu:** stay put, show a message. Don't force navigation.
11. **Home is one combined screen** (indicator + list), not two screens with an automatic
    transition between them — avoids an auto-navigating-out-from-under-the-user pattern.
12. **No menu-level pre-emption of the Scan/Wardriving radio conflict** — simplest option,
    accepted for v1.

## Open items this design creates (not yet actioned)

- **Add to [BACKLOG.md](BACKLOG.md):** a runtime BLE active/passive toggle (prerequisite for
  Scan's two passive modes) — new item, not previously tracked.
- **Correct [BACKLOG.md](BACKLOG.md):** its "extending [active scanning] to `wardriving`'s own
  capture engine" note is stale — confirmed already done in code as of the 2026-09-11
  reconnect-stall fix (all four `ble_gap_disc()` call sites already set `passive=0`). Only the
  runtime on/off *toggle* itself remains open, not the extension to wardriving.
- **Decide this design's placement in [PLAN.md](PLAN.md)** (which phase/step) before any
  implementation begins.
- **[BACKLOG.md](BACKLOG.md)'s existing "Adopt a real `ViewDispatcher`/scene-manager
  architecture" item becomes a hard prerequisite** for implementing this design, not just a
  nice-to-have — today's single `ViewPort`/`AppEvent`-queue pattern cannot support a real
  navigable menu list.
