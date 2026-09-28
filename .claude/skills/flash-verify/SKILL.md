---
name: flash-verify
description: Flash a board (esp32-c6, esp32-c5, heltec, or flipper) and capture its boot/log output to verify the firmware, following this project's per-board reset-safety rules. Only ever runs on an explicit, in-turn user request to flash — never invoked automatically.
disable-model-invocation: true
argument-hint: [c6|c5|heltec|flipper] [port]
---

Implements TP-13 from `docs/TOOLING_PLAN.md`. This skill touches physical hardware — treat
every step below as load-bearing, not a suggestion.

## 0. Hard gate — do this before anything else

Flashing, erasing, or writing a physical board is only allowed when the user's own message
**this turn** explicitly asked for a flash/verify pass on a named board. Being invoked (even
via `/flash-verify`) is not itself sufficient if the surrounding request is ambiguous — e.g. a
stale queued instruction, or a request that only asked to *check* or *investigate* something.
If there's any doubt, stop and ask. Read-only diagnostics (`flash_id`, log monitoring) never
need this gate; a real flash always does.

Never flash, erase, or reset a board this skill is not explicitly told to target, and never
touch GPIO0, 4, 5, 8, 9, 15 (strapping/JTAG pins) as a side effect of anything you do here.

## 1. Check for peer sessions

Multiple Claude Code sessions on this repo are common (see the `project_concurrent_sessions`
memory). Before touching any port:

- Call `ListAgents` to find peer sessions.
- Message every peer found, naming the board/port you're about to flash, and ask them to hold
  off on that same board/port for the duration.
- Treat a peer's self-report as unverified if many sessions are active — a clean read-only
  probe of the port right now is evidence it's free *right now*, not a guarantee it stays free.
- Notify peers again once the flash+capture is done.

## 2. Confirm the port by enumeration

COM assignments are **not stable across sessions/reboots** — never trust a remembered port
number without reconfirming. Enumerate live ports (e.g.
`[System.IO.Ports.SerialPort]::GetPortNames()` in PowerShell) and confirm the target port is
present. If the user didn't pass a port, or the enumerated list is ambiguous (more than one
plausible candidate, or the expected port is missing), stop and ask rather than guessing. Last
known ports (reconfirm, don't trust): C6 `COM9`, C5 `COM11`, Heltec `COM10`, Flipper `COM8`.

## 3. Per-board procedure and reset rules

Only two boards have a committed flash script in `tools/`. **Do not invent flash/monitor
commands for a board that doesn't have one** — say so and stop instead.

| Board | Script | Reset behavior | Notes |
| --- | --- | --- | --- |
| `c6` (esp32) | `tools/flash_esp32.ps1 -Port <port> [-CaptureBootLog]` | Native USB-Serial-JTAG. Opening the port **always resets the chip**, even with DTR/RTS forced low — there is no reset-free open on this board. Expected, not a bug. | Boot-log capture uses `ESP_IDF_MONITOR_TEST=1` internally to run `idf.py monitor` without a real TTY. |
| `flipper` | `tools/flash_flipper.ps1 -Port <port> [-FapPath <path>]` | Transfers the FAP over the Flipper's CLI serial protocol (`runfap.py`); no boot-log concept — it's a file transfer, not a firmware flash. | `runfap.py` always sends a `loader open` after transfer, which reliably fails with a transient "not enough memory" preload error on this device — **that failure is expected and does not mean the transfer failed.** Check the script's own output for the `Transferred ... on the Flipper's SD card` line; the app must be launched manually afterward. |
| `c5` (esp32c5) | **none exists** | OLIMEX MOD-ESP32-C5, native USB-Serial-JTAG, `COM11` last known. Do **not** pulse RTS or let a reconnect loop reset the chip repeatedly — open with `dtr=False; rts=False` preset if a raw serial capture is ever used, wait ~3 s after any flash, and check the log's own uptime stamps actually span the capture before trusting it. | No `tools/*.ps1` flash script is committed for this board yet. Report this gap to the user instead of improvising an `idf.py -p <port> flash` invocation from scratch. |
| `heltec` | **none exists** | Classic ESP32 + CP210x USB-UART bridge — a plain serial/monitor open resets it via the normal DTR/RTS auto-reset sequence, same as flashing. The user has explicitly forbidden resetting this board during investigation (it once wiped live RAM-only mesh-node state); always use `--no-reset` for any monitor/log connection that isn't itself the intended flash. | No `tools/*.ps1` flash script is committed for this board yet. Report this gap to the user instead of improvising commands. |

## 4. Delegate execution to a Haiku subagent

Once the gate, peer check, and port are confirmed, spawn a subagent via the **Agent** tool with
`model: "haiku"` to run the actual flash and capture. This is a known, already-documented
procedure being executed, not designed — cheap-model territory (see
`feedback_cheap_model_for_docs` memory, extended 2026-09-07 to hardware flash/verify passes).

Pass the subagent, verbatim:
- The board name, confirmed port, and exact script invocation from the table above (or, for a
  board with no script, instruct it to stop and report the gap rather than invent one).
- The board's reset-behavior notes from the table above.
- The success criteria from §5 below for that board.
- An instruction to report back in **10 lines or fewer**: script exit status, whether the
  expected banner/log lines appeared, and the log file path (if any) — no raw log dump.

## 5. Success criteria (expected boot banner/log lines)

- **C6**: flash script exits 0; the boot log (if captured) shows a clean ESP-IDF bootloader
  banner (no panic/backtrace) followed by one of this app's own first-boot lines —
  `board_id=...: stored pairing_secret found; attempting runtime auth` (already-paired board) or
  `board_id=...: no stored pairing_secret; pairing window open for ... ms` (fresh board).
- **Flipper**: `runfap.py`/the wrapper script prints `Transferred ... on the Flipper's SD card`.
  The subsequent `loader open` failure is expected and is not a failure signal by itself — a
  clean transfer line is the pass condition, not the exit code.
- **C5 / heltec**: no script exists, so there is no defined pass/fail capture procedure yet —
  report the gap; don't fabricate a banner to check for.

## 6. Report

≤10 lines: board, port used, script exit code, whether the expected line(s) from §5 appeared,
log file path if one was written, and confirmation the peer-session check was done. If a board
had no script, say so plainly instead of describing an invented flash attempt.
