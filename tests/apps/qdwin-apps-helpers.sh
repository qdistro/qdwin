#!/bin/bash
# qdwin-apps-helpers.sh — host-side helpers for the qdwin-apps scenarios.
#
# Sourced, not executed. See AGENTS.md.
#
# Differs from phase1/gui-tests/qdwin/qdwin-helpers.sh in that:
# - No qdshell (no /run/user/1000/qdshell.sock);
# - The shell role is held by `qdwin-bystander` (a 200-line C client
#   built from test-client/qdwin-bystander.c) which exposes
#   max/restore/min/close/focus on a FIFO at
#   /run/user/1000/qdwin-cmd.fifo;
# - The active wayland socket name is auto-detected because weston
#   restarts cycle through wayland-1 / wayland-2.

# qdwin_find_workspace() — shared with tests/gui/qdwin-helpers.sh; see
# tests/lib/workspace.sh for the rationale (worktree-aware upward search).
# shellcheck source=../lib/workspace.sh
source "$(dirname "${BASH_SOURCE[0]}")/../lib/workspace.sh"

: "${VMNAME:=}"
: "${QDWIN_VIRSH:=virsh -c qemu:///session}"
if [ -z "${QDWIN_WORKSPACE:-}" ]; then
    QDWIN_WORKSPACE=$(qdwin_find_workspace "${QDWIN_REPO:-$(dirname "${BASH_SOURCE[0]}")/../..}") \
        || QDWIN_WORKSPACE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd -P)
fi
export QDWIN_WORKSPACE
: "${QDWIN_VM_EXEC:=$QDWIN_WORKSPACE/qdistro/scripts/vm/vm-exec}"
export QDWIN_VM_EXEC
: "${QDWIN_HTTP_DIR:=${QDWIN_REPO}/extra}"
: "${QDWIN_HTTP_URL:=http://10.0.2.2:8765/extra}"
: "${QDWIN_BYSTANDER_FIFO:=/run/user/1000/qdwin-cmd.fifo}"
: "${QDWIN_BYSTANDER_LOG:=/tmp/bystander.log}"

qdwin_apps_set_vm() {
    VMNAME="$1"
}

qdwin_apps_require_vm() {
    if [ -z "${VMNAME:-}" ]; then
        VMNAME=$($QDWIN_VIRSH list --name --state-running | head -1)
    fi
    if [ -z "${VMNAME:-}" ] || ! $QDWIN_VIRSH dominfo "$VMNAME" >/dev/null 2>&1; then
        echo "qdwin-apps-helpers: no running VM (set VMNAME or qdwin_apps_set_vm)" >&2
        return 1
    fi
}

