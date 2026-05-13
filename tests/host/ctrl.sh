#!/bin/bash
# Send a single command to qdshell's --ctrl-socket; print the reply.
#
# Usage: ctrl.sh <test-id> <cmd> [args...]
#   e.g. ctrl.sh foo max 1
#        ctrl.sh foo state 1
#        ctrl.sh foo list
#
# Uses socat for unix-socket I/O; falls back to python if socat is
# missing. Exits non-zero on err replies (handy for assertions).

set -eo pipefail
. "$(dirname "$0")/lib.sh"

TEST_ID=${1:?usage: ctrl.sh <test-id> <cmd> [args...]}
shift
CMD="$*"
[ -n "$CMD" ] || { echo "[ctrl.sh] empty command" >&2; exit 2; }

CTRL=$(ht_ctrl "$TEST_ID")
[ -S "$CTRL" ] || { echo "[ctrl.sh] no ctrl-socket at $CTRL — start.sh not run?" >&2; exit 3; }

if command -v socat >/dev/null; then
    REPLY=$(echo "$CMD" | socat - UNIX-CONNECT:"$CTRL")
else
    REPLY=$(python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect('$CTRL')
s.sendall(b'$CMD\n')
print(s.recv(4096).decode().rstrip())
" )
fi
echo "$REPLY"
case "$REPLY" in
    err*) exit 1 ;;
    *)    exit 0 ;;
esac
