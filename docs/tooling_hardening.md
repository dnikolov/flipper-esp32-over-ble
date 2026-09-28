# Tooling hardening notes

Running log of tooling/process friction and token-optimization opportunities noticed
while doing implementation work, so they can be batched into fixes later. Not a backlog
of product bugs — see BACKLOG.md/HARDENING_BACKLOG.md for those. Append entries as they're
found; don't polish this into narrative prose.

## 2026-09-28 — Part A/B/C implementation (CSV row count, flush-gate redesign, mesh-dedup shrink)

- **CRLF/LF churn on cross-board file copies.** Copying `wardriving_validate.h/c` and
  `wardriving_dedup.c/h` from `esp32/main/` to `esp32c5/main/` and `heltec/main/` with a
  plain `cp` triggers `warning: LF will be replaced by CRLF` from every subsequent git
  command touching those paths (Windows core.autocrlf normalizing on checkout). Harmless
  here since `check_shared_headers.py` still reported byte-identical, but it's noise on
  every git status/diff/add touching these files going forward. Worth deciding once
  whether these C sources should be LF-normalized via `.gitattributes` (matching how the
  three boards' shared files are meant to be byte-identical anyway) instead of re-noticing
  this warning per session.
- **Token cost: subagent hand-back reports duplicate large code blocks already on disk.**
  The esp32-developer Part B report pasted the full final content of 4 files inline "for
  byte-identical copy to esp32c5/heltec" — but the orchestrator copied the files directly
  from the esp32/ working tree with `cp` instead, making that ~2K-token paste unused output.
  Optimization: when delegating a "shared file" step, tell the subagent explicitly *not* to
  paste full file contents back (the orchestrator can read/copy from disk), only to name
  the changed files and summarize the diff. This alone would have saved a meaningful
  fraction of that agent's report size.
- **`tools/check_shared_headers.py` run mid-flight over-reports.** Running it after only
  the esp32/ board's edit (before esp32c5/heltec catch up) correctly flags MISMATCH on the
  four in-flight files, but also re-prints the full OK list for every unrelated shared file
  every time. For a multi-board rollout done in stages, a `--only <path>` or `--changed`
  filter (diff against git HEAD) would cut a lot of repeated, unchanging OK-line output
  across the several times this script gets run over one plan's lifetime.
- **Stale Heltec DRAM-headroom figure cost a subagent real effort.** The auto-memory note
  and `docs/BACKLOG.md` BL25/BL27 still say "~8–40 B headroom," but the 2026-09-28 hardening
  pass (same day, landed earlier in the session) had already moved that to 364 B in
  BASELINES.md. The heltec-developer subagent, working from the stale figure, treated its
  own +16 B `.bss` growth as a possible near-crisis and burned a full stash/rebuild/`nm`-diff
  cycle to precisely characterize it, then flagged it back to the orchestrator as urgent —
  which then had to spend a further `idf.py size` round-trip confirming it was actually fine
  (348 B remaining). Two full ESP-IDF builds' worth of tokens/wall-clock went to resolving a
  false alarm. Whichever doc is meant to be the single current-headroom source of truth
  (BASELINES.md looks like the intended one) should be the only place this number lives, and
  BACKLOG.md/memory notes should point at it rather than restating the number themselves, so
  a fix landing in one place can't leave a stale copy elsewhere for the next agent to trip on.
