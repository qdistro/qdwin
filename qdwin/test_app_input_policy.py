#!/usr/bin/env python3
"""Pin real key-transition mechanics for application GUI scenarios."""

from pathlib import Path
import re
import sys


def fail(message: str) -> int:
    print(f"FAIL: {message}")
    return 1


def main() -> int:
    if len(sys.argv) != 4:
        return fail("usage: test_app_input_policy.py helper.sh audacity.md rdp.md")
    helper = Path(sys.argv[1]).read_text(encoding="utf-8")
    audacity = Path(sys.argv[2]).read_text(encoding="utf-8")
    rdp = Path(sys.argv[3]).read_text(encoding="utf-8")

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

    restore = helper[helper.find("\nqdwin_apps_restore_shell() {") :]
    for token in (
        "had_bystander=0",
        'grep -qE \'^(\\[[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}\\] )?qdwin: shell unbound$\'',
        "systemctl --user start qdshell.service",
        "qdwin: shell bound \\(uid=1000 pid=[0-9]+\\); replaying [0-9]+ toplevels$",
        'systemctl --user is-active --quiet qdshell.service',
    ):
        if token not in restore:
            return fail(f"shell restoration lacks compositor handoff token {token!r}")
    if not (restore.index("shell unbound$") <
            restore.index("systemctl --user start qdshell.service") <
            restore.index("qdwin: shell bound")):
        return fail("shell restoration does not release-before-start-before-bind")

    credential_tokens = (
        "for _i in $(seq 1 30)",
        "grep -q '^RDP_PASSWORD='",
        "credentials did not arrive within 6 seconds",
        'eval "$CREDS"',
    )
    missing = [token for token in credential_tokens if token not in rdp]
    if missing:
        return fail(f"RDP scenario lacks bounded credential poll: {missing}")
    if "Step 5 — disconnect cleanup" in rdp:
        return fail("RDP scenario still overclaims subscriber-disconnect coverage")

    print("PASS: GUI input uses real chords; shell takeover/restoration and "
          "RDP credential collection use bounded observable transitions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
