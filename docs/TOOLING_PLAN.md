# Tooling & agent-efficiency plan (2026-09-28)

Prioritized plan to cut token cost and wall-clock across the Claude Code setup for this repo:
agent definitions, harness settings, docs-as-context, and repeated procedures. Built from a
read-only review of `.claude/`, `~/.claude/settings.json`, and every session transcript for this
project. Firmware behavior is out of scope except TP-15, which is a refactor proposal only.

**Relationship to other docs:** [tooling_hardening.md](tooling_hardening.md) stays the running
log where friction gets noted as it's found. Its four 2026-09-28 entries are folded in here
(TP-07, TP-10, TP-11, TP-12). When a TP item lands, record the narrative in
[PROJECT_HISTORY.md](PROJECT_HISTORY.md) and flip its status here to ✅.

## 1. Measured baseline (2026-09-02 → 2026-09-28)

Source: 149 main-session + 290 subagent transcripts under
`~/.claude/projects/c--Users-Deyan-flipper-esp32-over-ble/`. Dollar figures use approximate list
prices. Treat them as relative weights: on a subscription they show how fast rate limits get
used up, not what gets billed.

| Metric | Value |
| --- | --- |
| Estimated total | ~$1,490 (main ~$790, subagents ~$700) |
| Share of cost from cache reads | ~60% |
| Main-thread model mix | Sonnet 8,182 calls / Opus 503 / Haiku 304 |
| Avg context per call | main (Sonnet) ~183K tokens, subagent (Sonnet) ~143K |
| Peak context per session | main median 155K, p90 348K; subagent median 80K, p90 262K |
| Compactions | 30 |
| Agent spawns | 299 (flipper 89, esp32 84, general-purpose 44, heltec 39, Explore 20, c5 12, monitors 9); ~48 with a Haiku override |
| SendMessage continuations | 538 |
| Bash `sleep` polls | 197 |
| `powershell` invoked through Git Bash | ~1,700 (incl. `powershell.exe`, `cmd.exe`) |
| Current global settings | `"model": "opus"`, `"effortLevel": "high"`, 103 one-off allow entries, no hooks |

Hottest reads:

| File | Size | Reads | Of which whole-file |
| --- | --- | --- | --- |
| `flipper/flipper_esp32_over_ble.c` | 326 KB (~85K tok) | 958 | 15 |
| `*/main/main.c` (C6 203 KB, Heltec 228 KB, C5 167 KB) | — | 945 | 33 |
| `docs/SESSION_MEMORY.md` | 50 KB (~12K tok) | 326 | 148 |
| `docs/PLAN.md` | 94 KB | 262 | 72 |
| `docs/BACKLOG.md` | 42 KB | 122 | 37 |
| `docs/PROTOCOL.md` | 74 KB | 110 | 32 |
| `docs/PROJECT_HISTORY.md` | 202 KB | 107 | 8 |

**Main finding:** the model price per token matters less than the context size each call carries,
multiplied by the number of calls. Large always-read docs, monolithic sources, long sessions,
verbose subagent handbacks, and poll loops all make that product bigger.

## 2. Open decisions (need the user before the affected item starts)

| ID | Question | Recommendation |
| --- | --- | --- |
| D1 | Main-thread default model/effort for this repo | Project `model: sonnet`, `effortLevel: medium`; `/model opus` on demand for design, review, and hard debugging. Alternative: keep Opus but at `medium`. |
| D2 | Fate of review-output docs not in CLAUDE.md's table: `CODE_REVIEW_FINDINGS.md`, `CODE_REVIEW_FIX_PLAN.md`, `BL06_FINDINGS_2026-09-13.md`, `grok-4.6-findings-2026-09-11.md` | Move to `docs/archive/` and fix inbound links (BACKLOG.md:26 points at the grok doc as the home of G-items; BACKLOG.md:124 cites the fix plan). Delete only what's fully superseded by HARDENING_*. |
| D3 | Whether TP-15 (source split) goes on the roadmap | Backlog it as a design item; don't start it inside this plan. |

## 3. Items

Status: ⬜ not started · 🔶 in progress · ✅ done

### Phase A — configuration (no code, ~1 h)

| ID | Status | Item | Done when |
| --- | --- | --- | --- |
| TP-01 | ⬜ | **Model/effort per D1.** Put `model` and `effortLevel` in project `.claude/settings.json` so the repo doesn't inherit the global Opus/high. | Project settings carry the chosen values; a new session reports that model. |
| TP-02 | ⬜ | **Monitoring agents to Haiku.** `esp32-monitoring.md`, `flipper-monitoring.md` `model: sonnet` → `haiku`. These only run a capture script and report a path. | Both files say `haiku`; one capture run each still produces its log banner. |
| TP-03 | ⬜ | **Permission cleanup.** Strip project-specific and one-off entries from `~/.claude/settings.json`: dead session-scratchpad paths, exact escaped command strings, and the global allow on a `Set-Content` rewrite of `flipper_esp32_over_ble.c`. Replace them in project `settings.json` with pattern rules for `tools/*.ps1`, `tools/*.py`, host test builds, read-only `git`, and serial-port listing. Seed from `/fewer-permission-prompts`. | Global allow list holds only cross-project entries; project list is pattern-based with no scratchpad paths; flashing and erase commands are **not** auto-allowed. |
| TP-04 | ⬜ | **Repo hygiene.** Add `*_output.txt` to `.gitignore` (`heltec/flash_output.txt` is untracked). Delete `.github/agents/` if Copilot is no longer used (stale duplicates of two agents; grep noise). Apply D2. | `git status` clean of tool output; no duplicate agent defs; archived docs' inbound links resolve. |

### Phase B — shrink per-call context (largest lever, ~½ day)

