---
name: build-verify
description: Build and/or host-test one or more of this project's targets (esp32, esp32c5, heltec, flipper, hosttests, shared) and report a compact pass/fail per target. Use when asked to build, verify a build, run host tests, or check shared headers — including after any change to a board's main/ code or to components/feb_protocol/.
argument-hint: [esp32|esp32c5|heltec|flipper|hosttests|shared ...] (default: all changed vs HEAD)
---

Implements TP-11 from `docs/TOOLING_PLAN.md`. Wraps the existing per-board/test build scripts
so a multi-target check reports a handful of lines instead of every script's full console
output.

## Targets

| Target | What it runs | Wrapper |
| --- | --- | --- |
| `esp32` | ESP32-C6 firmware build | `tools/build_esp32.ps1` (build only — omit `-Port`) |
| `esp32c5` | ESP32-C5 firmware build | `.claude/skills/build-verify/scripts/build_esp32c5.ps1` (no `tools/*.ps1` wrapper exists for this board yet) |
| `heltec` | Heltec firmware build | `.claude/skills/build-verify/scripts/build_heltec.ps1` (same reason) |
| `flipper` | Flipper FAP release build | `tools/build_flipper.ps1` (build only — omit `-Port`) |
| `hosttests` | All host-native codec/pairing/session/GPS/protocol tests (`tests/esp32/*.ps1`, `tests/flipper/*.ps1`) | `.claude/skills/build-verify/scripts/run_hosttests.ps1` |
| `shared` | Cross-board header/module drift check | `python tools/check_shared_headers.py --changed` (falls back to the full run below if run standalone with nothing changed and you want the complete report) |

## Choosing targets

- If the invocation names targets, use exactly those (validate against the table above; an
  unrecognized name is an error, not a guess).
- If none are named (the default), run
  `.claude/skills/build-verify/scripts/changed_targets.ps1` to get "all changed vs HEAD" —
  it prints one target per line based on which board directories, `components/feb_protocol/`,
  or `tests/` have tracked or untracked changes. If it prints nothing, say so and stop; there is
  nothing to verify.
- A change under `components/feb_protocol/` (the shared component all three ESP32-family boards
  and the host tests consume) always fans out to every target, matching this project's
  "a shared-code change affects every board's build" convention — don't narrow it back down.

## Running

Use the **PowerShell** tool for every `.ps1` script here — not `powershell.exe`/`cmd.exe`
invoked through the Bash tool. `tools/check_shared_headers.py` runs fine through either tool
since it's a plain `python` invocation with no shell-profile dependency.

`esp32`, `esp32c5`, `heltec`, and `flipper` builds are independent (separate build directories,
separate toolchain exports/checkouts) — launch them with `run_in_background: true` in parallel
rather than sequentially. `hosttests` (MSVC/`cl.exe`, no ESP-IDF or fbt involvement) and `shared`
(a fast synchronous Python script) can also run in parallel with the board builds. Wait for all
of them, then report together.

## Report format

One line per target:

```
PASS: esp32 (1m42s)
PASS: flipper (58s)
FAIL: heltec (23s)
```

For any `FAIL`, follow with the last ~15 lines of that target's output (the wrapper scripts
already tail their own output — `run_hosttests.ps1` re-tails per failing sub-script, the
`idf.py`/`fbt.cmd` wrappers tail via their own `-TailLines` parameter, defaulting to 150 lines
captured but only the last ~15 of those need to be surfaced in the report). Do not paste full
build logs into the report. Keep the whole report at or under ~20 lines when everything passes.
