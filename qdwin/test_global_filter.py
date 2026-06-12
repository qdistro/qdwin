#!/usr/bin/env python3
"""Static-invariant checks for the advertised-global visibility filter
(security; 02/S1, the §4b finding — cross-silo screen capture + privileged
manager reach).

The IME / virtual-keyboard manager globals carry their own bind-time uid/exe
pins, so the IME and VK source-invariant tests already cover their classify
rows. The globals whose ONLY headless protection is the filter wiring are:

  * weston_capture_v1 — libweston binds it with NO bind-time uid/exe pin, so
    the filter is the sole gate. It must be shell-only (denied to ordinary AND
    silo clients).
  * wp_security_context_manager_v1 — minting security contexts; shell-only.
  * ext_idle_notifier_v1 — whole-session idle/resume leaks presence/activity
    across silos. It stays visible to trusted session components but is hidden
    from secctx/silo clients.

The live half (a silo client's wl_registry actually lacks the globals) is
VM/B1-gated. The load-bearing pieces a headless source-invariant test CAN pin,
so that deleting any one of them is a mechanical failure rather than a silent
regression:

1. The filter is actually installed via wl_display_set_global_filter() with
   qdwin_secctx_global_filter. Without this call the whole matrix is dead code.
2. qdwin_classify_global maps the capture global
   (compositor->output_capture.weston_capture_v1) to QDWIN_GLOBAL_WESTON_CAPTURE
   the secctx manager global to QDWIN_GLOBAL_SECCTX_MANAGER, and qdwin's
   ext-idle global to QDWIN_GLOBAL_IDLE_NOTIFIER. Misclassifying any as
   ORDINARY makes it silo-visible again.
3. The filter delegates the per-class decision to qdwin_global_visible.
4. The pure policy gates BOTH capture and the secctx manager shell-only
   (cred == QDWIN_CRED_SHELL), i.e. denied to ordinary and silo clients.
   The idle notifier is denied to secctx/silo clients.
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


def _norm(code):
    """Strip comments and collapse all whitespace to single spaces, so a
    pointer-comparison and its return can be matched regardless of line breaks
    or indentation."""
    return re.sub(r"\s+", " ", _strip_comments(code)).strip()


def _function_body(source, signature_regex, name):
    """Brace-balanced body of the first function *definition* matching
    signature_regex (skips forward declarations)."""
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
            continue  # forward declaration
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


def check_filter_is_installed(source):
    """The global filter must actually be registered on the display; without
    this call qdwin_global_visible is dead code and every silo sees everything."""
    code = _strip_comments(source)
    if not re.search(
            r"wl_display_set_global_filter\s*\([^;]*qdwin_secctx_global_filter",
            code, re.DOTALL):
        return fail("qdwin_secctx_global_filter is not installed via "
                    "wl_display_set_global_filter (the visibility matrix would "
                    "never run)")
    return 0


def check_privileged_globals_classified(source):
    """qdwin_classify_global must map the gated globals to their privileged
    kinds — misclassifying any as ORDINARY re-opens it.
    Co-occurrence is not enough: require the branch that compares against the
    privileged pointer to RETURN the matching kind (so the pointer cannot be
    matched but routed to QDWIN_GLOBAL_ORDINARY or the wrong enum)."""
    body, err = _function_body(
        source, r"static enum qdwin_global_kind\s+qdwin_classify_global\s*\(",
        "qdwin_classify_global")
    if err:
        return fail(err)
    code = _norm(body)
    # `global == <ptr>) return <KIND>;` — the comparison and its return, with
    # nothing but the closing paren between them.
    if not re.search(
            r"global\s*==\s*[^;]*output_capture\.weston_capture_v1\s*\)\s*"
            r"return\s+QDWIN_GLOBAL_WESTON_CAPTURE\s*;", code):
        return fail("qdwin_classify_global does not return "
                    "QDWIN_GLOBAL_WESTON_CAPTURE for the "
                    "output_capture.weston_capture_v1 pointer (capture could "
                    "classify ORDINARY = visible to silos)")
    if not re.search(
            r"global\s*==\s*[^;]*security_context_manager_global\s*\)\s*"
            r"return\s+QDWIN_GLOBAL_SECCTX_MANAGER\s*;", code):
        return fail("qdwin_classify_global does not return "
                    "QDWIN_GLOBAL_SECCTX_MANAGER for the "
                    "security_context_manager_global pointer")
    if not re.search(
            r"global\s*==\s*[^;]*idle_notifier_global\s*\)\s*"
            r"return\s+QDWIN_GLOBAL_IDLE_NOTIFIER\s*;", code):
        return fail("qdwin_classify_global does not return "
                    "QDWIN_GLOBAL_IDLE_NOTIFIER for the "
                    "idle_notifier_global pointer (ext-idle could classify "
                    "ORDINARY = visible to silos)")
    return 0


def check_filter_consults_policy(source):
    """The filter must classify the global and delegate to qdwin_global_visible."""
    body, err = _function_body(
        source, r"static bool\s+qdwin_secctx_global_filter\s*\(",
        "qdwin_secctx_global_filter")
    if err:
        return fail(err)
    code = _strip_comments(body)
    if "qdwin_classify_global" not in code:
        return fail("qdwin_secctx_global_filter does not classify the global")
    if "qdwin_global_visible" not in code:
        return fail("qdwin_secctx_global_filter does not consult the "
                    "qdwin_global_visible policy matrix")
    return 0


def check_capture_and_secctx_are_shell_only(logic_source):
    """The pure policy must gate BOTH weston_capture and the secctx manager
    shell-only (cred == QDWIN_CRED_SHELL): denied to ordinary AND silo."""
    policy, err = _function_body(
        logic_source, r"bool\s+qdwin_global_visible\s*\(",
        "qdwin_global_visible")
    if err:
        return fail(err)
    code = _norm(policy)
    # Each shell-only case must resolve to EXACTLY `return cred ==
    # QDWIN_CRED_SHELL;`. Grab the first return after the case label (possibly
    # shared with an immediately-preceding fall-through case) and require the
    # exact predicate — a widened arm like `cred == QDWIN_CRED_SHELL || cred ==
    # QDWIN_CRED_ORDINARY` must NOT pass.
    for kind, label in (
            ("QDWIN_GLOBAL_WESTON_CAPTURE", "weston_capture_v1"),
            ("QDWIN_GLOBAL_SECCTX_MANAGER", "the secctx manager")):
        m = re.search(re.escape("case " + kind) +
                      r"\s*:(?:\s*case [A-Z_]+\s*:)*\s*(return[^;]*;)", code)
        if not m:
            return fail(f"qdwin_global_visible has no {kind} row with a "
                        f"direct return")
        ret = re.sub(r"\s+", " ", m.group(1)).strip()
        if ret != "return cred == QDWIN_CRED_SHELL ;" and \
           ret != "return cred == QDWIN_CRED_SHELL;":
            return fail(f"{label} is not gated EXACTLY shell-only "
                        f"(expected `return cred == QDWIN_CRED_SHELL;`, got "
                        f"`{ret}`) — a widened row would leak it to ordinary "
                        f"or silo clients")
    return 0


def check_idle_notifier_hidden_from_secctx(logic_source):
    """ext_idle_notifier_v1 must be denied to secctx/silo clients while
    remaining visible to session components."""
    policy, err = _function_body(
        logic_source, r"bool\s+qdwin_global_visible\s*\(",
        "qdwin_global_visible")
    if err:
        return fail(err)
    code = _norm(policy)
    m = re.search(
        r"case\s+QDWIN_GLOBAL_IDLE_NOTIFIER\s*:\s*(?:/\*.*?\*/\s*)?"
        r"(return[^;]*;)",
        code)
    if not m:
        return fail("qdwin_global_visible has no QDWIN_GLOBAL_IDLE_NOTIFIER "
                    "row with a direct return")
    ret = re.sub(r"\s+", " ", m.group(1)).strip()
    if ret != "return cred != QDWIN_CRED_SECCTX ;" and \
       ret != "return cred != QDWIN_CRED_SECCTX;":
        return fail("ext_idle_notifier_v1 is not gated as hidden from "
                    "secctx/silo clients (expected "
                    "`return cred != QDWIN_CRED_SECCTX;`, got "
                    f"`{ret}`)")
    return 0


def main():
    if len(sys.argv) != 3:
        return fail("usage: test_global_filter.py <qdwin.c> <qdwin-logic.c>")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    logic_source = Path(sys.argv[2]).read_text(encoding="utf-8")

    for check in (check_filter_is_installed,
                  check_privileged_globals_classified,
                  check_filter_consults_policy):
        rc = check(source)
        if rc:
            return rc
    rc = check_capture_and_secctx_are_shell_only(logic_source)
    if rc:
        return rc
    rc = check_idle_notifier_hidden_from_secctx(logic_source)
    if rc:
        return rc

    print("PASS: advertised-global filter (installed via "
          "wl_display_set_global_filter; weston_capture_v1 + secctx manager "
          "classified and gated shell-only; ext_idle_notifier_v1 classified "
          "and hidden from secctx/silo clients via qdwin_global_visible)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
