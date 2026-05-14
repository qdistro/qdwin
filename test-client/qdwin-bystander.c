/*
 * qdwin-bystander — long-lived stand-in shell for qdwin app testing.
 *
 * Binds qdwin_shell_v1 at v14 (set_keyboard_focus available), calls
 * bind_as_shell, and on every toplevel_added event:
 *   - calls set_border_color (releases the held-layer hold so pixels
 *     paint),
 *   - calls set_keyboard_focus on the default seat so the app
 *     receives keyboard input.
 *
 * CLI flags (optional):
 *   --subscribe <handle>   send subscribe_view_stream for HANDLE once
 *                          the bind roundtrip completes (or once a
 *                          toplevel with that handle is replayed/added).
 *   --subscribe last       subscribe to the first toplevel_added we
 *                          see after start (handy when the test spawns
 *                          the target after the bystander).
 *   --peer-label <s>       peer_label arg for the subscribe; default
 *                          "qdwin-bystander".
 *
 * On `approved`, the credentials are printed to stdout as KEY=value
 * lines suitable for shell sourcing:
 *   HANDLE=<n>
 *   PIPEWIRE_NODE_NAME=<s>
 *   RDP_PORT=<n>
 *   RDP_CERT_PATH=<s>
 *   RDP_PASSWORD=<s>
 * The wayland connection is kept open so the stream stays live.
 *
 * Reads commands on a FIFO (default /tmp/qdwin-cmd.fifo), one per line:
 *   max <handle>      → request_maximize(handle, 1)
 *   restore <handle>  → request_maximize(handle, 0)
 *   min <handle>      → request_minimize(handle)
 *   close <handle>    → request_close(handle)
 *   focus <handle>    → set_keyboard_focus("default", handle)
 *   subscribe <handle> → subscribe_view_stream(handle)
 *   subscribelast     → subscribe to the most recently added toplevel
 *   list              → print last seen toplevels to stderr
 *   maxlast / restorelast → operate on most recently added toplevel
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>
#include <errno.h>

#include <wayland-client.h>
#include "qdwin-shell-v1-client-protocol.h"

#define BIND_VERSION 14
#define MAX_TOPS 16
#define MAX_STREAMS 4
#define FIFO_PATH_DEFAULT "/tmp/qdwin-cmd.fifo"

struct top_info {
	uint32_t handle;
	int active;
};

struct stream_info {
	uint32_t handle;
	struct qdwin_view_stream_v1 *stream;
	struct app *app;
};

struct pending_subscribe {
	int armed;
	int wait_first;        /* 1 = subscribe to the next toplevel_added */
	uint32_t handle;
};

struct app {
	struct qdwin_shell_v1 *shell;
	struct top_info tops[MAX_TOPS];
	uint32_t last_handle;
	int got_last;
	struct stream_info streams[MAX_STREAMS];
	struct pending_subscribe pending;
	char peer_label[64];
};

static struct app g_app;

static void do_subscribe(struct app *a, uint32_t handle);

static void
on_hello(void *d, struct qdwin_shell_v1 *s, uint32_t uid)
{
	(void)d; (void)s;
	fprintf(stderr, "qdwin-bystander: hello uid=%u\n", uid);
}

static void
record_toplevel(struct app *a, uint32_t handle)
{
	for (int i = 0; i < MAX_TOPS; i++) {
		if (!a->tops[i].active) {
			a->tops[i].handle = handle;
			a->tops[i].active = 1;
			break;
		}
	}
	a->last_handle = handle;
	a->got_last = 1;
}

static void
forget_toplevel(struct app *a, uint32_t handle)
{
	for (int i = 0; i < MAX_TOPS; i++) {
		if (a->tops[i].active && a->tops[i].handle == handle)
			a->tops[i].active = 0;
	}
	if (a->got_last && a->last_handle == handle)
		a->got_last = 0;
}

