#!/usr/bin/env python3
"""Pin XWayland identity publication and geometry-stable state restores."""

from pathlib import Path
import re
import sys


def fail(message: str) -> int:
    print(f"FAIL: {message}")
    return 1


def body(source: str, name: str) -> str:
    match = re.search(rf"\n{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if not match:
        raise ValueError(f"{name} definition not found")
    start = source.index("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError(f"{name} has unbalanced braces")


def main() -> int:
    if len(sys.argv) != 2:
        return fail("usage: test_xwayland_window_policy.py qdwin.c")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    try:
        identity = body(source, "qdwin_toplevel_effective_app_id")
        announce = body(source, "qdwin_send_toplevel_added")
        seed = body(source, "qdwin_toplevel_seed_outer_from_committed")
        maximize = body(source, "qdwin_toplevel_set_maximized")
        fullscreen = body(source, "qdwin_toplevel_set_fullscreen")
        tile = body(source, "qdwin_toplevel_set_tiled")
    except ValueError as error:
        return fail(str(error))

    if "get_xwayland_window_name(surface, WM_CLASS)" not in identity:
        return fail("empty libweston app_id does not fall back to XWayland WM_CLASS")
    if "qdwin_toplevel_effective_app_id(qdwin, tl)" not in announce:
        return fail("toplevel_added does not publish the effective XWayland app id")

    geometry_at = seed.find("weston_desktop_surface_get_geometry")
    geometry_width_at = seed.find("geometry.width > 0")
    surface_width_at = seed.find("surface ? surface->width")
    if min(geometry_at, geometry_width_at, surface_width_at) < 0 or not (
            geometry_at < geometry_width_at < surface_width_at):
        return fail("restore seed must prefer desktop geometry over CSD-inflated buffer size")
    for scope, function_body in (
        ("maximize", maximize),
        ("fullscreen", fullscreen),
        ("tile", tile),
    ):
        if "qdwin_toplevel_seed_outer_from_committed(tl);" not in function_body:
            return fail(f"{scope} does not use the geometry-stable restore seed")

    print("PASS: XWayland identity and special-state restore invariants hold")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
