#!/usr/bin/env python3
"""Lock teardown fail-secure invariants (finding F9).

A locker crash must NOT drop the lock. The two lock-teardown callbacks
(qdwin_lock_surface_destroyed_cb, qdwin_lock_surface_resource_destroyed) must
behave like qdwin_locker_resource_destroy(): when the surface/resource is
destroyed while still locked and no reattach is in flight, they must HOLD the
lock (no `locked = 0`, no qdwin_show_non_lock_layers, no locked_changed(0)) and
instead demote any toplevel + schedule a repaint so the empty lock layer
composites to black. Recovery is systemd Restart=always re-binding a fresh
locker — not auto-unlock.

Separately, the default keyboard grab must drop keys while locked, so the
post-crash window (lock held, overlay grab gone) cannot leak keystrokes to the
focused desktop client.
"""

from pathlib import Path
import re
import sys


def fail(message):
    print(f"FAIL: {message}")
    return 1


def _strip_comments(code):
    code = re.sub(r"/\*.*?\*/", " ", code, flags=re.DOTALL)
    code = re.sub(r"//[^\n]*", " ", code)
    return code


def _function_body(source, signature_regex, name):
    for m in re.finditer(signature_regex, source, re.MULTILINE | re.DOTALL):
        paren = source.index("(", m.start())
        depth = 0
        close = None
        for i in range(paren, len(source)):
            if source[i] == "(":
                depth += 1
            elif source[i] == ")":
                depth -= 1
                if depth == 0:
                    close = i
                    break
        if close is None:
            continue
        j = close + 1
        while j < len(source) and source[j].isspace():
            j += 1
        if j >= len(source) or source[j] != "{":
            continue
        start = j
        depth = 0
        for i in range(start, len(source)):
            if source[i] == "{":
                depth += 1
            elif source[i] == "}":
                depth -= 1
                if depth == 0:
                    return source[start:i + 1], None
        return None, f"{name}: unbalanced braces"
    return None, f"{name} not found"


# The two callbacks that previously auto-unlocked on teardown.
_TEARDOWN_CALLBACKS = (
    (r"static void\s+qdwin_lock_surface_destroyed_cb\s*\(",
     "qdwin_lock_surface_destroyed_cb"),
    (r"static void\s+qdwin_lock_surface_resource_destroyed\s*\(",
     "qdwin_lock_surface_resource_destroyed"),
)


def check_teardown_holds_lock(source):
    for sig, name in _TEARDOWN_CALLBACKS:
        body, err = _function_body(source, sig, name)
        if err:
            return fail(err)
        code = _strip_comments(body)
        # Must not auto-unlock.
        if re.search(r"qdwin->locked\s*=\s*0", code):
            return fail(f"{name} still clears qdwin->locked (auto-unlock); "
                        "must hold the lock fail-secure")
        if "qdwin_show_non_lock_layers" in code:
            return fail(f"{name} still reveals the desktop via "
                        "qdwin_show_non_lock_layers; must stay black")
        if re.search(r"send_locked_changed\s*\([^;]*,\s*0\s*\)", code):
            return fail(f"{name} still emits locked_changed(0) on teardown")
        # Must hold + go black: demote any toplevel and force a repaint.
        if "qdwin_demote_lock_toplevel" not in code:
            return fail(f"{name} does not demote the stale lock toplevel")
        if "weston_compositor_schedule_repaint" not in code:
            return fail(f"{name} does not schedule a repaint to black the "
                        "empty lock layer")
    return 0


def check_default_keygrab_drops_while_locked(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_proxy_default_grab_key\s*\(",
        "qdwin_proxy_default_grab_key")
    if err:
        return fail(err)
    code = _strip_comments(body)
    guard = re.search(
        r"if\s*\(\s*qdwin_singleton\s*&&\s*qdwin_singleton->locked\s*\)\s*"
        r"return\s*;",
        code)
    if not guard:
        return fail("qdwin_proxy_default_grab_key lacks a locked guard "
                    "(`if (qdwin_singleton && qdwin_singleton->locked) "
                    "return;`) — keystrokes could leak to a desktop client "
                    "while the lock is held without the overlay grab")
    send = code.find("weston_keyboard_send_key")
    if send != -1 and guard.start() > send:
        return fail("locked guard runs AFTER weston_keyboard_send_key; the "
                    "key would already be delivered")
    idle = code.find("qdwin_idle_note_activity")
    if idle != -1 and guard.start() > idle:
        return fail("locked guard runs AFTER qdwin_idle_note_activity; the "
                    "guard must be the first statement so a lock-held key is "
                    "dropped without side effects")
    return 0


def check_default_modifiers_drop_while_locked(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_proxy_default_grab_modifiers\s*\(",
        "qdwin_proxy_default_grab_modifiers")
    if err:
        return fail(err)
    code = _strip_comments(body)
    guard = re.search(
        r"if\s*\(\s*qdwin_singleton\s*&&\s*qdwin_singleton->locked\s*\)\s*"
        r"return\s*;",
        code)
    if not guard:
        return fail("qdwin_proxy_default_grab_modifiers lacks a locked guard — "
                    "modifier state (Shift/Ctrl/Alt) could leak to a desktop "
                    "client while the lock is held without the overlay grab")
    send = code.find("weston_keyboard_send_modifiers")
    if send != -1 and guard.start() > send:
        return fail("locked guard runs AFTER weston_keyboard_send_modifiers")
    return 0


def main():
    if len(sys.argv) != 2:
        return fail("usage: test_lock_fail_secure.py <qdwin.c>")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    for check in (
            check_teardown_holds_lock,
            check_default_keygrab_drops_while_locked,
            check_default_modifiers_drop_while_locked):
        rc = check(source)
        if rc:
            return rc
    return 0


if __name__ == "__main__":
    sys.exit(main())
