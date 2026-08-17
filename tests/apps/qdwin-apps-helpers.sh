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
# catches filesystem readiness. Then wait separately for compositor-visible
# shell ownership; FIFO creation alone happens before the Wayland hello.
for _i in \$(seq 1 40); do
    [ -p "$QDWIN_BYSTANDER_FIFO" ] && break
    sleep 0.1
done
[ -p "$QDWIN_BYSTANDER_FIFO" ] || {
    echo "bystander FIFO never appeared at $QDWIN_BYSTANDER_FIFO" >&2
    tail -5 "$QDWIN_BYSTANDER_LOG" 2>/dev/null >&2
    exit 1
}
for _i in \$(seq 1 40); do
    grep -q 'hello uid=' "$QDWIN_BYSTANDER_LOG" 2>/dev/null && break
    sleep 0.1
done
if grep -q 'shell role already claimed' "$QDWIN_BYSTANDER_LOG" 2>/dev/null \
   || ! grep -q 'hello uid=' "$QDWIN_BYSTANDER_LOG" 2>/dev/null; then
    echo "bystander did not acquire singleton shell role" >&2
    tail -10 "$QDWIN_BYSTANDER_LOG" 2>/dev/null >&2
    exit 1
fi
echo "become-shell ok sock=$sock fifo=$QDWIN_BYSTANDER_FIFO"
EOSCRIPT
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
}

# Prepare for a scenario-specific shell-role probe. First use the normal,
# validated takeover path to stop qdshell and prove qdwin is reachable; then
# remove that suite bystander and its FIFO. The caller must immediately launch
# exactly one replacement and verify its hello. This avoids racing two clients
# for qdwin's singleton shell role while still permitting probe-only flags.
qdwin_apps_prepare_shell_probe() {
    if ! qdwin_apps_become_shell; then
        qdwin_apps_restore_shell
        return 1
    fi
    local cursor cursor_b64 b64
    cursor=$(qdwin_apps_journal_cursor) || {
        qdwin_apps_restore_shell
        return 1
    }
    [ -n "$cursor" ] || {
        echo "cannot observe shell ownership handoff: empty journal cursor" >&2
        qdwin_apps_restore_shell
        return 1
    }
    cursor_b64=$(printf '%s' "$cursor" | base64 -w0)
    b64=$(base64 -w0 <<EOSCRIPT
cursor=\$(printf '%s' '$cursor_b64' | base64 -d)
pkill -u admin -x qdwin-bystander 2>/dev/null || true
for _i in \$(seq 1 40); do
    pgrep -u admin -x qdwin-bystander >/dev/null 2>&1 || break
    sleep 0.1
done
if pgrep -u admin -x qdwin-bystander >/dev/null 2>&1; then
    echo "suite bystander still owns the singleton shell role" >&2
    exit 1
fi
if runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
       systemctl --user is-active --quiet qdshell.service; then
    echo "qdshell still owns/contends for the singleton shell role" >&2
    exit 1
fi
# Process exit is insufficient: wait until qdwin's resource-destroy callback
# has cleared shell_bound/shell_resource and logged the ownership boundary.
handoff_seen=0
for _i in \$(seq 1 40); do
    if runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
         journalctl --user -b -u qdwin-compositor.service \
         --after-cursor "\$cursor" --no-pager -o cat 2>/dev/null \
         | grep -qFx 'qdwin: shell unbound'; then
        handoff_seen=1
        break
    fi
    sleep 0.1
done
[ "\$handoff_seen" = 1 ] || {
    echo "compositor did not report shell unbound after suite bystander exit" >&2
    exit 1
}
rm -f "$QDWIN_BYSTANDER_FIFO"
echo "shell-probe slot ready after compositor handoff fifo=$QDWIN_BYSTANDER_FIFO"
EOSCRIPT
)
    if ! "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"; then
        qdwin_apps_restore_shell
        return 1
    fi
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
cmd=\$1
name=\$2
# Push only display/runtime vars into the D-Bus activation environment so
# GApplication single-instance apps (e.g. gnome-text-editor) inherit a display.
# Do not push toolkit backend variables: per-app XWayland overrides (e.g.
# env GDK_BACKEND=x11) must remain effective.
dbus-update-activation-environment --systemd \
    WAYLAND_DISPLAY DISPLAY XDG_RUNTIME_DIR 2>/dev/null || true
setsid sh -c "\$cmd" </dev/null >/tmp/\${name}.log 2>&1 &
EOADMIN
EOLAUNCH
)
    "$QDWIN_VM_EXEC" "$VMNAME" "env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/bin bash -c 'printf %s $b64 | base64 -d | bash'"
}

