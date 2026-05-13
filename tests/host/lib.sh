# Shared bits for host-tests harness scripts. Source me, don't run me.
#
# Per-test state lives under $TEST_ROOT/$TEST_ID/:
#   - runtime/        : XDG_RUNTIME_DIR for weston + clients
#   - weston.log      : weston log (qdwin-shell.so output included)
#   - qdshell.log     : qdshell stderr
#   - clients.log     : any other clients (weston-terminal, etc.)
#   - ctrl.sock       : qdshell --ctrl-socket
#   - pids/           : one file per spawned process (basename = role)
#   - shots/          : screenshots (PNGs)

set -eo pipefail

TEST_ROOT=${TEST_ROOT:-/tmp/qdwin-host-tests}
QDWIN_BUILD=${QDWIN_BUILD:-/tmp/qdwin-host-build}
QDWIN_INSTALL=${QDWIN_INSTALL:-/tmp/qdwin-host-install}
QDSHELL_PY=${QDSHELL_PY:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../qdshell" && pwd)/qdshell.py}

ht_dir()         { echo "$TEST_ROOT/$1"; }
ht_runtime()     { echo "$TEST_ROOT/$1/runtime"; }
ht_log_weston()  { echo "$TEST_ROOT/$1/weston.log"; }
ht_log_qdshell() { echo "$TEST_ROOT/$1/qdshell.log"; }
ht_log_clients() { echo "$TEST_ROOT/$1/clients.log"; }
ht_ctrl()        { echo "$TEST_ROOT/$1/ctrl.sock"; }
ht_pids()        { echo "$TEST_ROOT/$1/pids"; }
ht_shots()       { echo "$TEST_ROOT/$1/shots"; }
# Short, fixed socket name — per-test runtime dir already namespaces
# it. Longer names like qdwin-test-$TEST_ID overflow the 108-byte
# AF_UNIX sun_path limit for scenario filenames >~24 chars.
ht_socket()      { echo "wayland-1"; }

ht_require_build() {
    if [ ! -f "$QDWIN_INSTALL/lib/weston/qdwin-shell.so" ]; then
        echo "[harness] qdwin-shell.so missing at $QDWIN_INSTALL — building" >&2
        if ! command -v meson >/dev/null && [ -x "$HOME/.local/bin/meson" ]; then
            export PATH="$HOME/.local/bin:$PATH"
        fi
        local repo
        repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
        if [ ! -d "$QDWIN_BUILD" ]; then
            meson setup "$QDWIN_BUILD" "$repo" \
                --prefix="$QDWIN_INSTALL" --libdir=lib >/dev/null
        fi
        ninja -C "$QDWIN_BUILD" install >/dev/null
    fi
}

ht_pid_save() {
    # ht_pid_save <test-id> <role> <pid>
    local d
    d=$(ht_pids "$1")
    mkdir -p "$d"
    echo "$3" > "$d/$2"
}

ht_pid_load() {
    # ht_pid_load <test-id> <role>  -> echoes pid or empty
    local f
    f=$(ht_pids "$1")/$2
    [ -f "$f" ] && cat "$f" || true
}

ht_kill() {
    local pid=$1
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && kill "$pid" 2>/dev/null || true
}
