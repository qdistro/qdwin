/*
 * qdwin-workspace-probe — host test client for qdwin's ext-workspace-v1
 * server (the v24 workspace implementation, qdwin/qdwin.c).
 *
 * qdwin implements the standard ext_workspace_manager_v1: on bind it
 * streams one workspace_group + one ext_workspace_handle_v1 per workspace,
 * each carrying a 1-D coordinate (== index) and a state bitmask whose
 * ACTIVE bit (1) marks the active workspace, then a `done`. activate +
 * commit on a handle switches the active workspace; the compositor
 * re-broadcasts state and fires `done` again.
 *
 * This probe binds the manager, syncs the initial set, and (optionally)
 * activates a workspace and re-reads the active index — asserting the
 * compositor honoured the switch. Pure protocol, no pixels: it runs
 * against a headless qdwin in the host gate.
 *
 * Modes:
 *   (default)         sync and print "workspaces=N active=A".
 *   --expect-count=N  assert exactly N workspaces (else exit 1).
 *   --activate=K      activate workspace index K, then assert the active
 *                     index became K (else exit 1).
 *
 * Exit codes:
 *   0  success (and any assertion held)
 *   1  an assertion failed, or a protocol error was posted
 *   2  setup error (no display / manager global not advertised)
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <wayland-client.h>
#include "ext-workspace-v1-client-protocol.h"

#define MAX_WS 64

struct ws_entry {
	struct ext_workspace_handle_v1 *proxy;
	uint32_t coord;
	int have_coord;
	uint32_t state;
	int removed;
};

struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct ext_workspace_manager_v1 *manager;
	struct ext_workspace_group_handle_v1 *group;
	struct ws_entry ws[MAX_WS];
	int ws_count;
	int done_seen;
};

/* ---- handle ---- */
static struct ws_entry *entry_for(struct probe *p,
				  struct ext_workspace_handle_v1 *h)
{
	for (int i = 0; i < p->ws_count; i++)
		if (p->ws[i].proxy == h)
			return &p->ws[i];
	return NULL;
}

static void h_id(void *d, struct ext_workspace_handle_v1 *h, const char *id)
{ (void)d; (void)h; (void)id; }
static void h_name(void *d, struct ext_workspace_handle_v1 *h, const char *n)
{ (void)d; (void)h; (void)n; }
static void h_coordinates(void *d, struct ext_workspace_handle_v1 *h,
			  struct wl_array *coords)
{
	struct ws_entry *e = entry_for(d, h);
	if (e && coords && coords->size >= sizeof(uint32_t)) {
		e->coord = *(uint32_t *)coords->data;
		e->have_coord = 1;
	}
}
static void h_state(void *d, struct ext_workspace_handle_v1 *h, uint32_t state)
{
	struct ws_entry *e = entry_for(d, h);
	if (e)
		e->state = state;
}
static void h_capabilities(void *d, struct ext_workspace_handle_v1 *h,
			   uint32_t caps)
{ (void)d; (void)h; (void)caps; }
static void h_removed(void *d, struct ext_workspace_handle_v1 *h)
{
	struct ws_entry *e = entry_for(d, h);
	if (e)
		e->removed = 1;
}
static const struct ext_workspace_handle_v1_listener handle_listener = {
	.id = h_id, .name = h_name, .coordinates = h_coordinates,
	.state = h_state, .capabilities = h_capabilities, .removed = h_removed,
};

/* ---- group (we ignore membership; single group) ---- */
static void g_caps(void *d, struct ext_workspace_group_handle_v1 *g, uint32_t c)
{ (void)d; (void)g; (void)c; }
static void g_oenter(void *d, struct ext_workspace_group_handle_v1 *g,
		     struct wl_output *o) { (void)d; (void)g; (void)o; }
static void g_oleave(void *d, struct ext_workspace_group_handle_v1 *g,
		     struct wl_output *o) { (void)d; (void)g; (void)o; }
static void g_wenter(void *d, struct ext_workspace_group_handle_v1 *g,
		     struct ext_workspace_handle_v1 *w)
{ (void)d; (void)g; (void)w; }
static void g_wleave(void *d, struct ext_workspace_group_handle_v1 *g,
		     struct ext_workspace_handle_v1 *w)
{ (void)d; (void)g; (void)w; }
static void g_removed(void *d, struct ext_workspace_group_handle_v1 *g)
{ (void)d; (void)g; }
static const struct ext_workspace_group_handle_v1_listener group_listener = {
	.capabilities = g_caps, .output_enter = g_oenter,
	.output_leave = g_oleave, .workspace_enter = g_wenter,
	.workspace_leave = g_wleave, .removed = g_removed,
};

