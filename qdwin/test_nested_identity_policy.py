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


def check_bind_proxy_pixels_preserves_pending_gate(source):
    body, err = _function_body(
        source,
        r"qdwin_handle_bind_proxy_pixels\s*\(\s*struct wl_client",
        "qdwin_handle_bind_proxy_pixels",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in body:
        return fail("bind_proxy_pixels does not consult pending broker gate")
    if "&qdwin->held_layer.view_list" not in body:
        return fail("bind_proxy_pixels never keeps pending pixels on held layer")
    if not re.search(
            r"tl->nested_proxy_pending_decision\s*\?\s*"
            r"&qdwin->held_layer\.view_list\s*:\s*"
            r"&qdwin->normal_layer\.view_list",
            body, re.DOTALL):
        return fail("bind_proxy_pixels does not choose held vs normal layer "
                    "from nested_proxy_pending_decision")
    return 0


def check_pixel_destroy_preserves_pending_gate(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_proxy_pixel_surface_destroyed\s*\(",
        "qdwin_proxy_pixel_surface_destroyed",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in body:
        return fail("pixel-surface destroy path does not consult pending gate")
    if "&qdwin->held_layer.view_list" not in body:
        return fail("pixel-surface destroy path cannot restore held curtain")
    return 0


def check_geometry_resize_preserves_pending_gate(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_nested_proxy_set_geometry\s*\(",
        "qdwin_nested_proxy_set_geometry",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in body:
        return fail("nested-proxy geometry resize does not consult pending gate")
    if "&qdwin->held_layer.view_list" not in body:
        return fail("nested-proxy geometry resize cannot keep pending curtain "
                    "on held layer")
    return 0


def check_stale_decisions_are_idempotent(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_handle_nested_proxy_decision\s*\(",
        "qdwin_handle_nested_proxy_decision",
    )
    if err:
        return fail(err)
    deny_case = re.search(
        r"case\s+1:\s*/\*\s*deny\s*\*/(.*?)case\s+2:",
        body, re.DOTALL)
    if not deny_case:
        return fail("nested_proxy_decision deny case not found")
    text = deny_case.group(1)
    if "!tl->nested_proxy_pending_decision" not in text:
        return fail("nested_proxy_decision deny does not ignore non-pending "
                    "handles")
    if not re.search(
            r"if\s*\(\s*!tl->nested_proxy_pending_decision\s*\)\s*\{"
            r".*?return;",
            text, re.DOTALL):
        return fail("nested_proxy_decision stale deny does not return before "
                    "destroying proxy")

    defer_case = re.search(
        r"case\s+2:\s*/\*\s*defer\s*\*/(.*?)default:",
        body, re.DOTALL)
    if not defer_case:
        return fail("nested_proxy_decision defer case not found")
    text = defer_case.group(1)
    if "!tl->nested_proxy_pending_decision" not in text:
        return fail("nested_proxy_decision defer does not ignore non-pending "
                    "handles")
    if not re.search(
            r"if\s*\(\s*!tl->nested_proxy_pending_decision\s*\)\s*\{"
            r".*?return;",
            text, re.DOTALL):
        return fail("nested_proxy_decision stale defer does not return as "
                    "an idempotent no-op")
    return 0


def check_focus_requests_preserve_pending_gate(source):
    for fn in ("qdwin_handle_set_keyboard_focus",
               "qdwin_handle_set_keyboard_focus_v2"):
        body, err = _function_body(
            source,
            rf"static void\s+{fn}\s*\(",
            fn,
        )
        if err:
            return fail(err)
        if "tl->nested_proxy_pending_decision" not in body:
            return fail(f"{fn} does not reject pending nested proxies")
        if not re.search(
                r"if\s*\(\s*tl->nested_proxy_pending_decision\s*\)\s*\{"
                r".*?return;",
                body, re.DOTALL):
            return fail(f"{fn} pending nested-proxy branch does not return "
                        "before focusing")
    return 0


def check_focus_requests_shell_role_gated(source):
    """The set_keyboard_focus handlers (and clear_selection) redirect keyboard
    focus and/or clear the seat/primary selections — privileged shell-only
    operations ("Sent by the shell" in qdwin-shell-v1.xml). They must gate on
    qdwin_shell_require_bound (fail-closed: rejects until a client claims the
    shell role via bind_as_shell), like the rest of the shell request surface
    (move_toplevel_to_workspace, set_workspace_name, ...). Without it a
    non-shell allowed_uid client holding a qdwin_shell_v1 resource could steer
    focus and wipe selections before any shell binds (second-bind rejection
    only triggers once shell_bound is set)."""
    # Privileged side effects that must never run before the shell-bound gate.
    sinks = ("weston_seat_set_selection",
             "weston_keyboard_set_focus",
             "weston_seat_set_keyboard_focus",
             "qdwin_primary_seat_clear_selection")
    for fn in ("qdwin_handle_set_keyboard_focus",
               "qdwin_handle_set_keyboard_focus_v2",
               "qdwin_handle_clear_selection"):
        body, err = _function_body(
            source,
            rf"static void\s+{fn}\s*\(",
            fn,
        )
        if err:
            return fail(err)
        if "qdwin_shell_require_bound" not in body:
            return fail(f"{fn} does not gate on the shell-bound check "
                        "(qdwin_shell_require_bound)")
        # The gate must guard the privileged side effects: it has to appear
        # before the first selection-clearing / focus-setting call, not after.
        gate = body.index("qdwin_shell_require_bound")
        for sink in sinks:
            at = body.find(sink)
            if at != -1 and at < gate:
                return fail(f"{fn} reaches {sink} before the shell-bound "
                            "gate")
    return 0


def check_minimize_preserves_pending_gate(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_toplevel_set_minimized\s*\(",
        "qdwin_toplevel_set_minimized",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in body:
        return fail("minimize path does not reject pending nested proxies")
    if not re.search(
            r"if\s*\(\s*tl->nested_proxy_pending_decision\s*\)\s*\{"
            r".*?return;",
            body, re.DOTALL):
        return fail("minimize pending nested-proxy branch does not return "
                    "before moving layers")
    return 0


def check_view_stream_preserves_pending_gate(source):
    subscribe_body, err = _function_body(
        source,
        r"static void\s+qdwin_handle_subscribe_view_stream\s*\(",
        "qdwin_handle_subscribe_view_stream",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in subscribe_body:
        return fail("subscribe_view_stream does not reject pending nested "
                    "proxies")
    if "qdwin_view_stream_v1_send_denied" not in subscribe_body:
        return fail("subscribe_view_stream pending nested-proxy branch does "
                    "not send a denied event")

    pin_body, err = _function_body(
        source,
        r"static void\s+qdwin_view_stream_pin\s*\(",
        "qdwin_view_stream_pin",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in pin_body:
        return fail("view_stream_pin lacks defense-in-depth pending gate")

    focus_body, err = _function_body(
        source,
        r"static void\s+qdwin_stream_seat_assert_focus\s*\(",
        "qdwin_stream_seat_assert_focus",
    )
    if err:
        return fail(err)
    if "s->tl->nested_proxy_pending_decision" not in focus_body:
        return fail("stream input focus lacks defense-in-depth pending gate")
    return 0


def check_state_requests_preserve_pending_gate(source):
    for fn in ("qdwin_toplevel_set_maximized",
               "qdwin_toplevel_set_fullscreen",
               "qdwin_toplevel_set_tiled"):
        body, err = _function_body(
            source,
            rf"static void\s+{fn}\s*\(",
            fn,
        )
        if err:
            return fail(err)
        if "tl->nested_proxy_pending_decision" not in body:
            return fail(f"{fn} does not reject pending nested proxies")
        if not re.search(
                r"if\s*\(\s*tl->nested_proxy_pending_decision\s*\)\s*\{"
                r".*?return;",
                body, re.DOTALL):
            return fail(f"{fn} pending nested-proxy branch does not return "
                        "before moving/resizing")

    move_body, err = _function_body(
        source,
        r"static void\s+qdwin_handle_begin_interactive_move\s*\(",
        "qdwin_handle_begin_interactive_move",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in move_body:
        return fail("begin_interactive_move does not reject pending nested "
                    "proxies")
    return 0


def check_pixel_destroy_preserves_position(source):
    body, err = _function_body(
        source,
        r"static void\s+qdwin_proxy_pixel_surface_destroyed\s*\(",
        "qdwin_proxy_pixel_surface_destroyed",
    )
    if err:
        return fail(err)
    if "weston_view_get_pos_offset_global" not in body:
        return fail("pixel-surface destroy path does not preserve current "
                    "pixel-feed position")
    if "qdwin_primary_output" in body:
        return fail("pixel-surface destroy path recenters fallback curtain "
                    "instead of preserving position")
    return 0


def check_generic_raise_paths_preserve_pending_gate(source):
    move_body, err = _function_body(
        source,
        r"static void\s+qdwin_toplevel_move_to_layer\s*\(",
        "qdwin_toplevel_move_to_layer",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in move_body:
        return fail("generic toplevel move helper does not guard pending "
                    "nested proxies")
    if "&tl->qdwin->held_layer" not in move_body:
        return fail("generic toplevel move helper does not redirect pending "
                    "normal-layer moves to held_layer")

    request_body, err = _function_body(
        source,
        r"static void\s+qdwin_handle_request_raise\s*\(",
        "qdwin_handle_request_raise",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in request_body:
        return fail("request_raise does not ignore pending nested proxies")

    hit_body, err = _function_body(
        source,
        r"static struct qdwin_toplevel \*\s+qdwin_toplevel_at_pos\s*\(",
        "qdwin_toplevel_at_pos",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in hit_body:
        return fail("click hit-test does not skip pending nested proxies")

    chrome_body, err = _function_body(
        source,
        r"static struct qdwin_toplevel \*\s+qdwin_chrome_at_pos\s*\(",
        "qdwin_chrome_at_pos",
    )
    if err:
        return fail(err)
    if "tl->nested_proxy_pending_decision" not in chrome_body:
        return fail("chrome hit-test does not skip pending nested proxies")
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
        check_bind_proxy_pixels_preserves_pending_gate,
        check_pixel_destroy_preserves_pending_gate,
        check_geometry_resize_preserves_pending_gate,
        check_stale_decisions_are_idempotent,
        check_focus_requests_preserve_pending_gate,
        check_focus_requests_shell_role_gated,
        check_minimize_preserves_pending_gate,
        check_view_stream_preserves_pending_gate,
        check_state_requests_preserve_pending_gate,
        check_pixel_destroy_preserves_position,
        check_generic_raise_paths_preserve_pending_gate,
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
