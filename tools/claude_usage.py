#!/usr/bin/env python3
"""Reproduces docs/TOOLING_PLAN.md's usage-analysis tables (TP-16) from this project's Claude
Code session transcripts: per-scope/model-family token and approximate-cost-weight totals,
average and peak per-call context, compaction counts, agent-spawn counts by type and model
override, the hottest Read targets (whole-file vs partial), and a Bash/PowerShell sleep-poll
count.

Source data: `~/.claude/projects/<project-slug>/*.jsonl` (main-session transcripts) plus each
session's `<session-id>/subagents/agent-*.jsonl` (subagent transcripts, `isSidechain: true`).
Merged from two prototype scripts written during the 2026-09-28 tooling review
(usage.py + u2.py).

Dollar figures use approximate list prices (input/output/cache-write-5m/cache-read $ per
million tokens) and are a COST WEIGHT, not a bill -- see docs/TOOLING_PLAN.md section 1's own
caveat: on a subscription this shows how fast rate limits get used up, not what gets charged.

Usage:
    python tools/claude_usage.py [--since YYYY-MM-DD] [--project-dir PATH]

--project-dir defaults to this machine's `~/.claude/projects/<slug>` for the current working
directory, using the same slugging Claude Code itself uses (lowercase drive letter, `:`/`\\`/`/`
each replaced with `-`).
"""

import argparse
import collections
import glob
import json
import os
import re
import statistics
import sys

# Approximate list prices, $ per million tokens: (input, output, cache_write_5m, cache_read).
PRICING = {
    "opus": (5, 25, 6.25, 0.5),
    "sonnet": (3, 15, 3.75, 0.3),
    "haiku": (1, 5, 1.25, 0.1),
    "fable": (5, 25, 6.25, 0.5),
}


def model_family(model_name):
    name = (model_name or "").lower()
    for family in PRICING:
        if family in name:
            return family
    return "other"


def default_project_dir():
    cwd = os.getcwd()
    if len(cwd) >= 2 and cwd[1] == ":":
        cwd = cwd[0].lower() + cwd[1:]
    slug = re.sub(r"[:\\/]", "-", cwd)
    return os.path.expanduser(os.path.join("~", ".claude", "projects", slug))


def iter_transcripts(project_dir):
    """Yields (path, scope) for every session transcript: 'main' for a top-level
    <session-id>.jsonl, 'sub' for anything under a <session-id>/subagents/ directory."""
    for path in glob.glob(os.path.join(project_dir, "**", "*.jsonl"), recursive=True):
        rel = os.path.relpath(path, project_dir)
        parts = rel.split(os.sep)
        scope = "sub" if "subagents" in parts[:-1] else "main"
        yield path, scope


def cost_weight(family, u):
    if family not in PRICING:
        return 0.0
    p = PRICING[family]
    return (
        u.get("input_tokens", 0) * p[0]
        + u.get("output_tokens", 0) * p[1]
        + u.get("cache_creation_input_tokens", 0) * p[2]
        + u.get("cache_read_input_tokens", 0) * p[3]
    ) / 1e6


def context_size(u):
    return (
        u.get("input_tokens", 0)
        + u.get("cache_read_input_tokens", 0)
        + u.get("cache_creation_input_tokens", 0)
    )


def is_sleep_poll(tool_name, command):
    if tool_name not in ("Bash", "PowerShell") or not command:
        return False
    return re.search(r"(?<![\w-])(sleep|start-sleep)\b", command, re.IGNORECASE) is not None


class Stats:
    def __init__(self):
        self.tok = collections.defaultdict(collections.Counter)  # (scope, family) -> counters
        self.calls_ctx = collections.defaultdict(list)  # scope -> [context sizes] (all calls)
        self.session_peak_ctx = collections.defaultdict(list)  # scope -> [peak per transcript]
        self.compactions = 0
        self.agent_spawns = collections.Counter()  # subagent_type -> count
        self.agent_spawn_model = collections.Counter()  # (subagent_type, model) -> count
        self.reads = collections.Counter()  # basename -> [whole, partial]
        self.sleep_polls = collections.Counter()  # (tool, scope) -> count
        self.seen_calls = set()

    def read_entry(self, basename):
        pair = self.reads.get(basename)
        if pair is None:
            pair = [0, 0]
            self.reads[basename] = pair
        return pair


