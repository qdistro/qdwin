#!/bin/bash
# Start a fresh headless qdwin + qdshell pair for a scenario.
#
# Usage:
#   start.sh <test-id> [--colors <map>] [--width N] [--height N]
#                       [--no-shell] [--no-terminal]
#
# After return:
#   - weston is running headless on socket "qdwin-test-<id>"
#   - qdwin-bystander is running as the shell helper, with its command FIFO
#     at $(ht_ctrl)
#   - one weston-terminal is running as a client (unless --no-terminal)
#
# Stdout is the test-id (so callers can `ID=$(start.sh foo)`).
# Per-test state lives under /tmp/qdwin-host-tests/<id>/.

set -eo pipefail
. "$(dirname "$0")/lib.sh"

TEST_ID=${1:?usage: start.sh <test-id> [opts]}
shift
COLORS="$(id -u)=#22aaff"
WIDTH=1024
HEIGHT=640
WANT_SHELL=1
WANT_TERMINAL=1
ALLOWED_UID=$(id -u)
while [ $# -gt 0 ]; do
    case "$1" in
        --colors)      COLORS=$2; shift 2 ;;
        --width)       WIDTH=$2; shift 2 ;;
        --height)      HEIGHT=$2; shift 2 ;;
        --no-shell)    WANT_SHELL=0; shift ;;
        --no-terminal) WANT_TERMINAL=0; shift ;;
        # Override qdwin's allowed_uid. The secctx / locker / layer-shell
        # bind gates key on it; a scenario can set a FOREIGN uid here to
        # drive the "unauthorized client" branch without a second real uid
        # (see 06-secctx-bind-gate.md). Default: this uid.
        --allowed-uid) ALLOWED_UID=$2; shift 2 ;;
        *) echo "[start.sh] unknown opt $1" >&2; exit 2 ;;
    esac
done

ht_require_build

DIR=$(ht_dir "$TEST_ID")
RUNTIME=$(ht_runtime "$TEST_ID")
WLOG=$(ht_log_weston "$TEST_ID")
SLOG=$(ht_log_qdshell "$TEST_ID")
CLOG=$(ht_log_clients "$TEST_ID")
CTRL=$(ht_ctrl "$TEST_ID")
SOCK=$(ht_socket "$TEST_ID")
SHOTS=$(ht_shots "$TEST_ID")

rm -rf "$DIR"
mkdir -p "$RUNTIME" "$SHOTS"
chmod 0700 "$RUNTIME"
: >"$WLOG"; : >"$SLOG"; : >"$CLOG"

export XDG_RUNTIME_DIR="$RUNTIME"
export QDWIN_ALLOWED_UID=$ALLOWED_UID
# Host GUI scenarios use weston-screenshooter for assertions. qdwin keeps the
# screenshooter interface production-disabled unless this explicit dev/test
# flag is present.
export QDWIN_ENABLE_SCREENSHOOTER=1

# weston headless + qdwin-shell.so
weston \
    --backend=headless \
    --renderer=pixman \
    --shell="$QDWIN_INSTALL/lib/weston/qdwin-shell.so" \
    --width="$WIDTH" --height="$HEIGHT" \
    --debug \
    --log="$WLOG" --socket="$SOCK" >/dev/null 2>&1 &
WPID=$!
ht_pid_save "$TEST_ID" weston "$WPID"

# Wait for the wayland socket to appear (covers the bind→listen race).
for _ in $(seq 1 30); do
    [ -S "$RUNTIME/$SOCK" ] && break
    sleep 0.1
done
[ -S "$RUNTIME/$SOCK" ] || { echo "[start.sh] weston socket never appeared" >&2; cat "$WLOG" >&2; exit 7; }

if [ "$WANT_TERMINAL" = 1 ]; then
    WAYLAND_DISPLAY="$SOCK" weston-terminal >>"$CLOG" 2>&1 &
    ht_pid_save "$TEST_ID" terminal $!
    sleep 1.2
fi

if [ "$WANT_SHELL" = 1 ]; then
    QDWIN_BYSTANDER_FIFO="$CTRL" WAYLAND_DISPLAY="$SOCK" "$QDWIN_BYSTANDER" \
        >>"$SLOG" 2>&1 &
    ht_pid_save "$TEST_ID" qdshell $!
    # Wait for the command FIFO to be created.
    for _ in $(seq 1 30); do
        [ -p "$CTRL" ] && break
        sleep 0.1
    done
    [ -p "$CTRL" ] || { echo "[start.sh] command FIFO never appeared" >&2; cat "$SLOG" >&2; exit 8; }
fi

echo "$TEST_ID"
