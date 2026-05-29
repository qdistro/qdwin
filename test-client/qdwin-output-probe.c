/*
 * qdwin-output-probe — host test client for qdwin's wlr-output-management-v1
 * server (qdwin/qdwin.c, the output/display-management implementation).
 *
 * qdwin implements the standard zwlr_output_manager_v1: on bind it streams
 * one zwlr_output_head_v1 per output (with its modes, current mode, position,
 * scale, transform, and EDID-derived make/model/serial), then a `done`
 * carrying the configuration serial. A client builds a configuration against
 * that serial and `test`s or `apply`s it atomically; the compositor answers
 * succeeded / failed / cancelled.
 *
 * This probe binds the manager, syncs the head/mode set, prints
 * "heads=N modes=M serial=S", and (optionally) builds + tests/applies a
 * configuration that nudges the first head's position, asserting the
 * compositor's reply.
 *
 * Modes:
 *   (default)        sync and print the head/mode summary.
 *   --expect-heads=N assert exactly N heads (else exit 1).
 *   --test           build a position-only config for head 0 and `test` it,
 *                    asserting `succeeded`.
 *   --apply          same but `apply`; asserts `succeeded` and that a new
 *                    `done` (with a bumped serial) follows.
 *   --bad-serial     apply a config built against serial+1; asserts
 *                    `cancelled`.
 *
 * Exit codes: 0 ok, 1 assertion/protocol failure, 2 setup error.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <wayland-client.h>
#include "wlr-output-management-unstable-v1-client-protocol.h"

#define MAX_HEADS 16
#define MAX_MODES 64

struct mode_entry {
	struct zwlr_output_mode_v1 *proxy;
	int32_t w, h, refresh;
	int preferred;
};

struct head_entry {
	struct zwlr_output_head_v1 *proxy;
	char name[128];
	int enabled;
	int32_t pos_x, pos_y;
	struct mode_entry modes[MAX_MODES];
	int mode_count;
	struct zwlr_output_mode_v1 *current_mode;
};

struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct zwlr_output_manager_v1 *manager;
	struct head_entry heads[MAX_HEADS];
	int head_count;
	uint32_t serial;
	int done_seen;
	int cfg_succeeded, cfg_failed, cfg_cancelled;
};

static struct head_entry *head_for(struct probe *p,
				   struct zwlr_output_head_v1 *h)
{
	for (int i = 0; i < p->head_count; i++)
		if (p->heads[i].proxy == h)
			return &p->heads[i];
	return NULL;
}
static struct mode_entry *mode_for(struct probe *p,
				   struct zwlr_output_mode_v1 *m)
{
	for (int i = 0; i < p->head_count; i++)
		for (int j = 0; j < p->heads[i].mode_count; j++)
			if (p->heads[i].modes[j].proxy == m)
				return &p->heads[i].modes[j];
	return NULL;
}

/* ---- mode ---- */
static void md_size(void *d, struct zwlr_output_mode_v1 *m, int32_t w, int32_t h)
{ struct mode_entry *e = mode_for(d, m); if (e) { e->w = w; e->h = h; } }
static void md_refresh(void *d, struct zwlr_output_mode_v1 *m, int32_t r)
{ struct mode_entry *e = mode_for(d, m); if (e) e->refresh = r; }
static void md_preferred(void *d, struct zwlr_output_mode_v1 *m)
{ struct mode_entry *e = mode_for(d, m); if (e) e->preferred = 1; }
static void md_finished(void *d, struct zwlr_output_mode_v1 *m)
{ (void)d; (void)m; }
static const struct zwlr_output_mode_v1_listener mode_listener = {
	.size = md_size, .refresh = md_refresh,
	.preferred = md_preferred, .finished = md_finished,
};

