#!/usr/bin/env python3
"""Static-invariant checks for qdwin nested-proxy identity hardening.

These are negative-by-construction source checks (same style as
test_xdg_activation_policy.py): they parse qdwin.c and assert that the
nested-proxy paths bind client-asserted identity (origin_uid, input_sink,
bind_proxy_pixels) to the kernel/peer-resolved identity of the advertising
client rather than trusting the client's word.

The live multi-uid spoof scenarios (a client of uid B actually advertising
origin_uid=A, or connecting an input_sink owned by another uid) require a
running compositor with two uids and are exercised by the VM-only probe
host scenario; here we lock the enforcement in place so a regression that
drops a check fails the host test suite without a VM.
"""

from pathlib import Path
import re
import sys


def fail(message):
    print(f"FAIL: {message}")
    return 1


def _function_body(source, signature_regex, name):
    """Return the brace-balanced body of the first function *definition*
    whose opening matches signature_regex, or (None, error_message).

    Skips forward declarations (signature followed by ';') by requiring the
    parameter-list close ')' to be followed by an opening '{'."""
    for m in re.finditer(signature_regex, source, re.MULTILINE | re.DOTALL):
        # Find the ')' that closes this signature's parameter list, then the
        # first non-space char after it. A definition has '{'; a forward
        # declaration has ';'.
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
            continue  # forward declaration; keep looking
        start = j
        depth = 0
        for i in range(start, len(source)):
            c = source[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return source[start:i + 1], None
        return None, f"{name}: unbalanced braces"
    return None, f"{name} not found (no definition)"


def check_advertise_binds_origin_uid(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_nested_manager_advertise_toplevel\s*\(",
        "qdwin_nested_manager_advertise_toplevel",
    )
    if err:
        return fail(err)
    # The handler must resolve the advertising client's peer uid and
    # override a disagreeing client-asserted origin_uid.
    if "wl_client_get_credentials" not in body:
        return fail("advertise_toplevel does not resolve peer credentials")
    if not re.search(r"origin_uid\s*!=\s*\(uint32_t\)\s*peer_uid", body):
        return fail("advertise_toplevel does not compare origin_uid to "
                    "the advertising client's peer uid")
    if not re.search(r"origin_uid\s*=\s*\(uint32_t\)\s*peer_uid", body):
        return fail("advertise_toplevel does not override origin_uid with "
                    "the verified peer uid on mismatch")
    return 0


def check_input_sink_peercred(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_nested_manager_advertise_toplevel\s*\(",
        "qdwin_nested_manager_advertise_toplevel",
    )
    if err:
        return fail(err)
    # After connecting to the client-asserted input_sink, the handler must
    # verify the connected peer's uid (SO_PEERCRED) equals origin_uid and
    # fail closed (close fd) otherwise.
    if "qdwin_fd_peer_uid" not in body:
        return fail("advertise_toplevel does not peer-cred the input sink "
                    "socket (qdwin_fd_peer_uid missing)")
    if not re.search(r"sink_uid\s*!=\s*\(uid_t\)\s*origin_uid", body):
        return fail("advertise_toplevel does not compare input-sink peer "
                    "uid to origin_uid")
    # The mismatch branch must drop the fd (fail closed → display-only).
    guard = re.search(
        r"if\s*\(\s*!qdwin_fd_peer_uid\([^)]*\)\s*\|\|\s*"
        r"sink_uid\s*!=\s*\(uid_t\)\s*origin_uid\s*\)\s*\{(.*?)\}",
        body, re.DOTALL)
    if not guard:
        return fail("input-sink peer-uid guard not found in expected shape")
    if "close(fd)" not in guard.group(1):
        return fail("input-sink peer-uid mismatch does not close the fd "
                    "(fail closed)")
    return 0


def check_fd_peer_uid_fails_closed(source):
    body, err = _function_body(
        source,
        r"static int\s+qdwin_fd_peer_uid\s*\(",
        "qdwin_fd_peer_uid",
    )
    if err:
        return fail(err)
    if "SO_PEERCRED" not in body:
        return fail("qdwin_fd_peer_uid does not use SO_PEERCRED")
    # getsockopt failure (and short read) must return 0 (unverifiable).
    if "getsockopt" not in body or "return 0" not in body:
        return fail("qdwin_fd_peer_uid does not fail closed on getsockopt "
                    "failure")
    return 0


def check_bind_proxy_pixels_ownership(source):
    body, err = _function_body(
        source,
        r"qdwin_handle_bind_proxy_pixels\s*\(\s*struct wl_client",
        "qdwin_handle_bind_proxy_pixels",
    )
    if err:
        return fail(err)
    if "wl_client_get_credentials" not in body:
        return fail("bind_proxy_pixels does not resolve caller credentials")
    if not re.search(r"cuid\s*!=\s*\(uid_t\)\s*tl->proxy_origin_uid", body):
        return fail("bind_proxy_pixels does not compare caller uid to the "
                    "proxy's verified origin uid")
    # The mismatch branch must reject (post_error + return), not proceed.
    guard = re.search(
        r"if\s*\(\s*cuid\s*!=\s*\(uid_t\)\s*tl->proxy_origin_uid\s*\)\s*\{"
        r"(.*?)\n\t\}",
        body, re.DOTALL)
    if not guard:
        return fail("bind_proxy_pixels ownership guard not found")
    if "wl_resource_post_error" not in guard.group(1) or \
            "return;" not in guard.group(1):
        return fail("bind_proxy_pixels ownership mismatch does not reject")
    return 0


def check_stream_input_helper_bound(source):
    """The view-stream input claim must remain tied to the spawned helper
    pid (defence-in-depth alongside the one-shot access_token). This is the
    pre-existing contract we must not regress."""
    body, err = _function_body(
        source,
        r"static void\s+qdwin_stream_input_handle_claim\s*\(",
        "qdwin_stream_input_handle_claim",
    )
    if err:
        return fail(err)
    if "wl_client_get_credentials" not in body:
        return fail("stream_input claim does not resolve peer credentials")
    if "pid != s->forward_pid" not in body:
        return fail("stream_input claim no longer binds the claiming pid to "
                    "the spawned forward helper")
    return 0


def main():
    if len(sys.argv) != 2:
        return fail("usage: test_nested_identity_policy.py <qdwin.c>")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")

    checks = (
        check_fd_peer_uid_fails_closed,
        check_advertise_binds_origin_uid,
        check_input_sink_peercred,
        check_bind_proxy_pixels_ownership,
        check_stream_input_helper_bound,
    )
    for check in checks:
        rc = check(source)
        if rc:
            return rc

    print("PASS: nested-proxy identity hardening (origin_uid bound to peer "
          "uid, input_sink peer-cred-verified, bind_proxy_pixels "
          "owner-gated, stream-input helper-bound)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