static void
on_toplevel_added(void *d, struct qdwin_shell_v1 *s,
		  uint32_t handle, uint32_t owner_uid,
		  const char *app_id, const char *title, uint32_t is_xwayland)
{
	struct app *a = d;
	(void)owner_uid;
	fprintf(stderr,
		"qdwin-bystander: toplevel_added handle=%u app_id=\"%s\" title=\"%s\" xwayland=%u\n",
		handle, app_id ? app_id : "", title ? title : "", is_xwayland);
	qdwin_shell_v1_set_border_color(s, handle, 0x0088aaffu);
	qdwin_shell_v1_set_keyboard_focus(s, "default", handle);
	record_toplevel(a, handle);

	if (a->pending.armed) {
		int match = a->pending.wait_first
			|| (a->pending.handle == handle);
		if (match) {
			a->pending.armed = 0;
			do_subscribe(a, handle);
		}
	}
}

/* ---- noop event listeners (signatures must match exactly) ---- */
static void l_geom(void *d, struct qdwin_shell_v1 *s, uint32_t h,
		   int32_t x, int32_t y, uint32_t w, uint32_t H)
{ (void)d; (void)s; (void)h; (void)x; (void)y; (void)w; (void)H; }
static void l_state(void *d, struct qdwin_shell_v1 *s, uint32_t h, uint32_t st)
{
	(void)d; (void)s;
	fprintf(stderr, "qdwin-bystander: toplevel_state handle=%u state=0x%x\n",
		h, st);
}
static void l_title(void *d, struct qdwin_shell_v1 *s, uint32_t h, const char *t)
{ (void)d; (void)s; (void)h; (void)t; }
static void l_removed(void *d, struct qdwin_shell_v1 *s, uint32_t h)
{
	(void)s;
	struct app *a = d;
	fprintf(stderr, "qdwin-bystander: toplevel_removed handle=%u\n", h);
	forget_toplevel(a, h);
}
static void l_locked(void *d, struct qdwin_shell_v1 *s, uint32_t l)
{ (void)d; (void)s; (void)l; }
static void l_seat_created(void *d, struct qdwin_shell_v1 *s, const char *name)
{ (void)d; (void)s; (void)name; }
static void l_seat_removed(void *d, struct qdwin_shell_v1 *s, const char *name)
{ (void)d; (void)s; (void)name; }
static void l_output_created(void *d, struct qdwin_shell_v1 *s, const char *n)
{ (void)d; (void)s; (void)n; }
static void l_output_removed(void *d, struct qdwin_shell_v1 *s, const char *n)
{ (void)d; (void)s; (void)n; }
static void l_launcher(void *d, struct qdwin_shell_v1 *s)
{ (void)d; (void)s; }
static void l_switcher_next(void *d, struct qdwin_shell_v1 *s, int32_t dir)
{ (void)d; (void)s; (void)dir; }
static void l_switcher_commit(void *d, struct qdwin_shell_v1 *s)
{ (void)d; (void)s; }
static void l_lock_req(void *d, struct qdwin_shell_v1 *s)
{ (void)d; (void)s; }
static void l_idle_hint(void *d, struct qdwin_shell_v1 *s, uint32_t reason)
{ (void)d; (void)s; (void)reason; }
static void l_nested_pending(void *d, struct qdwin_shell_v1 *s,
			     uint32_t h, const char *app_id, uint32_t origin_uid)
{ (void)d; (void)s; (void)h; (void)app_id; (void)origin_uid; }
static void l_nested_pixsrc(void *d, struct qdwin_shell_v1 *s,
			    uint32_t h, const char *pw_node, const char *input_sink)
{ (void)d; (void)s; (void)h; (void)pw_node; (void)input_sink; }
static void l_selection_set(void *d, struct qdwin_shell_v1 *s,
			    const char *seat, uint32_t source_h,
			    const char *mimes, uint32_t serial)
{ (void)d; (void)s; (void)seat; (void)source_h; (void)mimes; (void)serial; }
static void l_activation_pending(void *d, struct qdwin_shell_v1 *s,
				 uint32_t h, uint32_t source_h,
				 uint32_t target_h, const char *token)
{ (void)d; (void)s; (void)h; (void)source_h; (void)target_h; (void)token; }
static void l_secctx(void *d, struct qdwin_shell_v1 *s,
		     uint32_t h, const char *engine, const char *app_id,
		     const char *instance_id)
{ (void)d; (void)s; (void)h; (void)engine; (void)app_id; (void)instance_id; }
static void l_seat_focus_changed(void *d, struct qdwin_shell_v1 *s,
				 const char *seat, uint32_t handle)
{ (void)d; (void)s; (void)seat; (void)handle; }