qdwin_apps_ctl() {
    qdwin_apps_require_vm || return 1
    local cmd="$*"
    case "$cmd" in
        maxlast) cmd=max ;;
        restorelast) cmd=restore ;;
    esac
    local b64 cmd_b64
    cmd_b64=$(printf '%s' "$cmd" | base64 -w0)
    b64=$(base64 -w0 <<EOCTL
set -eu
[ -p "$QDWIN_BYSTANDER_FIFO" ] || {
    echo "missing bystander FIFO: $QDWIN_BYSTANDER_FIFO" >&2; exit 1;
}
pgrep -u admin -x qdwin-bystander >/dev/null || {
    echo "no bystander reader for FIFO: $QDWIN_BYSTANDER_FIFO" >&2; exit 1;
}
cmd=\$(printf '%s' '$cmd_b64' | base64 -d)
QDWIN_FIFO_CMD="\$cmd" QDWIN_FIFO_PATH="$QDWIN_BYSTANDER_FIFO" \
    timeout 3 bash -c 'printf "%s\n" "\$QDWIN_FIFO_CMD" > "\$QDWIN_FIFO_PATH"'
EOCTL
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
}

qdwin_apps_screenshot() {
    qdwin_apps_require_vm || return 1
    local out="$1"
    $QDWIN_VIRSH screenshot "$VMNAME" "$out" 2>&1 | tail -1
}

# Return the current bystander log line count. Capture this immediately before
# launching an app, then pass it to qdwin_apps_assert_max_restore_last so a
# stale historical toplevel can never satisfy a new app assertion.
qdwin_apps_bystander_log_boundary() {
    qdwin_apps_require_vm || return 1
    "$QDWIN_VM_EXEC" "$VMNAME" "wc -l < '$QDWIN_BYSTANDER_LOG'" 2>/dev/null
}

