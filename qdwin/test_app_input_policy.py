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
    handoff_pattern = r"(^|\] )qdwin: shell unbound$"
    if "grep -qE '(^|\\] )qdwin: shell unbound$'" not in helper:
        return fail("shell handoff poll does not use the anchored, weston-"
                    "timestamp-tolerant filter")
    actual_message = "[18:27:17.713] qdwin: shell unbound"
    if re.search(handoff_pattern, actual_message) is None:
        return fail("shell handoff filter rejects an actual weston journal message")
    if re.search(handoff_pattern, actual_message + " late") is not None:
        return fail("shell handoff filter is not end-anchored")

    print("PASS: app GUI input uses real chords and shell handoff accepts "
          "weston-prefixed journal messages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