/* ---- manager ---- */
static void m_workspace_group(void *d, struct ext_workspace_manager_v1 *m,
			      struct ext_workspace_group_handle_v1 *grp)
{
	struct probe *p = d;
	(void)m;
	p->group = grp;
	ext_workspace_group_handle_v1_add_listener(grp, &group_listener, p);
}
static void m_workspace(void *d, struct ext_workspace_manager_v1 *m,
			struct ext_workspace_handle_v1 *ws)
{
	struct probe *p = d;
	(void)m;
	if (p->ws_count < MAX_WS) {
		p->ws[p->ws_count].proxy = ws;
		p->ws_count++;
		ext_workspace_handle_v1_add_listener(ws, &handle_listener, p);
	}
}
static void m_done(void *d, struct ext_workspace_manager_v1 *m)
{ struct probe *p = d; (void)m; p->done_seen++; }
static void m_finished(void *d, struct ext_workspace_manager_v1 *m)
{ (void)d; (void)m; }
static const struct ext_workspace_manager_v1_listener manager_listener = {
	.workspace_group = m_workspace_group, .workspace = m_workspace,
	.done = m_done, .finished = m_finished,
};

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
		      const char *interface, uint32_t version)
{
	struct probe *p = data;
	if (strcmp(interface, ext_workspace_manager_v1_interface.name) == 0)
		p->manager = wl_registry_bind(
			reg, name, &ext_workspace_manager_v1_interface,
			version < 1 ? version : 1);
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = {
	.global = on_global, .global_remove = on_global_remove,
};

/* Live (not removed) workspace count. */
static int live_count(struct probe *p)
{
	int n = 0;
	for (int i = 0; i < p->ws_count; i++)
		if (!p->ws[i].removed)
			n++;
	return n;
}

/* Active workspace index, by coordinate (matches qdwin's index). -1 if
 * none flagged active. */
static int active_index(struct probe *p)
{
	for (int i = 0; i < p->ws_count; i++)
		if (!p->ws[i].removed && (p->ws[i].state & 1u))
			return p->ws[i].have_coord ? (int)p->ws[i].coord : i;
	return -1;
}

static struct ws_entry *entry_by_coord(struct probe *p, uint32_t coord)
{
	for (int i = 0; i < p->ws_count; i++)
		if (!p->ws[i].removed && p->ws[i].have_coord &&
		    p->ws[i].coord == coord)
			return &p->ws[i];
	return NULL;
}

int main(int argc, char *argv[])
{
	int expect_count = -1;
	int activate = -1;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--expect-count=", 15) == 0)
			expect_count = atoi(argv[i] + 15);
		else if (strncmp(argv[i], "--activate=", 11) == 0)
			activate = atoi(argv[i] + 11);
	}

	struct probe p = {0};
	p.display = wl_display_connect(NULL);
	if (!p.display) {
		fprintf(stderr, "qdwin-workspace-probe: wl_display_connect "
			"failed: %s\n", strerror(errno));
		return 2;
	}
	p.registry = wl_display_get_registry(p.display);
	wl_registry_add_listener(p.registry, &registry_listener, &p);
	wl_display_roundtrip(p.display);

	if (!p.manager) {
		fprintf(stderr, "qdwin-workspace-probe: ext_workspace_manager_v1 "
			"not advertised\n");
		return 2;
	}
	ext_workspace_manager_v1_add_listener(p.manager, &manager_listener, &p);
	/* Drain the initial workspace burst + done. */
	wl_display_roundtrip(p.display);
	wl_display_roundtrip(p.display);

	int count = live_count(&p);
	int active = active_index(&p);
	printf("qdwin-workspace-probe: workspaces=%d active=%d done=%d\n",
	       count, active, p.done_seen);

	if (expect_count >= 0 && count != expect_count) {
		fprintf(stderr, "qdwin-workspace-probe: FAIL expected %d "
			"workspaces, got %d\n", expect_count, count);
		return 1;
	}

	if (activate >= 0) {
		struct ws_entry *e = entry_by_coord(&p, (uint32_t)activate);
		if (!e) {
			fprintf(stderr, "qdwin-workspace-probe: FAIL no "
				"workspace with index %d\n", activate);
			return 1;
		}
		ext_workspace_handle_v1_activate(e->proxy);
		ext_workspace_manager_v1_commit(p.manager);
		wl_display_roundtrip(p.display);
		wl_display_roundtrip(p.display);
		int now = active_index(&p);
		printf("qdwin-workspace-probe: after activate(%d) active=%d\n",
		       activate, now);
		if (now != activate) {
			fprintf(stderr, "qdwin-workspace-probe: FAIL active is "
				"%d, expected %d\n", now, activate);
			return 1;
		}
	}

	if (wl_display_get_error(p.display) != 0) {
		fprintf(stderr, "qdwin-workspace-probe: FAIL protocol error\n");
		return 1;
	}
	printf("qdwin-workspace-probe: OK\n");
	return 0;
}
