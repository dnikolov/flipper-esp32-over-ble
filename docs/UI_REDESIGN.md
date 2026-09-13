# Flipper FAP UI redesign

**Status: Home/menu shell implementation complete and hardware-verified 2026-09-13.** Tracked as **Phase 3a** in [PLAN.md](PLAN.md). The Home screen, capability-gated menu, and reconnect-stays-put behavior are all working end-to-end; the "Current-state baseline" section below describes exactly what's built vs. still open against the target design further down this file (notably: the `ViewDispatcher`/scene-manager architecture step was skipped, and the Scan screen is a placeholder picker, not yet the five-mode design). See [docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md)'s 2026-09-12 entry for the implementing commits and 2026-09-13 entry for hardware verification summary.

This replaces today's flat, button-shortcut Main screen with a menu-driven Home screen.
Reached via a grill-me design session with the user; decisions and rationale are recorded
below rather than left in chat, per this project's documentation conventions.

## Implementation sequencing

This pass was intentionally staged as a prerequisite-first refactor, not a one-shot UI rewrite.
Actual outcome as of 2026-09-12 (see "Current-state baseline" below for detail):

1. **Architecture prerequisite: NOT done, and skipped rather than deferred.** The Home shell
   (step 2) was built directly on the existing single `ViewPort`/`AppEvent` pattern; there is no
   `ViewDispatcher`/scene-manager anywhere in `flipper/flipper_esp32_over_ble.c` today. This
   contradicts this file's own original sequencing and [BACKLOG.md](BACKLOG.md)'s framing of the
   architecture change as a hard prerequisite — flagged as a documentation-vs-reality mismatch,
   not silently resolved.
2. **Home shell: done.** Single Home screen with a session/status header and a capability- and
   session-gated `HomeMenuItem` list (Wardriving/Scan/GPS/Settings/About, plus an extra `Legacy`
   item not in the original design — see below).
3. **Runtime behavior: done.** Reconnect-stays-put (a `connection_lost` flag keeps the active
   screen and shows a banner instead of forcing navigation to Home), stale scan/wardriving UI
   state cleared without leaving the active submenu, existing command-layer radio-conflict
   behavior unchanged.
4. **Capability screens: partially done.** `AppScreenGps` exists with honest stub labeling;
   Settings/About exist as placeholders (as intended). The "Scan" screen exists but is **not**
   the five-mode BLE-active/passive design below — see "Current-state baseline."
5. **Follow-on product work: not started.** No runtime BLE active/passive toggle, no persistent
   scan settings, no deeper settings management.

This is the implementation boundary for the approved v1 menu redesign; it intentionally does
not expand the existing product scope beyond the menu shell and the capability-aware screens
that the design requires.

## Current-state baseline (as of 2026-09-13, hardware-verified — read this, not the design
above, for what actually ships today)

- `AppScreen` enum (`flipper/flipper_esp32_over_ble.c`) now has 9 values: `AppScreenHome`,
  `AppScreenScan`, `AppScreenGps`, `AppScreenSettings`, `AppScreenAbout`, `AppScreenLegacy`, plus
  the existing `AppScreenWifiScanResults`/`AppScreenBleScanResults`/`AppScreenWardriving`.
  `AppScreenLegacy` is a compatibility screen preserving the old direct-button-shortcut flow
  (OK to start pairing/session, Up/Left/Right to jump straight to Wardriving/wifi_scan/ble_scan);
  it is not part of this design's original menu-item list and is reachable as its own always-
  visible `HomeMenuLegacy` item.
- **Still no `ViewDispatcher`/scene manager** — the Home menu shell was built directly on the
  original single `ViewPort`/`AppEvent`-queue pattern. See "Implementation sequencing" above.
- **Home is menu-driven**, matching the target design's navigation model: a `HomeMenuItem` list
  (`HomeMenuWardriving`, `HomeMenuScan`, `HomeMenuGps`, `HomeMenuSettings`, `HomeMenuAbout`,
  `HomeMenuLegacy`) with Up/Down to move, OK to select, and a scrolling window when more items
  are visible than fit four rows. `home_menu_visible()` hides Wardriving/Scan/GPS unless a
  session is active and the board's capability registry supports them; Settings/About/Legacy are
  always visible, matching this design's decisions #7 and #9.
