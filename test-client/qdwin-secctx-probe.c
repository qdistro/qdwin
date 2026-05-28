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
 *   (default)   bind the manager, report whether the bind was accepted
 *               or refused.
 *   --commit    on a successful bind, also create_listener + set the
 *               secctx strings + commit, and report whether that
 *               round-trips cleanly. This is the "can commit a listener"
 *               half of the open item.
 *
 * Exit codes:
 *   0  bind accepted   (and, with --commit, the listener committed cleanly)
 *   1  bind refused     (server posted a protocol/implementation error)
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

static int
denied_or_ok(struct probe *p, const char *what)
{
	int rc = wl_display_roundtrip(p->display);
	int err = wl_display_get_error(p->display);
	if (rc < 0 || err != 0) {
		uint32_t obj_id = 0, code = 0;
		const struct wl_interface *iface = NULL;
		code = wl_display_get_protocol_error(p->display,
						     &iface, &obj_id);
		fprintf(stderr,
			"qdwin-secctx-probe: %s REFUSED (errno=%d, proto "
			"code=%u on %s#%u)\n",
			what, err, code,
			iface ? iface->name : "(unknown)", obj_id);
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int commit_mode = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--commit") == 0)
			commit_mode = 1;
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

	/* Second roundtrip resolves the bind: an accepted bind is silent;
	 * a refused bind surfaces as a protocol/implementation error. */
	if (denied_or_ok(&p, "manager bind") != 0)
		return 1;

	if (!commit_mode) {
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
		return 2;
	}
	wp_security_context_v1_set_sandbox_engine(ctx, "qdistro");
	wp_security_context_v1_set_app_id(ctx, "org.qdistro.test.secctx-probe");
	wp_security_context_v1_set_instance_id(ctx, "secctx-probe-1");
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

	int rc = denied_or_ok(&p, "listener commit");
	close(close_fds[1]);   /* signal teardown only after the assertion */
	if (rc != 0)
		return 1;

	printf("qdwin-secctx-probe: manager bind ACCEPTED, listener "
	       "committed\n");
	return 0;
}
