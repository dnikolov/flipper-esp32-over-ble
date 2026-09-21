# User guide

What it's actually like to build, flash, and pair this project today. Scoped strictly to
what's implemented and hardware-verified: Phase 2 (steps 1-7 of [PLAN.md](PLAN.md)), Phase 3a (Flipper UI redesign), and Phase 3 (production-ready wardriving with real GPS). Runtime session authentication now runs after pairing, and after a successful reconnect the Flipper automatically queries and caches the ESP32's board identity and capabilities. An in-firmware factory-reset button-hold gesture exists on the ESP32 and is hardware-verified. For the architecture and wire protocol behind any of this, see [PROTOCOL.md](PROTOCOL.md) and [PAIRING.md](PAIRING.md); this guide covers using the two devices as they exist right now.

Step 6 (runtime session authentication) was hardware-verified on 2026-09-06. This changed how reconnection and re-pairing work: when a saved pairing already exists for a board, the Flipper app now auto-starts advertising immediately on launch — the OK-press is no longer required for reconnects. The OK-press is now reserved only for pairing a genuinely new board (no saved record). If the ESP32 has a stored secret from a previous pairing, on boot or reset it now attempts a "runtime auth" handshake (hello/hello_ack/client_auth) first, instead of unconditionally opening a new 120-second pairing window. A pairing window now only opens if the ESP32 has no stored secret yet, or if the Flipper rejects the board as unrecognized — and even then, only on the ESP32's *next* connection attempt, not the one that got rejected. This prevents the "stray reset silently re-pairs and overwrites the secret" quirk described further down — a stray reset while both sides already share a valid secret now resumes the session silently by design. The wrong-folder pairing-file storage bug is also fixed: new pairing files are now saved under this app's own correct data folder, `/ext/apps_data/flipper_esp32_over_ble/pairings/<board_id>.dat`, not `/ext/apps_data/bt/pairings/`. Important caveat: pairing records saved *before* this fix (from the step 1-5 era) are now orphaned in the old wrong folder and won't be found — that board needs a fresh pairing ceremony after upgrading to this firmware; this is a one-time thing per already-paired board.

Step 7 (board identity and capability registry) was hardware-verified on 2026-09-07. The first time the Flipper successfully completes runtime auth with a new board (`board_id`), it automatically sends a `capability_query` and caches the response in a new file: `/ext/apps_data/flipper_esp32_over_ble/capabilities/<board_id>.dat`. This query happens only once per board — the cache is keyed by the ESP32's factory-MAC-derived `board_id`, so re-pairing the same physical board (even after an ESP32 factory reset) doesn't force a re-query, since `board_id` doesn't change. The only way to force a fresh query today is to manually delete the cache file; there's no unpair/UI action yet that does this for you (that's future work, [PLAN.md](PLAN.md) step 8). On-screen, after a successful reconnect to a board that now has its capabilities cached, you'll see an additional status line showing the board model and its supported features — for example, `esp32-c6-devkit: wifi_scan`. This display is fully automatic and requires no user action. `wifi_scan` is the first capability with a real, invokable command — see "Scanning for Wi-Fi networks" below; further capabilities remain future work.

## What you need

- An **ESP32-C6-DevKitC-1-N4** (4 MB flash, confirmed by reading the chip, not assumed from
  the vendor guide — see [BASELINES.md](BASELINES.md)).
- A **Flipper Zero** running **Unleashed firmware, release `unlshd-092`**, with the
  `flipper_esp32_over_ble` FAP installed.
- Both devices' serial ports, so you can build/flash and watch logs. In past sessions the
  ESP32 showed up as `COM9` and the Flipper as `COM8` — **reconfirm both every session**;
  they are not guaranteed stable across reboots or reconnects.

## Building and flashing

Build/flash commands are in the project's top-level `CLAUDE.md`. In short: source
ESP-IDF's `export.ps1`, then `idf.py build` (and `idf.py -p COMx flash` when you actually
want to write the board) from `esp32/`; `fbt.cmd fap_flipper_esp32_over_ble` for the FAP
from the pinned Unleashed checkout. **Do not flash, erase, or write either physical board
unless you mean to** — read-only queries (`flash_id`, log monitoring) are always safe.