/* ---- head ---- */
static struct probe *g_probe;  /* mode events arrive without head context */
static void hd_name(void *d, struct zwlr_output_head_v1 *h, const char *n)
{ struct head_entry *e = head_for(d, h); if (e) snprintf(e->name, sizeof e->name, "%s", n); }
static void hd_description(void *d, struct zwlr_output_head_v1 *h, const char *s)
{ (void)d; (void)h; (void)s; }
static void hd_physical_size(void *d, struct zwlr_output_head_v1 *h, int32_t w, int32_t hh)
{ (void)d; (void)h; (void)w; (void)hh; }
static void hd_mode(void *d, struct zwlr_output_head_v1 *h, struct zwlr_output_mode_v1 *m)
{
	struct head_entry *e = head_for(d, h);
	if (e && e->mode_count < MAX_MODES) {
		e->modes[e->mode_count].proxy = m;
		e->mode_count++;
		zwlr_output_mode_v1_add_listener(m, &mode_listener, d);
	}
}
static void hd_enabled(void *d, struct zwlr_output_head_v1 *h, int32_t en)
{ struct head_entry *e = head_for(d, h); if (e) e->enabled = en; }
static void hd_current_mode(void *d, struct zwlr_output_head_v1 *h, struct zwlr_output_mode_v1 *m)
{ struct head_entry *e = head_for(d, h); if (e) e->current_mode = m; }
static void hd_position(void *d, struct zwlr_output_head_v1 *h, int32_t x, int32_t y)
{ struct head_entry *e = head_for(d, h); if (e) { e->pos_x = x; e->pos_y = y; } }
static void hd_transform(void *d, struct zwlr_output_head_v1 *h, int32_t t)
{ (void)d; (void)h; (void)t; }
static void hd_scale(void *d, struct zwlr_output_head_v1 *h, wl_fixed_t s)
{ (void)d; (void)h; (void)s; }
static void hd_finished(void *d, struct zwlr_output_head_v1 *h)
{ (void)d; (void)h; }
static void hd_make(void *d, struct zwlr_output_head_v1 *h, const char *s)
{ (void)d; (void)h; (void)s; }
static void hd_model(void *d, struct zwlr_output_head_v1 *h, const char *s)
{ (void)d; (void)h; (void)s; }
static void hd_serial(void *d, struct zwlr_output_head_v1 *h, const char *s)
{ (void)d; (void)h; (void)s; }
static void hd_adaptive_sync(void *d, struct zwlr_output_head_v1 *h, uint32_t s)
{ (void)d; (void)h; (void)s; }
static const struct zwlr_output_head_v1_listener head_listener = {
	.name = hd_name, .description = hd_description,
	.physical_size = hd_physical_size, .mode = hd_mode,
	.enabled = hd_enabled, .current_mode = hd_current_mode,
	.position = hd_position, .transform = hd_transform, .scale = hd_scale,
	.finished = hd_finished, .make = hd_make, .model = hd_model,
	.serial_number = hd_serial, .adaptive_sync = hd_adaptive_sync,
};

/* ---- manager ---- */
static void mgr_head(void *d, struct zwlr_output_manager_v1 *m, struct zwlr_output_head_v1 *h)
{
	struct probe *p = d;
	(void)m;
	if (p->head_count < MAX_HEADS) {
		p->heads[p->head_count].proxy = h;
		p->head_count++;
		zwlr_output_head_v1_add_listener(h, &head_listener, p);
	}
}
static void mgr_done(void *d, struct zwlr_output_manager_v1 *m, uint32_t serial)
{ struct probe *p = d; (void)m; p->serial = serial; p->done_seen++; }
static void mgr_finished(void *d, struct zwlr_output_manager_v1 *m)
{ (void)d; (void)m; }
static const struct zwlr_output_manager_v1_listener manager_listener = {
	.head = mgr_head, .done = mgr_done, .finished = mgr_finished,
};

/* ---- configuration ---- */
static void cfg_succeeded(void *d, struct zwlr_output_configuration_v1 *c)
{ struct probe *p = d; (void)c; p->cfg_succeeded++; }
static void cfg_failed(void *d, struct zwlr_output_configuration_v1 *c)
{ struct probe *p = d; (void)c; p->cfg_failed++; }
static void cfg_cancelled(void *d, struct zwlr_output_configuration_v1 *c)
{ struct probe *p = d; (void)c; p->cfg_cancelled++; }
static const struct zwlr_output_configuration_v1_listener config_listener = {
	.succeeded = cfg_succeeded, .failed = cfg_failed, .cancelled = cfg_cancelled,
};

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
		      const char *interface, uint32_t version)
{
	struct probe *p = data;
	if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0)
		p->manager = wl_registry_bind(reg, name,
			&zwlr_output_manager_v1_interface, version < 4 ? version : 4);
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = {
	.global = on_global, .global_remove = on_global_remove,
};

