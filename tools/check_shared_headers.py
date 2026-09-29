#!/usr/bin/env python3
"""Compares the shared esp32/flipper headers (HEADER_PAIRS) and the per-board module copies
(BOARD_MODULES) that esp32/, esp32c5/ and heltec/ each carry.

Convention (stated in both docs/*-developer.md agent files): these headers' actual API
surface -- function signatures, struct/enum layouts, macro names and values -- must stay
byte-identical between firmwares, with only comment wording allowed to differ. This script
strips comments and whitespace, then diffs #define macros and function prototypes between
each pair so that divergence is caught by running one command instead of a manual read-both-
files pass.

It is a heuristic regex scan, not a C parser: it does not check struct/enum bodies (only
their surrounding macros/prototypes), and a mismatch it reports may still need a human read
of the surrounding block to see the actual struct-shape difference (as happened with
feb_wardriving_record_t: a tagged union on one side, two always-present named fields on the
other -- same macros, same prototypes, invisible to this script's regexes). Treat a clean run
as "no macro/prototype drift found", not "the two headers are proven equivalent".

Per-board copies (HP-19): BOARD_IDENTICAL files must be byte-identical across all three
boards (compared after CRLF->LF only). BOARD_EQUIVALENT files may differ in comments and
whitespace only: their #define names/values are diffed (reported individually), and the rest of
the comment-stripped, whitespace-collapsed code must match too, which also catches struct-body
drift the flipper-pair check can't. BOARD_ALLOWED_MACRO_DIFFS lists the only intended
per-board differences (GPS pins); their values are masked before the code comparison and
printed as INFO. Multi-line #define continuations are joined before extraction everywhere.
Longer term these modules belong in a shared component (docs/HARDENING_PLAN.md HP-19).

Usage: python tools/check_shared_headers.py
Exit code 0 if every pair and every per-board module matches, 1 on any mismatch or a missing
per-board file.

--changed: only check groups (a HEADER_PAIRS pair, or one BOARD_IDENTICAL/BOARD_EQUIVALENT
module across its three board copies) where at least one member file differs from git HEAD
(tracked changes via `git diff --name-only HEAD`, plus untracked files via
`git ls-files --others --exclude-standard`). Prints MISMATCH blocks only -- no OK/INFO lines --
plus a one-line summary of how many groups were touched and how many mismatched. Intended for
staged multi-board rollouts where most groups are untouched and reprinting every OK line is
pure noise; the plain (no-flag) invocation is unchanged and remains the full report.
"""

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HEADER_PAIRS = [
    ("components/feb_protocol/cbor_codec.h", "flipper/cbor_codec.h"),
    ("components/feb_protocol/cbor_primitives.h", "flipper/cbor_primitives.h"),
    ("components/feb_protocol/cbor_records.h", "flipper/cbor_records.h"),
    ("components/feb_protocol/cbor_wifi_scan.h", "flipper/cbor_wifi_scan.h"),
    ("components/feb_protocol/cbor_ble_scan.h", "flipper/cbor_ble_scan.h"),
    ("components/feb_protocol/cbor_wardriving.h", "flipper/cbor_wardriving.h"),
    ("components/feb_protocol/cbor_gps.h", "flipper/cbor_gps.h"),
    ("components/feb_protocol/cbor_meshcore.h", "flipper/cbor_meshcore.h"),
    ("components/feb_protocol/cbor_mesh_log.h", "flipper/cbor_mesh_log.h"),
    ("components/feb_protocol/framing.h", "flipper/framing.h"),
    ("components/feb_protocol/pairing.h", "flipper/pairing.h"),
    ("components/feb_protocol/pairing_crypto.h", "flipper/pairing_crypto.h"),
    ("components/feb_protocol/session.h", "flipper/session.h"),
    ("components/feb_protocol/session_crypto.h", "flipper/session_crypto.h"),
]

BOARDS = ["esp32", "esp32c5", "heltec"]

BOARD_IDENTICAL = []

