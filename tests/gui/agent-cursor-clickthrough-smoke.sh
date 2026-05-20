#!/usr/bin/env bash
set -euo pipefail

# Agent/CI smoke: cursor sprite visible above shell UI does NOT capture
# clicks. plan3 H3 reproducer.
#
# Symptom this test guards: a cursor sprite that retains a non-empty
# input region becomes a pickable shell surface above qdshell/layer-
# shell UI, and pointer clicks land on the cursor view instead of the
# UI underneath. libweston's pointer_cursor_surface_committed clears
# both pending and current input regions on every commit
# (libweston/input.c:3521-3522). qdwin's install path clears them once;
# plan3 M2 adds a per-sprite commit listener that re-asserts the
# invariant on every commit. This smoke proves the invariant by-effect:
# with the cursor sprite mapped on cursor_layer, a click still lands on
# a known toplevel underneath.
#
# Failing-before / passing-after property
# --------------------------------------
# Set QDWIN_CURSOR_CLICKTHROUGH_FORCE_BREAK=1 to require that the
# regression IS reproducible — useful when validating the test itself
# against an old qdwin without the M2 listener. The script then
# expects no focus log line and exits 1 instead of 0.

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
    local tmp=/tmp/qdwin-clickthrough-screen-$$.ppm
    $QDWIN_VIRSH screenshot "$VMNAME" "$tmp" >/dev/null
    local dims
    dims=$(file -b "$tmp" | sed -n 's/.* \([0-9][0-9]*\) x \([0-9][0-9]*\).*/\1 \2/p')
    rm -f "$tmp"
    [ -n "$dims" ] || fail "could not read screenshot dimensions"
    echo "$dims"
}

wait_for_handle() {
    local cursor=$1
    for _ in $(seq 1 30); do
        local h
        h=$(journal_after "$cursor" \
            | sed -n 's/.*qdwin: toplevel_added handle=\([0-9][0-9]*\) uid=1000 app_id=qdistro-test-window.*/\1/p' \
            | head -n1)
        if [ -n "$h" ]; then
            echo "$h"
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

# Confirm services are active so we don't blame qdwin for a missing
# session.
vm_exec "runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 systemctl --user is-active qdwin-compositor.service qdistro-cursor-sprites.service" \
    | grep -qx active || fail "qdwin/cursor-sprite services are not active"
pass "qdwin and cursor-sprite services active"

# Confirm cursor sprite helper registered the default shape — preflight
# for the click-through assertion.
vm_exec "journalctl _UID=1000 -b --no-pager | grep -q 'cursor-sprite registered shape=pointer\\|cursor-sprite registered shape=default'" \
    || fail "cursor sprite helper did not register pointer/default shape"
pass "cursor sprite helper registered pointer/default shape"

run_id=$$
title="agent-clickthrough-$run_id"
cleanup() {
    vm_exec "pkill -KILL -u admin -f '[q]distro-test-window --title agent-clickthrough-' 2>/dev/null || true" >/dev/null || true
}
trap cleanup EXIT
cleanup

cursor_start=$(journal_cursor)
vm_exec "runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 qdistro-test-window --title '$title' --width 400 --height 240 --color 0xff207040 >/tmp/$title.log 2>&1 &"

handle=$(wait_for_handle "$cursor_start") \
    || fail "qdistro-test-window did not appear in qdwin journal"
pass "test window handle=$handle"

# Center pixel inside the visible content. The window is centered by
# qdwin's default placement; clicking screen center is sufficient.
target_x=$(( QDWIN_SCREEN_W / 2 ))
target_y=$(( QDWIN_SCREEN_H / 2 ))

# Move pointer over the target to force qdwin to install the active
# cursor sprite on the pointer (set_shape via wp_cursor_shape_v1 or the
# default-cursor fallback). The "mapped on cursor_layer" log line is the
# discriminator that the sprite is live.
cursor_move=$(journal_cursor)
qdwin_mouse_move "$target_x" "$target_y"
sleep 0.5

# Verify the cursor sprite has been mapped on cursor_layer since this
# session started. The exact map message can come from
# install_default_cursor or wp_cursor_shape_v1.set_shape; accept either.
if vm_exec "journalctl _UID=1000 -b --no-pager | grep -q 'mapped on cursor_layer'"; then
    pass "cursor sprite mapped on cursor_layer (Weston invariant)"
else
    fail "no cursor sprite mapped on cursor_layer — preconditions not met"
fi

# Click. If the cursor sprite were pickable, this click would never
# reach the toplevel and focus would not move to the test window.
cursor_click=$(journal_cursor)
qdwin_click "$target_x" "$target_y"

# plan3 M2 sanity: the per-sprite commit listener should never log
# "re-cleared input" in normal operation (the helper does not commit
# non-empty input regions). Its presence indicates either a regression
# in the helper or a defensive save. Record but do not fail.
if journal_after "$cursor_click" | grep -q 'cursor-sprite commit re-cleared input'; then
    echo "WARN: M2 listener observed a non-empty cursor input region "\
         "at commit — investigate qdistro-cursor-sprites helper"
fi

if [ "${QDWIN_CURSOR_CLICKTHROUGH_FORCE_BREAK:-0}" = 1 ]; then
    if wait_for_focus "$handle" "$cursor_click"; then
        fail "expected NO focus (FORCE_BREAK=1) but click landed on handle=$handle"
    fi
    pass "FORCE_BREAK: click did not land on toplevel (cursor capture reproduced)"
    qdwin_screenshot "/tmp/qdwin-cursor-clickthrough-FORCE_BREAK-$run_id.png" >/dev/null
    exit 0
fi

wait_for_focus "$handle" "$cursor_click" \
    || fail "click at ($target_x,$target_y) did not focus handle=$handle — cursor sprite may be capturing clicks"
pass "click landed on handle=$handle through visible cursor sprite"

qdwin_screenshot "/tmp/qdwin-cursor-clickthrough-$run_id.png" >/dev/null
pass "artifact /tmp/qdwin-cursor-clickthrough-$run_id.png"