## Pairing the ESP32 with the Flipper

1. Power/reset the ESP32-C6. **If it has no stored pairing secret yet** (a genuinely new
   board, or one the Flipper just rejected as unrecognized), this opens exactly one
   **120-second pairing window**, during which it scans for the Flipper and connects the
   moment it finds it — nothing else needs to happen on the ESP32 side. **If it already has a
   stored secret** from a previous pairing, it instead attempts a silent runtime-auth
   handshake first (see step 2a) — no pairing window opens unless that handshake is rejected.
2. On the Flipper, launch the `ESP32 over BLE` app. The screen shows a title, whether it
   already has a saved pairing for some board (`Have saved pairing` / `No saved pairing`),
   and a status line.
2a. **If you have a saved pairing**, the app auto-starts advertising immediately — no OK-press
   needed. Instead of the pair/confirm/save phases, you'll see:
   - `Authenticating...` — the ESP32 found the Flipper; the runtime auth handshake is running.
   - `Failed: proof verification failed` — the authentication handshake failed because the
     ESP32's stored secret doesn't match the Flipper's (they are out of sync). Reset the ESP32
     for a fresh pairing window and try again.
   - `ESP32 session active` — done. The authentication succeeded, the session is live, and the
     Flipper's LED goes solid blue.
3. **If you have no saved pairing**, press **OK**. This starts the app's custom BLE profile and
   begins advertising. You'll see:
   - `Waiting for ESP32...` — advertising, no connection yet.
   - `Connected, exchanging keys...` — the ESP32 found and connected to the Flipper; the
     X25519 key exchange is running.
   - `Confirming...` — both sides are checking that they derived the same shared secret.
   - `Saving...` — the pairing secret is being written to persistent storage on both
     devices.
   - `Paired` — done. The Flipper's LED goes from a blinking blue (during the whole
     handshake) to solid blue at this point, and only at this point — solid blue means the
     secret is actually saved, not just "a BLE link exists."
4. If anything goes wrong at any step, you'll see `Failed: <short reason>` and the LED
   turns off. **The ESP32 accepts exactly one pairing attempt per window** — whatever
   happens (success, failure, a dropped connection, an expired 120-second clock) ends that
   window immediately. To try again, physically reset the ESP32 for a fresh window.
5. Press **Back** at any time to exit the app. This always stops advertising, disconnects,
   and restores the Flipper's default Bluetooth profile — it's safe to back out mid-attempt.

## What "paired" means right now

