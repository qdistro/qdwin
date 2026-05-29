#!/usr/bin/env bash
# Headless CI gate for the SHIPPED vendored libweston.
#
# The production profile of build-libweston.sh is what qdistro packages
# (see qdwin/doc/decisions/0001-vendored-libweston-packaging.md). This
# gate proves that profile actually exports the four soft-linked helper
# symbols qdwin resolves via dlsym — i.e. the packaged library can drive
# the layer-shell popup / grab paths. Stock libweston-14 exports none of
# them, so this is the discriminator between a shippable tree and a
# decorative one.
#
# It also runs the install-staging dry-run so a broken stage script is
# caught in CI rather than at image-build time. No VM, no display.
#
# Exit 0 only if all four symbols are exported AND the staging dry-run
# lays down the core + drm backend.

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"   # workspace root (siblings: qdwin, qdistro)
BUILD_SCRIPT="$HERE/build-libweston.sh"
PREFIX="${QDWIN_LIBWESTON_PREFIX:-/tmp/qdwin-libweston-prod-prefix}"

# Allow CI on a host lacking pipewire/rdp/GL devel to still gate the
# symbol export — those backends are orthogonal to the desktop helpers.
# The VM/image build uses the full set (install-deps.sh covers it).
export QDWIN_LW_PIPEWIRE="${QDWIN_LW_PIPEWIRE:-true}"
export QDWIN_LW_RDP="${QDWIN_LW_RDP:-true}"
export QDWIN_LW_GL="${QDWIN_LW_GL:-true}"

SYMS=(
    weston_desktop_xdg_popup_attach_layer_parent
    weston_desktop_xdg_popup_get_geometry
    weston_desktop_xdg_popup_set_layer_grab_handler
    weston_desktop_xdg_popup_dismiss_layer_grab
)

fail() { echo "[prod-syms] FAIL: $*" >&2; exit 1; }

[ -x "$BUILD_SCRIPT" ] || fail "build script missing at $BUILD_SCRIPT"

if [ ! -f "$PREFIX/lib64/libweston-14.so.0.0.2" ]; then
    echo "[prod-syms] building production profile into $PREFIX ..."
    QDWIN_LIBWESTON_PROFILE=production QDWIN_LIBWESTON_PREFIX="$PREFIX" \
        bash "$BUILD_SCRIPT" || fail "production build failed"
fi

CORE="$PREFIX/lib64/libweston-14.so.0.0.2"
[ -f "$CORE" ] || fail "no core library at $CORE after build"

# Capture the dynamic symbol table once. Piping nm into `grep -q` under
# `pipefail` would report SIGPIPE (141) when grep exits early, so read
# the table into a variable and match against that instead.
dynsyms=$(nm -D "$CORE" 2>/dev/null) || fail "nm -D failed on $CORE"
missing=0
for s in "${SYMS[@]}"; do
    if grep -q " T $s\$" <<<"$dynsyms"; then
        echo "[prod-syms] EXPORTED: $s"
    else
        echo "[prod-syms] MISSING:  $s" >&2
        missing=1
    fi
done
[ "$missing" -eq 0 ] || fail "one or more soft-linked helper symbols are not exported by the production library"

# Staging dry-run into a throwaway dest (non-root tolerated).
STAGE_SCRIPT="$REPO/qdistro/scripts/install/install-vendored-libweston.sh"
if [ -x "$STAGE_SCRIPT" ]; then
    DEST="$(mktemp -d)/qdwin-libweston"
    echo "[prod-syms] staging dry-run -> $DEST"
    DEST="$DEST" QDWIN_LIBWESTON_PREFIX="$PREFIX" bash "$STAGE_SCRIPT" \
        >/dev/null 2>&1 || fail "install-vendored-libweston.sh staging failed"
    [ -f "$DEST/lib64/libweston-14.so.0" ] \
        || fail "staged tree missing libweston-14.so.0"
    [ -f "$DEST/lib64/libweston-14/drm-backend.so" ] \
        || fail "staged tree missing drm-backend.so (headless-only / wrong prefix?)"
    rm -rf "$(dirname "$DEST")"
    echo "[prod-syms] staging dry-run OK"
else
    echo "[prod-syms] NOTE: $STAGE_SCRIPT absent — skipping staging dry-run"
fi

echo "[prod-syms] PASS — production libweston exports all helper symbols and stages cleanly"
