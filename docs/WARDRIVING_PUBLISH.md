# Wardriving Log Publishing (Phase 6 — implemented and hardware-verified end-to-end 2026-09-18)

Reached via a grill-me design session with the user, 2026-09-17. Lets the Flipper publish its
captured wardriving CSV to [wdgwars.pl](https://wdgwars.pl) directly from a Windows host computer,
with **no ESP32 board present** — the ESP32-C6 stays mounted in the car with no internet access
during a wardriving run, so publishing has to work from the Flipper alone, later, at a computer.
wigle.net support is an acknowledged future extension, not built now (see "wigle.net" below).

This is its own roadmap phase (Phase 6 in `docs/PLAN.md`), proceeding **in parallel with Phase 4**
(Heltec board support) by explicit user decision — not gated on Phase 4 or Phase 5 completing.

**Status: implemented and hardware-verified end-to-end 2026-09-18**, including a real successful
publish against the live wdgwars.pl API. This doc still records the frozen design and the
decisions behind it; see [docs/PROJECT_HISTORY.md](PROJECT_HISTORY.md)'s "Phase 6" entry for the
implementation pass and the five hardware bugs found and fixed along the way (stack-size MPU
fault, a CSV-path regression, DTR/port-discovery timing, CLI echo handling). Inline notes below
marked "corrected"/"confirmed" reflect what implementation actually found, layered onto the
original design text.

## Why not just use the ESP32's Wi-Fi?

The obvious alternative — have the ESP32-C6 upload over Wi-Fi — doesn't work for this project's
actual usage pattern: the C6 stays wired into the car for wardriving sessions with no internet
uplink there. The Flipper needs to publish standalone, later, from wherever the user next has a
Windows machine with internet — the ESP32 is not expected to be present at publish time at all.

## Capture-side change: the CSV file lifecycle

Today (`flipper/wardriving_csv.c`), the Flipper writes one WiGLE-1.6-format CSV file **per calendar
day** (`wardriving_YYYYMMDD.csv`), reopened in append mode across same-day reconnects. This phase
changes that:

- **Stop naming by calendar date.** There is one "current" file that keeps accumulating across
  however many wardriving sessions/days happen between publishes. It only rolls over to an
  archived, publishable state when a publish actually succeeds (see "Result handling" below) — not
  on any time- or size-based schedule.
- **No size cap, no chunking.** If the current file grows large enough that wdgwars rejects or
  chokes on it, publishing just fails with a clear on-screen error, and the user deals with it
  manually (e.g. by publishing more often). This was an explicit choice over adding an automatic
  size-based rollover or host-side chunking — both add real complexity for a case that's easy to
  avoid by publishing regularly.
- WiGLE-1.6 format itself is unchanged — no new capture-side work beyond the rollover-trigger change.

## wdgwars.pl API (confirmed from the official docs, `https://wdgwars.pl/help/#api-docs`, read 2026-09-17)

This project targets **Method 1 (CSV Upload)**, since the Flipper already produces a WiGLE-1.6 CSV
and the server-side parser is the whole point of using this method (no on-device CSV parsing needed):

- `POST https://wdgwars.pl/api/upload-csv`
  Header: `X-API-Key: <64-char hex key, from the user's wdgwars.pl profile → API Keys>`
  Body: `multipart/form-data`, field `file` = the WiGLE-1.6 `.csv`
  Response (JSON): `{"ok": true, "imported": N, "captured": N, "updated": N, "duplicates": N,
  "no_gps": N, "bad_rows": N, "cooldown": N, ...}`
- `POST /api/v2/upload-csv` — same auth/body shape, for large/slow-link cases. Returns `202` +
  `{"ok": true, "job_id": ..., "poll_url": "/api/v2/upload-job/<id>"}` immediately; the client then
  polls `GET /api/v2/upload-job/<id>` (same `X-API-Key`) until `status` is `done` or `failed`, at
  which point `result` carries the same fields as v1's inline response. The official docs recommend
  v2 for files ≥20 MB or unreliable links; v1 stays simpler for the "a few MB" case a single
  wardriving stretch will usually produce.
- `GET /api/upload-history` (same auth header, optional `?limit=N`, 1–50) returns the account's
  recent uploads with their original `result` payload — useful if a publish's outcome was ever
  ambiguous and needs an independent check.
- No official rate-limit is stated for the CSV endpoints specifically (only `/api/me` documents
  `120 req/min`). Treat the CSV endpoints as "don't hammer it, one attempt per publish action"
  rather than assuming a specific number.
- **Partially confirmed against the live API, 2026-09-18:** a missing/invalid `X-API-Key`
  returns HTTP `401` with body `{"ok":false,"error":"Missing or invalid API key"}` — observed via
  a real end-to-end run of `scripts/publish_wardriving.ps1` against `https://wdgwars.pl/api/
  upload-csv` with a deliberately-wrong key. The host script's existing handling (treat any
  non-`200`-plus-`ok:true` response as a generic failure, never rename the CSV) already covers
  this correctly without needing a code change. Still unconfirmed: malformed-CSV and
  oversized-file behavior, which need a real account and a real (or intentionally-broken) upload
  to observe — don't assume specific codes for those without testing.

