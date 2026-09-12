# MAI-Code-1.1-Flash findings — 2026-09-10

## Review scope

This review synthesizes the project’s markdown documentation and the implemented firmware code in [README.md](README.md), [CLAUDE.md](CLAUDE.md), [docs](docs), [esp32](esp32), and [flipper](flipper). The goal was to understand the product intent, assess risk against the documented protocol, and produce a structured, actionable review for other models or humans to continue from.

## Executive summary

This project is an authenticated BLE bridge between a Flipper Zero and an ESP32-C6. The design is intentionally layered:

- trusted-environment X25519 pairing
- runtime session authentication with AES-256-GCM
- protocol-level CBOR framing and fragmentation
- capability discovery and command/status dispatch
- experimental or production features like Wi-Fi scan, BLE scan, and wardriving

The project is not a small “Bluetooth demo”; it is a custom protocol stack and two-target firmware integration project with security and protocol design choices baked into the code and docs. The most important risk is not syntax or build correctness; it is protocol and state-machine correctness under real hardware load, especially at the BLE callback edge and when reconnects, idle disconnects, and wardriving loops overlap.

## What the project is about

The docs clearly establish the intent:

- A Flipper Zero acts as the BLE peripheral / GATT server.
- An ESP32-C6 acts as the central / GATT client.
- The protocol uses X25519 for pairing, HKDF-HMAC for derivation, and AES-256-GCM for runtime traffic.
- The runtime session is then used to issue capabilities such as Wi-Fi scan, BLE scan, and wardriving.

Primary references:

- [CLAUDE.md](CLAUDE.md)
- [docs/PROTOCOL.md](docs/PROTOCOL.md)
- [docs/PAIRING.md](docs/PAIRING.md)
- [docs/PLAN.md](docs/PLAN.md)
- [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md)

## Architecture reading

### 1. Security and transport split

The code and docs separate the phases cleanly:

- pairing/bootstrap
- session auth
- protected protocol records
- capability commands/statuses

This layering is valuable, and the project documents the protocol thoroughly. The architecture is aligned with the idea of a fixed wire contract and separate runtime state machines. The strongest evidence is in [docs/PROTOCOL.md](docs/PROTOCOL.md), with real implementation state in [esp32/main/main.c](esp32/main/main.c) and [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c).

### 2. Real-device validation is a first-class concern

The project is unusually disciplined about documenting real failures and fixes. That is a strength of this repo. For example, [docs/LESSONS.md](docs/LESSONS.md) records repeated stack-budget bugs, reassembly issues, and attachment-size mismatches that were caught only on hardware. This is exactly the kind of instrumentation needed for a low-level BLE project.

### 3. Documentation maturity is high, but operational drift is present

The repo contains an unusually strong set of design documents. However, there is also evidence of drift between current state and historical narrative:

- [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) is the current-state document meant to be fast to read.
- [docs/PROJECT_HISTORY.md](docs/PROJECT_HISTORY.md) is the narrative history.
- [docs/PLAN.md](docs/PLAN.md) is the roadmap.
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md) is the user-facing behavior guide.

This split is sensible but easy to misuse. A future model should treat the operational state as the combination of [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md), [docs/PROTOCOL.md](docs/PROTOCOL.md), and the current code, not a single document alone.

## Findings

### F1. Hardcoded MTU for application sends is likely wrong after negotiation

Severity: High
Status: Active / likely still present

Evidence:

- In [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L304-L321), `send_pairing_record()` calls `feb_fragment_capacity(FEB_DEFAULT_ATT_MTU)` unconditionally.
- The surrounding comments explicitly mention both pairing and post-negotiation traffic, but the implementation does not distinguish them.
- Same concern is described in [docs/LESSONS.md](docs/LESSONS.md) under “att-mtu-vs-attribute-length” and “flipper-facts-must-be-read-not-assumed”.

Why it matters:

- The function is currently using a conservative default MTU for all records, not the actual negotiated link capacity.
- This increases BLE packet count and connection-event contention, especially during runtime session auth and large responses.
- It is a correctness/performance issue because the comment and the protocol both assume the actual negotiated MTU is available and should be used whenever known.

