#!/usr/bin/env python3
"""Pin the two-lifetime view-stream teardown contract in qdwin.c."""

import re
import sys
from pathlib import Path


def fail(message):
    print(f"FAIL: {message}")
    return 1


def function_body(source, name):
    match = re.search(
        rf"static\s+(?:void|int)\s+{name}\s*\([^)]*\)\s*\{{", source
    )
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


def require_order(body, markers, context):
    for marker in markers:
        if marker not in body:
            return fail(f"{context} lacks {marker!r}")
    positions = [body.index(marker) for marker in markers]
    if positions != sorted(positions):
        return fail(f"{context} has unsafe ordering: {markers!r}")
    return 0


def main():
    if len(sys.argv) != 3:
        return fail(
            "usage: test_view_stream_lifecycle.py <qdwin.c> "
            "<qdwin-shell-v1.xml>"
        )

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    protocol = Path(sys.argv[2]).read_text(encoding="utf-8")

    release = function_body(source, "qdwin_view_stream_release_server_state")
    if release is None:
        return fail("server-state release helper is missing or malformed")
    rc = require_order(
        release,
        (
            "if (s->server_state_released)",
            "s->server_state_released = 1",
            "qdwin_view_stream_reap_forward(s)",
            "qdwin_view_stream_unpin(s)",
            "qdwin_stream_seat_release(s)",
            "wl_list_remove(&s->link)",
            "memset(s->access_token",
            "memset(s->rdp_password",
            "view_stream_server_state_released handle=%u",
        ),
        "exactly-once server-state release",
    )
    if rc:
        return rc
    for marker in (
        "wl_resource_destroy(h)",
        "s->input_claimed = 0",
        "s->allow_input = 0",
        "s->tl = NULL",
        "s->pw_output = NULL",
        "s->listed = 0",
    ):
        if marker not in release:
            return fail(f"server-state release does not revoke {marker!r}")

    death = function_body(source, "qdwin_forward_pidfd_ready")
    if death is None:
        return fail("forwarder-death callback is missing or malformed")
    rc = require_order(
        death,
        (
            "s->torn_down_sent = 1",
            "view_stream_torn_down handle=%u pid=%d",
            "qdwin_view_stream_v1_send_torn_down",
            "qdwin_view_stream_release_server_state(s)",
        ),
        "forwarder-death callback",
    )
    if rc:
        return rc
    if "wl_resource_destroy" in death:
        return fail("forwarder death destroys the client-owned tombstone")

    destroyed = function_body(source, "qdwin_stream_resource_destroyed")
    if destroyed is None:
        return fail("resource-destroy callback is missing or malformed")
    rc = require_order(
        destroyed,
        ("qdwin_view_stream_release_server_state(s)", "free(s)"),
        "destroy/disconnect callback",
    )
    if rc:
        return rc

    client_destroy = function_body(source, "qdwin_stream_handle_destroy")
    if client_destroy is None:
        return fail("client destroy handler is missing or malformed")
    if "send_torn_down" in client_destroy:
        return fail("client destroy emits a duplicate terminal event")
    if "wl_resource_destroy(resource)" not in client_destroy:
        return fail("client destroy does not release its protocol tombstone")

    if "server-originated\n        termination" not in protocol:
        return fail("protocol does not reserve torn_down for server termination")
    if "inert tombstone" not in protocol or "ignores this event" not in protocol:
        return fail("protocol does not specify ignored-client cleanup semantics")

    print(
        "PASS: server death releases active state exactly once; compliant, "
        "ignoring, and disconnected clients retain only a valid tombstone"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
