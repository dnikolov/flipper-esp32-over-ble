---
name: sync-user-guide
description: Sync a already-decided, already-implemented behavior change into docs/USER_GUIDE.md by delegating the mechanical prose edit to a cheap model. Use after landing a change that touches anything the guide documents (on-screen text, button/LED behavior, build/flash/pairing/reconnect/reset steps, known quirks) — not for open-ended or not-yet-verified work.
argument-hint: [behavior-change summary] [optional file/commit refs]
---

Implements TP-14 from `docs/TOOLING_PLAN.md`. This is CLAUDE.md's existing "keep USER_GUIDE.md
in sync" rule, turned into one command — it doesn't change what must be documented, only who
(cheaply) does the edit.

## When to use

Only for a change that is already decided and already implemented/hardware-verified, where the
remaining work is reflecting known prose into `docs/USER_GUIDE.md`'s existing structure and
tone — not for drafting new scope, resolving an ambiguity, or documenting anything not yet
implemented. `docs/USER_GUIDE.md`'s own top-of-file note states its scope (implemented and
hardware-verified behavior only, currently through Phase 3/3a); do not add a step-6-or-later or
otherwise unverified feature to it just because this skill ran.

## Input

Take from the invocation (or ask if missing):
- A short summary of the behavior change (what changed, from the user's point of view).
- Optionally, file paths or commit refs the change lives in, so the subagent can confirm
  current behavior against real code rather than trusting the summary alone.

## Procedure

1. Do not edit `docs/USER_GUIDE.md` yourself. Spawn a subagent via the **Agent** tool with
   `model: "haiku"` — this is mechanical prose sync, not work needing the main model's
   reasoning budget (see `feedback_cheap_model_for_docs` memory and CLAUDE.md's Conventions
   section).
2. Give that subagent, in the prompt:
   - The behavior-change summary and any file/commit refs verbatim.
   - `docs/USER_GUIDE.md`'s own top-of-file scope note (implemented + hardware-verified only)
     and an instruction to match the file's existing structure/section layout and tone rather
     than inventing new sections or reformatting unrelated parts.
   - An instruction to read the specific section(s) of `docs/USER_GUIDE.md` the change affects
     before editing, and to read any cited source files/commits to confirm the described
     behavior rather than paraphrasing the summary blind.
   - A hard instruction not to touch anything outside `docs/USER_GUIDE.md`.
3. Ask the subagent to report back: which section(s) it changed, and a one-line description of
   the edit per section.

## Report

Relay the subagent's changed-section list back to the user. If the subagent flagged that the
described change is out of the guide's scope (not yet hardware-verified, or belongs to a later
phase), surface that rather than forcing the edit through.