# Ask the VM which wayland-N socket weston is currently serving. Reads
# /proc/<weston-pid>/fd/* lock targets — robust against the
# wayland-1 / wayland-2 rotation that happens after weston restarts.
qdwin_apps_active_socket() {
    qdwin_apps_require_vm || return 1
    local b64
    b64=$(base64 -w0 <<'EOSCRIPT'
WPID=$(pgrep -u admin weston | head -1)
[ -z "$WPID" ] && exit 1
ls -l /proc/$WPID/fd 2>/dev/null \
    | grep -oE 'wayland-[0-9]+\.lock' \
    | head -1 \
    | sed 's/\.lock$//'
EOSCRIPT
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash" 2>/dev/null
}

_qdwin_apps_check_session() {
    local sock; sock=$(qdwin_apps_active_socket)
    [ -n "$sock" ] || { echo "qdwin-apps-helpers: weston not running" >&2; return 1; }
    local b64; b64=$(base64 -w0 <<EOSCRIPT
[ -p "$QDWIN_BYSTANDER_FIFO" ] || { echo "no fifo $QDWIN_BYSTANDER_FIFO" >&2; exit 1; }
pgrep -au admin -f qdwin-bystander >/dev/null || { echo "no bystander running" >&2; exit 1; }
echo "ok sock=$sock"
EOSCRIPT
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
}

# Assert the app-test session is healthy: weston up, bystander holding the
# shell role, command FIFO present at the canonical path. Self-healing — if
# the bystander/FIFO aren't ready (e.g. fresh boot still running qdshell, or a
# bystander that defaulted its FIFO elsewhere) it runs qdwin_apps_become_shell
# once and re-checks, so scenarios get a deterministic shell without each
# Setup block reimplementing the takeover.
qdwin_apps_session_up() {
    qdwin_apps_require_vm || return 1
    if _qdwin_apps_check_session; then
        return 0
    fi
    echo "qdwin-apps-helpers: session not ready; taking over shell role" >&2
    qdwin_apps_become_shell || return 1
    _qdwin_apps_check_session
}

# Deterministically take over the qdwin shell role as the bystander.
#
# The qdwin app scenarios need exactly ONE shell-role client. If qdshell is
# running (or systemd's Restart= relaunches it mid-test) it competes with the
# bystander for the qdwin_shell_v1 role: the loser logs
# "qdwin_shell_v1: shell role already claimed", crashes 255, and respawns
# until `start-limit-hit` — spamming the journal and racing the bystander's
# bind. Agents previously improvised this transition (kill qdshell, start
# bystander) inconsistently and often without QDWIN_BYSTANDER_FIFO set, so the
# FIFO landed in /tmp instead of the polled /run/user/1000 path.
#
# This centralises it: cleanly STOP qdshell (a manual `systemctl stop`
# suppresses Restart= — systemd never relaunches an explicitly-stopped unit —
# so the role stays free WITHOUT masking, leaving no persistent state to leak
# if a scenario aborts before restore), evict any stray non-systemd `qs`, then
# (re)launch the bystander with the canonical FIFO path + wayland env explicit
# and wait for the FIFO. Call in Setup before qdwin_apps_session_up; pair with
# qdwin_apps_restore_shell in Teardown to bring the desktop shell back.
qdwin_apps_become_shell() {
    qdwin_apps_require_vm || return 1
    local sock; sock=$(qdwin_apps_active_socket)
    [ -n "$sock" ] || { echo "qdwin-apps-helpers: weston not running" >&2; return 1; }
    local b64; b64=$(base64 -w0 <<EOSCRIPT
# reset-failed first in case qdshell parked in failed(start-limit-hit); the
# clean stop then keeps it down (Restart= does not fire on an explicit stop).
runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
    systemctl --user reset-failed qdshell.service 2>/dev/null || true
runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
    systemctl --user stop qdshell.service 2>/dev/null || true
pkill -u admin -x qs 2>/dev/null || true
pkill -u admin -x qdwin-bystander 2>/dev/null || true
sleep 0.5
rm -f "$QDWIN_BYSTANDER_FIFO"
runuser -u admin -- bash -c '
    export XDG_RUNTIME_DIR=/run/user/1000
    export WAYLAND_DISPLAY=$sock
    export QDWIN_BYSTANDER_FIFO="$QDWIN_BYSTANDER_FIFO"
    setsid qdwin-bystander >"$QDWIN_BYSTANDER_LOG" 2>&1 &
'
# The bystander creates the FIFO before its wayland connect, so a short poll
# is enough; fail loudly if it never appears.
for _i in \$(seq 1 40); do
    [ -p "$QDWIN_BYSTANDER_FIFO" ] && break
    sleep 0.1
done
[ -p "$QDWIN_BYSTANDER_FIFO" ] || {
    echo "bystander FIFO never appeared at $QDWIN_BYSTANDER_FIFO" >&2
    tail -5 "$QDWIN_BYSTANDER_LOG" 2>/dev/null >&2
    exit 1
}
echo "become-shell ok sock=$sock fifo=$QDWIN_BYSTANDER_FIFO"
EOSCRIPT
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
}

# Undo qdwin_apps_become_shell: stop the bystander and restart qdshell so the
# normal desktop session reclaims the shell role after the app matrix finishes.
# Best-effort. (No unmask needed — become_shell only stops, never masks.)
qdwin_apps_restore_shell() {
    qdwin_apps_require_vm || return 1
    local b64; b64=$(base64 -w0 <<'EOSCRIPT'
pkill -u admin -x qdwin-bystander 2>/dev/null || true
runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
    systemctl --user start qdshell.service 2>/dev/null || true
true
EOSCRIPT
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash" >/dev/null 2>&1 || true
}

# Launch <name> as admin against the active wayland socket. Logs go to
# /tmp/<name>.log inside the VM. Background; returns once the
# vm-exec call completes (the app keeps running via setsid).
qdwin_apps_launch() {
    qdwin_apps_require_vm || return 1
    local name="$1"; shift
    local cmd="$*"
    local sock; sock=$(qdwin_apps_active_socket)
    [ -n "$sock" ] || return 1
    local b64 cmd_b64
    cmd_b64=$(printf '%s' "$cmd" | base64 -w0)
    b64=$(base64 -w0 <<EOLAUNCH
set -eu
cmd=\$(printf '%s' '$cmd_b64' | base64 -d)
runuser -u admin -- env -i \
    HOME=/home/admin \
    USER=admin \
    LOGNAME=admin \
    SHELL=/bin/bash \
    PATH=/usr/local/bin:/usr/bin:/bin \
    XDG_RUNTIME_DIR=/run/user/1000 \
    WAYLAND_DISPLAY=$sock \
    DISPLAY=:0 \
    MOZ_ENABLE_WAYLAND=1 \
    QT_QPA_PLATFORM=wayland \
    GDK_BACKEND=wayland \
    bash -s -- "\$cmd" "$name" <<'EOADMIN'
set -eu
cmd=$1
name=$2
# Push only display/runtime vars into the D-Bus activation environment so
# GApplication single-instance apps (e.g. gnome-text-editor) inherit a display.
# Do not push toolkit backend variables: per-app XWayland overrides such as
# `env GDK_BACKEND=x11 ...` must remain effective.
dbus-update-activation-environment --systemd \
    WAYLAND_DISPLAY DISPLAY XDG_RUNTIME_DIR 2>/dev/null || true
setsid sh -c "$cmd" </dev/null >/tmp/${name}.log 2>&1 &
EOADMIN
EOLAUNCH
)
    "$QDWIN_VM_EXEC" "$VMNAME" "env -i PATH=/usr/local/bin:/usr/bin:/bin bash -c 'printf %s $b64 | base64 -d | bash'"
}

qdwin_apps_ctl() {
    qdwin_apps_require_vm || return 1
    local cmd="$*"
    local b64
    b64=$(base64 -w0 <<EOCTL
echo '$cmd' > $QDWIN_BYSTANDER_FIFO
EOCTL
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
}

qdwin_apps_screenshot() {
    qdwin_apps_require_vm || return 1
    local out="$1"
    $QDWIN_VIRSH screenshot "$VMNAME" "$out" 2>&1 | tail -1
}

qdwin_apps_send_key() {
    qdwin_apps_require_vm || return 1
    $QDWIN_VIRSH send-key "$VMNAME" --codeset linux "$@"
}

# Type a lowercase ASCII string one character at a time. Avoids the
# "all keys pressed simultaneously → one chord" issue.
qdwin_apps_type() {
    local s="$1"
    local i ch up
    for (( i=0; i<${#s}; i++ )); do
        ch="${s:i:1}"
        case "$ch" in
            [a-z]) up=$(echo "$ch" | tr a-z A-Z); qdwin_apps_send_key "KEY_$up" ;;
            [0-9]) qdwin_apps_send_key "KEY_$ch" ;;
            ' ')   qdwin_apps_send_key "KEY_SPACE" ;;
            *) echo "qdwin_apps_type: unsupported char '$ch'" >&2 ;;
        esac
        sleep 0.04
    done
}

qdwin_apps_log_grep() {
    qdwin_apps_require_vm || return 1
    local pattern="$1"
    local b64
    b64=$(base64 -w0 <<EOGREP
grep -E '$pattern' /home/admin/.local/share/qdwin.log /tmp/bystander.log 2>/dev/null
EOGREP
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
}

qdwin_apps_kill_all() {
    qdwin_apps_require_vm || return 1
    local b64; b64=$(base64 -w0 <<'EOKILL'
for app in firefox thunderbird vlc kate krita gimp obsidian Obsidian \
           xterm xeyes thunar libreoffice soffice inkscape mpv \
           foot gnome-text-editor gedit gnome-system-monitor \
           gnome-calculator chromium audacity gpick feh qbittorrent \
           qpdfview eog ristretto evince python3 java SwingDemo \
           fltk-demo Xwayland; do
    pkill -u admin -9 -f "$app" 2>/dev/null
done
sleep 1
true
EOKILL
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash" >/dev/null 2>&1 || true
}