Once you see `Paired`, both devices have independently derived and saved the same 32-byte
secret, and the ESP32 disconnects and goes fully idle — it does not scan or connect again
until its next physical reset. On the next reconnection, runtime session authentication will
run and you'll see `ESP32 session active`. At that point, the Flipper will also automatically
query the ESP32's board identity and capabilities (if not already cached), and an additional
status line will appear on screen showing the board model and its supported features — for
example, `esp32-c6-devkit: wifi_scan ble_scan`. This capability line isn't just informational:
both `wifi_scan` and `ble_scan` are now real, invokable capabilities (see "Scanning for Wi-Fi
networks" and "Scanning for BLE devices" below) — the first two capability commands implemented
end to end. Any capability beyond these is still future work (see [PLAN.md](PLAN.md) step 8
onward), not a bug in what's built today.

Pairing records are stored **per board** (one file per `board_id`, derived from the ESP32's
factory MAC address), so pairing a second ESP32 later won't disturb a pairing you already
have for a different one — this was specifically verified on real hardware, not just
assumed from the design.

## LED status indicators

The onboard LED on each device provides visual feedback about the connection and session state (hardware-verified):

**Flipper (built-in notification LED):**
- **Blinking blue** — waiting for the ESP32 to connect, or advertising for pairing.
- **Solid blue** — a runtime session is authenticated and active.
- **Solid green** — a wardriving backlog batch is being received from the ESP32; returns to solid blue once complete.

**ESP32 (onboard WS2812 RGB LED on GPIO8):**
- **Blinking blue** (or **blinking purple** if a wardriving session is active) — connecting: scanning for the Flipper, physically BLE-connected but not yet
  authenticated, or running the pairing ceremony (the ESP32 disconnects after pairing completes
  and only re-authenticates on its next boot).
- **Solid blue** (or **solid purple** if a wardriving session is active) — a runtime session is authenticated and active.
- **Solid green** — a wardriving backlog batch is actively being sent to the Flipper; returns to solid blue (or solid purple if a wardriving session is active) once complete.
- **Dim red blink** (BOOT-button factory-reset only) — released early to cancel the reset; the LED then
  returns to the actual connection state (blinking/solid blue, blinking/solid purple, or solid green) instead of turning off.

## Idle-connection behavior during an active session

Once `ESP32 session active` (solid blue LED) is established, if 30 seconds pass with no traffic, the ESP32 automatically disconnects and rescans. Upon reconnection, it runs the runtime-auth handshake again — fully automatic, no user action. The Flipper may briefly leave `ESP32 session active` and return, though the board identity and capability line cached in step 7 will persist through these reconnections. Since capability commands now exist (wifi_scan is implemented; see below), this 30-second idle-reconnect cycle repeats indefinitely while both devices are powered and in range unless you actively trigger a scan. This is expected behavior, not a malfunction — just something to know if watching the LED or logs.

## Scanning for Wi-Fi networks

Once an authenticated session is active and the Flipper's status line shows the board model and its capabilities (e.g., `esp32-c6-devkit: wifi_scan`), you can trigger a Wi-Fi network scan directly from the Flipper — the first capability command now implemented end to end.

**How to scan:** With the app showing `ESP32 session active`, press **Left** from the main screen to start a Wi-Fi scan on the connected ESP32 (if the board advertises `ble_scan` too, **Right** triggers that instead — see the status line's feature list). The app moves to a new results view showing all detected Wi-Fi access points (APs). From inside that results view, pressing **OK** re-triggers another scan.

**Results display:** Each line shows:
- **SSID** — the network's name, or empty if the network is hidden.
- **Signal strength (dBm)** — the received signal power, ranging from very weak to very strong.
- **PHY generation** — the highest Wi-Fi standard the AP advertises (e.g., `11n` for 802.11n, `11ax` for Wi-Fi 6).
- **Auth mode** — the security type the AP uses (e.g., `WPA2_PSK`, `OPEN`).

The results list is scrollable via **Up/Down**. A header line at the top shows the total count of APs found.

**Scan behavior:**
- The scan runs for roughly 1-2 seconds and reports up to 32 APs. If more networks are present, only the strongest by signal are shown.
- Pressing **OK** again while a scan is already in progress has no effect — the app prevents sending a second command until the first completes.
- Pressing **Back** exits the results view and clears all results from the display. Results are not saved, exported, or persisted in any way — they appear on screen only.
- If the ESP32 disconnects and reconnects (triggered by the 30-second idle timeout or by the Flipper leaving range briefly), the results remain on screen and your scroll position is preserved — you can continue viewing the same scan results before starting a new one.

**Known limitation:** SSIDs containing non-ASCII or non-printable bytes are displayed after sanitization. A real Wi-Fi network with such an SSID has not been tested on real hardware (the required network was not available during verification), so rendering in this case is not confirmed. The protocol and host-side tests cover this case; hardware coverage is a backlog item if it becomes relevant.

## Scanning for BLE devices

Once an authenticated session is active and the Flipper's status line shows the board model and its capabilities (e.g., `esp32-c6-devkit: wifi_scan ble_scan`), you can trigger a BLE advertisement scan directly from the Flipper — the second capability command now implemented end to end.

**How to scan:** With the app showing `ESP32 session active`, press **Right** from the main screen to start a BLE scan on the connected ESP32 (if the board advertises only `wifi_scan`, the Right button does nothing — **Left** triggers `wifi_scan` instead). The app moves to a new results view showing all detected BLE devices. From inside that results view, pressing **OK** re-triggers another scan.

**Results display:** Each line shows:
- **Address** — the device's Bluetooth MAC address.
- **Name** — the device's advertised name, if present in its advertisement.
- **Signal strength (dBm)** — the received signal power.
- **Address type** — either `public` or `random`.

The results list is scrollable via **Up/Down**. A header line at the top shows the total count of devices found.

**Scan behavior:**
- The scan runs for roughly 1-2 seconds and reports up to 32 devices. If more devices are present, only the strongest by signal are shown.
- Pressing **OK** again while a scan is already in progress has no effect — the app prevents sending a second command until the first completes.
- Pressing **Back** exits the results view and clears all results from the display. Results are not saved, exported, or persisted in any way — they appear on screen only.
- If the ESP32 disconnects and reconnects (triggered by the 30-second idle timeout or by the Flipper leaving range briefly), the results remain on screen and your scroll position is preserved — you can continue viewing the same scan results before starting a new one.

## Wardriving

Once an authenticated session is active and the Flipper's status line shows the board
advertises `wardriving` (e.g., `esp32-c6-devkit: wifi_scan ble_scan wardriving gps`), press **Up**
from the main screen to open the wardriving screen. **Note: This feature is build-verified as of
2026-09-21 but has not yet been tested on physical hardware.**

The wardriving screen has two layouts depending on whether recording is currently running:

**Stopped screen** — a scrollable settings list (shown when wardriving is not running). Use **Up**/**Down** to move the highlighted row, and **Left**/**Right** to change that row's value. Each setting persists across app restarts. The available rows depend on what radios the connected board supports:

| Row | Values | Shown when |
| --- | --- | --- |
| Mode | WiFi+BLE / WiFi / BLE | board advertises both `wifi_scan` and `ble_scan` |
| WiFi Swelling | Normal / Aggressive / Speed-based | board advertises `wifi_scan` |
| WiFi Cooldown | 5s / 2s / 0s | board advertises `wifi_scan` |
| BLE Mode | Active / Passive | board advertises `ble_scan` |
| Country | BG / RoW | board advertises `wifi_scan` |

The WiFi Swelling setting controls the scan dwell time per channel: Normal uses the ESP32's default (fastest), Aggressive uses 85ms per channel for denser capture at the cost of slower scanning, and Speed-based automatically switches to Aggressive when the board's GPS reports movement faster than ~10 km/h (and back to Normal below ~8 km/h). The WiFi Cooldown values (5s/2s/0s) set the time between successive Wi-Fi scans. Country controls the WiFi regulatory region: BG sets Bulgaria's regulatory domain specifically (unlocking channels 1-13, active scan only), while RoW (Rest of World) keeps the safe worldwide default of channels 1-11 only — pick BG only if the board is actually operating in Bulgaria, not generically "in Europe." **Note: Passive BLE scanning (BLE Mode: Passive) is currently known to be broken in this firmware and omits BLE devices entirely rather than scanning passively — this is a known limitation, not a selection between two working modes.**

Press **OK** to start wardriving with the current settings. The footer shows `OK: start` when the board reports a real GPS fix, or `OK: start (delayed)` while still acquiring a fix — the label differs but the button press works either way (wardriving is never blocked on having a fix).

**Running screen** — shown when wardriving is actively recording. Displays:
- **Header** — `Wardriving: RUNNING`.
- **Records/backlog line** — a running count of records received this connected session, plus either `Backlog: N` (still draining previously-buffered results from the ESP32's flash log) or `Live` (caught up).
- **Last result line** — the most recently received record's kind (WiFi/BLE) and a short summary (SSID or BLE address).
- **GPS fix indicator** — the Flipper polls the board's `gps` status every 2 seconds and shows it appended as `GPS:Fix`, `GPS:Acq` (acquiring), `GPS:No sig` (no signal), or `GPS:?` (not polled yet this session). Only shown if the board advertises the `gps` capability.
- **Error line** — appears only when something needs attention (e.g. the board reports it's already running, or a CSV write failed).

Press **OK** to stop wardriving; press **Back** to return to the main screen without stopping the capture (wardriving runs autonomously on the ESP32, so leaving the screen is pure navigation, not an implicit stop).

**Backlog drain:** Whenever the Flipper connects and authenticates, the ESP32 automatically
sends any wardriving records it has buffered since the last connection — this happens whether
or not wardriving is currently running, and whether or not you've opened this screen. These
records are captured and exported the same as live results (see below).

**Location data:** a real UART/NMEA GPS driver and per-record fix-dependency are implemented and hardware-verified. Any record captured while the board's location driver is not reporting a real fix (`GGA` fix quality > 0 and `RMC` status `A`) is discarded rather than logged or sent — losing a fix mid-capture pauses logging until it returns, without stopping the capture itself. `start`/`stop` are never gated on having a fix (see "Starting and stopping" above). See [CAPABILITIES.md](CAPABILITIES.md) for the full design.

**SD card export:** every wardriving record (both backlog-drained and live) is appended, incrementally and in WiGLE CSV format, to a single file, `/ext/apps_data/flipper_esp32_over_ble/wardriving/wardriving_current.csv`, on the Flipper's SD card. Unlike the older per-calendar-day naming, this one file keeps accumulating across however many sessions/days happen between publishes (see [WARDRIVING_PUBLISH.md](WARDRIVING_PUBLISH.md)) — it's only renamed away, to a timestamped archive in that same `wardriving/` directory, by the publish flow's host script, after a confirmed successful upload. Records are written to the file as they arrive rather than held in memory, so a capture session can run for hours without growing the app's RAM usage. Each record's exported `FirstSeen` timestamp is a real UTC wall-clock time derived from the board's GPS fix at capture time (`YYYY-MM-DD hh:mm:ss`, matching WiGLE's format, hardware-verified).

## Factory-resetting the ESP32 without a PC

If you don't have a PC handy for the `esptool`/`parttool` NVS-erase method described elsewhere
in this project's docs, the ESP32 also supports an in-firmware factory-reset gesture. It's
built and hardware-verified 2026-09-06:

- Hold the onboard **BOOT button** (the same button used for flashing) for **5 continuous
  seconds**, at any point while the board is powered on — you don't need to be mid-pairing or
  in any particular state.
- While held, the onboard RGB LED blinks to show the hold is being tracked. Release early and
  nothing happens: no erase, the LED just stops, normal operation resumes with no other signal.
- At 5 seconds, the board erases its entire NVS partition (the same scope as the PC-based
  `esptool`/`parttool` erase — this removes the saved pairing secret and any other stored
  state) and restarts. There's no distinct "confirmed" blink at 5 seconds — the erase-and-
  restart cycle itself is the confirmation.
- After restarting, the board behaves like a brand-new board: no stored secret, so it opens a
  fresh 120-second pairing window automatically.

This is entirely ESP32-side — the Flipper isn't involved and doesn't need to be nearby or even
powered on.

## Home menu behavior with wardriving

When you connect to a board that advertises the `wardriving` capability, the Home menu's cursor automatically jumps to the Wardriving option as soon as the session becomes active, even if you were viewing a different menu item (like Settings or About). This is a convenience feature to make wardriving easily accessible — if you want to stay on a different menu item, you can navigate away as normal. Additionally, in the Home menu, the Publish option now appears immediately after Wardriving in the menu order, making the publish flow easier to access after a wardriving session.

## GPS screen

Once an authenticated session is active and the Flipper's status line shows the board
advertises `gps` (e.g., `esp32-c6-devkit: wifi_scan ble_scan wardriving gps`), press **Down**
from the main screen to view live GPS data read from the board. The screen displays:

- **Latitude/Longitude** — the board's current position as reported by the GPS receiver.
- **Altitude and Speed** — altitude in meters and ground speed in km/h (shown as `--` when there is no GPS fix yet).
- **Fix state and time** — whether the board currently has a real GPS fix, along with the last-known UTC time.

This screen updates in real time while the board has an active GPS fix. Speed data is particularly useful when wardriving with Speed-based WiFi Swelling enabled — the same speed value determines whether the scan dwell time switches to aggressive 85ms per-channel tuning.

## Wardriving autostart and boot-button toggle

Once wardriving has been started on the ESP32, the board persists its on/off state to NVS flash. On the next boot, if wardriving was running when powered off, it automatically resumes — no action required.

You can also toggle wardriving on/off using the onboard **BOOT button** without a Flipper nearby:

- **Press and release within 100 ms to 1 second** to toggle wardriving on/off using the
  ESP32's own last-used source/cooldown settings (or sensible defaults — both Wi-Fi and active
  BLE enabled, 5s cooldown — if wardriving has never run on this board).
- **Presses under ~100 ms** are ignored — contact bounce and accidental taps don't trigger.
- **Presses held over 1 second but before the 5-second factory-reset point** are also ignored — treated as an aborted reset, so releasing after the first second is safe.
- The LED reflects wardriving's state (blinking/solid purple when active, blinking/solid blue when idle); there's no distinct signal for the button press itself.

**Important:** WiFi Swelling and Country (the two brand-new settings on the Flipper's Wardriving
Stopped screen) are only ever applied when you start wardriving via the Flipper's OK press —
starting via the BOOT button or boot autostart always uses Normal/RoW regardless of what's
saved on the Flipper, since the ESP32 doesn't persist these two settings itself. Mode, WiFi
Cooldown, and BLE Mode, by contrast, use whatever the ESP32 last had running (unchanged
behavior from before this feature) — they just won't necessarily match what's currently shown
on the Flipper's settings list if you've changed it there since the ESP32 last started
wardriving on its own. This is a known simplification, not a bug.

This is entirely ESP32-side — the Flipper isn't involved and doesn't need to be nearby or even powered on.

## Two things to know before you rely on this

These two items were found during the 2026-09-05 hardware verification session in the
pre-step-6 firmware and are both now fixed as of step 6 (2026-09-06):

- **A stray ESP32 reset while the Flipper app is still open on the `Paired` screen could
  silently start a second pairing ceremony and overwrite the saved secret — with no OK
  press, no warning.** This was observed happening by accident (an unrelated `esptool`
  operation that resets the board as a side effect). **Fixed as of step 6:** a stray reset
  while both sides already share a valid secret now resumes the session silently instead
  (see the step 6 section above).
- **The Flipper's pairing files ended up in the wrong app-data folder** —
  `/ext/apps_data/bt/pairings/<board_id>.dat` instead of this app's own folder. This was a
  known bug (tracked in [PLAN.md](PLAN.md)'s Backlog). **Fixed as of step 6:** pairing files
  are now saved correctly in `/ext/apps_data/flipper_esp32_over_ble/pairings/<board_id>.dat`
  (see the step 6 section above).

## Troubleshooting

- **Flipper stuck on `Waiting for ESP32...`**: confirm the ESP32 is actually powered and
  within its 120-second window (reset it again if you're not sure); check both boards are
  actually in BLE range of each other.
- **`Failed: ...` every time**: reset the ESP32 for a fresh window before trying again —
  there is no retry within an already-consumed window.
- **Can't find the board's serial port**: `COM9`/`COM8` are what showed up in past sessions,
  not a guarantee — check your OS's device list. One past session also saw a Flipper's port
  vanish from the OS's list for under a minute for no identified reason, then come back on
  its own after a physical check — not something to work around, just don't panic if it
  happens.
