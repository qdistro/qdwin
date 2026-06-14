#!/usr/bin/env bash
# One-shot build wrapper for the qdistro-vendored libweston-14.
#
# Two profiles (set QDWIN_LIBWESTON_PROFILE):
#
#   headless (default) — minimal headless-only build for host-side
#       protocol tests (run-null-parent-test.sh, run-protocol-tests.sh).
#       Output under /tmp/qdwin-libweston-prefix. Nothing global touched.
#
#   production         — full backend set (drm + pipewire + rdp + wayland
#       + x11 + headless, GL renderer, xwayland, lcms) for shipping in a
#       qdistro VM/image. The system `weston` binary loads this
#       libweston-14.so.0 via LD_LIBRARY_PATH and its backends via
#       WESTON_MODULE_MAP. Installed under $PREFIX (default
#       /tmp/qdwin-libweston-prod-prefix); qdistro's
#       install-vendored-libweston.sh stages that tree into
#       /usr/libexec/qdistro/qdwin-libweston/. See
#       doc/decisions/0001-vendored-libweston-packaging.md.
#
# This is intentionally not wired into qdwin's meson.build because
# (a) it builds a different pinned-version sub-project with its own
# meson, (b) we want the build cache invalidated only when the
# vendored sources change, not on every qdwin rebuild.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"

PROFILE="${QDWIN_LIBWESTON_PROFILE:-headless}"

case "$PROFILE" in
headless)
    BUILD="$SRC/build"
    PREFIX="${QDWIN_LIBWESTON_PREFIX:-/tmp/qdwin-libweston-prefix}"
    MESON_OPTS=(
        -Dbackend-drm=false
        -Dbackend-rdp=false
        -Dbackend-vnc=false
        -Dbackend-pipewire=false
        -Dbackend-wayland=false
        -Dbackend-x11=false
        -Dbackend-headless=true
        -Dbackend-default=headless
        -Drenderer-gl=false
        -Dxwayland=false
        -Dremoting=false
        -Dpipewire=false
        -Dcolor-management-lcms=false
    )
    NINJA_TARGETS=(
        libweston/libweston-14.so.0.0.2
        libweston/backend-headless/headless-backend.so
    )
    ;;
production)
    # Separate build dir so the production cache does not clobber the
    # headless test cache (different backend set → full reconfigure).
    BUILD="$SRC/build-prod"
    PREFIX="${QDWIN_LIBWESTON_PREFIX:-/tmp/qdwin-libweston-prod-prefix}"
    # Backend set mirrors the qdwin systemd unit's WESTON_MODULE_MAP
    # (drm + pipewire + rdp + wayland + x11 + headless + xwayland + lcms).
    # GL renderer ON for real-GPU hosts (pixman still selectable in
    # weston.ini for virtio-gpu). VNC stays off (unused by qdwin).
    #
    # Each backend defaults ON but can be forced off for host-side build
    # validation on machines that lack a specific devel package, e.g.
    #   QDWIN_LW_PIPEWIRE=false QDWIN_LW_RDP=false ... production build
    # The VM/image deps (scripts/.../install-deps.sh) cover the full set,
    # so leave these unset for a shippable tree.
    MESON_OPTS=(
        -Dbackend-drm="${QDWIN_LW_DRM:-true}"
        -Dbackend-rdp="${QDWIN_LW_RDP:-true}"
        -Dbackend-vnc=false
        -Dbackend-pipewire="${QDWIN_LW_PIPEWIRE:-true}"
        -Dbackend-wayland="${QDWIN_LW_WAYLAND:-true}"
        -Dbackend-x11="${QDWIN_LW_X11:-true}"
        -Dbackend-headless=true
        -Dbackend-default=drm
        -Drenderer-gl="${QDWIN_LW_GL:-true}"
        -Dxwayland="${QDWIN_LW_XWAYLAND:-true}"
        -Dremoting=false
        -Dpipewire="${QDWIN_LW_PIPEWIRE:-true}"
        -Dcolor-management-lcms="${QDWIN_LW_LCMS:-true}"
    )
    # Build everything that gets installed for this backend set.
    NINJA_TARGETS=()
    ;;
