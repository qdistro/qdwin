#!/usr/bin/env python3
"""Pin real key-transition mechanics for application GUI scenarios."""

from pathlib import Path
import re
import sys


def fail(message: str) -> int:
    print(f"FAIL: {message}")
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: test_app_input_policy.py helper.sh audacity.md")
    helper = Path(sys.argv[1]).read_text(encoding="utf-8")
    audacity = Path(sys.argv[2]).read_text(encoding="utf-8")

    required = (
        "qdwin_apps_qmp_key()",
        '\\"execute\\": \\"input-send-event\\"',
        "qdwin_apps_chord()",
        'qdwin_apps_qmp_key "$key" down',
        'qdwin_apps_qmp_key "$key" up',
    )
    missing = [token for token in required if token not in helper]
    if missing:
        return fail(f"app input helper missing real-transition token(s): {missing}")
    if '$QDWIN_VIRSH send-key' in helper:
        return fail("app helper regressed to simultaneous virsh send-key injection")
    if "qdwin_apps_chord alt -- f" not in audacity:
        return fail("Audacity File-menu assertion does not use a modifier-held chord")

    # weston_log() embeds its own timestamp in MESSAGE, even with journalctl
    # -o cat. The shell handoff poll must accept that real prefix while keeping
    # the terminal event anchored so an adjacent diagnostic cannot satisfy it.
    handoff_pattern = (
        r"^(\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] )?"
        r"qdwin: shell unbound$"
    )
    shell_filter = (
        "grep -qE '^(\\[[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}\\] )?"
        "qdwin: shell unbound$'"
    )
    if shell_filter not in helper:
        return fail("shell handoff poll does not use the anchored, weston-"
                    "timestamp-tolerant filter")
    positives = (
        "qdwin: shell unbound",
        "[18:27:17.713] qdwin: shell unbound",
    )
    negatives = (
        "noise] qdwin: shell unbound",
        "prefix [foo] qdwin: shell unbound",
        "[8:27:17.713] qdwin: shell unbound",
        "[18:27:17.71] qdwin: shell unbound",
        "[18:27:17x713] qdwin: shell unbound",
        "[18:27:17.713] qdwin: shell unbound late",
    )
    rejected = [message for message in positives
                if re.fullmatch(handoff_pattern, message) is None]
    accepted = [message for message in negatives
                if re.fullmatch(handoff_pattern, message) is not None]
    if rejected:
        return fail(f"shell handoff filter rejects valid message(s): {rejected}")
    if accepted:
        return fail(f"shell handoff filter accepts invalid message(s): {accepted}")

    print("PASS: app GUI input uses real chords and shell handoff accepts "
          "weston-prefixed journal messages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
