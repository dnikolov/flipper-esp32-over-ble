"""PostToolUse hook: warn (non-blocking) when a fast-read doc exceeds its size cap.

Caps are from docs/TOOLING_PLAN.md TP-05/TP-06. Reads the hook JSON on stdin and emits
hookSpecificOutput.additionalContext so the model sees the warning.
"""
import json
import os
import sys

CAPS = {
    "SESSION_MEMORY.md": 10 * 1024,
    "PLAN.md": 36 * 1024,
}


def main():
    try:
        event = json.load(sys.stdin)
    except ValueError:
        return
    path = (event.get("tool_input") or {}).get("file_path") or ""
    cap = CAPS.get(os.path.basename(path))
    if cap is None or os.path.basename(os.path.dirname(path)) != "docs":
        return
    try:
        size = os.path.getsize(path)
    except OSError:
        return
    if size <= cap:
        return
    msg = (
        f"{os.path.basename(path)} is {size // 1024} KB, over its {cap // 1024} KB cap "
        "(docs/TOOLING_PLAN.md TP-05). Move finished narrative to docs/PROJECT_HISTORY.md "
        "(or completed phases to docs/PLAN_ARCHIVE.md) and leave a one-line pointer."
    )
    json.dump({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": msg}}, sys.stdout)


if __name__ == "__main__":
    main()
