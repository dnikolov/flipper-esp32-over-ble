#!/usr/bin/env python3
"""Compares the API surface of the shared esp32/flipper headers listed in HEADER_PAIRS.

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

Usage: python tools/check_shared_headers.py
Exit code 0 if every pair matches, 1 if any pair has a macro or prototype-level mismatch.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HEADER_PAIRS = [
    ("esp32/main/cbor_codec.h", "flipper/cbor_codec.h"),
    ("esp32/main/cbor_primitives.h", "flipper/cbor_primitives.h"),
    ("esp32/main/cbor_records.h", "flipper/cbor_records.h"),
    ("esp32/main/cbor_wifi_scan.h", "flipper/cbor_wifi_scan.h"),
    ("esp32/main/cbor_ble_scan.h", "flipper/cbor_ble_scan.h"),
    ("esp32/main/cbor_wardriving.h", "flipper/cbor_wardriving.h"),
    ("esp32/main/framing.h", "flipper/framing.h"),
    ("esp32/main/pairing.h", "flipper/pairing.h"),
    ("esp32/main/pairing_crypto.h", "flipper/pairing_crypto.h"),
    ("esp32/main/session.h", "flipper/session.h"),
    ("esp32/main/session_crypto.h", "flipper/session_crypto.h"),
]


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


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


def main():
    any_problems = False
    for path_a, path_b in HEADER_PAIRS:
        problems = compare(path_a, path_b)
        if problems:
            any_problems = True
            print(f"MISMATCH: {path_a} <-> {path_b}")
            for p in problems:
                print(p)
        else:
            print(f"OK: {path_a} <-> {path_b}")

    if any_problems:
        print(
            "\nSee this script's docstring: macro/prototype match does not prove struct "
            "bodies agree -- read the surrounding block for any file flagged above."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