# Assert a shell-driven maximise/restore round-trip for the expected toplevel
# added after a caller-captured boundary. Arguments are artifact prefix,
# pre-launch log line count, exact app_id, and exact title. This writes
# <prefix>-{max,restore}.png and <prefix>-roundtrip.log.
# A FIFO write alone is not success: state and geometry must appear after a
# fresh log boundary, and restore must reproduce the committed baseline.
qdwin_apps_assert_max_restore_last() {
    qdwin_apps_require_vm || return 1
    local prefix="$1"
    local launch_boundary="$2"
    local expected_app_id="$3"
    local expected_title="$4"
    local baseline handle bx by bw bh max_start restore_start
    local max_evidence restore_evidence max_geometry restore_geometry
    local mx my mw mh rx ry rw rh

    [ -n "$prefix" ] || {
        echo "FAIL: max/restore assertion requires a host artifact prefix" >&2
        return 1
    }
    [[ "$launch_boundary" =~ ^[0-9]+$ ]] || {
        echo "FAIL: expected numeric pre-launch log boundary; observed '$launch_boundary' (artifact_prefix=$prefix)" >&2
        return 1
    }
    [ -n "$expected_app_id" ] && [ -n "$expected_title" ] || {
        echo "FAIL: expected exact app_id and title for round-trip assertion (artifact_prefix=$prefix)" >&2
        return 1
    }
    mkdir -p "$(dirname "$prefix")" || return 1

    local app_b64 title_b64
    app_b64=$(printf '%s' "$expected_app_id" | base64 -w0)
    title_b64=$(printf '%s' "$expected_title" | base64 -w0)
    baseline=$("$QDWIN_VM_EXEC" "$VMNAME" "
log='$QDWIN_BYSTANDER_LOG'
start=$launch_boundary
expected_app=\$(printf '%s' '$app_b64' | base64 -d)
expected_title=\$(printf '%s' '$title_b64' | base64 -d)
delta=\$(tail -n +\$((start + 1)) \"\$log\")
added=\$(printf '%s\\n' \"\$delta\" | grep -F 'qdwin-bystander: toplevel_added handle=' \
    | grep -F \" app_id=\\\"\$expected_app\\\" title=\\\"\$expected_title\\\" xwayland=1\" | tail -1)
handle=\$(printf '%s\\n' \"\$added\" | sed -n 's/.*toplevel_added handle=\\([0-9][0-9]*\\).*/\\1/p')
[ -n \"\$handle\" ] || {
    echo \"expected post-boundary app_id=\\\"\$expected_app\\\" title=\\\"\$expected_title\\\" xwayland=1; observed:\" >&2
    printf '%s\\n' \"\$delta\" >&2
    exit 1
}
geometry=\$(printf '%s\\n' \"\$delta\" | sed -n \"s/.*toplevel_geometry handle=\$handle x=\\(-*[0-9][0-9]*\\) y=\\(-*[0-9][0-9]*\\) w=\\([0-9][0-9]*\\) h=\\([0-9][0-9]*\\).*/\$handle \\1 \\2 \\3 \\4/p\" | tail -1)
[ -n \"\$geometry\" ] || {
    echo \"expected post-boundary geometry for handle=\$handle; observed:\" >&2
    printf '%s\\n' \"\$delta\" >&2
    exit 1
}
printf '%s\\n' \"\$geometry\"
" 2>/dev/null) || true
    read -r handle bx by bw bh <<<"$baseline"
    if ! [[ "$handle $bx $by $bw $bh" =~ ^[0-9]+\ -?[0-9]+\ -?[0-9]+\ [1-9][0-9]*\ [1-9][0-9]*$ ]]; then
        echo "FAIL: expected post-boundary $expected_app_id/$expected_title xwayland=1 with positive geometry; observed '${baseline:-<none>}' (log=$QDWIN_BYSTANDER_LOG artifact_prefix=$prefix)" >&2
        return 1
    fi

    max_start=$("$QDWIN_VM_EXEC" "$VMNAME" "wc -l < '$QDWIN_BYSTANDER_LOG'" 2>/dev/null) || return 1
    qdwin_apps_ctl maxlast || {
        echo "FAIL: max command failed for handle=$handle (artifact_prefix=$prefix)" >&2
        return 1
    }
    max_evidence=$("$QDWIN_VM_EXEC" "$VMNAME" "
log='$QDWIN_BYSTANDER_LOG'; start=$max_start; handle=$handle
for _i in \$(seq 1 40); do
    delta=\$(tail -n +\$((start + 1)) \"\$log\")
    if printf '%s\\n' \"\$delta\" | grep -qFx \"qdwin-bystander: toplevel_state handle=\$handle state=0x1\" \
       && printf '%s\\n' \"\$delta\" | grep -q \"toplevel_geometry handle=\$handle \"; then
        printf '%s\\n' \"\$delta\"; exit 0
    fi
    sleep 0.1
done
echo \"expected post-command max state=0x1 and geometry for handle=\$handle; observed:\" >&2
tail -n +\$((start + 1)) \"\$log\" >&2
exit 1
" 2>&1) || {
        printf 'FAIL: %s (artifact_prefix=%s)\n' "$max_evidence" "$prefix" >&2
        return 1
    }
    max_geometry=$(printf '%s\n' "$max_evidence" \
        | sed -n "s/.*toplevel_geometry handle=$handle x=\\(-*[0-9][0-9]*\\) y=\\(-*[0-9][0-9]*\\) w=\\([0-9][0-9]*\\) h=\\([0-9][0-9]*\\).*/\\1 \\2 \\3 \\4/p" \
        | tail -1)
    read -r mx my mw mh <<<"$max_geometry"
    if [ "$mx $my $mw $mh" != "0 0 1280 800" ]; then
        echo "FAIL: expected maximized geometry 0 0 1280 800 for handle=$handle; observed '${max_geometry:-<none>}' (artifact_prefix=$prefix)" >&2
        return 1
    fi
    qdwin_apps_screenshot "${prefix}-max.png" || return 1

    restore_start=$("$QDWIN_VM_EXEC" "$VMNAME" "wc -l < '$QDWIN_BYSTANDER_LOG'" 2>/dev/null) || return 1
    qdwin_apps_ctl restorelast || {
        echo "FAIL: restore command failed for handle=$handle (artifact_prefix=$prefix)" >&2
        return 1
    }
    restore_evidence=$("$QDWIN_VM_EXEC" "$VMNAME" "
log='$QDWIN_BYSTANDER_LOG'; start=$restore_start; handle=$handle
for _i in \$(seq 1 40); do
    delta=\$(tail -n +\$((start + 1)) \"\$log\")
    if printf '%s\\n' \"\$delta\" | grep -qFx \"qdwin-bystander: toplevel_state handle=\$handle state=0x0\" \
       && printf '%s\\n' \"\$delta\" | grep -q \"toplevel_geometry handle=\$handle \"; then
        printf '%s\\n' \"\$delta\"; exit 0
    fi
    sleep 0.1
done
echo \"expected post-command restore state=0x0 and geometry for handle=\$handle; observed:\" >&2
tail -n +\$((start + 1)) \"\$log\" >&2
exit 1
" 2>&1) || {
        printf 'FAIL: %s (artifact_prefix=%s)\n' "$restore_evidence" "$prefix" >&2
        return 1
    }
    restore_geometry=$(printf '%s\n' "$restore_evidence" \
        | sed -n "s/.*toplevel_geometry handle=$handle x=\\(-*[0-9][0-9]*\\) y=\\(-*[0-9][0-9]*\\) w=\\([0-9][0-9]*\\) h=\\([0-9][0-9]*\\).*/\\1 \\2 \\3 \\4/p" \
        | tail -1)
    read -r rx ry rw rh <<<"$restore_geometry"
    if [ "$rx $ry $rw $rh" != "$bx $by $bw $bh" ]; then
        echo "FAIL: expected restored geometry $bx $by $bw $bh for handle=$handle; observed '${restore_geometry:-<none>}' (artifact_prefix=$prefix)" >&2
        return 1
    fi
    qdwin_apps_screenshot "${prefix}-restore.png" || return 1

    {
        printf 'PASS: handle=%s app_id="%s" title="%s" xwayland=1 launch_boundary=%s baseline=%s %s %s %s\n' \
            "$handle" "$expected_app_id" "$expected_title" "$launch_boundary" \
            "$bx" "$by" "$bw" "$bh"
        printf '%s\n' "$max_evidence"
        printf '%s\n' "$restore_evidence"
    } | tee "${prefix}-roundtrip.log"
}

qdwin_apps_send_key() {
    qdwin_apps_require_vm || return 1
    local key qcode
    for key in "$@"; do
        case "$key" in
            KEY_LEFTCTRL) qcode=ctrl ;;
            KEY_RIGHTCTRL) qcode=ctrl_r ;;
            KEY_LEFTALT) qcode=alt ;;
            KEY_RIGHTALT) qcode=alt_r ;;
            KEY_LEFTSHIFT) qcode=shift ;;
            KEY_RIGHTSHIFT) qcode=shift_r ;;
            KEY_LEFTMETA) qcode=meta_l ;;
            KEY_RIGHTMETA) qcode=meta_r ;;
            KEY_TAB) qcode=tab ;;
            KEY_ESC) qcode=esc ;;
            KEY_ENTER|KEY_KPENTER) qcode=ret ;;
            KEY_SPACE) qcode=spc ;;
            KEY_BACKSPACE) qcode=backspace ;;
            KEY_DOT) qcode=dot ;;
            KEY_MINUS) qcode=minus ;;
            KEY_UP) qcode=up ;;
            KEY_DOWN) qcode=down ;;
            KEY_LEFT) qcode=left ;;
            KEY_RIGHT) qcode=right ;;
            KEY_[A-Z]) qcode=$(printf '%s' "${key#KEY_}" | tr A-Z a-z) ;;
            KEY_[0-9]) qcode=${key#KEY_} ;;
            *) echo "qdwin-apps: no qcode for $key" >&2; return 1 ;;
        esac
        qdwin_apps_qmp_key "$qcode" down || return 1
        sleep 0.03
        qdwin_apps_qmp_key "$qcode" up || return 1
        sleep 0.03
    done
}