def process_transcript(path, scope, since, stats):
    peak = 0
    with open(path, encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                e = json.loads(line)
            except (json.JSONDecodeError, ValueError):
                continue

            ts = e.get("timestamp", "")
            if since and ts and ts[:10] < since:
                continue

            if e.get("type") == "system" and "compact" in str(e.get("subtype", "")):
                stats.compactions += 1
            if e.get("isCompactSummary"):
                stats.compactions += 1

            msg = e.get("message")
            if e.get("type") != "assistant" or not isinstance(msg, dict):
                continue

            usage = msg.get("usage") or {}
            family = model_family(msg.get("model"))
            call_key = (msg.get("id"), e.get("requestId"))
            if usage and msg.get("id") and call_key not in stats.seen_calls:
                stats.seen_calls.add(call_key)
                c = stats.tok[(scope, family)]
                c["in"] += usage.get("input_tokens", 0)
                c["out"] += usage.get("output_tokens", 0)
                c["cache_write"] += usage.get("cache_creation_input_tokens", 0)
                c["cache_read"] += usage.get("cache_read_input_tokens", 0)
                c["calls"] += 1
                c["usd"] += cost_weight(family, usage)

                ctx = context_size(usage)
                stats.calls_ctx[scope].append(ctx)
                peak = max(peak, ctx)

            for block in msg.get("content") or []:
                if not isinstance(block, dict) or block.get("type") != "tool_use":
                    continue
                name = block.get("name")
                tool_input = block.get("input") or {}

                if name == "Read":
                    basename = os.path.basename(tool_input.get("file_path", "") or "")
                    if basename:
                        pair = stats.read_entry(basename)
                        if tool_input.get("limit") or tool_input.get("offset"):
                            pair[1] += 1
                        else:
                            pair[0] += 1

                if name in ("Agent", "Task"):
                    subagent_type = tool_input.get("subagent_type", "general-purpose")
                    model_used = tool_input.get("model", "(default)")
                    stats.agent_spawns[subagent_type] += 1
                    stats.agent_spawn_model[(subagent_type, model_used)] += 1

                if name in ("Bash", "PowerShell") and is_sleep_poll(
                    name, tool_input.get("command", "")
                ):
                    stats.sleep_polls[(name, scope)] += 1

    stats.session_peak_ctx[scope].append(peak)


def pct(values, p):
    if not values:
        return 0
    values = sorted(values)
    idx = min(len(values) - 1, int(len(values) * p))
    return values[idx]


def fmt_m(n):
    return f"{n / 1e6:.2f}M"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--since", default=None, help="only count events at/after this date (YYYY-MM-DD)")
    parser.add_argument(
        "--project-dir",
        default=None,
        help="path to ~/.claude/projects/<slug> (default: derived from the current directory)",
    )
    args = parser.parse_args()

    project_dir = args.project_dir or default_project_dir()
    if not os.path.isdir(project_dir):
        print(f"error: project dir not found: {project_dir}", file=sys.stderr)
        return 1

    stats = Stats()
    for path, scope in iter_transcripts(project_dir):
        process_transcript(path, scope, args.since, stats)

    since_note = f" (since {args.since})" if args.since else ""
    print(f"Project: {project_dir}{since_note}\n")

    print("== Tokens / cost-weight by scope x model family (approximate list-price weight)")
    print(f"{'scope':<6} {'family':<8} {'calls':>7} {'in(M)':>8} {'out(M)':>8} {'cache_w(M)':>11} {'cache_r(M)':>11} {'$weight':>10}")
    total_usd = 0.0
    for (scope, family), c in sorted(stats.tok.items()):
        total_usd += c["usd"]
        print(
            f"{scope:<6} {family:<8} {c['calls']:>7} {fmt_m(c['in']):>8} {fmt_m(c['out']):>8} "
            f"{fmt_m(c['cache_write']):>11} {fmt_m(c['cache_read']):>11} {c['usd']:>10.2f}"
        )
    print(f"{'TOTAL approx $ weight':<45} {total_usd:>10.2f}\n")

    print("== Avg context per call, peak context per session/agent (median/p90), by scope")
    print(f"{'scope':<6} {'calls':>7} {'avg_ctx':>9} {'sessions':>9} {'peak_median':>12} {'peak_p90':>10}")
    for scope in sorted(stats.calls_ctx):
        calls = stats.calls_ctx[scope]
        peaks = stats.session_peak_ctx[scope]
        avg_ctx = statistics.mean(calls) if calls else 0
        print(
            f"{scope:<6} {len(calls):>7} {avg_ctx:>9.0f} {len(peaks):>9} "
            f"{pct(peaks, 0.5):>12.0f} {pct(peaks, 0.9):>10.0f}"
        )
    print()

    print(f"== Compactions: {stats.compactions}\n")

    print("== Agent spawns by type (top 20)")
    for k, v in stats.agent_spawns.most_common(20):
        print(f"  {k:<24} {v}")
    print("\n== Agent spawns by type/model override (top 20)")
    for (subtype, model), v in stats.agent_spawn_model.most_common(20):
        print(f"  {subtype:<24} {model:<20} {v}")
    print()

    print("== Hottest Read targets, whole vs partial (top 25)")
    print(f"{'file':<40} {'whole':>7} {'partial':>9} {'total':>7}")
    ranked = sorted(stats.reads.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))
    for name, (whole, partial) in ranked[:25]:
        print(f"{name:<40} {whole:>7} {partial:>9} {whole + partial:>7}")
    print()

    print("== Bash/PowerShell sleep-poll count (approximate: any 'sleep'/'Start-Sleep' token)")
    total_sleep = sum(stats.sleep_polls.values())
    for (tool, scope), v in sorted(stats.sleep_polls.items()):
        print(f"  {tool:<12} {scope:<6} {v}")
    print(f"  TOTAL: {total_sleep}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