BOARD_EQUIVALENT = [
    "location.h",
    "location.c",
]

# Intended per-board differences, derived by diffing the current copies (2026-09-28): only the
# GPS UART pins differ (C6 RX18/TX19, C5 RX4/TX5, Heltec RX17/TX23).
BOARD_ALLOWED_MACRO_DIFFS = {
    "location.c": {"FEB_GPS_UART_RX_GPIO", "FEB_GPS_UART_TX_GPIO"},
}


def join_continuations(text):
    return re.sub(r"\\[ \t]*\r?\n", " ", text)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return join_continuations(text)


def extract_macros(text):
    macros = {}
    for m in re.finditer(r"^[ \t]*#define[ \t]+(\w+)(\([^)]*\))?[ \t]*(.*)$", text, re.M):
        name, params, value = m.group(1), m.group(2) or "", m.group(3)
        macros[name] = re.sub(r"\s+", "", params + value)
    return macros


def extract_prototypes(text):
    protos = {}
    for m in re.finditer(r"([A-Za-z_][\w \t\*]*?)\b(feb_\w+)\s*\(([^;]*?)\)\s*;", text, re.S):
        ret, name, params = m.group(1), m.group(2), m.group(3)
        norm_ret = re.sub(r"\s+", "", ret).replace("*", " *").strip()
        norm_params = re.sub(r"\s+", "", params)
        protos[name] = (norm_ret, norm_params)
    return protos


def compare(path_a, path_b):
    text_a = strip_comments((ROOT / path_a).read_text())
    text_b = strip_comments((ROOT / path_b).read_text())

    macros_a, macros_b = extract_macros(text_a), extract_macros(text_b)
    protos_a, protos_b = extract_prototypes(text_a), extract_prototypes(text_b)

    problems = []

    only_a = sorted(set(macros_a) - set(macros_b))
    only_b = sorted(set(macros_b) - set(macros_a))
    if only_a:
        problems.append(f"  macros only in {path_a}: {only_a}")
    if only_b:
        problems.append(f"  macros only in {path_b}: {only_b}")
    for name in sorted(set(macros_a) & set(macros_b)):
        if macros_a[name] != macros_b[name]:
            problems.append(
                f"  macro {name} differs: {path_a}={macros_a[name]!r} {path_b}={macros_b[name]!r}"
            )

    only_a = sorted(set(protos_a) - set(protos_b))
    only_b = sorted(set(protos_b) - set(protos_a))
    if only_a:
        problems.append(f"  functions only in {path_a}: {only_a}")
    if only_b:
        problems.append(f"  functions only in {path_b}: {only_b}")
    for name in sorted(set(protos_a) & set(protos_b)):
        if protos_a[name] != protos_b[name]:
            problems.append(
                f"  function {name} signature differs:\n"
                f"    {path_a}: {protos_a[name]}\n"
                f"    {path_b}: {protos_b[name]}"
            )

    return problems


def read_board_file(board, name):
    path = ROOT / board / "main" / name
    if not path.is_file():
        return None
    return path.read_bytes().replace(b"\r\n", b"\n")


def normalized_code(text, masked_macros):
    def mask(m):
        return f"#define {m.group(1)} <per-board>" if m.group(1) in masked_macros else m.group(0)

    text = re.sub(r"^[ \t]*#define[ \t]+(\w+)\b.*$", mask, text, flags=re.M)
    return re.sub(r"\s+", " ", text).strip()