static const struct qdwin_shell_v1_listener listener = {
	.hello                  = on_hello,
	.toplevel_added         = on_toplevel_added,
	.toplevel_geometry      = l_geom,
	.toplevel_state         = l_state,
	.toplevel_title         = l_title,
	.toplevel_removed       = l_removed,
	.locked_changed         = l_locked,
	.seat_created           = l_seat_created,
	.seat_removed           = l_seat_removed,
	.output_created         = l_output_created,
	.output_removed         = l_output_removed,
	.launcher_requested     = l_launcher,
	.switcher_next          = l_switcher_next,
	.switcher_commit        = l_switcher_commit,
	.lock_requested         = l_lock_req,
	.idle_lock_hint         = l_idle_hint,
	.nested_proxy_pending   = l_nested_pending,
	.nested_proxy_pixel_source = l_nested_pixsrc,
	.selection_set          = l_selection_set,
	.activation_pending     = l_activation_pending,
	.toplevel_security_context = l_secctx,
	.seat_focus_changed     = l_seat_focus_changed,
};

/* ---- qdwin_view_stream_v1 listener ---- */
static void
on_stream_approved(void *d, struct qdwin_view_stream_v1 *stream,
		   const char *pipewire_node_name, uint32_t rdp_port,
		   const char *rdp_cert_path, const char *rdp_password)
{
	struct stream_info *si = d;
	(void)stream;
	fprintf(stderr,
		"qdwin-bystander: view_stream approved handle=%u "
		"pw=%s port=%u cert=%s\n",
		si->handle, pipewire_node_name ? pipewire_node_name : "",
		rdp_port, rdp_cert_path ? rdp_cert_path : "");
	printf("HANDLE=%u\n", si->handle);
	printf("PIPEWIRE_NODE_NAME=%s\n",
	       pipewire_node_name ? pipewire_node_name : "");
	printf("RDP_PORT=%u\n", rdp_port);
	printf("RDP_CERT_PATH=%s\n", rdp_cert_path ? rdp_cert_path : "");
	printf("RDP_PASSWORD=%s\n", rdp_password ? rdp_password : "");
	fflush(stdout);
}

static void
on_stream_denied(void *d, struct qdwin_view_stream_v1 *stream,
		 const char *reason)
{
	struct stream_info *si = d;
	fprintf(stderr,
		"qdwin-bystander: view_stream denied handle=%u reason=\"%s\"\n",
		si->handle, reason ? reason : "");
	qdwin_view_stream_v1_destroy(stream);
	si->stream = NULL;
}

static void
on_stream_torn_down(void *d, struct qdwin_view_stream_v1 *stream,
		    const char *reason)
{
	struct stream_info *si = d;
	fprintf(stderr,
		"qdwin-bystander: view_stream torn_down handle=%u reason=\"%s\"\n",
		si->handle, reason ? reason : "");
	qdwin_view_stream_v1_destroy(stream);
	si->stream = NULL;
}

static const struct qdwin_view_stream_v1_listener stream_listener = {
	.approved  = on_stream_approved,
	.denied    = on_stream_denied,
	.torn_down = on_stream_torn_down,
};