## Publish flow

### 1. Launch: BadUSB as a bootstrap trigger only

**Resolved 2026-09-17 (Open Item 4): direct HID typing, not chain-launching the bundled BadUSB
app.** Investigated both options against the real Loader ABI
(`docs/references/flipper-firmware/upstream/applications/services/loader/loader.c`):
`loader_start()` requires `loader->app.thread` to be `NULL` first
(`loader_do_is_locked()`/`loader_do_start_by_name()`) — while this FAP is the running
foreground app, that thread pointer *is this app's own thread*, so a `loader_start(loader,
"bad_usb", <payload path>, NULL)` call from inside it fails outright with
`LoaderStatusErrorAppStarted` ("please close ... first"). `loader_enqueue_launch()` sidesteps
that lock, but only fires the queued app *after this one fully exits* — which is incompatible
with the Publish screen's own requirement to stay resident and poll for
`wardriving_publish_result.txt` once BadUSB has run (see "Publish flow" above). Since no path
through the Loader lets this FAP both trigger BadUSB and remain the foreground app afterward,
the FAP drives HID directly itself via `furi_hal_hid_kb_press`/`furi_hal_hid_kb_release`/
`furi_hal_hid_kb_release_all` (all confirmed exported) plus the `hid_asciimap`/
`HID_ASCII_TO_KEY` table already provided in the exported header `furi_hal_usb_hid.h` (a
compile-time header constant, not a linked symbol, so no separate export entry is needed for
it) — implemented in `flipper/flipper_esp32_over_ble.c`'s `publish_trigger_badusb()`.

The Flipper's USB switches to HID (`furi_hal_usb_set_config`/`usb_hid` — confirmed exported in
`docs/references/flipper-firmware/upstream/targets/f7/api_symbols.csv`) and types a short DuckyScript-equivalent
sequence modeled directly on Unleashed's own bundled
`applications/main/bad_usb/resources/badusb/examples/Install_qFlipper_windows.txt` example:
`GUI r` (Win+R) → `powershell` → Enter opens a **fresh, known-focused** PowerShell window,
regardless of whatever window had focus before — this is what makes blind HID keystroke injection
safe to do unattended here; the script never types into an unknown, already-focused window. It then
types out a short bootstrap (visible in the new console, matching the "no interaction needed, but
user can watch it work" requirement) that fetches and runs the real publish script.

- **Windows-only for v1.** No macOS/Linux DuckyScript variant is in scope.
- **No confirmation gate.** The script runs unattended once launched; transparency comes from the
  visible console output, not from a prompt the user has to answer.
- **The fetched script is pinned to a specific commit/tag**, embedded as a constant in the Flipper
  firmware — never "whatever's on the repo's default branch right now." Bump it deliberately when
  the script's contract changes, the same discipline `docs/PROTOCOL.md` already applies to the wire
  format. This matters because the script runs unattended, with no review step, and handles a live
  upload credential — it's the one part of this whole feature that isn't already covered by the
  project's existing crypto/pairing trust model.
- **Accepted friction, not mitigated:** Windows Defender/SmartScreen will likely flag a
  downloaded-and-immediately-run, self-deleting script as suspicious. This is a known v1 limitation,
  to be documented plainly for the user (why it's safe, how to allow it) once this ships — not
  solved with code signing, which is disproportionate for a personal-use tool where the user
  controls the pinned commit.

### 2. Data transfer: the Flipper's existing CLI-over-USB-serial, not Mass Storage

**Rejected alternative, and why:** an earlier pass through this design proposed having the Flipper
switch its USB personality to Mass Storage to expose the SD card as a drive. That's not feasible —
`api_symbols.csv` has **zero exported symbols** for mass storage/SCSI; the only USB Mass Storage
implementation in the whole Unleashed firmware tree
(`applications/system/js_app/modules/js_usbdisk/`) is a private internal module of the built-in
JS-scripting system app, never exposed through the external-FAP ABI. This is the same class of hard
ABI wall as the GATT-central constraint already documented in `docs/STANDALONE_FAP.md` — not a
workaround-able gap.

**What's actually used instead:** the Flipper's CLI-over-USB-serial system is fully exported —
`cli_registry_add_command`, `furi_hal_cdc_send`/`furi_hal_cdc_receive`, `cli_shell_*` (all confirmed
in `api_symbols.csv`). This is the same channel `scripts/storage.py` already uses in this project to
read/write the Flipper's SD card over serial (see the `reference_flipper_sd_card_access` note on
using PowerShell, not Git Bash, for that). The FAP registers its own custom CLI command; the host
script (fetched via the pinned-commit BadUSB bootstrap above) opens the Flipper's CLI COM port and
invokes that command directly to:

- pull the current wardriving CSV,
- read the stored wdgwars API key off the Flipper's SD card, if present,
- write a freshly-entered key back to the Flipper's SD card, if not present (see "Credential
  storage" below),
- and signal the publish outcome back so the Flipper can rename the CSV and update its own screen.

**Resolved 2026-09-17, before implementation started:** no custom FAP-registered CLI command is
needed at all. Unleashed's **built-in `storage` CLI command** (`applications/services/storage/
storage_cli.c`, always present, not something this project's FAP registers) already exposes
exactly the operations this flow needs — `storage stat`, `storage read_chunks`, `storage
write_chunk`, `storage rename`, `storage remove` — over the same CLI-serial channel. This is the
exact protocol `scripts/storage.py` (`docs/references/flipper-firmware/upstream/scripts/flipper/
storage.py`) already implements and this project already relies on (see
`reference_flipper_sd_card_access` project memory). The host script speaks this protocol directly
(see "Host script implementation" below) — the FAP's only job is reading/writing plain files at
known paths with the `Storage` API it already uses for CSV writing; it registers no new CLI verb.
This also resolves Open Item 3 below by elimination: there is no new command name to pick.

**On-SD file layout**, under `/ext/apps_data/flipper_esp32_over_ble/` (same directory the
capability cache already lives in) — **corrected 2026-09-18**, after briefly flattening
everything during initial implementation:
- `wardriving/wardriving_current.csv` — the live accumulating capture file (replaces the old
  `wardriving/wardriving_YYYYMMDD.csv` per-day naming), in the same subdirectory the older
  per-day files already lived in. A successful publish archives it, in place, to
  `wardriving/<timestamp>.csv` (see "Result handling" below) — archived captures live
  alongside the current file, not in a separate archive location.
- `wdgwars_credentials.txt` — the keyed credential store described below. Flat at the app data
  root, not inside `wardriving/` — it's publish-flow plumbing, not wardriving data, and (unlike
  the CSV) forward-looking for wigle.net too.
- `wardriving_publish_result.txt` — written by the host script after each publish attempt; read
  and displayed by the FAP's Publish screen. Also flat, same reasoning as the credentials file.

### Host script implementation: pure PowerShell, no external dependency

**Explicit user decision 2026-09-17:** the host script re-implements the Flipper CLI's text/binary
protocol directly over `System.IO.Ports.SerialPort` in PowerShell itself, rather than depending on
a system Python install to run the existing `scripts/storage.py`/`flipper.storage` library (the
lower-effort alternative, rejected because it would fail outright on a Windows machine without
Python — undermining the "works from any Windows machine" requirement) or bundling a portable
Python interpreter (rejected as unnecessary added complexity/Defender-friction for this feature's
scope). The protocol being replicated (spec taken directly from
`docs/references/flipper-firmware/upstream/scripts/flipper/storage.py`, a working reference
implementation, and `applications/services/storage/storage_cli.c`'s command table) is:

- Prompt is literal `>: `, line ending is `\r\n`. After opening the port, wait ~0.5s, read until
  the first prompt, then flush.
- `storage stat "<path>"\r` → one EOL-terminated line (`File, size: N`, `Directory`, or
  `Storage error: <msg>`), then the prompt.
- `storage read_chunks "<path>" <buffer_size>\r` → one line `Size: N` (or an error line); then,
  until `read_size >= N`: read a line `\r\nReady?\r\n`, write a single unterminated `y` byte (no
  `\r`), then read exactly `min(remaining, buffer_size)` raw bytes off the wire. Finishes with the
  prompt.
- `storage write_chunk "<path>" <size>\r` → two EOL lines (an echo, then either empty or a
  `Storage error: ...`); if not an error, write exactly `size` raw bytes, then read the prompt.
- `storage rename "<old>" "<new>"\r` → an EOL line (empty, or `Storage error: ...`) then the prompt.
- Every command's error form is the same: a line containing `Storage error:` followed by one of a
  fixed set of reason strings (`filesystem not ready`, `file/dir already exist`, `file/dir not
  exist`, `invalid parameter`, `access denied`, `invalid name/path`, `internal error`, `function
  not implemented`, `file is already open`) — treat unrecognized text the same as `unknown error`
  rather than assuming a closed set.

This is new code, unverified against real hardware until this phase reaches its hardware-testing
pass (same caveat this project already applies elsewhere, e.g. `docs/BASELINES.md`'s Heltec
section) — flag any protocol-framing mismatch found during that pass here and in
`docs/PROJECT_HISTORY.md`, don't silently patch around it.

### 3. Credential storage: host-console entry, Flipper SD persistence

The wdgwars API key is a 64-character hex string — long enough that entering it via the Flipper's
D-pad would be genuinely painful, so **no on-device keyboard/text-entry UI is built for this**
(a real scope reduction from an earlier pass through this design, which had proposed adding one).
Instead:

- On a publish run, the host script checks (over the CLI-serial channel above) whether a key file
  already exists on the Flipper's SD card.
- If not, it prompts once in the host's own console (a full keyboard, trivial to paste/type a
  64-char key into) and writes the key back to the Flipper's SD card over the same channel.
- If it does, it just reads it — no prompt.
- This means the key **persists on the Flipper**, not the host: it travels with the device across
  different Windows machines, matching the portability the user wants, without needing new
  on-device UI to get it there.
- Stored plaintext on SD, same risk class and same accepted precedent as the pairing secret
  (`docs/STANDALONE_FAP.md` already states this Flipper is not a hardware-backed secret vault) — a
  live upload credential is a different stakes level than a pairing secret scoped to this project's
  own two devices, but the user explicitly chose portability over reducing that exposure.
- **Storage format is a small keyed list** (target name → credential), not a single flat value —
  chosen so wigle.net (see below) can be added later without redesigning this flow, even though only
  `wdgwars` is ever populated or used by v1. Concretely, `wdgwars_credentials.txt` is plain
  `key=value` lines (one target per line, e.g. `wdgwars=<64-char hex key>`) — deliberately not
  JSON, since the Flipper firmware has no JSON decoder in this codebase (CBOR is the only
  structured format already in use) and a flat line format is trivial for the FAP to parse without
  adding one.
- **Host-side cleanup, precisely scoped:** "the script deletes its data from the host computer"
  means the temporary local CSV copy and any in-memory state from that run — never the
  Flipper-SD-persisted key, which is designed to survive across runs and across host machines.

### 4. Result handling

- **Rename the CSV to an archived, timestamped name (e.g. `2026-09-17_13-31-05.csv`, using hyphens
  rather than the originally-proposed colons — colons aren't valid in a FAT32 filename, which is
  what the Flipper's SD card uses) only on a confirmed successful response** (`ok: true` in the
  JSON body). Never rename on a network error, timeout, or any response the script can't positively
  confirm as success — the CSV is the only record of that data, and a wrongly-archived file paired
  with an actually-failed upload is a real data-loss risk. Since wdgwars's own response already
  reports a `duplicates` count, a safe re-upload of a file that partially succeeded earlier is not
  wasteful.
  - **Explicit user decision, 2026-09-18, confirmed against the live API:** `POST /api/upload-csv`
    (v1) can itself respond `202` with `{"ok": true, "queued": true, "job_id": ..., "poll_url":
    ...}` — the same async-queued shape the docs above describe for v2, observed in practice from
    the v1 endpoint too. `ok: true` at `202` is treated as confirmed enough to archive, the same as
    a `200`; the script does not poll `poll_url` to wait for the queued job to actually finish
    before renaming. This trades a small chance of archiving a CSV whose queued import later fails
    server-side for not needing a whole second polling sub-flow — accepted deliberately, not an
    oversight.
- **One attempt per publish action — no automatic retry loop.** If a re-attempt is ever needed
  (e.g. a transient failure), the user re-triggers publish later; the script doesn't sleep-and-retry
  unattended. This keeps the "script deletes itself and its data when done" cleanup step simple and
  keeps the host session bounded.
- The Flipper's own screen shows the outcome (success with the `imported`/`duplicates`/etc. counts,
  or a plain failure indication) after the host script reports back over the CLI-serial channel.
- **Result-reporting format:** the host script writes `wardriving_publish_result.txt` (flat
  `key=value` lines, same reasoning as the credentials file above — no JSON decoder in this
  codebase): `status=ok|fail|nothing_to_publish`, then on `ok` the response's own `imported`/
  `captured`/`updated`/`duplicates`/`no_gps`/`bad_rows` fields verbatim, or on `fail` a single
  free-text `message=...` line. `nothing_to_publish` covers the case where the current CSV doesn't
  exist yet or has no data rows beyond its header — the script doesn't attempt an upload for an
  empty file, and this status tells the Flipper screen to say so plainly rather than showing a
  false failure.

## wigle.net (explicitly deferred)

Not built in this phase. The only forward-looking accommodation made now is the keyed
credential-storage format above, so adding wigle.net later doesn't require re-touching the
credential flow. wigle.net's actual upload logic, response-shape handling, and any CSV-variant
differences are entirely out of scope until that's its own design pass.

## Roadmap placement

New standalone **Phase 6** in `docs/PLAN.md`'s roadmap phase list, proceeding in parallel with
Phase 4 (Heltec) rather than waiting for Phase 4/5 to complete — an explicit user decision, recorded
in `docs/PLAN.md` with the same kind of override note Phase 4 itself carries for its own
Phase-3-backlog gate.

## Open items (resolved during implementation, kept for the design record)

1. ~~Design the actual wire format for the CLI-registered command~~ **Resolved 2026-09-17**: no
   custom command needed — reuses the built-in `storage` CLI verbs; see "Data transfer" above.
2. ~~Confirm wdgwars.pl's actual non-200 error behavior for the CSV endpoints against a real
   account~~ **Resolved 2026-09-18**, confirmed against the live API during hardware testing: a
   bad API key returns HTTP `401` with `{"ok":false,"error":"..."}`, already handled correctly by
   the existing generic-failure path (any non-`ok:true`/non-2xx response). A real successful
   upload returned `202` with `{"ok":true,"queued":true,...}` from the v1 endpoint (see "Result
   handling" above for the `202`-as-success decision this prompted). Malformed-CSV and
   oversized-file behavior specifically were still not exercised — not a blocker, just untested.
3. ~~Decide the exact on-Flipper CLI command name/namespace~~ **Resolved by elimination
   2026-09-17**: no new CLI command is registered at all, so there's no name to pick or collide.
4. ~~Whether the Flipper types the BadUSB DuckyScript sequence via raw HID output driven
   directly by this FAP, or by writing a payload file and chain-launching Unleashed's existing
   bundled BadUSB app through the Loader service~~ **Resolved 2026-09-17**: direct HID typing —
   the Loader ABI has no path that lets this FAP both trigger BadUSB and stay resident afterward
   to poll for the publish result; see "Launch: BadUSB as a bootstrap trigger only" above for the
   full investigation.
