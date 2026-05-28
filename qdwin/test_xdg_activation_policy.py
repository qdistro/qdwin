#!/usr/bin/env python3
"""Regression checks for qdwin xdg-activation gate policy."""

from pathlib import Path
import re
import sys


def fail(message):
    print(f"FAIL: {message}")
    return 1


def main():
    if len(sys.argv) != 2:
        return fail("usage: test_xdg_activation_policy.py <qdwin.c>")

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    activate = re.search(
        r"static void\s+qdwin_activation_activate\s*\([^{}]*\)\s*\{",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not activate:
        return fail("qdwin_activation_activate not found")

    start = activate.end()
    next_function = source.find("\nstatic ", start)
    body = source[start: next_function if next_function != -1 else len(source)]

    alloc = body.find("calloc(1, sizeof *ap)")
    if alloc == -1:
        return fail("pending activation allocation not found")

    fail_branch = body.find("if (!ap)", alloc)
    if fail_branch == -1:
        return fail("allocation-failure branch not found")

    return_stmt = body.find("return;", fail_branch)
    if return_stmt == -1:
        return fail("allocation-failure branch does not return")

    branch = body[fail_branch:return_stmt]
    if "qdwin_activation_token_free(t);" not in branch:
        return fail("allocation-failure branch does not consume/free token")
    if "qdwin_activation_perform" in branch:
        return fail("allocation-failure branch performs activation")

    print("PASS: xdg-activation pending allocation failure fails closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
