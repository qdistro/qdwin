#!/bin/bash
# qdwin-helpers.sh — host-side helpers for driving a qdwin-on-tty3
# session from automated tests / LLM agents.
#
# Why this is separate from phase1/gui-tests/../vm-gui:
# - vm-gui assumes labwc + XWayland (Phase 1-5). It uses xdotool via
#   DISPLAY=:0, which doesn't exist under the qdwin compositor.
# - qdwin runs on bare DRM via libweston. Input goes through evdev
#   (kernel input layer) and key bindings fire in the compositor.
# - The launcher overlay does NOT redirect keyboard input — it can
#   only be driven via qdshell's ctrl-socket. (See launcher.py:17-23
#   for the upstream gap; a real wl_keyboard grab is a §6.8 follow-up.)
#
# The three primitives that DO work end-to-end:
#   1. virsh send-key — injects at QEMU's emulated keyboard (evdev
#      layer, below Wayland entirely). Compositor key bindings
#      (Ctrl+Space launcher, Alt+Tab switcher, Ctrl+Alt+L lock) AND
#      input directed at focused toplevels both flow through this.
#   2. qdshell ctrl-socket — `socat - UNIX-CONNECT:/run/user/1000/qdshell.sock`
#      drives launcher / switcher / locker / windows, and returns
#      machine-readable status snapshots.
#   3. virsh screenshot — captures the framebuffer. Note: cursor is
#      drawn on a KMS plane (not in the FB) so the pointer never
#      appears in the screenshot. Toplevels and chrome do.
#
# Usage:
#     source phase1/gui-tests/qdwin/qdwin-helpers.sh
#     qdwin_set_vm demo-260430-0805
#     qdwin_send_key KEY_LEFTCTRL KEY_SPACE        # open launcher
#     qdwin_ctrl "launcher-type foot"
#     qdwin_ctrl "launcher-activate"
#     qdwin_screenshot /tmp/foo.png

# NOTE: this file is sourced — do not `set -e/-u` here; those flags
# bleed into the caller's shell and break interactive use. Helpers
# return nonzero on failure; callers can `set -e` themselves.

# qdwin_find_workspace() — shared with tests/apps/qdwin-apps-helpers.sh; see
# tests/lib/workspace.sh for the rationale (worktree-aware upward search).
# shellcheck source=../lib/workspace.sh
source "$(dirname "${BASH_SOURCE[0]}")/../lib/workspace.sh"

: "${VMNAME:=}"
: "${QDWIN_VIRSH:=virsh -c qemu:///session}"
# Anchor the upward search at the qdwin checkout (QDWIN_REPO when the caller
# set it, else this file's own repo root); fall back to the legacy two-up path
# only when no qdistro sibling exists anywhere above (degraded, but no worse
# than before).
if [ -z "${QDWIN_WORKSPACE:-}" ]; then
    QDWIN_WORKSPACE=$(qdwin_find_workspace "${QDWIN_REPO:-$(dirname "${BASH_SOURCE[0]}")/../..}") \
        || QDWIN_WORKSPACE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd -P)
fi
export QDWIN_WORKSPACE
: "${QDWIN_VM_EXEC:=$QDWIN_WORKSPACE/qdistro/scripts/vm/vm-exec}"
export QDWIN_VM_EXEC
: "${QDWIN_HTTP_DIR:=${QDWIN_REPO}/extra}"
: "${QDWIN_HTTP_URL:=http://10.0.2.2:8765/extra}"

qdwin_set_vm() {
    VMNAME="$1"
}

qdwin_require_vm() {
    if [ -z "${VMNAME:-}" ]; then
        VMNAME=$($QDWIN_VIRSH list --name --state-running | head -1)
    fi
    if [ -z "$VMNAME" ]; then
        echo "qdwin-helpers: no running VM (set VMNAME or qdwin_set_vm)" >&2
        return 1
    fi
}

# ---------------------------------------------------------------- key
#
# All key injection goes through QMP input-send-event so modifier
# state stays consistent across calls. Mixing virsh send-key with the
# QMP qcode path leaves dangling modifiers — KEY_ENTER right after a
# qdwin_chord ctrl alt -- l ends up sent with ctrl/alt still held in
# QEMU's input layer, and a focused terminal sees `^[[13;7~` (xterm
# CSI for Ctrl+Shift+Enter) instead of CR.