Suggested fix:

- Carry negotiated MTU into the send path.
- Use the actual effective MTU for the write characteristic and only fall back to a conservative default while negotiation is still pending.

Priority: fix before expanding transport traffic volume further.

---

### F2. BLE callback-path state is still too easy to corrupt under concurrent timing

Severity: High
Status: Active risk

Evidence:

- The ESP32 uses shared state around wardriving, scan dispatch, and transaction completion in [esp32/main/main.c](esp32/main/main.c#L240-L258), [esp32/main/main.c](esp32/main/main.c#L330-L335), and [esp32/main/main.c](esp32/main/main.c#L1868-L1954).
- The code comments describe a real 2026-09-10 write-flood bug and the fix in the file itself.
- The same broad pattern appears in [docs/LESSONS.md](docs/LESSONS.md) under “nimble-host-stack-budget” and “wardriving-tx-in-flight-cleared-before-delivery-confirmed”.

Why it matters:

- This project repeatedly hits the same class of bug: BLE callback path state is mutated from multiple event sources and timers.
- The code has already had at least one hardware-confirmed write-flood and one reconnect-race bug.
- The risk is not theoretical; the project documents it as recurring rather than exceptional.

Suggested fix:

- Make a single authorized state machine for each send path and centralize transitions.
- Do not clear “in-flight” state until final write completion is confirmed.
- Reduce the number of cross-task handoff flags.
- Add a strict unit or integration test that forces overlapping send/scan/timeout transitions.

Priority: high, because it directly affects reliability under real usage.

---

### F3. The project is using a mix of historical and current documentation as if they are identical

Severity: Medium
Status: Documentation risk

Evidence:

- [docs/PLAN.md](docs/PLAN.md) contains roadmap and implementation decisions.
- [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) is the current-state summary.
- [docs/PROJECT_HISTORY.md](docs/PROJECT_HISTORY.md) is historical narrative.
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md) documents actual runtime behavior.
- [docs/LESSONS.md](docs/LESSONS.md) is a bug/lesson archive.

Why it matters:

- The repo is large and these files are intentionally layered, but the distinction is easy to miss during an AI-driven review or debugging run.
- A model may incorrectly treat a historical description as the active implementation state.

Suggested fix:

- Give each file a one-line “read this for X” contract at the top and keep it stable.
- Add a small “source of truth” section near the repo root summarizing which doc is policy vs. history vs. guide.
- Prefer code + [docs/PROTOCOL.md](docs/PROTOCOL.md) as the authoritative live contract when there is a mismatch.

Priority: medium, but important for future maintenance and AI-assisted work.

---

### F4. Pairing/session code relies heavily on size/validation invariants that are easy to regress

Severity: Medium
Status: Active risk

Evidence:

