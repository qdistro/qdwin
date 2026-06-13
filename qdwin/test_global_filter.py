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
5. INVENTORY (02/S1 fail-open closure): qdwin_classify_global returns ORDINARY
   (visible to ALL clients, silos included) for any global it does not match by
   pointer identity. So a NEW privileged global added to qdwin.c that someone
   forgets to wire into the classifier silently fails OPEN. To make that a
   mechanical failure rather than a silent regression, this test enumerates
   every `qdwin->*_global = wl_global_create(...)` site in qdwin.c and requires
   each created global to be CONSCIOUSLY categorized: either filter-classified
   in qdwin_classify_global, or listed on the reviewed INTENTIONALLY_VISIBLE
   inventory below (filter-visible by design; protected — if at all — by a
   bind-time identity handler, not the visibility filter). Adding a global to
   neither set fails this test until it is categorized. (The ~25 globals
   INHERITED from libweston core are not created in qdwin.c and cannot be
   enumerated from this source; that residual fail-open is recorded in
   threat-model.md's risk register.)
"""

from pathlib import Path
import re
import sys


def fail(message):
    print(f"FAIL: {message}")
    return 1


# A preprocessing directive's `#` may also be spelled with the digraph `%:`
# (always active in standard C, no flag needed), so every directive guard must
# accept both. (Trigraph `??=` is off by default in the build's `cc` mode, so it
# is intentionally not modeled.)
_DIRECTIVE = r"(?:#|%:)\s*"


def _definition_offsets(source, signature_regex):
    """Start offsets of every FUNCTION DEFINITION (signature followed by a `{`
    body, not a `;` forward declaration) matching signature_regex in
    comment-stripped source. Used to reject conditional-compilation shadowing — a
    canonical body hidden under `#if 0` with a later real (bad) definition yields
    two offsets; and to require the definition be outside any conditional."""
    code = _strip_comments(source)
    offsets = []
    for m in re.finditer(signature_regex, code):
        try:
            paren = code.index("(", m.start())
        except ValueError:
            continue
        depth = 0
        close = None
        for i in range(paren, len(code)):
            if code[i] == "(":
                depth += 1
            elif code[i] == ")":
                depth -= 1
                if depth == 0:
                    close = i
                    break
        if close is None:
            continue
        j = close + 1
        while j < len(code) and code[j].isspace():
            j += 1
        if j < len(code) and code[j] == "{":
            offsets.append(m.start())
    return offsets


def reject_shadowing_and_conditionals(source, signature_regex, body, name, where):
    """Reject (a) more than one DEFINITION of `name` — a `#if 0`-dead canonical
    body plus a later compiled bad body would let the test validate the dead one;
    and (b) any preprocessor conditional (`#if`/`#ifdef`/.../`#endif`) inside the
    protected `body`, AND (c) the definition itself being ENCLOSED by a
    conditional (so a `#if 0`-dead canonical body cannot sit beside a live
    macro-generated replacement). Returns 0 or a fail()."""
    offsets = _definition_offsets(source, signature_regex)
    if len(offsets) != 1:
        return fail(
            f"{where}: found {len(offsets)} definitions of `{name}` (expected "
            f"exactly 1) — a conditional-compiled (`#if 0` / `#ifdef`) duplicate "
            f"could shadow the definition this test validates while the compiler "
            f"builds another.")
    if _inside_any_conditional(_strip_comments(source), offsets[0]):
        return fail(
            f"{where}: the definition of `{name}` is inside a preprocessor "
            f"conditional (`#if`/`#ifdef`/...) — it may be compiled out (with a "
            f"live macro-generated replacement built instead), so the body this "
            f"test validates is not the compiled one. Keep it unconditional.")
    if re.search(_DIRECTIVE + r"(?:if|ifdef|ifndef|elif|else|endif)\b",
                 _strip_comments(body)):
        return fail(
            f"{where}: `{name}` body contains a preprocessor conditional "
            f"(`#if`/`#ifdef`/.../`#endif`) — it could compile out a branch the "
            f"raw text this test validates still shows. Remove it or make the test "
            f"preprocessor-conditional aware.")
    return 0


def _fmt_arms(arms):
    """Readable rendering of a {frozenset(case-labels): predicate} arm map."""
    return "; ".join(
        "|".join(sorted(labels)) + "->" + pred
        for labels, pred in sorted(arms.items(), key=lambda kv: sorted(kv[0])))


def _identifiers(text):
    """Every C identifier token in `text` (keywords included — `if`, `return`,
    `case` are identifiers to the lexer and can be `#define`d in gnu11 mode)."""
    return set(re.findall(r"[A-Za-z_]\w*", _strip_comments(text)))


def reject_macro_aliasing(source, tokens, where):
    """Fail if `source` (already phase-2 line-spliced, comments not yet stripped)
    `#define`/`#undef`s any load-bearing `tokens`. These source-grep checks reason
    over RAW text, so aliasing a pinned identifier would let the COMPILED code
    diverge from the canonical source the test validates (e.g. `#define
    QDWIN_CRED_SHELL QDWIN_CRED_ORDINARY` widens a shell-only row). Returns 0 or a
    fail()."""
    code = _strip_comments(source)
    for tok in sorted(tokens):
        if re.search(_DIRECTIVE + r"(?:define|undef)\s+" + re.escape(tok) + r"\b",
                     code):
            return fail(
                f"{where} has a #define/#undef of load-bearing token `{tok}` — a "
                f"macro could alias it so the COMPILED behavior differs from the "
                f"canonical raw source this test validates, routing a gated "
                f"pointer to a visible/wrong policy. Remove the directive, or "
                f"extend this test to parse preprocessed source.")
    return 0


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


def _inside_any_conditional(code, offset):
    """True if `offset` in `code` lies inside ANY preprocessor conditional
    (`#if`/`#ifdef`/`#ifndef` ... `#endif`) region. The canonical filter install
    is unconditional, so requiring it to be outside every conditional is both
    faithful and complete — it sidesteps having to evaluate `#if` expressions
    (`#if 0`, `#if (0)`, `#if defined(NEVER)`, an inactive `#elif` arm, etc.),
    any of which could compile the install out while the raw call stays visible."""
    depth = 0
    for m in re.finditer(_DIRECTIVE + r"(if|ifdef|ifndef|endif)\b", code):
        if m.start() > offset:
            break
        if m.group(1) == "endif":
            depth = max(0, depth - 1)
        else:
            depth += 1
    return depth > 0


def check_filter_is_installed(source):
    """The global filter must actually be registered on the display, exactly once,
    with the canonical callback — without this the policy is dead code and every
    silo sees everything. Presence alone is not enough: a `#if 0`-dead install, a
    later override (`(display, NULL, NULL)`), or a macro-aliased callback would
    leave the raw call visible while installing no/another filter."""
    code = _strip_comments(source)
    calls = list(re.finditer(
        r"wl_display_set_global_filter\s*\(([^;]*)\)\s*;", code, re.DOTALL))
    if len(calls) != 1:
        return fail(
            f"qdwin.c has {len(calls)} wl_display_set_global_filter(...) call(s) "
            "(expected exactly 1) — a duplicate/override (e.g. a later "
            "`(display, NULL, NULL)`) could clear the canonical filter; none "
            "leaves the visibility matrix uninstalled.")
    args = re.sub(r"\s+", "", calls[0].group(1))
    if args != "ec->wl_display,qdwin_secctx_global_filter,qdwin":
        return fail(
            f"wl_display_set_global_filter args are `{args}`, expected "
            "`ec->wl_display,qdwin_secctx_global_filter,qdwin` — a different "
            "display, callback, or data pointer would install a different/no "
            "filter.")
    if _inside_any_conditional(code, calls[0].start()):
        return fail(
            "the wl_display_set_global_filter install is inside a preprocessor "
            "conditional (`#if`/`#ifdef`/...) — it may be compiled out (`#if 0`, "
            "`#if (0)`, an inactive `#elif`/`#else` arm), leaving no global filter "
            "installed. The canonical install must be unconditional.")
    # The install expression's load-bearing tokens must not be macro-aliased
    # (e.g. `#define qdwin_secctx_global_filter permissive_filter`).
    rc = reject_macro_aliasing(
        source,
        {"wl_display_set_global_filter", "qdwin_secctx_global_filter"},
        "qdwin.c (filter install)")
    if rc:
        return rc
    # `wl_display_set_global_filter` must appear EXACTLY ONCE — the single direct
    # call above. A second reference (a function-pointer capture `fp =
    # wl_display_set_global_filter`, or a `#define SYN wl_display_set_global_filter`
    # synonym called as `SYN(display, NULL, NULL)`) could clear/replace the filter
    # while the one direct canonical call stays intact.
    n_tok = len(re.findall(r"\bwl_display_set_global_filter\b", code))
    if n_tok != 1:
        return fail(
            f"`wl_display_set_global_filter` appears {n_tok} times in qdwin.c "
            "(expected exactly 1, the canonical install) — a second reference "
            "(function-pointer capture or macro synonym) could clear or replace "
            "the filter, leaving gated globals visible.")
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


# The EXACT canonical normalized body of qdwin_secctx_global_filter. Pinning the
# decision SPINE (classify-once, fast-path, final return) is not enough: the body
# could still mutate `global` before classification (`global = qdwin->shell_global;`
# so every gated pointer classifies ORDINARY) or coerce `cred` after credential
# resolution (`cred = QDWIN_CRED_SHELL;`) while keeping the spine canonical. Since
# this is a tiny, security-critical, stable function, we require its whole body to
# match exactly — any change (a benign refactor included) must update this string
# under review, and a malicious mutation cannot keep the body canonical.
CANONICAL_FILTER_BODY = (
    "{ struct qdwin *qdwin = data; "
    "struct wl_client *cw = (struct wl_client *)client; "
    "enum qdwin_global_kind kind = qdwin_classify_global(qdwin, global); "
    "if (kind == QDWIN_GLOBAL_ORDINARY) return true; "
    "enum qdwin_cred_class cred; "
    "if (qdwin_secctx_client_find(qdwin, cw) != NULL) { cred = QDWIN_CRED_SECCTX; } "
    "else if (kind == QDWIN_GLOBAL_SECCTX_MANAGER || "
    "kind == QDWIN_GLOBAL_WESTON_CAPTURE) { "
    "pid_t pid; uid_t uid; gid_t gid; "
    "wl_client_get_credentials(cw, &pid, &uid, &gid); "
    "cred = qdwin_secctx_client_is_authorized(qdwin, cw, pid, uid) "
    "? QDWIN_CRED_SHELL : QDWIN_CRED_ORDINARY; } "
    "else { cred = QDWIN_CRED_ORDINARY; } "
    "return qdwin_global_visible(cred, kind); }")


def check_filter_consults_policy(source):
    """The filter must classify the global and delegate to qdwin_global_visible
    on the UNMODIFIED classification. We pin its whole body to the exact canonical
    form (see CANONICAL_FILTER_BODY): a spine-only pin would still let the body
    mutate `global` before classify or coerce `cred` before the policy call."""
    body, err = _function_body(
        source, r"static bool\s+qdwin_secctx_global_filter\s*\(",
        "qdwin_secctx_global_filter")
    if err:
        return fail(err)
    got = _norm(body)
    if got != CANONICAL_FILTER_BODY:
        return fail(
            "qdwin_secctx_global_filter body is not the exact canonical form — it "
            "must classify once, fast-path allow only ORDINARY, resolve the "
            "credential class, and return qdwin_global_visible(cred, kind) on the "
            "unmodified inputs, with no other statements (no `global =`/`cred =` "
            "coercion, extra returns, or control flow). Got:\n  " + got +
            "\nexpected:\n  " + CANONICAL_FILTER_BODY +
            "\n(If this is a reviewed refactor, update CANONICAL_FILTER_BODY.)")
    return 0


def check_capture_and_secctx_are_shell_only(logic_source):
    """The pure policy must gate BOTH weston_capture and the secctx manager
    shell-only (cred == QDWIN_CRED_SHELL): denied to ordinary AND silo."""
    policy, err = _function_body(
        logic_source, r"bool\s+qdwin_global_visible\s*\(",
        "qdwin_global_visible")
    if err:
        return fail(err)
    # Policy-spine alias guard: the checks below pin the EXACT returned predicates
    # in the raw qdwin-logic.c text, so an alias like `#define QDWIN_CRED_SHELL
    # QDWIN_CRED_ORDINARY` (or `#define return ...`) would leave the pinned text
    # intact while the COMPILED policy widens a shell-only row. Reject aliasing of
    # EXACTLY the identifiers the policy body uses (a complete in-file closure).
    # Include the function NAME itself: `#define qdwin_global_visible other` could
    # rename the canonical definition while a token-pasted replacement emits the
    # real exported symbol with a permissive body. Guarding the name makes the
    # rename a directive this scan rejects (and any token-paste duplicate would
    # then collide at link time instead of silently replacing it).
    rc = reject_macro_aliasing(
        logic_source, _identifiers(policy) | {"qdwin_global_visible"},
        "qdwin-logic.c (qdwin_global_visible)")
    if rc:
        return rc
    rc = reject_shadowing_and_conditionals(
        logic_source, r"bool\s+qdwin_global_visible\s*\(", policy,
        "qdwin_global_visible", "qdwin-logic.c")
    if rc:
        return rc

    # Whole-body canonical grammar for qdwin_global_visible (authoritative): the
    # body must be EXACTLY `switch (kind) { <arms> } return false;` — nothing
    # before the switch, no extra statements/returns, and a fail-closed default.
    # This rejects an early reachable return that bypasses the switch (e.g.
    # `if (kind == QDWIN_GLOBAL_SECCTX_MANAGER) return true;` ahead of it) while
    # the canonical case row is left intact. Each arm's case-label SET maps to an
    # exact predicate; case groups may be reordered but labels↔predicate and the
    # default are pinned.
    inner = _norm(policy).strip()
    if inner.startswith("{"):
        inner = inner[1:]
    if inner.endswith("}"):
        inner = inner[:-1]
    inner = inner.strip()
    m = re.fullmatch(
        r"switch\s*\(\s*kind\s*\)\s*\{(.*)\}\s*return\s+false\s*;",
        inner, re.DOTALL)
    if not m:
        return fail(
            "qdwin_global_visible body is not exactly `switch (kind) { <cases> } "
            "return false;` — an early return ahead of the switch, an extra "
            "statement, or a missing fail-closed default could route a gated "
            "pointer to a visible/wrong policy regardless of the case rows.")
    arms_text = m.group(1).strip()
    arm_re = re.compile(r"((?:case\s+\w+\s*:\s*)+)return\s+([^;]*);\s*")
    parsed = {}
    pos = 0
    while pos < len(arms_text):
        am = arm_re.match(arms_text, pos)
        if not am:
            return fail(
                "qdwin_global_visible switch has a non-canonical arm near "
                f"`{arms_text[pos:pos + 80]}` — every arm must be one or more "
                "`case <KIND>:` labels followed by a single `return <predicate>;` "
                "(no extra statements, fall-through, or early returns).")
        labels = frozenset(re.findall(r"case\s+(\w+)\s*:", am.group(1)))
        parsed[labels] = re.sub(r"\s+", "", am.group(2))
        pos = am.end()
    expected_arms = {
        frozenset(["QDWIN_GLOBAL_ORDINARY"]): "true",
        frozenset(["QDWIN_GLOBAL_INPUT_METHOD",
                   "QDWIN_GLOBAL_VIRTUAL_KEYBOARD"]): "cred!=QDWIN_CRED_SECCTX",
        frozenset(["QDWIN_GLOBAL_WESTON_CAPTURE"]): "cred==QDWIN_CRED_SHELL",
        frozenset(["QDWIN_GLOBAL_SECCTX_MANAGER"]): "cred==QDWIN_CRED_SHELL",
        frozenset(["QDWIN_GLOBAL_IDLE_NOTIFIER"]): "cred!=QDWIN_CRED_SECCTX",
        # findings F0: the trusted shell interface, hidden from secctx/silo
        # clients (the in-scope cross-silo threat). Not shell-only (CRED_SHELL):
        # qdshell is an ORDINARY admin client until it calls bind_as_shell, so a
        # CRED_SHELL policy would hide the global from the client that must bind
        # it. bind_qdwin_shell applies allowed_uid + singleton + secctx-deny.
        frozenset(["QDWIN_GLOBAL_SHELL"]): "cred!=QDWIN_CRED_SECCTX",
        # findings F4: layer-shell hidden from secctx clients (overlays / lock
        # surfaces are shell/locker-only); bind gate remains as second layer.
        frozenset(["QDWIN_GLOBAL_LAYER_SHELL"]): "cred!=QDWIN_CRED_SECCTX",
    }
    if parsed != expected_arms:
        return fail(
            "qdwin_global_visible switch arms are not the canonical policy. "
            f"Got {{{_fmt_arms(parsed)}}}, expected {{{_fmt_arms(expected_arms)}}} "
            "— a changed predicate, label grouping, missing row, or extra row "
            "could leak a gated global to ordinary/silo clients.")

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


# qdwin-created globals whose ONLY visibility protection is the filter: each
# MUST be matched by pointer identity in qdwin_classify_global and routed to the
# EXACT non-ORDINARY kind mapped here — a branch that matched the pointer but
# returned QDWIN_GLOBAL_ORDINARY (or the wrong kind) would re-open it to silos,
# so the cross-check pins the exact return. (weston_capture_v1 is a fifth
# filter-gated global but it is created by libweston core, not by a
# qdwin->*_global = wl_global_create site, so it is pinned by
# check_privileged_globals_classified, not the inventory.)
FILTER_GATED_KIND = {
    "input_method_manager_global": "QDWIN_GLOBAL_INPUT_METHOD",
    "virtual_keyboard_manager_global": "QDWIN_GLOBAL_VIRTUAL_KEYBOARD",
    "security_context_manager_global": "QDWIN_GLOBAL_SECCTX_MANAGER",
    "idle_notifier_global": "QDWIN_GLOBAL_IDLE_NOTIFIER",
    # findings F0: the trusted shell interface is the ultimate cross-silo
    # escalation surface; hidden from secctx/silo clients by the filter
    # (cred != SECCTX), with bind_qdwin_shell applying allowed_uid + singleton
    # + secctx-deny on top.
    "shell_global": "QDWIN_GLOBAL_SHELL",
    # findings F4: layer-shell (overlays / lock surfaces) — used only by the
    # shell + locker; hidden from secctx clients so the bind gate is a second
    # layer, not the sole defense. Closes qdwin's own "Production TODO".
    "layer_shell_global": "QDWIN_GLOBAL_LAYER_SHELL",
}
FILTER_GATED_GLOBALS = set(FILTER_GATED_KIND)

# qdwin-created globals that are filter-VISIBLE by design. Any access control is
# enforced at BIND or request time by the global's own handler (noted below),
# NOT by the visibility filter — so leaving them ORDINARY is accepted/reviewed
# for the current S1 gate (some carry a "Production TODO" to hide them outright
# later). A global may move from here into FILTER_GATED_KIND, but it must never
# silently vanish from both: that is the fail-open this inventory closes.
INTENTIONALLY_VISIBLE_GLOBALS = {
    # identity-gated at bind or request time (covered by their own host/source
    # tests), so filter-visibility is acceptable:
    # NOTE: shell_global moved to FILTER_GATED_KIND (QDWIN_GLOBAL_SHELL) —
    # findings F0; it is now hidden from secctx clients by the filter.
    "output_mgmt_global",      # output-management: S13 om_mutation_allowed gate
    "locker_global",           # session locker: locker bind gate (host 07)
    "xdg_activation_global",   # xdg-activation: activation gating (host 09)
    "nested_manager_global",   # nested manager: nested-identity gate (host 16)
    # NOTE: layer_shell_global moved to FILTER_GATED_KIND
    # (QDWIN_GLOBAL_LAYER_SHELL) — findings F4; now hidden from secctx clients.
    "stream_input_global",     # stream-input: request-time claim gate (one-shot
                               # access_token + forward-child pid pin), not bind
    # unprivileged session protocols, visible to all session clients by design:
    "ext_ws_global",
    "idle_inhibit_manager_global",
    "cursor_shape_manager_global",
    "fractional_scale_manager_global",
    "primary_selection_manager_global",
    "text_input_manager_global",
    "xdg_decoration_manager_global",
}

# The exact (interface, bind-handler) each created global must advertise. The
# member name alone is not enough: a reviewed-VISIBLE slot (which classifies
# ORDINARY) could be repointed to a PRIVILEGED interface + bind handler (e.g.
# `qdwin->shell_global = wl_global_create(..., &wp_security_context_manager_v1_
# interface, 1, qdwin, bind_qdwin_secctx_manager)`), advertising the secctx
# manager to everyone while the classifier/policy stay canonical. Pinning the
# interface + bind handler for every create site closes that: each slot can only
# host its intended protocol, so no privileged interface can ride a visible slot
# and no gated member can be swapped to a benign one.
CANONICAL_CREATE_TUPLE = {
    "shell_global": ("qdwin_shell_v1_interface", "bind_qdwin_shell"),
    "ext_ws_global": ("ext_workspace_manager_v1_interface",
                      "bind_ext_workspace_manager"),
    "output_mgmt_global": ("zwlr_output_manager_v1_interface",
                           "bind_output_manager"),
    "locker_global": ("qdwin_locker_v1_interface", "bind_qdwin_locker"),
    "stream_input_global": ("qdwin_stream_input_v1_interface",
                            "bind_qdwin_stream_input"),
    "xdg_activation_global": ("xdg_activation_v1_interface",
                              "bind_xdg_activation"),
    "idle_notifier_global": ("ext_idle_notifier_v1_interface",
                             "bind_qdwin_idle_notifier"),
    "idle_inhibit_manager_global": ("zwp_idle_inhibit_manager_v1_interface",
                                    "bind_qdwin_idle_inhibit_manager"),
    "cursor_shape_manager_global": ("wp_cursor_shape_manager_v1_interface",
                                    "bind_qdwin_cursor_shape_manager"),
    "fractional_scale_manager_global": (
        "wp_fractional_scale_manager_v1_interface",
        "bind_qdwin_fractional_scale_manager"),
    "primary_selection_manager_global": (
        "zwp_primary_selection_device_manager_v1_interface",
        "bind_qdwin_primary_manager"),
    "text_input_manager_global": ("zwp_text_input_manager_v3_interface",
                                  "bind_qdwin_text_input_manager"),
    "input_method_manager_global": ("zwp_input_method_manager_v2_interface",
                                    "bind_qdwin_input_method_manager"),
    "virtual_keyboard_manager_global": (
        "zwp_virtual_keyboard_manager_v1_interface",
        "bind_qdwin_virtual_keyboard_manager"),
    "security_context_manager_global": (
        "wp_security_context_manager_v1_interface", "bind_qdwin_secctx_manager"),
    "nested_manager_global": ("qdwin_nested_manager_v1_interface",
                              "bind_qdwin_nested_manager"),
    "layer_shell_global": ("zwlr_layer_shell_v1_interface",
                           "bind_qdwin_layer_shell"),
    "xdg_decoration_manager_global": ("zxdg_decoration_manager_v1_interface",
                                      "bind_qdwin_xdg_decoration_manager"),
}


def _split_top_level_commas(args):
    """Split a call's argument text on commas that are not nested in (), so each
    `wl_global_create` argument is recovered as a whole."""
    parts, depth, cur = [], 0, ""
    for ch in args:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    return [p.strip() for p in parts]


def check_created_global_inventory(source):
    """Every `qdwin->*_global = wl_global_create(...)` site must be consciously
    categorized — filter-classified or reviewed-visible — so a newly-added
    privileged global cannot silently classify ORDINARY (visible to silos)."""
    # NOTE: `source` is already phase-2 line-spliced by main(), so every scan
    # below (comment stripping, the #define/#undef alias scan, create-site
    # enumeration, classifier-body validation) sees what the compiler reads.
    code = _strip_comments(source)
    created = set(re.findall(
        r"qdwin->(\w+_global)\s*=\s*wl_global_create\s*\(", code))
    if not created:
        return fail("no `qdwin->*_global = wl_global_create(...)` sites found — "
                    "the inventory regex is stale (qdwin.c restructured?)")

    # Evasion guard: EVERY wl_global_create(...) call in qdwin.c must be of the
    # recognized `qdwin->*_global = wl_global_create(...)` form. A create whose
    # result is stashed via a local/helper/macro (e.g. `g = wl_global_create();
    # qdwin->x = g;`) would be invisible to the inventory above, so a privileged
    # global could be created and escape categorization while the direct sites
    # keep this check satisfied. Counting all calls vs. direct assignments closes
    # that route. (Today all 18 calls are direct; the failed-log strings say
    # "wl_global_create failed" with no `(`, so they do not match.)
    all_calls = len(re.findall(r"\bwl_global_create\s*\(", code))
    direct_calls = len(re.findall(
        r"qdwin->\w+_global\s*=\s*wl_global_create\s*\(", code))
    if all_calls != direct_calls:
        return fail(
            f"{all_calls - direct_calls} wl_global_create(...) call(s) in qdwin.c "
            "are NOT of the recognized `qdwin->*_global = wl_global_create(...)` "
            "form (assigned via a local/helper/macro?) — the inventory cannot see "
            "them, so a privileged global could be created and escape "
            "categorization. Assign every created global directly to a "
            "qdwin->*_global member, or extend this inventory to cover the form.")

    # Indirect-creation guard: every `wl_global_create` TOKEN must be either a
    # direct call (`wl_global_create(`) or the failed-log word
    # (`wl_global_create failed`). A token in any other position is a non-call use
    # the count guard above cannot model — a function-pointer capture (`mk =
    # wl_global_create;` then `qdwin->x_global = mk(...)`) or a `#define MK
    # wl_global_create` synonym — which would create a global the inventory never
    # sees. Reject it.
    stray = re.findall(r"\bwl_global_create\b(?!\s*\()(?!\s+failed\b)", code)
    if stray:
        return fail(
            f"{len(stray)} `wl_global_create` token(s) in qdwin.c are neither a "
            "direct call nor the failed-log word — a function-pointer capture or "
            "macro synonym could create an uncategorized privileged global the "
            "inventory cannot see. Create globals only via a direct "
            "`qdwin->*_global = wl_global_create(...)` call.")

    # The pinned tuple tokens must not be macro-aliased — `#define
    # qdwin_shell_v1_interface wp_security_context_manager_v1_interface` would
    # keep the raw create site canonical while the compiled global advertises the
    # secctx manager on a visible slot.
    tuple_tokens = set()
    for iface, bind in CANONICAL_CREATE_TUPLE.values():
        tuple_tokens.add(iface)
        tuple_tokens.add(bind)
    rc = reject_macro_aliasing(source, tuple_tokens, "qdwin.c (global create tuples)")
    if rc:
        return rc

    # Pin each create site's advertised (interface, bind-handler) tuple, so a
    # reviewed-visible slot cannot host a privileged interface and a gated slot
    # cannot be swapped to a benign one (see CANONICAL_CREATE_TUPLE).
    for m in re.finditer(
            r"qdwin->(\w+_global)\s*=\s*wl_global_create\s*\(", code):
        member = m.group(1)
        paren = code.index("(", m.start())
        depth, close = 0, None
        for i in range(paren, len(code)):
            if code[i] == "(":
                depth += 1
            elif code[i] == ")":
                depth -= 1
                if depth == 0:
                    close = i
                    break
        if close is None:
            return fail(f"unbalanced wl_global_create(...) for {member}")
        parts = _split_top_level_commas(code[paren + 1:close])
        if len(parts) != 5:
            return fail(
                f"{member} create site has {len(parts)} args (expected 5: "
                f"display, &interface, version, data, bind) — `{code[paren + 1:close].strip()[:80]}`")
        iface = parts[1].lstrip("&").strip()
        bind = parts[4].strip()
        expected = CANONICAL_CREATE_TUPLE.get(member)
        if expected is None:
            return fail(
                f"{member} has no pinned (interface, bind) tuple — add it to "
                f"CANONICAL_CREATE_TUPLE so its advertised protocol is reviewed.")
        if (iface, bind) != expected:
            return fail(
                f"{member} create site advertises ({iface}, {bind}), expected "
                f"{expected} — a slot must host only its intended protocol; "
                f"hosting a privileged interface on a visible slot (or swapping a "
                f"gated slot) would route it to a wrong visibility class.")

    known = FILTER_GATED_GLOBALS | INTENTIONALLY_VISIBLE_GLOBALS

    # New, uncategorized globals: the fail-open this check exists to catch.
    uncategorized = created - known
    if uncategorized:
        return fail(
            "qdwin creates global(s) " + ", ".join(sorted(uncategorized)) +
            " that are NOT categorized: qdwin_classify_global returns ORDINARY "
            "for unmatched globals, so each would be visible to silo clients. "
            "Add it to qdwin_classify_global + FILTER_GATED_KIND if it is "
            "privileged, or to INTENTIONALLY_VISIBLE_GLOBALS (with a bind-gate "
            "or unprivileged-by-design rationale) if it is meant to be visible.")

    # Stale entries: a listed global no longer created → list has rotted.
    stale = known - created
    if stale:
        return fail(
            "inventory lists global(s) " + ", ".join(sorted(stale)) +
            " no longer created by a wl_global_create site in qdwin.c — remove "
            "the stale entries so the inventory stays truthful")

    # Preprocessor-alias guard. This test validates the RAW (unexpanded) text of
    # the classifier + filter, so a `#define`/`#undef` of ANY token they USE could
    # leave the raw body looking canonical while the COMPILED code diverges — be it
    # a struct/enum token (`#define input_method_manager_global shell_global`,
    # `#define QDWIN_GLOBAL_WESTON_CAPTURE QDWIN_GLOBAL_ORDINARY`) or a control-flow
    # keyword (`#define if(x) if (0 && (x))`, gnu11 expands macro-named `if`). The
    # preprocessor can only alter a function by redefining a token that appears in
    # it, so we reject aliasing of EXACTLY the identifiers used in those two bodies
    # — a complete in-file/TU closure, not a hand-maintained denylist.
    classify_body_for_ids, err = _function_body(
        source, r"static enum qdwin_global_kind\s+qdwin_classify_global\s*\(",
        "qdwin_classify_global")
    if err:
        return fail(err)
    filter_body_for_ids, err = _function_body(
        source, r"static bool\s+qdwin_secctx_global_filter\s*\(",
        "qdwin_secctx_global_filter")
    if err:
        return fail(err)
    rc = reject_shadowing_and_conditionals(
        source, r"static enum qdwin_global_kind\s+qdwin_classify_global\s*\(",
        classify_body_for_ids, "qdwin_classify_global", "qdwin.c")
    if rc:
        return rc
    rc = reject_shadowing_and_conditionals(
        source, r"static bool\s+qdwin_secctx_global_filter\s*\(",
        filter_body_for_ids, "qdwin_secctx_global_filter", "qdwin.c")
    if rc:
        return rc
    rc = reject_macro_aliasing(
        source,
        _identifiers(classify_body_for_ids) | _identifiers(filter_body_for_ids) |
        {"qdwin_classify_global", "qdwin_secctx_global_filter"},
        "qdwin.c (classifier/filter)")
    if rc:
        return rc

    # The filter-gated members must actually be classified to their EXACT kind
    # (not just listed, and not routed to ORDINARY/the wrong kind): cross-check
    # against the classify body so the two cannot drift. This is the only check
    # in this file pinning the IME/VK rows — check_privileged_globals_classified
    # only pins capture/secctx/idle.
    body, err = _function_body(
        source, r"static enum qdwin_global_kind\s+qdwin_classify_global\s*\(",
        "qdwin_classify_global")
    if err:
        return fail(err)
    classify = _norm(body)

    # No-macro/no-call guard: the test parses the RAW (pre-preprocessor) body, so
    # a macro invoked here could expand to an earlier shadowing branch invisible
    # in raw source. The canonical classifier is a pure pointer dispatch that
    # needs NO calls, so forbid any call/macro-like token: an identifier
    # immediately followed by `(` that is not the `if` keyword (`switch`/`for`/
    # `while`/helper()/MACRO() all trip this).
    calls = {ident for ident in re.findall(r"\b(\w+)\s*\(", classify)
             if ident != "if"}
    if calls:
        return fail(
            "qdwin_classify_global body contains call/macro-like token(s) " +
            ", ".join(sorted(calls)) + " — the classifier must be only canonical "
            "`if (<identity guard>) return QDWIN_GLOBAL_...;` branches plus the "
            "final ORDINARY return. A macro/helper could expand to an unparsed "
            "earlier branch routing a filter-gated pointer to ORDINARY. Inline "
            "it, or extend this test to model the construct faithfully.")

    # Whole-body ANCHORED-SEQUENCE grammar (authoritative backstop): the body must
    # be exactly a run of `if (<cond>) [{] return QDWIN_GLOBAL_...; [}]` branches
    # FOLLOWED BY exactly one final `return QDWIN_GLOBAL_ORDINARY;` — in that
    # order, with nothing else. We consume canonical-shaped branches from the
    # FRONT, then require the remainder to be precisely the single trailing
    # ORDINARY return. This rejects: an EARLY unconditional `return
    # QDWIN_GLOBAL_ORDINARY;` (or any other return) ahead of the branches that
    # makes every gated pointer fall through to ORDINARY (position matters — an
    # un-anchored residue check would wrongly accept it); a bare object-like macro
    # STATEMENT (`QDWIN_SHADOW_IME;`); and stray declarations/labels/assignments/
    # trailing junk. (Conditions contain no nested parens, so `[^()]*` consumes a
    # whole guard; a helper-call condition with inner parens stops the front-run
    # and is rejected here as well as by the no-call guard.)
    inner = classify.strip()
    if inner.startswith("{"):
        inner = inner[1:]
    if inner.endswith("}"):
        inner = inner[:-1]
    inner = inner.strip()
    front_branch = re.compile(
        r"^if\s*\(\s*[^()]*\)\s*\{?\s*return\s+QDWIN_GLOBAL_\w+\s*;\s*\}?\s*")
    rest = inner
    while True:
        m = front_branch.match(rest)
        if not m:
            break
        rest = rest[m.end():]
    if not re.fullmatch(r"return\s+QDWIN_GLOBAL_ORDINARY\s*;", rest.strip()):
        return fail(
            "qdwin_classify_global body is not the canonical sequence of "
            "`if (<identity guard>) return QDWIN_GLOBAL_...;` branches followed by "
            "EXACTLY one final `return QDWIN_GLOBAL_ORDINARY;`. Unexpected/early/"
            f"trailing content: `{rest.strip()[:120]}` — an early unconditional "
            "return, a stray statement, or a macro/declaration/label could route "
            "gated pointers to ORDINARY; inline it or extend this test.")

    # Full-CONDITION whitelist (the load-bearing semantic pin). Pinning only the
    # right-hand pointer is not enough: a branch like `if (0 && global == ptr)
    # return KIND;` has the exact pointer yet never fires at runtime, so the gated
    # global falls through to ORDINARY. We therefore require each branch's WHOLE
    # normalized condition to equal one of the exact canonical identity guards,
    # mapped to the exact kind it must return. This rejects a falsified guard
    # (`0 &&`), a wrong/extra conjunct (`shell_global && global == secctx...`), a
    # reversed compare (`ptr == global`), an object-macro-alias pointer, and a
    # wrong returned kind. (No-null-guard or any other intended form must be added
    # here explicitly and reviewed.)
    canonical_cond = {}
    for member, kind in FILTER_GATED_KIND.items():
        ptr = "qdwin->" + member
        canonical_cond[f"{ptr}&&global=={ptr}"] = kind
    cap = "qdwin->compositor->output_capture.weston_capture_v1"
    canonical_cond[f"qdwin->compositor&&{cap}&&global=={cap}"] = \
        "QDWIN_GLOBAL_WESTON_CAPTURE"

    full_branch_re = (r"if\s*\(\s*([^()]*?)\s*\)\s*\{?\s*"
                      r"return\s+(QDWIN_GLOBAL_\w+)\s*;")
    seen = set()
    for cond, kind in re.findall(full_branch_re, classify):
        cond_norm = re.sub(r"\s+", "", cond)
        if cond_norm not in canonical_cond:
            return fail(
                f"qdwin_classify_global has a branch with a non-canonical "
                f"condition `{cond_norm[:120]}` (returns {kind}) — only the exact "
                f"canonical identity guards are allowed. A falsified/extra/wrong "
                f"guard (e.g. `0 && ...`, `shell_global && global == secctx...`, "
                f"reversed `ptr == global`) could never fire or mis-route, leaking "
                f"a gated pointer to ORDINARY. Allowed: "
                f"{', '.join(sorted(canonical_cond))}")
        if kind != canonical_cond[cond_norm]:
            return fail(
                f"qdwin_classify_global branch `{cond_norm}` returns {kind}, "
                f"expected {canonical_cond[cond_norm]} — a wrong/ORDINARY kind "
                f"would leave it visible to silo clients")
        seen.add(cond_norm)
    missing = set(canonical_cond) - seen
    if missing:
        return fail(
            "qdwin_classify_global is missing canonical branch(es): " +
            ", ".join(sorted(missing)) + " — the gated global would classify "
            "ORDINARY (visible to silos)")
    return 0


def main():
    if len(sys.argv) != 3:
        return fail("usage: test_global_filter.py <qdwin.c> <qdwin-logic.c>")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    logic_source = Path(sys.argv[2]).read_text(encoding="utf-8")

    # Model C translation ONCE, up front, so EVERY check (incl. the raw
    # `_function_body` body extractors) reasons over the SAME text the compiler
    # reads — never a mix of raw and comment-stripped views, which would let a
    # block-COMMENTED canonical body spoof the grammar while a later real bad
    # definition compiles. Phase 2: delete backslash-newline line splices (so a
    # spliced `#def\<newline>ine X` is seen as `#define`). Phase 3: strip comments
    # (done after splicing, matching the standard's order — a `//` comment can
    # extend through a spliced line).
    source = _strip_comments(re.sub(r"\\\r?\n", "", source))
    logic_source = _strip_comments(re.sub(r"\\\r?\n", "", logic_source))

    for check in (check_filter_is_installed,
                  check_privileged_globals_classified,
                  check_filter_consults_policy,
                  check_created_global_inventory):
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
          "and hidden from secctx/silo clients via qdwin_global_visible; "
          "every wl_global_create site is a direct qdwin->*_global assignment "
          "and categorized filter-gated [exact kind] or reviewed-visible, so a "
          "new uncategorized qdwin-created global fails closed here)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