# Linux KEY_* → qemu qcode mapping (subset covering the harness's needs).
_qdwin_linux_to_qcode() {
    case "$1" in
        KEY_LEFTCTRL)  echo ctrl ;;
        KEY_RIGHTCTRL) echo ctrl_r ;;
        KEY_LEFTALT)   echo alt ;;
        KEY_RIGHTALT)  echo alt_r ;;
        KEY_LEFTSHIFT) echo shift ;;
        KEY_RIGHTSHIFT) echo shift_r ;;
        KEY_LEFTMETA|KEY_RIGHTMETA) echo meta_l ;;
        KEY_TAB)       echo tab ;;
        KEY_ESC)       echo esc ;;
        KEY_ENTER|KEY_KPENTER) echo ret ;;
        KEY_SPACE)     echo spc ;;
        KEY_BACKSPACE) echo backspace ;;
        KEY_DOT)       echo dot ;;
        KEY_MINUS)     echo minus ;;
        KEY_UP)        echo up ;;
        KEY_DOWN)      echo down ;;
        KEY_LEFT)      echo left ;;
        KEY_RIGHT)     echo right ;;
        KEY_[A-Z])     printf "%s" "${1#KEY_}" | tr A-Z a-z ;;
        KEY_[0-9])     echo "${1#KEY_}" ;;
        *)
            echo "qdwin: no qcode for $1" >&2
            return 1 ;;
    esac
}

# Send each linux KEY_* arg as a discrete press+release in order.
# For chord-style (hold-and-tap) use qdwin_chord instead.
qdwin_send_key() {
    qdwin_require_vm
    local k qcode
    for k in "$@"; do
        qcode=$(_qdwin_linux_to_qcode "$k") || return 1
        qdwin_qmp_key "$qcode" down
        sleep 0.03
        qdwin_qmp_key "$qcode" up
        sleep 0.03
    done
}

# --------------------------------------------- QMP input (real chords)
#
# `virsh send-key` presses all listed keys, holds, and releases them
# all in reverse order. That doesn't match a real-keyboard chord like
# Alt+Tab — there's no "Alt held alone, Tab released" intermediate
# state. weston's modifier-release binding (and the qdwin switcher
# grab's modifiers callback) require that intermediate.
#
# QMP `input-send-event` lets us push individual key-down/key-up
# events as separate atomic ops. qcodes (qemu key codes) are NOT
# linux KEY_* names — see qapi/ui.json `QKeyCode` enum. Common ones:
#   alt, alt_r, ctrl, ctrl_r, shift, shift_r, meta_l, meta_r
#   tab, esc, ret, spc, backspace
#   a..z, 0..9, f1..f12, left/right/up/down
qdwin_qmp_key() {
    # qdwin_qmp_key <qcode> <down|up>
    qdwin_require_vm
    local qcode="$1" updown="$2"
    local down=true
    [ "$updown" = up ] && down=false
    $QDWIN_VIRSH qemu-monitor-command "$VMNAME" \
        "{\"execute\": \"input-send-event\", \"arguments\": {\"events\": [{\"type\": \"key\", \"data\": {\"down\": $down, \"key\": {\"type\": \"qcode\", \"data\": \"$qcode\"}}}]}}" \
        >/dev/null
}

# Force every modifier into the released state. A compositor lock transition
# can happen between a chord's key-down and key-up events; QEMU has delivered
# the releases, but the newly promoted lock surface may consume them before
# libweston's normal seat state observes them. Releasing is idempotent, so GUI
# scenarios should call this after an unlock/restart boundary before asserting
# ordinary text input.
qdwin_release_modifiers() {
    qdwin_require_vm
    local k
    for k in ctrl ctrl_r alt alt_r shift shift_r meta_l meta_r; do
        qdwin_qmp_key "$k" up || return $?
        sleep 0.02
    done
}

