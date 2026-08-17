#!/usr/bin/env python3
"""Ensure forwarder death publishes an attributable teardown boundary."""

import re
import sys
from pathlib import Path


def fail(message):
    print(f"FAIL: {message}")
    return 1


def function_body(source, name):
    match = re.search(rf"static int\s+{name}\s*\([^)]*\)\s*\{{", source)
    if not match:
        return None
    start = match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    return source[start:pos - 1] if depth == 0 else None


def main():
    if len(sys.argv) != 2:
        return fail("usage: test_view_stream_lifecycle.py <qdwin.c>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    body = function_body(source, "qdwin_forward_pidfd_ready")
    if body is None:
        return fail("qdwin_forward_pidfd_ready is missing or malformed")

    log_marker = 'view_stream_torn_down handle=%u pid=%d'
    event_marker = "qdwin_view_stream_v1_send_torn_down"
    for marker in (log_marker, event_marker):
        if marker not in body:
            return fail(f"forwarder-death teardown lacks {marker!r}")
    if not body.index(log_marker) < body.index(event_marker):
        return fail("forwarder-death teardown must log its stable handle/pid, "
                    "then send torn_down")
    if "s->toplevel_handle" not in body or "(int)dead" not in body:
        return fail("view_stream_torn_down log is not attributable to the "
                    "stable toplevel handle and exited forwarder pid")
    if "wl_resource_destroy" in body:
        return fail("forwarder-death callback destroys the client-owned stream "
                    "before its torn_down handler can send the destructor")

    print("PASS: forwarder death logs handle/pid and leaves stream "
          "destruction to the client")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