# Emit one atomic key transition.  Unlike `virsh send-key`, this lets a chord
# hold its modifier while the non-modifier key is pressed and released.
qdwin_apps_qmp_key() {
    qdwin_apps_require_vm || return 1
    local qcode="$1" direction="$2" down=true
    [ "$direction" = up ] && down=false
    $QDWIN_VIRSH qemu-monitor-command "$VMNAME" \
        "{\"execute\": \"input-send-event\", \"arguments\": {\"events\": [{\"type\": \"key\", \"data\": {\"down\": $down, \"key\": {\"type\": \"qcode\", \"data\": \"$qcode\"}}}]}}" \
        >/dev/null
}

# Real keyboard sequence: hold modifiers, tap keys, release modifiers in
# reverse order. Usage: qdwin_apps_chord alt -- f
qdwin_apps_chord() {
    qdwin_apps_require_vm || return 1
    local -a holds=() taps=()
    local phase=hold key index
    for key in "$@"; do
        if [ "$key" = -- ]; then phase=tap; continue; fi
        if [ "$phase" = hold ]; then holds+=("$key"); else taps+=("$key"); fi
    done
    [ "${#holds[@]}" -gt 0 ] && [ "${#taps[@]}" -gt 0 ] || {
        echo "qdwin-apps: chord requires <holds> -- <taps>" >&2
        return 1
    }
    for key in "${holds[@]}"; do
        qdwin_apps_qmp_key "$key" down || return 1
        sleep 0.03
    done
    for key in "${taps[@]}"; do
        qdwin_apps_qmp_key "$key" down || return 1
        sleep 0.05
        qdwin_apps_qmp_key "$key" up || return 1
        sleep 0.05
    done
    for ((index=${#holds[@]} - 1; index >= 0; index--)); do
        qdwin_apps_qmp_key "${holds[index]}" up || return 1
        sleep 0.03
    done
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
    local b64 pattern_b64
    pattern_b64=$(printf '%s' "$pattern" | base64 -w0)
    b64=$(base64 -w0 <<EOGREP
pattern=\$(printf '%s' '$pattern_b64' | base64 -d)
{
    runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
        journalctl --user -b -u qdwin-compositor.service --no-pager -o cat 2>/dev/null
    cat "$QDWIN_BYSTANDER_LOG" 2>/dev/null || true
} | grep -E "\$pattern"
EOGREP
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
}

# Return a cursor at the current end of qdwin-compositor.service's user journal.
# Scenarios capture this before an action and query only the resulting delta, so
# a stale line from compositor startup cannot satisfy an action assertion.
qdwin_apps_journal_cursor() {
    qdwin_apps_require_vm || return 1
    local b64
    b64=$(base64 -w0 <<'EOCURSOR'
runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
    journalctl --user -b -u qdwin-compositor.service -n0 --show-cursor \
    2>/dev/null | sed -n 's/^-- cursor: //p'
EOCURSOR
)
    "$QDWIN_VM_EXEC" "$VMNAME" "echo $b64 | base64 -d | bash"
}

# qdwin_apps_log_since_cursor <cursor> <extended-regexp>
# Print matching qdwin compositor lines emitted strictly after <cursor>.
qdwin_apps_log_since_cursor() {
    qdwin_apps_require_vm || return 1
    local cursor=$1 pattern=$2 b64 cursor_b64 pattern_b64
    [ -n "$cursor" ] || { echo "qdwin-apps: journal cursor is empty" >&2; return 2; }
    cursor_b64=$(printf '%s' "$cursor" | base64 -w0)
    pattern_b64=$(printf '%s' "$pattern" | base64 -w0)
    b64=$(base64 -w0 <<EOJOURNAL
cursor=\$(printf '%s' '$cursor_b64' | base64 -d)
pattern=\$(printf '%s' '$pattern_b64' | base64 -d)
runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 \
    journalctl --user -b -u qdwin-compositor.service \
    --after-cursor "\$cursor" --no-pager -o cat 2>/dev/null \
    | grep -E "\$pattern"
EOJOURNAL
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
