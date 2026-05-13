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

: "${VMNAME:=}"
: "${QDWIN_VIRSH:=virsh -c qemu:///session}"
: "${QDWIN_VM_EXEC:=$(dirname "${BASH_SOURCE[0]}")/../../../scripts/vm/vm-exec}"
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

qdwin_apps_session_up() {
    qdwin_apps_require_vm || return 1
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

# Launch <name> as admin against the active wayland socket. Logs go to
# /tmp/<name>.log inside the VM. Background; returns once the
# vm-exec call completes (the app keeps running via setsid).
qdwin_apps_launch() {
    qdwin_apps_require_vm || return 1
    local name="$1"; shift
    local cmd="$*"
    local sock; sock=$(qdwin_apps_active_socket)
    [ -n "$sock" ] || return 1
    local b64
    b64=$(base64 -w0 <<EOLAUNCH
runuser -u admin -- bash -c '
    export XDG_RUNTIME_DIR=/run/user/1000
    export WAYLAND_DISPLAY=$sock
    export DISPLAY=:0
    export MOZ_ENABLE_WAYLAND=1
    export QT_QPA_PLATFORM=wayland
    export GDK_BACKEND=wayland
    setsid sh -c "$cmd" </dev/null >/tmp/${name}.log 2>&1 &
'
EOLAUNCH
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
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
