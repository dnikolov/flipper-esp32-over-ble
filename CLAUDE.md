# flipper-esp32-over-ble

Pairs a Flipper Zero with an ESP32-C6 over BLE using a trusted-environment X25519
key exchange, then exposes board capabilities (Wi-Fi scan, GPIO, sensors, ...) through
an authenticated CBOR protocol. Two independent firmware targets, one shared protocol
contract.

A git repository (initialized 2026-09-06). Roadmap steps are tagged on completion
(e.g. `step6-complete`) — see `git tag -l` and `git log` for history. Only commit or tag
when the user explicitly asks; do not push anywhere without explicit confirmation.

## Read this first

The `docs/` directory is the source of truth and is kept current — read the relevant
file before touching related code rather than relying on this summary, which will drift:

| File | Read it for |
| --- | --- |
| [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) | Current state, what's implemented, what's next. **Start here every session.** |
| [docs/USER_GUIDE.md](docs/USER_GUIDE.md) | Building, flashing, and pairing the two devices as they work today — scoped to what's actually implemented and hardware-verified. |
| [docs/PLAN.md](docs/PLAN.md) | Phased roadmap and per-phase "done when" acceptance criteria. |
| [docs/BACKLOG.md](docs/BACKLOG.md) | The single centralized list of open, actionable items — defects, deferred product decisions, cost/efficiency work. Check before starting anything not already in the current roadmap step. |
| [docs/HARDENING_BACKLOG.md](docs/HARDENING_BACKLOG.md) | Deeper structural/robustness issues found during live testing that need their own investigation/design pass before fixing — distinct from BACKLOG.md's ready-to-fix items. |
| [docs/BACKLOG_COMPLETED.md](docs/BACKLOG_COMPLETED.md) | Scannable one-line-per-item archive of resolved BACKLOG.md rows. Full narrative for any of them is in PROJECT_HISTORY.md. |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Why pairing/transport/delivery choices were made, and their accepted tradeoffs. |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | The v2 wire contract — CBOR shapes, crypto derivations, UUIDs. Source of truth for both firmwares. |
| [docs/PAIRING.md](docs/PAIRING.md) | The reset-gated X25519 pairing ceremony, step by step. |
| [docs/CAPABILITIES.md](docs/CAPABILITIES.md) | Capability-registry string format and record shape. |
| [docs/BASELINES.md](docs/BASELINES.md) | Pinned toolchain/board/firmware versions and build verification status. |
| [docs/UI_REDESIGN.md](docs/UI_REDESIGN.md) | Design-only Flipper FAP menu/navigation overhaul (Home/Menu/Scan/GPS/Settings/About) — no phase assigned yet, no code written against it. |
| [docs/CLUSTER.md](docs/CLUSTER.md) | Phase 9 design (frozen, not yet implemented): C6 + C5 + Heltec wired together over UART, each dedicated to one scanning job, to eliminate radio coexistence and share one GPS module. |
| [docs/WARDRIVING_REDESIGN.md](docs/WARDRIVING_REDESIGN.md) | Phase 7 (implemented, hardware-verified 2026-09-21): Wardriving Stopped/Running screen split, persisted per-run settings, WiFi scan-dwell/country-code control, GPS speed. Supersedes UI_REDESIGN.md's Wardriving subsection. |
| [docs/WARDRIVING_PUBLISH.md](docs/WARDRIVING_PUBLISH.md) | Phase 6 design (frozen, not yet implemented): publishing the wardriving CSV to wdgwars.pl via a Flipper-triggered BadUSB/host-script flow. |
| [docs/LESSONS.md](docs/LESSONS.md) | Narrative bug writeups the two developer subagents link to instead of restating inline — read for the "why" behind a rule. |
| [docs/STANDALONE_FAP.md](docs/STANDALONE_FAP.md) | What the Flipper external-app ABI can and can't do; feasibility evidence with file citations. |
| [docs/PROJECT_HISTORY.md](docs/PROJECT_HISTORY.md) | Dated narrative log of setup/debugging (toolchain repairs, root causes). Reference, don't duplicate. |
| [docs/hardware/esp32-c6-devkitc-1/README.md](docs/hardware/esp32-c6-devkitc-1/README.md) | Board pinout, strapping pins, USB paths, vendor datasheets. |
| [docs/references/flipper-firmware/](docs/references/flipper-firmware/) | Locally cached Unleashed firmware source — the ABI ground truth (`upstream/targets/f7/api_symbols.csv`). Check `REVISION.txt` for the pinned commit. |

When a doc and the code disagree, the code is more current for *what's implemented*, but
the docs are authoritative for *what's intended* (protocol shapes, security properties). Flag
the mismatch rather than silently picking one.

## Pinned baselines — do not silently change

- ESP32 board: **ESP32-C6-DevKitC-1-N4**, target `esp32c6`, 4 MB flash (verified read-only
  via `esptool flash_id`, not assumed from the vendor guide, which describes an 8 MB variant).
- ESP-IDF: **v5.5.2**, installed at `C:\Users\Deyan\esp\esp-idf`.
- Flipper firmware: **Unleashed stable `unlshd-092`**, commit `3c9be0fdd9d301a9436765099a2d1780b36a1795`, reported API `88.4`.
- Flipper delivery: standalone external FAP, target `f7`, requires `gui`.
- BLE roles: **Flipper is the BLE peripheral/GATT server; ESP32-C6 is the BLE central/GATT client.**
  This is fixed by an ABI constraint (a standalone FAP cannot act as GATT client), not a
  style choice — see [docs/STANDALONE_FAP.md](docs/STANDALONE_FAP.md).