- **Reconnect/disconnect while inside any screen: stay put, matching decision #10.** A
  `connection_lost` flag (set on a BLE-unavailable event, a fatal session error, or a "connection
  lost" pairing failure) keeps whichever screen is active and overlays a "Connection lost"
  message rather than forcing navigation to Home. Only Back is accepted while lost; it returns to
  Home and clears the flag.
- **Settings and About are still deliberate, unscoped placeholders** (decision #6), not stale
  documentation catching up to real content: both screens render static "TBD" text today.
  Notably, an intermediate version of both screens briefly showed real data (saved-pairing state
  in Settings; board/protocol/session info in About) before commit `a744bb4` ("Complete Phase 3a
  UI polish") explicitly reverted them back to plain placeholders to match this file's own
  decision #6 — a deliberate choice, not a regression.
- **The Scan screen exists but is not yet this design's five-mode BLE-active/passive screen.**
  Today's `AppScreenScan` (`draw_scan_screen`, a 2-item `ScanMenuItem` list) is only a picker
  between the existing one-shot `wifi_scan` and `ble_scan` commands/results screens — it does not
  reuse Wardriving's capture engine, has no BLE-active/passive mode cycling, and has no live,
  non-persisting view. It is a placeholder-level stand-in, not a partial implementation of the
  target design below.
- `wifi_scan`/`ble_scan` are still one-shot, manually-triggered scans. `wardriving` is still
  continuous, interval-based, dual-source (Wi-Fi+BLE) capture with flash-log + WiGLE CSV
  persistence. Unchanged from before this pass.
- BLE active scanning is still hardcoded on (`passive=0`) at every `ble_gap_disc()` call site in
  `esp32/main/main.c` — there is still no active/passive runtime toggle anywhere, so the Scan
  screen's two passive modes (below) remain unbuildable. Unchanged from before this pass; also
  tracked in [BACKLOG.md](BACKLOG.md).
- **The GPS screen is now wired to the real `gps` capability** (2026-09-12 implementation, 2026-09-13 hardware-verified).
  It shows the real three-state fix status, real coordinates and GPS-derived UTC time when a fix exists (falling back to the
  Flipper's own RTC clock, clearly labeled as such, otherwise), and still `--` for speed (a
  separate, still-backlogged follow-on). Along the way, a capability-gating bug was found and
  fixed: `HomeMenuGps`'s visibility was checking `capability_has_wardriving` instead of
  `capability_has_gps`. See [PLAN.md](PLAN.md)'s "Real GPS driver..." section for full details.

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

New: a GPS fix/no-fix icon, plus a Start action label that toggles "Start"/"Start (delayed)" —
both implemented 2026-09-12, build-verified only (see [PLAN.md](PLAN.md)'s "Real GPS driver,
wardriving fix-dependency, and real wardriving-record timestamps"), reading the real `gps`
capability's live status rather than a hardcoded stub value.

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

#### GPS (implemented 2026-09-12, build-verified only — see "Current-state baseline" above)

- **Coordinates** — real degrees, decoded from the `gps` capability's `lat_e7_offset`/
  `lon_e7_offset` when the current status is `fix`; `--` otherwise.
- **Current time** — real GPS-derived UTC time (from `utc_timestamp_s`) when `fix`; falls back to
  the Flipper's own RTC clock, clearly labeled as such (never presented as GPS-derived), whenever
  status is `no_signal`/`acquiring`.
- **Speed** — still shown as `--`. Parsing real speed from `RMC` remains a separate, backlogged
  follow-on (see [BACKLOG.md](BACKLOG.md)), not part of this pass.

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

## Open items (updated 2026-09-12 against what's actually implemented)

- **Runtime BLE active/passive toggle: still open, still tracked in [BACKLOG.md](BACKLOG.md).**
  Prerequisite for the Scan screen's real five-mode design; nothing on the wire protocol or
  either firmware can request passive scanning yet.
- **[BACKLOG.md](BACKLOG.md)'s "extending [active scanning] to `wardriving`'s own capture
  engine" note:** already corrected there — confirmed done in code, not open.
- **This design's placement in [PLAN.md](PLAN.md):** done — it is Phase 3a.
- **[BACKLOG.md](BACKLOG.md)'s "Adopt a real `ViewDispatcher`/scene-manager architecture" item
  is stale and needs correcting, not closing.** It still frames the architecture change as a
  hard prerequisite for this design, but the Home menu shell shipped 2026-09-12 built directly on
  the existing single `ViewPort`/`AppEvent`-queue pattern instead — the prerequisite was skipped,
  not satisfied. The item should be reworded to reflect that the menu redesign proceeded without
  it (so it's back to a structural nice-to-have, not a blocking dependency) rather than implying
  it's still blocking work that has, in fact, already happened.
- **New open item found during implementation, not part of the original design:** an
  `AppScreenLegacy`/`HomeMenuLegacy` compatibility screen exists, preserving the old direct-
  button-shortcut flow alongside the new menu. This file's target design below never mentions a
  "Legacy" menu item — worth a decision on whether it's kept long-term or removed once the new
  Scan/GPS/Settings/About screens are trusted to fully replace it.
- **New open item:** the Scan screen (today's 2-item Wi-Fi/BLE picker) and the GPS screen (still
  stub-only) both still need the actual target-design work below — they are placeholders that
  happen to occupy the right menu slot, not partial implementations to build incrementally on.
