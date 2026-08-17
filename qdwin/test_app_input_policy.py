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
        "restore: qdshell already owns compositor shell role pid=$qpid",
        "systemctl --user show qdshell.service -p MainPID --value",
        'pid=$qpid\\\\); replaying [0-9]+ toplevels$',
        "had_bystander=0",
        'grep -qE \'^(\\[[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}\\] )?qdwin: shell unbound$\'',
        "systemctl --user start qdshell.service",
        'systemctl --user is-active --quiet qdshell.service',
    ):
        if token not in restore:
            return fail(f"shell restoration lacks compositor handoff token {token!r}")
    had_pos = restore.index("had_bystander=0")
    unbound_pos = restore.index("shell unbound$", had_pos)
    start_pos = restore.index("systemctl --user start qdshell.service")
    bound_pos = restore.index("qdwin: shell bound", start_pos)
    if not (unbound_pos < start_pos < bound_pos):
        return fail("shell restoration does not release-before-start-before-bind")

    # Behavioral truth table for the already-restored fast path. Active alone
    # is insufficient: the latest compositor lifecycle record must be a bound
    # record naming the service's live MainPID. This makes a second EXIT-trap
    # restore harmless without accepting stale/mismatched ownership.
    def already_bound(active, main_pid, latest):
        if not active or not main_pid or main_pid <= 0:
            return False
        pattern = (
            r"^(?:\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] )?"
            rf"qdwin: shell bound \(uid=1000 pid={main_pid}\); "
            r"replaying [0-9]+ toplevels$"
        )
        return re.fullmatch(pattern, latest) is not None

    positives = (
        (True, 2299, "qdwin: shell bound (uid=1000 pid=2299); replaying 0 toplevels"),
        (True, 2299, "[19:00:13.073] qdwin: shell bound (uid=1000 pid=2299); replaying 4 toplevels"),
    )
    negatives = (
        (False, 2299, positives[0][2]),
        (True, 0, positives[0][2]),
        (True, 2300, positives[0][2]),
        (True, 2299, "qdwin: shell unbound"),
        (True, 2299, "prefix qdwin: shell bound (uid=1000 pid=2299); replaying 0 toplevels"),
    )
    if not all(already_bound(*case) for case in positives):
        return fail("already-bound restore model rejects a valid terminal state")
    if any(already_bound(*case) for case in negatives):
        return fail("already-bound restore model accepts inactive/stale ownership")

    credential_tokens = (
        "for _i in $(seq 1 30)",
        "grep -q '^RDP_PASSWORD='",
        "credentials did not arrive within 6 seconds",
        'eval "$CREDS"',
        "RDP_AUTH_CURSOR=$(qdwin_apps_journal_cursor)",
        'qdwin_apps_log_since_cursor "$RDP_AUTH_CURSOR"',
        'pid=$FORWARD_PID reason=\\"forward exited\\"',
        'pid=$SECOND_FORWARD_PID reason=\\"forward exited\\"',
        "ALL_TORN_COUNT",
        "SECOND_ALL_TORN_COUNT",
        "FAIL: qdshell restoration did not reach compositor-bound state",
    )
    missing = [token for token in credential_tokens if token not in rdp]
    if missing:
        return fail(f"RDP scenario lacks bounded credential poll: {missing}")
    if "Step 5 — disconnect cleanup" in rdp:
        return fail("RDP scenario still overclaims subscriber-disconnect coverage")
    restore_call = rdp.rfind("qdwin_apps_restore_shell ||")
    trap_clear = rdp.rfind("trap - EXIT")
    if restore_call < 0 or trap_clear < 0 or restore_call > trap_clear:
        return fail("RDP cleanup disarms EXIT trap before checked shell restore")

    print("PASS: GUI input uses real chords; shell takeover/restoration and "
          "RDP credential collection use bounded observable transitions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
