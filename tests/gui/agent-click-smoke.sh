#!/usr/bin/env bash
set -euo pipefail

# Agent/CI smoke test for the qdwin real mouse path.
#
# This intentionally does not depend on qdshell's historical ctrl socket.
# Current qdshell is Quickshell IPC based, while qdwin's click-to-focus path
# is below the shell and is observable through qdwin's journal focus logs.

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
QDWIN_REPO=${QDWIN_REPO:-$ROOT}
WORKSPACE=$(cd "$ROOT/.." && pwd)
VMNAME=${VMNAME:-$(virsh -c qemu:///session list --name --state-running | head -n1)}

export QDWIN_REPO
export QDWIN_VM_EXEC=${QDWIN_VM_EXEC:-$WORKSPACE/qdistro/scripts/vm/vm-exec}
export QDWIN_VIRSH=${QDWIN_VIRSH:-virsh -c qemu:///session}

# shellcheck source=qdwin-helpers.sh
source "$QDWIN_REPO/tests/gui/qdwin-helpers.sh"
qdwin_set_vm "$VMNAME"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "PASS: $*"
}

vm_exec() {
    "$QDWIN_VM_EXEC" "$VMNAME" "$@"
}

journal_cursor() {
    vm_exec "journalctl _UID=1000 -n 1 --show-cursor --no-pager 2>/dev/null | tail -1 | sed 's/^-- cursor: //'"
}

journal_after() {
    local cursor=$1
    vm_exec "journalctl _UID=1000 --after-cursor='$cursor' --no-pager 2>/dev/null"
}

detect_screen_size() {
    local tmp=/tmp/qdwin-agent-screen-$$.ppm
    $QDWIN_VIRSH screenshot "$VMNAME" "$tmp" >/dev/null
    local dims
    dims=$(file -b "$tmp" | sed -n 's/.* \([0-9][0-9]*\) x \([0-9][0-9]*\).*/\1 \2/p')
    if [ -z "$dims" ]; then
        dims=$(awk '
            /^#/ { next }
            NR == 1 { next }
            NF >= 2 { print $1, $2; exit }
        ' "$tmp")
    fi
    rm -f "$tmp"
    [ -n "$dims" ] || fail "could not read screenshot dimensions"
    echo "$dims"
}

wait_for_new_handles() {
    local cursor=$1 handles=
    for _ in $(seq 1 30); do
        handles=$(journal_after "$cursor" \
            | sed -n 's/.*qdwin: toplevel_added handle=\([0-9][0-9]*\) uid=1000 app_id=qdistro-test-window.*/\1/p' \
            | head -n2 \
            | tr '\n' ' ')
        if [ "$(wc -w <<<"$handles")" -ge 2 ]; then
            echo "$handles"
            return 0
        fi
        sleep 0.2
    done
    return 1
}

wait_for_focus() {
    local handle=$1 cursor=$2
    for _ in $(seq 1 20); do
        if journal_after "$cursor" | grep -q "qdwin: focus handle=$handle "; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

[ -n "$VMNAME" ] || fail "no running VM; set VMNAME or start qdistro-daily"
$QDWIN_VIRSH domstate "$VMNAME" >/dev/null || fail "VM '$VMNAME' not found"
vm_exec "test -S /run/user/1000/wayland-1" >/dev/null \
    || fail "admin qdwin Wayland socket is not up"
vm_exec "command -v qdistro-test-window" >/dev/null \
    || fail "qdistro-test-window is not installed in the VM"

read -r QDWIN_SCREEN_W QDWIN_SCREEN_H < <(detect_screen_size)
export QDWIN_SCREEN_W QDWIN_SCREEN_H
pass "screen ${QDWIN_SCREEN_W}x${QDWIN_SCREEN_H}"

run_id=$$
title_one="agent-click-one-$run_id"
title_two="agent-click-two-$run_id"

cleanup() {
    vm_exec "pkill -KILL -u admin -f '[q]distro-test-window --title agent-click-' 2>/dev/null || true" >/dev/null || true
}
trap cleanup EXIT
cleanup

cursor_start=$(journal_cursor)
vm_exec "runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 qdistro-test-window --title '$title_one' --width 300 --height 180 --color 0xff203060 >/tmp/$title_one.log 2>&1 &"
sleep 0.8
vm_exec "runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 qdistro-test-window --title '$title_two' --width 300 --height 180 --color 0xff604020 >/tmp/$title_two.log 2>&1 &"

read -r handle_one handle_two < <(wait_for_new_handles "$cursor_start") \
    || fail "test windows did not appear in qdwin journal"
pass "windows handle_one=$handle_one handle_two=$handle_two"

content_w=300
content_h=180
one_x=$(( (QDWIN_SCREEN_W - content_w) / 2 ))
one_y=$(( (QDWIN_SCREEN_H - content_h) / 2 ))
two_x=$(( one_x + 40 ))
two_y=$(( one_y + 40 ))

# Click the exposed top-left content of the first window, which is behind the
# second cascaded window. This should raise and focus handle_one.
cursor_click_one=$(journal_cursor)
qdwin_click "$((one_x + 20))" "$((one_y + 20))"
wait_for_focus "$handle_one" "$cursor_click_one" \
    || fail "click on first/background window did not focus handle=$handle_one"
pass "background-window click focused handle=$handle_one"

# Click the second window again to prove focus can move back through the same
# QMP mouse path.
cursor_click_two=$(journal_cursor)
qdwin_click "$((two_x + content_w / 2))" "$((two_y + content_h / 2))"
wait_for_focus "$handle_two" "$cursor_click_two" \
    || fail "click on second window did not focus handle=$handle_two"
pass "foreground-window click focused handle=$handle_two"

# The user-facing launcher icon is qdshell-owned UI, not the compositor's
# Ctrl+Space launcher keybinding. Keep the probe visible for agents, but do
# not fail the core click-to-focus CI smoke unless explicitly requested.
cursor_launcher=$(journal_cursor)
qdwin_click 16 16
sleep 0.5
if journal_after "$cursor_launcher" | grep -q "qdwin: launcher_requested"; then
    pass "launcher click emitted qdwin: launcher_requested"
elif [ "${QDWIN_REQUIRE_LAUNCHER_CLICK:-0}" = 1 ]; then
    fail "top-left launcher click did not emit qdwin: launcher_requested"
else
    echo "GAP: top-left launcher click did not emit qdwin: launcher_requested"
fi

qdwin_screenshot "/tmp/qdwin-agent-click-smoke-$run_id.png" >/dev/null
pass "artifact /tmp/qdwin-agent-click-smoke-$run_id.png"
