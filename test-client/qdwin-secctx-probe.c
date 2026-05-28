/*
 * qdwin-secctx-probe — test client for the wp_security_context_manager_v1
 * bind gate (bind_qdwin_secctx_manager, qdwin.c). Verifies the Option-A
 * launcher-gate from todo/decisions/secctx-identity-contract.md:
 *
 *   The compositor must REFUSE a manager bind from a client that is
 *   neither the bound qdshell client nor running as qdwin's allowed_uid,
 *   and must ACCEPT it (allowing a listener commit) from an authorized
 *   client.
 *
 * The gate is keyed on (!from_shell && uid != allowed_uid). A headless
 * host test cannot run two real uids, so the harness drives the
 * "unauthorized" case by starting qdwin with a FOREIGN QDWIN_ALLOWED_UID
 * (and --no-shell, so from_shell can't be true either); the probe's real
 * uid then mismatches allowed_uid and the reject branch fires. The
 * "authorized" case uses the normal allowed_uid (== this uid).
 *
 * NOTE: this cannot isolate the from_shell branch from the uid branch
 * headlessly — proving "shell client allowed despite wrong uid" needs the
 * shell under a different real uid (root / multi-uid VM). See the
 * companion scenario tests/host/06-secctx-bind-gate.md.
 *
 * Modes:
 *   (default)         bind the manager, report whether the bind was
 *                     accepted or refused.
 *   --commit          on a successful bind, also create_listener + set the
 *                     secctx strings + commit, and report whether that
 *                     round-trips cleanly. This is the "can commit a
 *                     listener" half of the bind-gate item.
 *
 * Identity-edge modes (08-secctx-identity-edges.md) — each binds the
 * manager, create_listener's, then exercises one commit-sequence edge and
 * asserts qdwin's ACTUAL behavior (read from the qdwin_secctx setters and
 * qdwin_secctx_commit):
 *   --commit-no-appid commit with engine + instance_id but NO app_id. A
 *                     valid sequence: qdwin has no required-field check and
 *                     accepts it (and logs app_id=?). Asserts clean commit.
 *   --commit-bare     commit with NO setters at all (engine/app_id/
 *                     instance_id all unset). Also accepted; qdwin logs all
 *                     three as "?". Asserts clean commit.
 *   --commit-engine-only  set sandbox_engine only, then commit. Accepted.
 *   --double-commit   commit, then commit again on the same context. The
 *                     context is single-use: qdwin posts the protocol error
 *                     already_used (=1) on the second commit. Asserts that.
 *   --set-after-commit commit, then set_app_id. qdwin posts already_used
 *                     (=1) on the post-commit setter. Asserts that.
 *
 * Exit codes:
 *   0  expected-accept path completed cleanly (bind / commit round-tripped)
 *   4  default bind run: the manager bind was REFUSED with the expected
 *      implementation error on wl_display (the PASS signal for the
 *      unauthorized case in 06-secctx-bind-gate.md)
 *   1  the expected accept failed, OR the bind failed with an UNEXPECTED
 *      error (wrong interface/code) — always a FAILURE
 *   3  --double-commit / --set-after-commit: the second op was rejected
 *      with the expected already_used error on wp_security_context_v1
 *      (the PASS signal)
 *   2  setup/other error (no display, manager global not advertised, ...)
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <wayland-client.h>
#include "security-context-v1-client-protocol.h"

struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wp_security_context_manager_v1 *mgr;
	int saw_manager;
};

static void
on_global(void *data, struct wl_registry *reg, uint32_t name,
	  const char *interface, uint32_t version)
{
	struct probe *p = data;
	if (strcmp(interface,
		   wp_security_context_manager_v1_interface.name) == 0) {
		p->saw_manager = 1;
		/* Bind at the version qdwin advertises (cap to the one we
		 * generated against). The bind itself is what the gate
		 * accepts or refuses. */
		p->mgr = wl_registry_bind(
			reg, name,
			&wp_security_context_manager_v1_interface,
			version < 1 ? version : 1);
	}
}

static void
on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }

static const struct wl_registry_listener registry_listener = {
	.global = on_global,
	.global_remove = on_global_remove,
};

/* A real listening AF_UNIX socket for create_listener's listen_fd.
 * The compositor integrates it into its event loop on commit; a
 * non-listening fd could be rejected, so we listen() for real. Linux
 * autobind (empty sun_path) gives an abstract address with no cleanup. */
