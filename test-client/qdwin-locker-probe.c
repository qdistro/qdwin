/*
 * qdwin-locker-probe — test client for the qdwin_locker_v1 bind gate
 * (bind_qdwin_locker + qdwin_handle_bind_as_locker, qdwin/qdwin.c).
 *
 * Trust model (qdwin/qdwin-locker-v1.xml): qdwin advertises the
 * qdwin_locker_v1 global to every client, but the *bind* handler rejects
 * any client whose peer uid != qdwin->allowed_locker_uid (which, absent an
 * explicit --qdwin-allowed-locker-uid, falls back to qdwin->allowed_uid).
 * The reject is posted as a wl_client implementation error at GLOBAL BIND
 * time — before the client can issue any request on the resource. The
 * authorized client then calls bind_as_locker and gets the `ready` event.
 *
 * Log note: the uid reject is delivered to the CLIENT as a wl_client
 * implementation error ("uid N not permitted") — there is NO dedicated
 * server-log "REJECTED" line for the locker uid branch (unlike secctx).
 * The companion scenario therefore asserts on this probe's exit code plus
 * the "locker bind attempt … uid=… allowed_locker_uid=…" line and the
 * absence of a "locker bound" line.
 *
 * Headless limitation (same as 06-secctx): a single-uid host cannot run a
 * client under a second real uid, so the "unauthorized" case is driven by
 * starting qdwin with a FOREIGN allowed_uid (tests/host/start.sh
 * --allowed-uid) so the probe's real uid mismatches allowed_locker_uid and
 * the reject branch fires. This faithfully exercises the uid reject branch.
 * The optional exe/SELinux peer checks (df8f3d5) and the from_shell
 * interaction are NOT exercised here (they need a configured policy and/or
 * a second real uid — see the scenario's follow-up note).
 *
 * Modes (mutually exclusive):
 *   (default)       Bind the global, then bind_as_locker. Report whether
 *                   the sequence was accepted (got `ready`) or refused
 *                   (server posted a protocol/implementation error).
 *   --double-bind   Bind the global once; call bind_as_locker TWICE on the
 *                   same resource. Per the XML the second call is rejected
 *                   with the dedicated already_bound (=1) error. Asserts
 *                   the FIRST succeeded (ready) and the SECOND raised
 *                   exactly that error.
 *   --rebind        Bind the global TWICE (two resources on one client);
 *                   call bind_as_locker on each. The first becomes the
 *                   live locker. Because this probe's process (the locker
 *                   peer) is still ALIVE, qdwin REFUSES the second
 *                   bind_as_locker with the dedicated `locker_present`
 *                   (=5) error rather than evicting the live locker —
 *                   the FINDING #5 takeover-rejection fix. Asserts the
 *                   first bind_as_locker got `ready` and the second
 *                   raised exactly locker_present on qdwin_locker_v1.
 *
 * Exit codes:
 *   0  expected-accept path completed cleanly
 *   4  unauthorized default run: the bind was REFUSED with the expected
 *      implementation error on wl_display (the PASS signal for that mode)
 *   1  the expected accept failed, OR the bind failed with an UNEXPECTED
 *      error (wrong interface/code) — always a FAILURE
 *   3  --double-bind: second bind_as_locker raised the expected
 *      already_bound error on qdwin_locker_v1 (the PASS signal for that mode)
 *   5  --rebind: second bind_as_locker (live-peer takeover) raised the
 *      expected locker_present error (the PASS signal for that mode)
 *   2  setup/other error (no display, global not advertised, ...)
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <wayland-client.h>
#include "qdwin-locker-v1-client-protocol.h"

struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	uint32_t locker_name;
	uint32_t locker_version;
	int saw_global;
	int got_ready;
};

static void
on_ready(void *data, struct qdwin_locker_v1 *locker, uint32_t initially_locked)
{
	struct probe *p = data;
	(void)locker; (void)initially_locked;
	p->got_ready = 1;
}

static void on_locked_changed(void *d, struct qdwin_locker_v1 *l, uint32_t v)
{ (void)d; (void)l; (void)v; }
static void on_lock_requested(void *d, struct qdwin_locker_v1 *l, uint32_t v)
{ (void)d; (void)l; (void)v; }
static void on_overlay_key(void *d, struct qdwin_locker_v1 *l, uint32_t sym,
			   const char *utf8)
{ (void)d; (void)l; (void)sym; (void)utf8; }

static const struct qdwin_locker_v1_listener locker_listener = {
	.ready = on_ready,
	.locked_changed = on_locked_changed,
	.lock_requested = on_lock_requested,
	.overlay_key = on_overlay_key,
};

static void
on_global(void *data, struct wl_registry *reg, uint32_t name,
	  const char *interface, uint32_t version)
{
	struct probe *p = data;
	(void)reg;
	if (strcmp(interface, qdwin_locker_v1_interface.name) == 0) {
		p->saw_global = 1;
		p->locker_name = name;
		p->locker_version = version < 1 ? version : 1;
	}
}

static void
on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }

static const struct wl_registry_listener registry_listener = {
	.global = on_global,
	.global_remove = on_global_remove,
};

/* Roundtrip and report whether the server posted a fatal protocol error.
 * Returns 0 = clean, the wayland error code (>0) otherwise. On error,
 * *out_code receives the protocol error code and *out_iface the interface
 * the error was raised on (so callers can verify it is the SPECIFIC error
 * they expect, not an unrelated one that happens to share a code). */