- [esp32/main/session.c](esp32/main/session.c#L443-L467) performs board-id-bound checks before deriving keys and zeroizes on overflow.
- [flipper/session.c](flipper/session.c) and [esp32/main/session.c](esp32/main/session.c) both document the need to zeroize secret material carefully.
- [docs/PROTOCOL.md](docs/PROTOCOL.md) defines canonical field ordering and strict validation requirements.

Why it matters:

- The project intentionally avoids general-purpose CBOR and implements strict canonical behavior.
- That is good for security but fragile to small regressions in field order, length checks, or AAD reconstruction.
- The project has already had real drift on these exact behaviors; the docs call this out as a recurring issue.

Suggested fix:

- Add end-to-end protocol vectors for every envelope and protected record type.
- Include negative tests for malformed field ordering, extra trailing bytes, and overlong keys.
- Keep a single versioned canonical encoder/decoder contract and generate tests from it.

Priority: medium/high in a security-sensitive codebase.

---

### F5. The repo contains many known open items that are not yet reflected as a single triage list

Severity: Low/Medium
Status: Operational debt

Evidence:

- [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) lists a large number of known open items.
- [docs/PLAN.md](docs/PLAN.md) contains roadmap and backlog items.
- [docs/CODE_REVIEW_FINDINGS.md](docs/CODE_REVIEW_FINDINGS.md) records historical code-review issues.
- [docs/OPTIMIZATION.md](docs/OPTIMIZATION.md) tracks cost and maintainability backlog.

Why it matters:

- The project has enough real-world validation and bug history that maintenance can become a “distributed knowledge” problem.
- The chance of work being re-opened or re-discovered is high if a single model is not given the active triage list.

Suggested fix:

- Create a single actionable backlog file that merges “open items,” “known risks,” and “next actions” in one place.
- Keep the narrative docs for root-cause explanation, but reduce maintenance cost by keeping a shorter active work file.

Priority: medium.

---

## Optimization opportunities

### O1. Split large stateful logic into capability modules

The largest risk area is [esp32/main/main.c](esp32/main/main.c) and [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c). They are both effectively doing transport, pair/auth, capability dispatch, and UI/state orchestration in one place.

Recommended refactor:

- `transport.c` / `transport.h`
- `pairing_state.c` / `pairing_state.h`
- `runtime_auth.c` / `runtime_auth.h`
- `capability_dispatch.c`
- `wardriving_runtime.c`

This reduces state confusion and makes hidden coupling more visible.

### O2. Standardize all magic values into generated or central config headers

The project has many defaults, limits, and protocol constants spread across shared headers and code.

A better pattern:

- single protocol constants file
- single runtime-config file
- explicit “safe defaults” and “verified runtime bounds” separation

This will reduce regressions like the historical drift in capacities and size assumptions.

### O3. Add a dedicated state-transition test harness

The biggest protocol risk is not “bad bytes” but “bad ordering under reconnects and timeouts.”

Add tests that model:

- connect -> hello -> timeout -> reconnect
- pairing window expiry mid-flow
- auth fail -> no recovery window
- wardriving start while backlog drain is active
- write-complete race with status batch chaining

This is the highest-value runtime validation regime for this codebase.

### O4. Make BLE callback buffers non-stack by default

The project already documents repeated stack overflows and callback-path memory pressure. This should be treated as a default rule, not a debugging lesson.

Rule of thumb:

- any non-trivial buffer on a BLE event or timer path => static or heap-backed
- never allow large nested structs on the callback stack
- make stack-budget checks a required part of firmware review

### O5. Use typed state machines instead of scattered booleans

Multiple booleans describe state transitions: pairing, auth, in-flight send, active scan, source ownership, and pending self-stop.

This is error-prone. A typed state enum plus guarded transitions around those state changes would reduce classification mistakes.

## Project strengths

1. Clear architecture and protocol documentation.
2. Real bug tracking and documentation of root causes in [docs/LESSONS.md](docs/LESSONS.md) and [docs/PROJECT_HISTORY.md](docs/PROJECT_HISTORY.md).
3. Explicit security requirements and decisions recorded in [docs/PROTOCOL.md](docs/PROTOCOL.md) and [docs/DECISIONS.md](docs/DECISIONS.md).
4. Hardware-aware design: the repo is not treating the ESP32 and Flipper as generic devices; it is actively encoding their constraints.
5. Good separation between protocol contract and implementation state, even though the code is still large and complex.

## Recommended next actions for another model

1. Start from [docs/PROTOCOL.md](docs/PROTOCOL.md) and [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md), not from a single feature file.
2. Focus first on the BLE callback/state-machine risk in [esp32/main/main.c](esp32/main/main.c).
3. Address the MTU/send-path mismatch in [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c).
4. Add or expand protocol regression tests before modifying core framing or session logic.
5. Treat the wardriving code path as concurrency-sensitive and validated only under real hardware conditions.

## Bottom line

The project is unusually well documented and far more advanced than a typical BLE prototype. The codebase is credible and hardware-focused, but its highest-risk parts are exactly the ones that are easiest to get wrong in embedded BLE systems: lifecycle coordination, timing-sensitive reconnect logic, and callback path state.

The safest next move is not “more feature work.” It is state-machine hardening and regression validation around the current BLE/session/wardriving logic.
