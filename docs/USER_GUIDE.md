# User guide

What it's actually like to build, flash, and pair this project today. Scoped strictly to
what's implemented and hardware-verified (steps 1-6 of [PLAN.md](PLAN.md), verified
2026-09-06) — runtime session authentication now runs after pairing, but there are no
capability commands yet. An in-firmware factory-reset button-hold gesture now exists on the ESP32 (see below) and has been hardware-verified. For the architecture and wire protocol
behind any of this, see [PROTOCOL.md](PROTOCOL.md) and [PAIRING.md](PAIRING.md); this guide
only covers using the two devices as they exist right now.

Step 6 (runtime session authentication) was hardware-verified on 2026-09-06. This changed how reconnection and re-pairing work: when a saved pairing already exists for a board, the Flipper app now auto-starts advertising immediately on launch — the OK-press is no longer required for reconnects. The OK-press is now reserved only for pairing a genuinely new board (no saved record). If the ESP32 has a stored secret from a previous pairing, on boot or reset it now attempts a "runtime auth" handshake (hello/hello_ack/client_auth) first, instead of unconditionally opening a new 120-second pairing window. A pairing window now only opens if the ESP32 has no stored secret yet, or if the Flipper rejects the board as unrecognized — and even then, only on the ESP32's *next* connection attempt, not the one that got rejected. This prevents the "stray reset silently re-pairs and overwrites the secret" quirk described further down — a stray reset while both sides already share a valid secret now resumes the session silently by design. The wrong-folder pairing-file storage bug is also fixed: new pairing files are now saved under this app's own correct data folder, `/ext/apps_data/flipper_esp32_over_ble/pairings/<board_id>.dat`, not `/ext/apps_data/bt/pairings/`. Important caveat: pairing records saved *before* this fix (from the step 1-5 era) are now orphaned in the old wrong folder and won't be found — that board needs a fresh pairing ceremony after upgrading to this firmware; this is a one-time thing per already-paired board.

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
until its next physical reset. There is currently **no further communication after
pairing**: no Wi-Fi scan results, no live session, nothing else on screen. That's expected
— runtime session authentication is now implemented (step 6) and will run on reconnection;
capability commands are future work (see [PLAN.md](PLAN.md) step 7 onward), not a bug in
what's built today.

Pairing records are stored **per board** (one file per `board_id`, derived from the ESP32's
factory MAC address), so pairing a second ESP32 later won't disturb a pairing you already
have for a different one — this was specifically verified on real hardware, not just
assumed from the design.

## Idle-connection behavior during an active session

Once `ESP32 session active` (solid blue LED) is established, if 30 seconds pass with no traffic, the ESP32 automatically disconnects and rescans. Upon reconnection, it runs the runtime-auth handshake again — fully automatic, no user action. The Flipper may briefly leave `ESP32 session active` and return. Since capability commands don't yet exist (future step 7), this 30-second idle-reconnect cycle repeats indefinitely while both devices are powered and in range. This is expected behavior, not a malfunction — just something to know if watching the LED or logs.

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