static int
make_listen_fd(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	/* sun_path left zeroed → autobind (abstract, kernel-assigned). */
	if (bind(fd, (struct sockaddr *)&addr,
		 sizeof(sa_family_t)) < 0) {
		close(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Roundtrip; if the server posted a fatal error, log it and return 1,
 * storing the wayland protocol-error code in *out_code and the errored
 * interface in *out_iface (both 0/NULL when the caller passes NULL or
 * there was no protocol code). Returns 0 on a clean roundtrip. */
static int
denied_or_ok_code(struct probe *p, const char *what, uint32_t *out_code,
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
		code = wl_display_get_protocol_error(p->display,
						     &iface, &obj_id);
		if (out_code)
			*out_code = code;
		if (out_iface)
			*out_iface = iface;
		fprintf(stderr,
			"qdwin-secctx-probe: %s REFUSED (errno=%d, proto "
			"code=%u on %s#%u)\n",
			what, err, code,
			iface ? iface->name : "(unknown)", obj_id);
		return 1;
	}
	return 0;
}

enum secctx_mode {
	M_BIND,            /* default: bind only */
	M_COMMIT,          /* full set + commit */
	M_COMMIT_NO_APPID, /* engine + instance_id, no app_id */
	M_COMMIT_BARE,     /* no setters */
	M_COMMIT_ENGINE,   /* engine only */
	M_DOUBLE_COMMIT,   /* commit twice */
	M_SET_AFTER_COMMIT /* commit then set_app_id */
};

int main(int argc, char *argv[])
{
	enum secctx_mode mode = M_BIND;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--commit") == 0)
			mode = M_COMMIT;
		else if (strcmp(argv[i], "--commit-no-appid") == 0)
			mode = M_COMMIT_NO_APPID;
		else if (strcmp(argv[i], "--commit-bare") == 0)
			mode = M_COMMIT_BARE;
		else if (strcmp(argv[i], "--commit-engine-only") == 0)
			mode = M_COMMIT_ENGINE;
		else if (strcmp(argv[i], "--double-commit") == 0)
			mode = M_DOUBLE_COMMIT;
		else if (strcmp(argv[i], "--set-after-commit") == 0)
			mode = M_SET_AFTER_COMMIT;
	}

	struct probe p = {0};
	p.display = wl_display_connect(NULL);
	if (!p.display) {
		fprintf(stderr, "qdwin-secctx-probe: wl_display_connect "
			"failed: %s\n", strerror(errno));
		return 2;
	}

	p.registry = wl_display_get_registry(p.display);
	wl_registry_add_listener(p.registry, &registry_listener, &p);

	/* First roundtrip: discover globals; the manager bind is issued
	 * from on_global during this dispatch. */
	wl_display_roundtrip(p.display);

	if (!p.saw_manager) {
		/* The global filter only hides the manager from clients that
		 * are ALREADY secctx-tagged; an untagged probe must always
		 * see it. Not seeing it is a harness/server problem. */
		fprintf(stderr, "qdwin-secctx-probe: "
			"wp_security_context_manager_v1 not advertised\n");
		return 2;
	}

	/* Second roundtrip resolves the bind: an accepted bind is silent; a
	 * refused bind surfaces via wl_client_post_implementation_error,
	 * i.e. WL_DISPLAY_ERROR_IMPLEMENTATION (=3) on the wl_display. Verify
	 * it is SPECIFICALLY that error so the reject case can't false-pass
	 * on an unrelated protocol error or a crash-disconnect. */
	{
		uint32_t bcode = 0;
		const struct wl_interface *biface = NULL;
		if (denied_or_ok_code(&p, "manager bind", &bcode, &biface) != 0) {
			if (biface == &wl_display_interface &&
			    bcode == WL_DISPLAY_ERROR_IMPLEMENTATION) {
				/* The EXPECTED gate reject. Distinct exit code 4
				 * so a scenario can require this specific error
				 * and not false-pass on an unrelated failure
				 * (which exits 1 below). */
				printf("qdwin-secctx-probe: manager bind "
				       "REFUSED with implementation error\n");
				return 4;
			}
			fprintf(stderr, "qdwin-secctx-probe: manager bind failed "
				"but not with the expected implementation error "
				"(code=%u iface=%s)\n", bcode,
				biface ? biface->name : "(none)");
			return 1;
		}
	}

	if (mode == M_BIND) {
		printf("qdwin-secctx-probe: manager bind ACCEPTED\n");
		return 0;
	}

	int listen_fd = make_listen_fd();
	int close_fds[2];
	if (listen_fd < 0 || pipe(close_fds) < 0) {
		fprintf(stderr, "qdwin-secctx-probe: fd setup failed: %s\n",
			strerror(errno));
		if (listen_fd >= 0)
			close(listen_fd);
		return 2;
	}

	struct wp_security_context_v1 *ctx =
		wp_security_context_manager_v1_create_listener(
			p.mgr, listen_fd, close_fds[0]);
	if (!ctx) {
		fprintf(stderr, "qdwin-secctx-probe: create_listener "
			"returned NULL\n");
		close(listen_fd);
		close(close_fds[0]);
		close(close_fds[1]);
		return 2;
	}

	/* Per-mode metadata setters. qdwin has NO required-field check, so
	 * the "missing"/"bare" variants are still valid commit sequences. */
	switch (mode) {
	case M_COMMIT:
		wp_security_context_v1_set_sandbox_engine(ctx, "qdistro");
		wp_security_context_v1_set_app_id(ctx,
			"org.qdistro.test.secctx-probe");
		wp_security_context_v1_set_instance_id(ctx, "secctx-probe-1");
		break;
	case M_COMMIT_NO_APPID:
		wp_security_context_v1_set_sandbox_engine(ctx, "qdistro");
		wp_security_context_v1_set_instance_id(ctx, "secctx-probe-1");
		break;
	case M_COMMIT_ENGINE:
		wp_security_context_v1_set_sandbox_engine(ctx, "qdistro");
		break;
	case M_DOUBLE_COMMIT:
	case M_SET_AFTER_COMMIT:
		/* A normal complete set, so the FIRST commit is unambiguously
		 * valid and the reuse error is the only thing under test. */
		wp_security_context_v1_set_sandbox_engine(ctx, "qdistro");
		wp_security_context_v1_set_app_id(ctx,
			"org.qdistro.test.secctx-probe");
		wp_security_context_v1_set_instance_id(ctx, "secctx-probe-1");
		break;
	case M_COMMIT_BARE:
	default:
		break;  /* no setters */
	}

	wp_security_context_v1_commit(ctx);
	/* Push the commit + its fd transfers to the server BEFORE dropping
	 * any local fd copies, so closing them can't race the in-flight
	 * request. */
	wl_display_flush(p.display);

	/* listen_fd and the close-pipe read end (close_fds[0]) were dup'd
	 * into the server by the commit; drop our local copies. Keep the
	 * close-pipe WRITE end (close_fds[1]) open across the assertion: the
	 * compositor tears the context down on HUP of the read end, which
	 * fires once all write ends close — closing it now would let the
	 * listener be reaped mid-check. */
	close(listen_fd);
	close(close_fds[0]);

	uint32_t code = 0;
	const struct wl_interface *iface = NULL;
	int rc = denied_or_ok_code(&p, "listener commit", &code, &iface);

	/* Reuse edges: issue the second op only after the first commit has
	 * round-tripped cleanly, so a reuse error is unambiguous. */
	if (rc == 0 && (mode == M_DOUBLE_COMMIT ||
			mode == M_SET_AFTER_COMMIT)) {
		if (mode == M_DOUBLE_COMMIT)
			wp_security_context_v1_commit(ctx);
		else
			wp_security_context_v1_set_app_id(ctx,
				"org.qdistro.test.secctx-probe-2");
		rc = denied_or_ok_code(&p, "context reuse", &code, &iface);
		close(close_fds[1]);
		if (rc == 0) {
			fprintf(stderr, "qdwin-secctx-probe: reuse was NOT "
				"rejected\n");
			return 1;
		}
		/* Verify it is already_used ON wp_security_context_v1 — not an
		 * unrelated protocol error that happens to share code 1. */
		if (iface != &wp_security_context_v1_interface ||
		    code != WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED) {
			fprintf(stderr, "qdwin-secctx-probe: reuse got code=%u "
				"on %s, want already_used=%d on "
				"wp_security_context_v1\n", code,
				iface ? iface->name : "(none)",
				WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED);
			return 1;
		}
		printf("qdwin-secctx-probe: context reuse REJECTED with "
		       "already_used\n");
		return 3;
	}

	close(close_fds[1]);   /* signal teardown only after the assertion */
	if (rc != 0)
		return 1;

	printf("qdwin-secctx-probe: manager bind ACCEPTED, listener "
	       "committed\n");
	return 0;
}