static void
do_subscribe(struct app *a, uint32_t handle)
{
	if (!a->shell) {
		fprintf(stderr, "qdwin-bystander: subscribe handle=%u: shell not bound\n",
			handle);
		return;
	}
	int slot = -1;
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (!a->streams[i].stream) { slot = i; break; }
	}
	if (slot < 0) {
		fprintf(stderr, "qdwin-bystander: subscribe handle=%u: stream table full\n",
			handle);
		return;
	}
	const char *label = a->peer_label[0] ? a->peer_label : "qdwin-bystander";
	struct qdwin_view_stream_v1 *s = qdwin_shell_v1_subscribe_view_stream(
		a->shell, handle, label, 0, 0, 0);
	if (!s) {
		fprintf(stderr, "qdwin-bystander: subscribe handle=%u: proxy create failed\n",
			handle);
		return;
	}
	a->streams[slot].handle = handle;
	a->streams[slot].stream = s;
	a->streams[slot].app = a;
	qdwin_view_stream_v1_add_listener(s, &stream_listener, &a->streams[slot]);
	fprintf(stderr, "qdwin-bystander: subscribe sent handle=%u peer_label=\"%s\"\n",
		handle, label);
}

static void
on_global(void *data, struct wl_registry *reg, uint32_t name,
	  const char *interface, uint32_t version)
{
	struct app *a = data;
	if (strcmp(interface, qdwin_shell_v1_interface.name) == 0) {
		uint32_t v = version < BIND_VERSION ? version : BIND_VERSION;
		a->shell = wl_registry_bind(reg, name,
					    &qdwin_shell_v1_interface, v);
		qdwin_shell_v1_add_listener(a->shell, &listener, a);
		fprintf(stderr, "qdwin-bystander: bound qdwin_shell_v1 v%u\n", v);
	}
}

static void on_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }

static const struct wl_registry_listener registry_listener = {
	.global = on_global,
	.global_remove = on_global_remove,
};

static volatile sig_atomic_t stop = 0;
static void on_int(int s) { (void)s; stop = 1; }

static void
process_command(struct app *a, char *line)
{
	while (*line == ' ' || *line == '\t') line++;
	if (*line == '\0' || *line == '#') return;

	char *cmd = strtok(line, " \t\r\n");
	char *arg = strtok(NULL, " \t\r\n");
	if (!cmd) return;

	uint32_t handle = 0;
	if (arg) handle = (uint32_t)strtoul(arg, NULL, 10);
	int has_handle = (arg != NULL);
	if (!has_handle && a->got_last) {
		handle = a->last_handle;
		has_handle = 1;
	}

	if (strcmp(cmd, "max") == 0 && has_handle) {
		fprintf(stderr, "qdwin-bystander: cmd max handle=%u\n", handle);
		qdwin_shell_v1_request_maximize(a->shell, handle, 1);
	} else if (strcmp(cmd, "restore") == 0 && has_handle) {
		fprintf(stderr, "qdwin-bystander: cmd restore handle=%u\n", handle);
		qdwin_shell_v1_request_maximize(a->shell, handle, 0);
	} else if (strcmp(cmd, "min") == 0 && has_handle) {
		fprintf(stderr, "qdwin-bystander: cmd min handle=%u\n", handle);
		qdwin_shell_v1_request_minimize(a->shell, handle);
	} else if (strcmp(cmd, "close") == 0 && has_handle) {
		fprintf(stderr, "qdwin-bystander: cmd close handle=%u\n", handle);
		qdwin_shell_v1_request_close(a->shell, handle);
	} else if (strcmp(cmd, "focus") == 0 && has_handle) {
		fprintf(stderr, "qdwin-bystander: cmd focus handle=%u\n", handle);
		qdwin_shell_v1_set_keyboard_focus(a->shell, "default", handle);
	} else if (strcmp(cmd, "subscribe") == 0 && has_handle) {
		fprintf(stderr, "qdwin-bystander: cmd subscribe handle=%u\n", handle);
		do_subscribe(a, handle);
	} else if (strcmp(cmd, "subscribelast") == 0 && a->got_last) {
		fprintf(stderr, "qdwin-bystander: cmd subscribelast handle=%u\n",
			a->last_handle);
		do_subscribe(a, a->last_handle);
	} else if (strcmp(cmd, "list") == 0) {
		fprintf(stderr, "qdwin-bystander: tracked toplevels:");
		for (int i = 0; i < MAX_TOPS; i++)
			if (a->tops[i].active)
				fprintf(stderr, " %u", a->tops[i].handle);
		fprintf(stderr, "\n");
	} else {
		fprintf(stderr, "qdwin-bystander: unknown cmd '%s'\n", cmd);
	}
}

