#!/usr/bin/env bash
# One-shot build wrapper for the qdistro-vendored libweston-14.
# Output: src/build/libweston/libweston-14.so.0.0.2
#
# This is intentionally not wired into compositor/meson.build because
# (a) it builds a different pinned-version sub-project with its own
# meson, (b) we want the build cache invalidated only when the
# vendored sources change, not on every qdwin rebuild.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
BUILD="$SRC/build"

if [[ ! -d "$SRC" ]]; then
    echo "error: $SRC missing — re-extract weston @ tag $(cat "$HERE/VERSION") into src/" >&2
    exit 1
fi

cd "$SRC"

# Reconfigure if build/ doesn't exist OR if any source file is newer than
# the existing build.ninja. Cheap heuristic; meson handles incremental.
if [[ ! -f "$BUILD/build.ninja" ]]; then
    rm -rf "$BUILD"
    meson setup "$BUILD" \
        --prefix=/tmp/qdwin-libweston-prefix \
        -Dbackend-drm=false \
        -Dbackend-rdp=false \
        -Dbackend-vnc=false \
        -Dbackend-pipewire=false \
        -Dbackend-wayland=false \
        -Dbackend-x11=false \
        -Dbackend-headless=true \
        -Dbackend-default=headless \
        -Drenderer-gl=false \
        -Dscreenshare=false \
        -Dxwayland=false \
        -Dremoting=false \
        -Dpipewire=false \
        -Dshell-desktop=false \
        -Dshell-fullscreen=false \
        -Dshell-ivi=false \
        -Dshell-kiosk=false \
        -Dcolor-management-lcms=false \
        -Dimage-jpeg=false \
        -Dimage-webp=false \
        -Dsystemd=false \
        -Dtools= \
        -Dtests=false \
        -Ddemo-clients=false \
        -Dsimple-clients= \
        -Ddoc=false \
        -Dwcap-decode=false
fi

ninja -C "$BUILD" \
    libweston/libweston-14.so.0.0.2 \
    libweston/backend-headless/headless-backend.so

# Install into the build's --prefix. libweston has its module-search
# directory baked in at compile time (= ${prefix}/lib64/libweston-14),
# so weston only finds the headless backend if the install populates
# that directory. We use a writable temp prefix; nothing global is
# touched.
DESTDIR= ninja -C "$BUILD" install >/dev/null

PREFIX=/tmp/qdwin-libweston-prefix

echo
echo "Built libweston: $BUILD/libweston/libweston-14.so.0.0.2"
echo "Built backend:   $BUILD/libweston/backend-headless/headless-backend.so"
echo "Installed under: $PREFIX"
ls -la "$PREFIX/lib64/libweston-14.so"* "$PREFIX/lib64/libweston-14/headless-backend.so"
echo
echo "Use with:"
echo "  LD_LIBRARY_PATH=$PREFIX/lib64\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH} <qdwin-launch-cmd>"