Changing any of these is a project decision, not a local dev convenience. If a task seems to
require it, say so explicitly and confirm with the user before proceeding — update
[docs/BASELINES.md](docs/BASELINES.md) and [docs/PLAN.md](docs/PLAN.md) if it happens.

## Build commands

ESP32 (from a fresh PowerShell):

```powershell
. C:\Users\Deyan\esp\esp-idf\export.ps1
Set-Location C:\Users\Deyan\flipper-esp32-over-ble\esp32
idf.py build
```

Flipper FAP (pinned Unleashed checkout at `C:\Users\Deyan\unleashed-firmware-unlshd-092`).
On Windows this FBT revision only resolves `APPSRC` from a recognized `applications_user/`
subdirectory, so the app source is synced into a temp copy there before building:

```powershell
fbt.cmd fap_flipper_esp32_over_ble
```

Artifacts: ESP32 image under `esp32/build/`; FAP under
`build/f7-firmware-D/.extapps/flipper_esp32_over_ble.fap` in the Unleashed checkout.

## Hardware safety

- **Do not flash, erase, or write the physical board unless the user explicitly asks.**
  Read-only queries (`flash_id`, log monitoring) are fine.
- Known serial ports from prior sessions: ESP32-C6 on `COM9`, Flipper Zero on `COM8` — reconfirm,
  ports are not stable across sessions/reboots.
- Treat GPIO0, 4, 5, 8, 9, 15 as strapping/JTAG pins — do not wire or drive them without checking
  the chip datasheet first.

## Conventions

- Keep the two firmwares in lockstep with [docs/PROTOCOL.md](docs/PROTOCOL.md): a wire-format
  or crypto-derivation change is not done until both sides implement it identically and both
  build.
- Zeroize ephemeral key material (X25519 private keys, shared secrets, session keys) on every
  success and failure path — this is a stated security property, not incidental cleanup.
- Do not implement roadmap steps out of order (see [docs/PLAN.md](docs/PLAN.md)) — e.g. don't
  add capability commands before runtime session auth exists, don't add persistence before the
  pairing ceremony it stores is implemented.
- Match the existing docs' precision: when you learn something about the hardware, protocol, or
  build (a root cause, a pinned commit, a verified measurement), record it in the relevant doc
  rather than only in a commit message or chat — the docs, not git history, are the source of
  truth for *why*.
- **`docs/PLAN.md` and `docs/SESSION_MEMORY.md` keep re-drifting into narrative dumping
  grounds** (fixed once already 2026-09-07, found re-bloated to ~48K again by 2026-09-09 —
  a recurring failure mode, not a one-off). When you're about to add a paragraph to either
  file describing something that already happened and is finished (a bug found+fixed, a
  hardware-verification pass, a wire-format re-derivation that's already frozen in
  [docs/PROTOCOL.md](docs/PROTOCOL.md)), it belongs in
  [docs/PROJECT_HISTORY.md](docs/PROJECT_HISTORY.md) instead, with only a one-line "✅ done,
  see PROJECT_HISTORY.md" pointer left behind. `SESSION_MEMORY.md` is current-state-only
  (read every session, keep it a fast read); `PLAN.md` is roadmap + binding "done when"
  criteria/specs only; PROJECT_HISTORY.md is the only file allowed to carry dated narrative.
- No comment-heavy style in either firmware; both existing `.c` files are comment-sparse by
  design, matching the flipper-developer agent's low-level C standards below.
- **Keep [docs/USER_GUIDE.md](docs/USER_GUIDE.md) in sync with actual behavior.** Any change
  that touches what's described there — on-screen text, button/LED behavior, build/flash
  steps, pairing/reconnect/reset behavior, known quirks — must update the guide in the same
  pass, not as a follow-up. It documents only what's implemented and verified today (see its
  own top-of-file scope note); a step-6-or-later feature doesn't get added to it until it's
  actually built, but a change to something it already documents means the guide is stale
  the moment the change lands, not just at the next release.
  **Do the actual edit with the cheapest available model (Haiku 4.5, or cheaper if one
  exists) — delegate via the Agent tool with `model: "haiku"` — rather than editing it
  inline with whatever model is doing the main task.** It's a mechanical sync (reflect a
  known behavior change into prose that already has an established structure/tone), not
  work that needs the main model's reasoning budget.

## Specialized agents

Project-tuned subagents are defined in `.claude/agents/` (the first two adapted from the
previous Copilot agents in `.github/agents/`, corrected for this project's actual board/
firmware pins instead of their generic defaults):

- **esp32-developer** — ESP-IDF/NimBLE work on the ESP32-C6-DevKitC-1-N4 central role.
- **flipper-developer** — Unleashed FAP work, GATT peripheral role, FBT builds, Furi/GUI conventions.
- **heltec-developer** — ESP-IDF/NimBLE work on the Heltec WiFi LoRa 32 V2 (classic ESP32)
  central role, added 2026-09-17 for Phase 4 capability-porting work.

Prefer delegating board- or firmware-specific implementation work to these agents; they carry
the board pinout and ABI constraints so you don't have to re-derive them each time.