static void
usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--subscribe <handle>|last] [--peer-label <s>]\n",
		argv0);
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--subscribe") == 0 && i + 1 < argc) {
			const char *v = argv[++i];
			g_app.pending.armed = 1;
			if (strcmp(v, "last") == 0 || strcmp(v, "first") == 0) {
				g_app.pending.wait_first = 1;
				g_app.pending.handle = 0;
			} else {
				g_app.pending.wait_first = 0;
				g_app.pending.handle = (uint32_t)strtoul(v, NULL, 10);
			}
		} else if (strcmp(argv[i], "--peer-label") == 0 && i + 1 < argc) {
			snprintf(g_app.peer_label, sizeof g_app.peer_label,
				 "%s", argv[++i]);
		} else if (strcmp(argv[i], "--help") == 0
			   || strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "qdwin-bystander: unknown arg '%s'\n",
				argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	const char *fifo_path = getenv("QDWIN_BYSTANDER_FIFO");
	if (!fifo_path) fifo_path = FIFO_PATH_DEFAULT;
	unlink(fifo_path);
	if (mkfifo(fifo_path, 0600) != 0) {
		fprintf(stderr, "qdwin-bystander: mkfifo %s failed: %s\n",
			fifo_path, strerror(errno));
		return 1;
	}
	int fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
	if (fifo_fd < 0) {
		fprintf(stderr, "qdwin-bystander: open fifo failed\n");
		return 1;
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "qdwin-bystander: wl_display_connect failed\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, &g_app);
	wl_display_roundtrip(display);
	if (!g_app.shell) {
		fprintf(stderr, "qdwin-bystander: qdwin_shell_v1 not advertised\n");
		return 1;
	}
	qdwin_shell_v1_bind_as_shell(g_app.shell);
	wl_display_roundtrip(display);

	signal(SIGINT, on_int);
	signal(SIGTERM, on_int);
	signal(SIGPIPE, SIG_IGN);

	int wl_fd = wl_display_get_fd(display);
	char buf[1024];
	size_t buf_len = 0;

	while (!stop) {
		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) == -1)
				goto out;
		}
		if (wl_display_flush(display) == -1) {
			wl_display_cancel_read(display);
			goto out;
		}
		struct pollfd fds[2] = {
			{ .fd = wl_fd, .events = POLLIN },
			{ .fd = fifo_fd, .events = POLLIN },
		};
		int pr = poll(fds, 2, -1);
		if (pr < 0) {
			wl_display_cancel_read(display);
			if (errno == EINTR) continue;
			break;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(display) == -1) goto out;
		} else {
			wl_display_cancel_read(display);
		}
		if (wl_display_dispatch_pending(display) == -1) goto out;

		if (fds[1].revents & POLLIN) {
			ssize_t n = read(fifo_fd, buf + buf_len,
					 sizeof(buf) - 1 - buf_len);
			if (n > 0) {
				buf_len += (size_t)n;
				buf[buf_len] = '\0';
				char *nl;
				while ((nl = strchr(buf, '\n')) != NULL) {
					*nl = '\0';
					process_command(&g_app, buf);
					wl_display_flush(display);
					size_t consumed = (size_t)(nl - buf) + 1;
					memmove(buf, buf + consumed, buf_len - consumed);
					buf_len -= consumed;
				}
				if (buf_len >= sizeof(buf) - 1)
					buf_len = 0;
			}
		}
	}
out:
	wl_display_disconnect(display);
	close(fifo_fd);
	unlink(fifo_path);
	return 0;
}