*)
    echo "error: unknown QDWIN_LIBWESTON_PROFILE='$PROFILE'" \
         "(want 'headless' or 'production')" >&2
    exit 2
    ;;
esac

if [[ ! -d "$SRC" ]]; then
    echo "error: $SRC missing — re-extract weston @ tag $(cat "$HERE/VERSION") into src/" >&2
    exit 1
fi

cd "$SRC"

# Reconfigure if build/ doesn't exist. meson handles incremental rebuilds
# thereafter; delete the build dir to force a profile/option change.
if [[ ! -f "$BUILD/build.ninja" ]]; then
    rm -rf "$BUILD"
    # The trailing -D flags disable optional weston features qdwin does not use
    # so the production tree builds without their devel deps. backend-drm's
    # VA-API screencast recorder (-Dbackend-drm-screencast-vaapi) defaults ON and
    # hard-requires libva (`ERROR: VA-API recorder requires libva >= 0.34.0`);
    # qdwin records nothing through it (view streaming goes via pipewire), so
    # disable it rather than pull libva-devel into every bake.
    meson setup "$BUILD" \
        --prefix="$PREFIX" \
        "${MESON_OPTS[@]}" \
        -Dscreenshare=false \
        -Dshell-desktop=false \
        -Dshell-fullscreen=false \
        -Dshell-ivi=false \
        -Dshell-kiosk=false \
        -Dimage-jpeg=false \
        -Dimage-webp=false \
        -Dsystemd=false \
        -Dtools= \
        -Dtests=false \
        -Ddemo-clients=false \
        -Dsimple-clients= \
        -Ddoc=false \
        -Dwcap-decode=false \
        -Dbackend-drm-screencast-vaapi=false
fi

if [[ ${#NINJA_TARGETS[@]} -gt 0 ]]; then
    ninja -C "$BUILD" "${NINJA_TARGETS[@]}"
else
    ninja -C "$BUILD"
fi

# Install into the build's --prefix. libweston bakes its module-search
# directory in at compile time (= ${prefix}/${libdir}/libweston-14), so
# weston only finds the backends if the install populates that
# directory. We use a writable temp prefix; nothing global is touched.
DESTDIR= ninja -C "$BUILD" install >/dev/null

# meson's default libdir is distro-dependent: lib64 on openSUSE, the arch
# multiarch dir (lib/x86_64-linux-gnu) on Debian/Ubuntu. We deliberately do
# NOT force --libdir, so locate whichever directory the install actually used
# rather than assuming lib64.
CORE_SO=$(ls "$PREFIX"/lib64/libweston-14.so.0.0.2 \
             "$PREFIX"/lib/*/libweston-14.so.0.0.2 \
             "$PREFIX"/lib/libweston-14.so.0.0.2 2>/dev/null | head -n1 || true)
[[ -n "$CORE_SO" ]] || { echo "error: no libweston-14.so under $PREFIX after install" >&2; exit 1; }
LIBDIR=$(dirname "$CORE_SO")

echo
echo "Profile:         $PROFILE"
echo "Built libweston: $BUILD/libweston/libweston-14.so.0.0.2"
echo "Installed under: $PREFIX (libdir: ${LIBDIR#$PREFIX/})"
ls -la "$LIBDIR/libweston-14.so"*
echo "Backends:"
ls -1 "$LIBDIR/libweston-14/"*.so 2>/dev/null | sed 's/^/  /'
echo
if [[ "$PROFILE" == production ]]; then
    echo "Stage into a VM/image with:"
    echo "  QDWIN_LIBWESTON_PREFIX=$PREFIX \\"
    echo "    qdistro/scripts/install/install-vendored-libweston.sh"
else
    echo "Use with:"
    echo "  LD_LIBRARY_PATH=$LIBDIR\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH} <qdwin-launch-cmd>"
fi