| ID | Status | Item | Done when |
| --- | --- | --- | --- |
| TP-05 | ⬜ | **Re-prune `SESSION_MEMORY.md`** (50 KB → ≤10 KB, current state only; history goes to PROJECT_HISTORY.md per CLAUDE.md). **Split `PLAN.md`**: completed phases move to `PLAN_ARCHIVE.md`; PLAN.md keeps active and future phases plus binding "done when" specs. | SESSION_MEMORY ≤10 KB, PLAN ≤30 KB; no binding spec lost (diff-reviewed). |
| TP-06 | ⬜ | **Enforce the size caps with a hook.** A PostToolUse hook on Edit/Write matching those two files warns (non-blocking) when either exceeds its cap. This is the third re-bloat, so the rule needs automatic enforcement rather than another written instruction. | Hook in project settings; an over-cap test edit triggers the warning. |
| TP-07 | ⬜ | **Subagent startup and handback contract.** In the four developer agents, replace "Read SESSION_MEMORY.md first" with: the caller supplies task context; read only your board's section of SESSION_MEMORY/PLAN if needed. Add a handback rule: ≤300 words, `file:line` refs, changed-file list plus a diff summary, **never paste file contents already on disk** (tooling_hardening 2026-09-28). | All four agent files updated; the next delegated task's handback meets the contract. |
| TP-08 | ⬜ | **De-duplicate agent prompts.** The four developer agents are 11–19 KB each and repeat the same LESSONS pointers and codec/struct-convergence rules. Move the shared block into one place (a short shared skill or a single LESSONS index section) and keep only board-specific facts per agent. | Combined agent-prompt size down ≥30% with no board fact lost. |
| TP-09 | ⬜ | **Session-hygiene rules in CLAUDE.md** (short): one task per session or `/clear` between tasks; compact before about 150K; use `run_in_background`/Monitor instead of `sleep` loops; prefer a fresh agent over SendMessage when the follow-up is unrelated. Check with claude-code-guide whether an earlier auto-compact threshold is configurable before relying on one. | Rules present (≤10 lines); any verified setting applied. |
| TP-10 | ⬜ | **Single source for volatile numbers.** Memory headroom figures (Heltec IRAM/DRAM, FAP section sizes) live only in BASELINES.md; BACKLOG.md rows and memory notes point there instead of restating them. A stale restated figure cost a heltec-developer run two full builds on 2026-09-28. | `grep` finds each headroom number only in BASELINES.md; the stale memory note is updated. |

### Phase C — skills for repeated procedures (~½ day)

| ID | Status | Item | Done when |
| --- | --- | --- | --- |
| TP-11 | ⬜ | **`build-verify` skill** wrapping `tools/build_esp32.ps1`, `build_flipper.ps1`, the host test suites, and `check_shared_headers.py` for named targets, returning a pass/fail line plus the error tail only. Add `--changed` (diff vs HEAD) to `check_shared_headers.py` so staged multi-board rollouts don't reprint every OK line (tooling_hardening 2026-09-28). | One invocation builds and tests the chosen targets with ≤20 lines of output on success. |
| TP-12 | ⬜ | **`.gitattributes` LF for shared C sources** so the byte-identical cross-board files stop producing CRLF warnings on every git command (tooling_hardening 2026-09-28). | No LF/CRLF warnings touching shared files; `check_shared_headers.py` still passes. |
| TP-13 | ⬜ | **`flash-verify` skill, Haiku-only**, encoding the known per-board flash and capture procedure and reset rules (C6 native USB resets on open; C5 COM11 DTR/RTS preset; Heltec always `--no-reset`). Runs only on an explicit user flash request. | Skill exists and states the hardware-safety gate; one supervised run per board matches the manual procedure. |
| TP-14 | ⬜ | **`sync-user-guide` skill** turning CLAUDE.md's "delegate USER_GUIDE edits to Haiku" rule into one command that takes a behavior-change summary. | Skill spawns a Haiku agent; CLAUDE.md's rule points at it. |

### Phase D — structural (design pass first; gated on D3)

| ID | Status | Item | Done when |
| --- | --- | --- | --- |
| TP-15 | ⬜ | **Split monolithic sources.** `flipper_esp32_over_ble.c` (326 KB) by scene/UI, wardriving, capability dispatch, and settings; the three ESP `main.c` files likewise, taking the chance to pull still-duplicated code into `components/`. This drives about 1,900 sliced reads and much of the Grep-then-Read churn, and it's where the parallel-codec convergence bugs keep appearing. Binary and heap footprint (H04) must not regress; measure `.text`/`.bss` before and after. | Design note approved; no source file >60 KB; all five targets build; FAP section sizes within noise. |

### Phase E — measure and maintain

| ID | Status | Item | Done when |
| --- | --- | --- | --- |
| TP-16 | ⬜ | **Commit the usage-analysis script** as `tools/claude_usage.py`: per-model/scope tokens, cost weight, peak context, hottest reads, agent spawns. | Script reproduces §1's table. |
| TP-17 | ⬜ | **Prune stale memory** (GPS "hardware pending", UI-redesign in-progress, idle-timeout step pointer, and similar) so MEMORY.md reflects the current state. | Every memory file is current or deleted. |
| TP-18 | ⬜ | **Re-measure about 2 weeks after Phase B lands** and record the result in PROJECT_HISTORY.md. | Targets: median main peak context <100K; SESSION_MEMORY whole-reads per session ≤1; subagent avg context per call <90K. |

## 4. Suggested order

TP-01…04 → TP-16 (lock in the baseline tool) → TP-05…10 → TP-11…14 → TP-17 → TP-18. TP-15 only
after D3, as its own roadmap item.