int main(int argc, char *argv[])
{
	int expect_heads = -1;
	int do_test = 0, do_apply = 0, bad_serial = 0;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--expect-heads=", 15) == 0)
			expect_heads = atoi(argv[i] + 15);
		else if (strcmp(argv[i], "--test") == 0)
			do_test = 1;
		else if (strcmp(argv[i], "--apply") == 0)
			do_apply = 1;
		else if (strcmp(argv[i], "--bad-serial") == 0)
			bad_serial = 1;
	}

	struct probe p = {0};
	g_probe = &p;
	p.display = wl_display_connect(NULL);
	if (!p.display) {
		fprintf(stderr, "qdwin-output-probe: wl_display_connect failed: %s\n",
			strerror(errno));
		return 2;
	}
	p.registry = wl_display_get_registry(p.display);
	wl_registry_add_listener(p.registry, &registry_listener, &p);
	wl_display_roundtrip(p.display);

	if (!p.manager) {
		fprintf(stderr, "qdwin-output-probe: zwlr_output_manager_v1 not advertised\n");
		return 2;
	}
	zwlr_output_manager_v1_add_listener(p.manager, &manager_listener, &p);
	wl_display_roundtrip(p.display);
	wl_display_roundtrip(p.display);

	int total_modes = 0;
	for (int i = 0; i < p.head_count; i++)
		total_modes += p.heads[i].mode_count;
	printf("qdwin-output-probe: heads=%d modes=%d serial=%u done=%d\n",
	       p.head_count, total_modes, p.serial, p.done_seen);
	for (int i = 0; i < p.head_count; i++)
		printf("  head[%d] name=%s enabled=%d pos=%d,%d modes=%d\n",
		       i, p.heads[i].name, p.heads[i].enabled,
		       p.heads[i].pos_x, p.heads[i].pos_y, p.heads[i].mode_count);

	if (expect_heads >= 0 && p.head_count != expect_heads) {
		fprintf(stderr, "qdwin-output-probe: FAIL expected %d heads, got %d\n",
			expect_heads, p.head_count);
		return 1;
	}

	if ((do_test || do_apply || bad_serial) && p.head_count > 0) {
		uint32_t use_serial = bad_serial ? p.serial + 1 : p.serial;
		struct zwlr_output_configuration_v1 *cfg =
			zwlr_output_manager_v1_create_configuration(p.manager, use_serial);
		zwlr_output_configuration_v1_add_listener(cfg, &config_listener, &p);
		/* Enable + reposition head 0; leave all others enabled untouched. */
		for (int i = 0; i < p.head_count; i++) {
			struct zwlr_output_configuration_head_v1 *chh =
				zwlr_output_configuration_v1_enable_head(cfg, p.heads[i].proxy);
			if (i == 0)
				zwlr_output_configuration_head_v1_set_position(chh,
					p.heads[i].pos_x, p.heads[i].pos_y);
		}
		if (do_apply || bad_serial)
			zwlr_output_configuration_v1_apply(cfg);
		else
			zwlr_output_configuration_v1_test(cfg);
		wl_display_roundtrip(p.display);
		wl_display_roundtrip(p.display);

		printf("qdwin-output-probe: cfg succeeded=%d failed=%d cancelled=%d\n",
		       p.cfg_succeeded, p.cfg_failed, p.cfg_cancelled);
		if (bad_serial) {
			if (p.cfg_cancelled != 1) {
				fprintf(stderr, "qdwin-output-probe: FAIL expected cancelled\n");
				return 1;
			}
		} else if (p.cfg_succeeded != 1) {
			fprintf(stderr, "qdwin-output-probe: FAIL expected succeeded\n");
			return 1;
		}
	}

	if (wl_display_get_error(p.display) != 0) {
		fprintf(stderr, "qdwin-output-probe: FAIL protocol error\n");
		return 1;
	}
	printf("qdwin-output-probe: OK\n");
	return 0;
}