# Real-keyboard chord: hold modifier(s), tap key(s), release modifier(s).
# Args: <hold-key1> [hold-key2 ...] -- <tap-key1> [tap-key2 ...]
# Example: qdwin_chord alt -- tab           # hold Alt, tap Tab, release Alt
#          qdwin_chord ctrl alt -- l        # Ctrl+Alt+L
#          qdwin_chord alt -- tab tab       # hold Alt, tap Tab twice, release
qdwin_chord() {
    local hold=()
    local tap=()
    local in_tap=0
    for arg in "$@"; do
        if [ "$arg" = "--" ]; then in_tap=1; continue; fi
        if [ "$in_tap" = 0 ]; then hold+=("$arg"); else tap+=("$arg"); fi
    done
    local k
    # Press all holds in order
    for k in "${hold[@]}"; do qdwin_qmp_key "$k" down; sleep 0.03; done
    # Tap each tap-key (down + up)
    for k in "${tap[@]}"; do
        qdwin_qmp_key "$k" down; sleep 0.05
        qdwin_qmp_key "$k" up;   sleep 0.05
    done
    # Release holds in reverse order
    for ((i=${#hold[@]}-1; i>=0; i--)); do
        qdwin_qmp_key "${hold[i]}" up; sleep 0.03
    done
}

# Type a string letter-by-letter via QMP. Lowercase ASCII + space + a
# few punctuation. Slow (~10/sec) — prefer the launcher ctrl-socket
# when the launcher itself is the target; this is for typing into
# focused terminals.
qdwin_type_lower() {
    qdwin_require_vm
    local s="$1"
    local i ch qcode
    for ((i = 0; i < ${#s}; i++)); do
        ch="${s:i:1}"
        case "$ch" in
            ' ') qcode=spc ;;
            '.') qcode=dot ;;
            '-') qcode=minus ;;
            [a-z]) qcode="$ch" ;;
            [0-9]) qcode="$ch" ;;
            *)
                echo "qdwin_type_lower: unsupported char '$ch'" >&2
                return 1 ;;
        esac
        qdwin_qmp_key "$qcode" down
        sleep 0.03
        qdwin_qmp_key "$qcode" up
        sleep 0.04
    done
}

# --------------------------------------------------------- ctrl-socket
#
# Sends a one-line command to qdshell's ctrl-socket and prints the
# response. Runs as `admin` because the socket is owned by uid 1000.
# Available commands (qdshell.py around line 1466+):
#   launcher                       (snapshot)
#   launcher-toggle
#   launcher-type <text>           (sets filter)
#   launcher-activate              (spawns selected entry)
#   switcher
#   switcher-next / switcher-commit
#   list                           (lists toplevels)
#   tray, panel, locker            (snapshots)
#
# Pushing the runner script via the existing host:8765 server is the
# robust path; vm-exec's JSON quoting trips on embedded `"`.
qdwin_ctrl() {
    qdwin_require_vm
    local cmd="$1"
    local script="qdwin-ctrl-$$.sh"
    cat > "$QDWIN_HTTP_DIR/$script" <<EOF
#!/bin/bash
runuser -u admin -- bash -c "echo '$cmd' | socat -t 2 - UNIX-CONNECT:/run/user/1000/qdshell.sock"
EOF
    "$QDWIN_VM_EXEC" "$VMNAME" "wget -qO /tmp/qc.sh $QDWIN_HTTP_URL/$script && bash /tmp/qc.sh" 2>&1 | grep -v '^\[vm-exec\]'
    rm -f "$QDWIN_HTTP_DIR/$script"
}

# ----------------------------------------------------- mouse (QMP)
#
# QMP `input-send-event` also covers mouse via three event types:
#   abs: {"type":"abs", "data":{"axis":"x|y", "value":<0..32767>}}
#   rel: {"type":"rel", "data":{"axis":"x|y", "value":<int>}}
#   btn: {"type":"btn", "data":{"button":"left|middle|right", "down":bool}}
# Multiple events can ship in one call (atomic at QEMU's input layer).
# QEMU's USB Tablet uses absolute coordinates 0..32767 mapped across
# the screen — we convert pixel coords to that range based on output
# size (default 1280x800, override via QDWIN_SCREEN_W / _H).
: "${QDWIN_SCREEN_W:=1280}"
: "${QDWIN_SCREEN_H:=800}"

# Move pointer to absolute pixel (x, y).
qdwin_mouse_move() {
    qdwin_require_vm
    local x="$1" y="$2"
    local ax=$(( x * 32767 / QDWIN_SCREEN_W ))
    local ay=$(( y * 32767 / QDWIN_SCREEN_H ))
    $QDWIN_VIRSH qemu-monitor-command "$VMNAME" \
        "{\"execute\": \"input-send-event\", \"arguments\": {\"events\": [
            {\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$ax}},
            {\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$ay}}
        ]}}" >/dev/null
}

# Send a mouse button event (left/middle/right) without moving.
qdwin_mouse_button() {
    qdwin_require_vm
    local btn="$1" updown="$2"
    local down=true
    [ "$updown" = up ] && down=false
    $QDWIN_VIRSH qemu-monitor-command "$VMNAME" \
        "{\"execute\": \"input-send-event\", \"arguments\": {\"events\": [
            {\"type\":\"btn\",\"data\":{\"button\":\"$btn\",\"down\":$down}}
        ]}}" >/dev/null
}