def compare_board_module(name, identical):
    problems, info = [], []
    raw = {b: read_board_file(b, name) for b in BOARDS}
    missing = [b for b in BOARDS if raw[b] is None]
    if missing:
        problems.append(f"  missing in: {missing}")
        return problems, info
    ref = BOARDS[0]

    if identical:
        digests = {b: hashlib.sha256(raw[b]).hexdigest()[:12] for b in BOARDS}
        if len(set(digests.values())) != 1:
            problems.append("  not byte-identical: " + ", ".join(f"{b}={d}" for b, d in digests.items()))
        return problems, info

    allowed = BOARD_ALLOWED_MACRO_DIFFS.get(name, set())
    texts = {b: strip_comments(raw[b].decode("utf-8", "replace")) for b in BOARDS}
    macros = {b: extract_macros(texts[b]) for b in BOARDS}
    all_names = sorted(set().union(*(set(m) for m in macros.values())))
    for macro in all_names:
        present = [b for b in BOARDS if macro in macros[b]]
        if len(present) != len(BOARDS):
            absent = [b for b in BOARDS if b not in present]
            problems.append(f"  macro {macro} only in {present} (missing in {absent})")
            continue
        values = {b: macros[b][macro] for b in BOARDS}
        if len(set(values.values())) != 1:
            rendered = ", ".join(f"{b}={v!r}" for b, v in values.items())
            if macro in allowed:
                info.append(f"  INFO allowed per-board macro {macro}: {rendered}")
            else:
                problems.append(f"  macro {macro} differs: {rendered}")

    if not problems:
        codes = {b: normalized_code(texts[b], allowed) for b in BOARDS}
        for b in BOARDS[1:]:
            if codes[b] != codes[ref]:
                a_code, b_code = codes[ref], codes[b]
                i = next((k for k in range(min(len(a_code), len(b_code))) if a_code[k] != b_code[k]),
                         min(len(a_code), len(b_code)))
                problems.append(
                    f"  code differs (outside comments/macros) {ref} vs {b} near: "
                    f"{a_code[max(0, i - 40):i + 40]!r} / {b_code[max(0, i - 40):i + 40]!r}"
                )
    return problems, info


def get_changed_files():
    changed = set()
    for cmd in (
        ["git", "diff", "--name-only", "HEAD"],
        ["git", "ls-files", "--others", "--exclude-standard"],
    ):
        try:
            out = subprocess.run(
                cmd, cwd=ROOT, capture_output=True, text=True, check=True
            ).stdout
        except (OSError, subprocess.CalledProcessError):
            continue
        changed.update(line.strip() for line in out.splitlines() if line.strip())
    return {c.replace("\\", "/") for c in changed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--changed",
        action="store_true",
        help="only check groups touching a file that differs from git HEAD (tracked + "
        "untracked); print MISMATCH lines and a one-line summary only",
    )
    args = parser.parse_args()

    changed_files = get_changed_files() if args.changed else None

    any_problems = False
    touched = 0
    mismatched = 0

    for path_a, path_b in HEADER_PAIRS:
        if changed_files is not None and not ({path_a, path_b} & changed_files):
            continue
        touched += 1
        problems = compare(path_a, path_b)
        if problems:
            any_problems = True
            mismatched += 1
            print(f"MISMATCH: {path_a} <-> {path_b}")
            for p in problems:
                print(p)
        elif not args.changed:
            print(f"OK: {path_a} <-> {path_b}")

    for names, identical in ((BOARD_IDENTICAL, True), (BOARD_EQUIVALENT, False)):
        for name in names:
            if changed_files is not None:
                board_paths = {f"{b}/main/{name}" for b in BOARDS}
                if not (board_paths & changed_files):
                    continue
            touched += 1
            problems, info = compare_board_module(name, identical)
            label = f"{'/'.join(BOARDS)} main/{name}" + (" [byte-identical]" if identical else "")
            if problems:
                any_problems = True
                mismatched += 1
                print(f"MISMATCH: {label}")
                for p in problems:
                    print(p)
                for line in info:
                    print(line)
            elif not args.changed:
                print(f"OK: {label}")
                for line in info:
                    print(line)

    if args.changed:
        print(f"--changed: {touched} group(s) touched, {mismatched} mismatch(es)")
        return 1 if any_problems else 0

    if any_problems:
        print(
            "\nSee this script's docstring: macro/prototype match does not prove struct "
            "bodies agree -- read the surrounding block for any file flagged above."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