static int
roundtrip_err(struct probe *p, const char *what, uint32_t *out_code,
	      const struct wl_interface **out_iface)
{
	int rc = wl_display_roundtrip(p->display);
	int err = wl_display_get_error(p->display);
	if (out_code)
		*out_code = 0;
	if (out_iface)
		*out_iface = NULL;
	if (rc < 0 || err != 0) {
		uint32_t obj_id = 0, code = 0;
		const struct wl_interface *iface = NULL;
		code = wl_display_get_protocol_error(p->display, &iface, &obj_id);
		if (out_code)
			*out_code = code;
		if (out_iface)
			*out_iface = iface;
		fprintf(stderr,
			"qdwin-locker-probe: %s ERROR (errno=%d, proto code=%u "
			"on %s#%u)\n",
			what, err, code,
			iface ? iface->name : "(unknown)", obj_id);
		return err ? err : 1;
	}
	return 0;
}

static struct qdwin_locker_v1 *
bind_locker(struct probe *p)
{
	struct qdwin_locker_v1 *l = wl_registry_bind(
		p->registry, p->locker_name,
		&qdwin_locker_v1_interface, p->locker_version);
	if (l)
		qdwin_locker_v1_add_listener(l, &locker_listener, p);
	return l;
}

int main(int argc, char *argv[])
{
	enum { M_DEFAULT, M_DOUBLE, M_REBIND } mode = M_DEFAULT;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--double-bind") == 0)
			mode = M_DOUBLE;
		else if (strcmp(argv[i], "--rebind") == 0)
			mode = M_REBIND;
	}

	struct probe p = {0};
	p.display = wl_display_connect(NULL);
	if (!p.display) {
		fprintf(stderr, "qdwin-locker-probe: wl_display_connect "
			"failed: %s\n", strerror(errno));
		return 2;
	}
	p.registry = wl_display_get_registry(p.display);
	wl_registry_add_listener(p.registry, &registry_listener, &p);
	wl_display_roundtrip(p.display);

	if (!p.saw_global) {
		/* The locker global is always advertised (gating is at bind
		 * time, not via the registry filter). Not seeing it is a
		 * harness/server problem, not a gate decision. */
		fprintf(stderr, "qdwin-locker-probe: qdwin_locker_v1 not "
			"advertised\n");
		return 2;
	}

	struct qdwin_locker_v1 *l = bind_locker(&p);
	if (!l) {
		fprintf(stderr, "qdwin-locker-probe: bind returned NULL\n");
		return 2;
	}

	/* Resolve the bind itself: an unauthorized uid is rejected here, at
	 * global-bind time, via wl_client_post_implementation_error — which
	 * surfaces as WL_DISPLAY_ERROR_IMPLEMENTATION (=3) on the wl_display.
	 * Verify it is SPECIFICALLY that error, not an unrelated disconnect,
	 * so the uid-reject case can't false-pass on a crash. */
	{
		uint32_t code = 0;
		const struct wl_interface *iface = NULL;
		if (roundtrip_err(&p, "locker global bind", &code, &iface) != 0) {
			if (iface == &wl_display_interface &&
			    code == WL_DISPLAY_ERROR_IMPLEMENTATION) {
				/* The EXPECTED uid reject. Distinct exit code 4
				 * so a scenario can require this specific error
				 * and not false-pass on an unrelated failure
				 * (which exits 1 below). */
				printf("qdwin-locker-probe: locker bind REFUSED "
				       "with implementation error\n");
				return 4;
			}
			fprintf(stderr, "qdwin-locker-probe: bind failed but not "
				"with the expected implementation error "
				"(code=%u iface=%s)\n", code,
				iface ? iface->name : "(none)");
			return 1;
		}
	}

	if (mode == M_REBIND) {
		/* Second resource on the SAME client. */
		struct qdwin_locker_v1 *l2 = bind_locker(&p);
		if (!l2) {
			fprintf(stderr, "qdwin-locker-probe: 2nd bind NULL\n");
			return 2;
		}
		if (roundtrip_err(&p, "locker 2nd global bind", NULL, NULL) != 0)
			return 1;
		qdwin_locker_v1_bind_as_locker(l);
		if (roundtrip_err(&p, "first bind_as_locker", NULL, NULL) != 0)
			return 1;
		if (!p.got_ready) {
			fprintf(stderr, "qdwin-locker-probe: first bind_as_locker "
				"accepted but no `ready`\n");
			return 1;
		}
		/* FINDING #5 fix: the first locker (this very process) is still
		 * alive, so qdwin must REFUSE the takeover with locker_present
		 * instead of evicting the live locker. */
		uint32_t code = 0;
		const struct wl_interface *iface = NULL;
		qdwin_locker_v1_bind_as_locker(l2);
		int err = roundtrip_err(&p, "second bind_as_locker (rebind)",
					&code, &iface);
		if (err == 0) {
			fprintf(stderr, "qdwin-locker-probe: second bind_as_locker "
				"was NOT rejected — live locker was evicted "
				"(takeover vuln)\n");
			return 1;
		}
		if (iface != &qdwin_locker_v1_interface ||
		    code != QDWIN_LOCKER_V1_ERROR_LOCKER_PRESENT) {
			fprintf(stderr, "qdwin-locker-probe: rebind got code=%u on "
				"%s, want locker_present=%d on qdwin_locker_v1\n",
				code, iface ? iface->name : "(none)",
				QDWIN_LOCKER_V1_ERROR_LOCKER_PRESENT);
			return 1;
		}
		printf("qdwin-locker-probe: rebind REFUSED with locker_present "
		       "(live locker not evicted)\n");
		return 5;
	}

	/* Default + double-bind both do a first bind_as_locker. */
	qdwin_locker_v1_bind_as_locker(l);
	if (roundtrip_err(&p, "bind_as_locker", NULL, NULL) != 0)
		return 1;
	if (!p.got_ready) {
		fprintf(stderr, "qdwin-locker-probe: bind_as_locker accepted "
			"but no `ready` event\n");
		return 1;
	}

	if (mode == M_DOUBLE) {
		uint32_t code = 0;
		const struct wl_interface *iface = NULL;
		qdwin_locker_v1_bind_as_locker(l);  /* second on same resource */
		int err = roundtrip_err(&p, "double bind_as_locker", &code,
					&iface);
		if (err == 0) {
			fprintf(stderr, "qdwin-locker-probe: second "
				"bind_as_locker was NOT rejected\n");
			return 1;
		}
		/* Verify it is the already_bound error ON qdwin_locker_v1 — not
		 * an unrelated protocol error that happens to share code 1. */
		if (iface != &qdwin_locker_v1_interface ||
		    code != QDWIN_LOCKER_V1_ERROR_ALREADY_BOUND) {
			fprintf(stderr, "qdwin-locker-probe: double bind got "
				"code=%u on %s, want already_bound=%d on "
				"qdwin_locker_v1\n", code,
				iface ? iface->name : "(none)",
				QDWIN_LOCKER_V1_ERROR_ALREADY_BOUND);
			return 1;
		}
		printf("qdwin-locker-probe: double bind_as_locker REJECTED "
		       "with already_bound\n");
		return 3;
	}

	printf("qdwin-locker-probe: locker bind ACCEPTED (ready)\n");
	return 0;
}