# Send an abs-move AND a button event in a SINGLE input-send-event so the
# pointer position and the button transition arrive atomically at QEMU's
# input layer. QEMU's USB tablet can drop a standalone `btn` that is not
# accompanied by a position update on the same report; coalescing the move
# with the press/release is the reliable shape (see comment above re:
# atomic multi-event batches).
qdwin_mouse_move_button() {
    qdwin_require_vm
    local x="$1" y="$2" btn="$3" updown="$4"
    local ax=$(( x * 32767 / QDWIN_SCREEN_W ))
    local ay=$(( y * 32767 / QDWIN_SCREEN_H ))
    local down=true
    [ "$updown" = up ] && down=false
    $QDWIN_VIRSH qemu-monitor-command "$VMNAME" \
        "{\"execute\": \"input-send-event\", \"arguments\": {\"events\": [
            {\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$ax}},
            {\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$ay}},
            {\"type\":\"btn\",\"data\":{\"button\":\"$btn\",\"down\":$down}}
        ]}}" >/dev/null
}

# Click left button at (x, y) — press (with move) then release (with move).
# The move is batched into BOTH the press and release events so the button
# transition is never sent as a standalone report (which QEMU's tablet can
# drop). See qdwin_mouse_move_button.
qdwin_click() {
    local x="$1" y="$2" btn="${3:-left}"
    qdwin_mouse_move "$x" "$y"
    sleep 0.05
    qdwin_mouse_move_button "$x" "$y" "$btn" down
    sleep 0.05
    qdwin_mouse_move_button "$x" "$y" "$btn" up
    sleep 0.05
}

# Mouse-drag from (x1,y1) to (x2,y2) with the left button held.
# Emits: move-to-start → button-down → N intermediate moves → button-up.
# The intermediate steps matter — qdwin's move grab translates the
# toplevel on every motion event; a single jump from start to end gives
# a much-less-realistic test (and on some compositors collapses to no
# visible motion). Default 8 steps with 30ms gaps; override via
# QDWIN_DRAG_STEPS / QDWIN_DRAG_STEP_MS.
: "${QDWIN_DRAG_STEPS:=8}"
: "${QDWIN_DRAG_STEP_MS:=30}"
qdwin_drag() {
    qdwin_require_vm
    local x1="$1" y1="$2" x2="$3" y2="$4" btn="${5:-left}"
    qdwin_mouse_move "$x1" "$y1"
    sleep 0.08
    qdwin_mouse_button "$btn" down
    sleep 0.08
    local steps="$QDWIN_DRAG_STEPS"
    local i
    for (( i=1; i<=steps; i++ )); do
        local cx=$(( x1 + (x2 - x1) * i / steps ))
        local cy=$(( y1 + (y2 - y1) * i / steps ))
        qdwin_mouse_move "$cx" "$cy"
        # bash sleep accepts fractional seconds
        sleep "$(awk "BEGIN { printf \"%.3f\", $QDWIN_DRAG_STEP_MS/1000 }")"
    done
    qdwin_mouse_button "$btn" up
    sleep 0.1
}

# --------------------------------------------------------- screenshot
qdwin_screenshot() {
    qdwin_require_vm
    local out="${1:-/tmp/qdwin-shot.png}"
    local tmp="${out%.png}.ppm"
    $QDWIN_VIRSH screenshot "$VMNAME" "$tmp" >/dev/null
    mv "$tmp" "$out"
    echo "$out"
}

# ----------------------------------------------------- session sanity
#
# Returns 0 if the qdwin user session is up: the admin wayland socket
# exists and the qdwin-compositor + qdshell user units are active.
# Mirrors the gui gate liveness probe (qdistro/ci/lib/gates/gui.sh) — the
# old qdshell ctrl-socket ("launcher" command) was removed when qdshell
# moved to Quickshell IPC, so probing it always failed. Use as the first
# step of any scenario.
qdwin_session_healthy() {
    qdwin_require_vm || return 2
    "$QDWIN_VM_EXEC" "$VMNAME" \
        "test -S /run/user/1000/wayland-1 && runuser -u admin -- env XDG_RUNTIME_DIR=/run/user/1000 systemctl --user is-active qdwin-compositor.service qdshell.service >/dev/null"
}
