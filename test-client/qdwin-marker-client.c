/*
 * qdwin-marker-client — deterministic marker client for the multi-machine
 * two-VM display harness (todo/multi-machine/09-test-strategy.md).
 *
 * Paints the marker CONTRACT defined in
 *   qdistro/multimachine/harness/marker.py
 * pixel-for-pixel at scale 1.0: vertical colour bands at known x-ranges, 8x8
 * fiducial checkers (hidden-scale detection), and a machine-readable corner
 * barcode carrying output id / dock generation / frame number / logical rect /
 * scale. The Python pixel oracle (oracle.py) decodes exactly what this paints;
 * the render/golden tests compare against the Python reference renderer.
 *
 * KEEP IN SYNC with marker.py — the palette, band layout, CRC8, payload wire
 * format, and cell layout are a shared contract. If you change one, change both
 * and re-run tests/unit/test_mm_marker_oracle.py + the golden test.
 *
 * Usage:
 *   qdwin-marker-client [--width W] [--height H] [--seam-x X]
 *                       [--output-id N] [--generation N] [--frame N]
 *                       [--animate-ms MS]
 * Honours $WAYLAND_DISPLAY. Exits non-zero only on connection/setup failure.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

/* ---- marker contract constants (mirror marker.py) -------------------- */
#define GRID_ROWS 14
#define GRID_COLS 14
#define CELL_PX 12
#define QUIET_CELLS 1
#define FIDUCIAL_CELLS 8
#define FIDUCIAL_CELL_PX 4
#define MARKER_VERSION 1
#define PAYLOAD_LEN 20  /* 19-byte body + CRC8 */
#define DATA_CAPACITY_BITS ((GRID_ROWS - 1) * (GRID_COLS - 1))  /* 169 */

/* Palette (R,G,B) — order matches marker.PALETTE. */
static const uint8_t PAL_RED[3]    = {0xE0, 0x20, 0x20};
static const uint8_t PAL_GREEN[3]  = {0x20, 0xC0, 0x60};
static const uint8_t PAL_BLUE[3]   = {0x20, 0x60, 0xE0};
static const uint8_t PAL_YELLOW[3] = {0xE0, 0xD0, 0x20};
static const uint8_t PAL_WHITE[3]  = {0xFF, 0xFF, 0xFF};
static const uint8_t PAL_BLACK[3]  = {0x00, 0x00, 0x00};

struct band { const char *name; const uint8_t *color; int x0, x1; };

static int running = 1;
static void on_sig(int s) { (void)s; running = 0; }

/* ---- CRC-8 (poly 0x07, init 0x00); mirrors marker.crc8 --------------- */
static uint8_t crc8(const uint8_t *data, size_t n)
{
	uint8_t crc = 0;
	for (size_t i = 0; i < n; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
					   : (uint8_t)(crc << 1);
	}
	return crc;
}

/* Pack the 20-byte payload, little-endian, matching struct "<2sBBHIhhhhB". */
static void pack_payload(uint8_t out[PAYLOAD_LEN], uint8_t output_id,
			 uint16_t generation, uint32_t frame, int16_t x,
			 int16_t y, int16_t w, int16_t h, uint8_t scale_x100)
{
	uint8_t *p = out;
	*p++ = 'M'; *p++ = 'M';
	*p++ = MARKER_VERSION;
	*p++ = output_id;
	*p++ = (uint8_t)(generation & 0xFF); *p++ = (uint8_t)(generation >> 8);
	*p++ = (uint8_t)(frame & 0xFF); *p++ = (uint8_t)((frame >> 8) & 0xFF);
	*p++ = (uint8_t)((frame >> 16) & 0xFF); *p++ = (uint8_t)((frame >> 24) & 0xFF);
	*p++ = (uint8_t)(x & 0xFF); *p++ = (uint8_t)((x >> 8) & 0xFF);
	*p++ = (uint8_t)(y & 0xFF); *p++ = (uint8_t)((y >> 8) & 0xFF);
	*p++ = (uint8_t)(w & 0xFF); *p++ = (uint8_t)((w >> 8) & 0xFF);
	*p++ = (uint8_t)(h & 0xFF); *p++ = (uint8_t)((h >> 8) & 0xFF);
	*p++ = scale_x100;
	out[19] = crc8(out, 19);
}

/* Build the GRID_ROWS x GRID_COLS cell matrix (1=white, 0=black). */
static void build_cells(uint8_t grid[GRID_ROWS][GRID_COLS],
			const uint8_t payload[PAYLOAD_LEN])
{
	for (int r = 0; r < GRID_ROWS; r++)
		for (int c = 0; c < GRID_COLS; c++)
			grid[r][c] = 0;
	grid[0][0] = 0;  /* origin anchor */
	for (int c = 1; c < GRID_COLS; c++) grid[0][c] = (c % 2 == 1) ? 1 : 0;
	for (int r = 1; r < GRID_ROWS; r++) grid[r][0] = (r % 2 == 1) ? 1 : 0;

	/* data bits, MSB-first, padded with 0xA5 up to capacity */
	uint8_t bits[DATA_CAPACITY_BITS];
	int nbits = 0;
	for (int i = 0; i < PAYLOAD_LEN && nbits < DATA_CAPACITY_BITS; i++)
		for (int b = 7; b >= 0 && nbits < DATA_CAPACITY_BITS; b--)
			bits[nbits++] = (payload[i] >> b) & 1;
	while (nbits < DATA_CAPACITY_BITS) {
		uint8_t pad = 0xA5;
		for (int b = 7; b >= 0 && nbits < DATA_CAPACITY_BITS; b--)
			bits[nbits++] = (pad >> b) & 1;
	}
	int idx = 0;
	for (int r = 1; r < GRID_ROWS; r++)
		for (int c = 1; c < GRID_COLS; c++)
			grid[r][c] = bits[idx++];
}

static void put_px(uint32_t *fb, int W, int H, int x, int y, const uint8_t rgb[3])
{
	if (x < 0 || y < 0 || x >= W || y >= H) return;
	fb[y * W + x] = 0xFF000000u | ((uint32_t)rgb[0] << 16) |
			((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
}

static void fill_rect(uint32_t *fb, int W, int H, int x0, int y0, int x1,
		      int y1, const uint8_t rgb[3])
{
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			put_px(fb, W, H, x, y, rgb);
}

/* Render the full marker contract into an ARGB8888 framebuffer (scale 1.0). */
static void render_marker(uint32_t *fb, int W, int H, struct band *bands,
			  int nbands, const uint8_t payload[PAYLOAD_LEN])
{
	/* 1) bands fill the surface */
	for (int i = 0; i < nbands; i++)
		fill_rect(fb, W, H, bands[i].x0, 0, bands[i].x1, H, bands[i].color);

	int corner_y1 = GRID_ROWS * CELL_PX + 2 * QUIET_CELLS * CELL_PX;  /* 192 */

	/* 2) fiducial 8x8 checkers inside each band */
	for (int i = 0; i < nbands; i++) {
		int fx = bands[i].x0 + 2, fy = corner_y1 + 4;
		for (int r = 0; r < FIDUCIAL_CELLS; r++)
			for (int c = 0; c < FIDUCIAL_CELLS; c++) {
				const uint8_t *v = ((r + c) % 2 == 0) ? PAL_WHITE : PAL_BLACK;
				fill_rect(fb, W, H, fx + c * FIDUCIAL_CELL_PX,
					  fy + r * FIDUCIAL_CELL_PX,
					  fx + (c + 1) * FIDUCIAL_CELL_PX,
					  fy + (r + 1) * FIDUCIAL_CELL_PX, v);
			}
	}

	/* 3) corner barcode: quiet zone white, then cells */
	uint8_t grid[GRID_ROWS][GRID_COLS];
	build_cells(grid, payload);
	int qz = QUIET_CELLS * CELL_PX;
	int corner_x1 = GRID_COLS * CELL_PX + 2 * QUIET_CELLS * CELL_PX;
	fill_rect(fb, W, H, 0, 0, corner_x1, corner_y1, PAL_WHITE);
	for (int r = 0; r < GRID_ROWS; r++)
		for (int c = 0; c < GRID_COLS; c++) {
			const uint8_t *v = grid[r][c] ? PAL_WHITE : PAL_BLACK;
			fill_rect(fb, W, H, qz + c * CELL_PX, qz + r * CELL_PX,
				  qz + (c + 1) * CELL_PX, qz + (r + 1) * CELL_PX, v);
		}
}

/* Distribute the 6 named bands across W; mirrors marker.compute_layout. */
static int build_bands(struct band *out, int W, int seam_x)
{
	int seam_half = CELL_PX > W / 8 ? CELL_PX : W / 8;
	int sl_x0 = seam_x - seam_half; if (sl_x0 < 0) sl_x0 = 0;
	int sr_x1 = seam_x + seam_half; if (sr_x1 > W) sr_x1 = W;
	int left_split = sl_x0 / 2;
	int right_split = sr_x1 + (W - sr_x1) / 2;
	out[0] = (struct band){"left-anchor", PAL_RED,    0,          left_split};
	out[1] = (struct band){"pre-seam",    PAL_GREEN,  left_split, sl_x0};
	out[2] = (struct band){"seam-left",   PAL_BLUE,   sl_x0,      seam_x};
	out[3] = (struct band){"seam-right",  PAL_YELLOW, seam_x,     sr_x1};
	out[4] = (struct band){"post-seam",   PAL_GREEN,  sr_x1,      right_split};
	out[5] = (struct band){"right-anchor",PAL_RED,    right_split, W};
	return 6;
}

/* ---- Wayland boilerplate (mirrors qdistro-test-window.c) ------------- */
struct ctx {
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *xdg_wm_base;
};

static void xdg_wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t s)
{ (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener xdg_wm_base_impl = { .ping = xdg_wm_base_ping };

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
			    const char *iface, uint32_t version)
{
	struct ctx *c = data;
	if (!strcmp(iface, wl_compositor_interface.name))
		c->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
						 version > 4 ? 4 : version);
	else if (!strcmp(iface, wl_shm_interface.name))
		c->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		c->xdg_wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(c->xdg_wm_base, &xdg_wm_base_impl, NULL);
	}
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_global_remove,
};

static void xdg_surface_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{ (void)d; xdg_surface_ack_configure(xs, serial); }
static const struct xdg_surface_listener xdg_surface_impl = {
	.configure = xdg_surface_configure,
};
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
			 struct wl_array *st) { (void)d;(void)t;(void)w;(void)h;(void)st; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d;(void)t; running = 0; }
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d;(void)t;(void)w;(void)h; }
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{ (void)d;(void)t;(void)c; }
static const struct xdg_toplevel_listener tl_impl = {
	.configure = tl_configure, .close = tl_close,
	.configure_bounds = tl_bounds, .wm_capabilities = tl_caps,
};

static uint32_t *map_buffer(struct wl_shm *shm, int W, int H,
			    struct wl_buffer **out_buf)
{
	int stride = W * 4, size = stride * H;
	int fd = memfd_create("qdwin-marker", MFD_CLOEXEC);
	if (fd < 0) return NULL;
	if (ftruncate(fd, size) < 0) { close(fd); return NULL; }
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { close(fd); return NULL; }
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	*out_buf = wl_shm_pool_create_buffer(pool, 0, W, H, stride,
					     WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return p;  /* caller keeps the mapping for re-render (animate) */
}

/* Write an ARGB8888 framebuffer as a binary PPM (P6, RGB) for offscreen
 * cross-language contract testing against the Python oracle. */
static int write_ppm(const char *path, const uint32_t *fb, int W, int H)
{
	FILE *f = fopen(path, "wb");
	if (!f) return 1;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint32_t px = fb[i];
		unsigned char rgb[3] = {(unsigned char)((px >> 16) & 0xFF),
					(unsigned char)((px >> 8) & 0xFF),
					(unsigned char)(px & 0xFF)};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	int W = 1280, H = 480, seam_x = -1, animate_ms = 0, fullscreen = 0;
	long output_id = 1, generation = 1, frame = 0;
	const char *dump_ppm = NULL;
	struct option opts[] = {
		{"width", required_argument, 0, 'w'},
		{"height", required_argument, 0, 'h'},
		{"seam-x", required_argument, 0, 's'},
		{"output-id", required_argument, 0, 'o'},
		{"generation", required_argument, 0, 'g'},
		{"frame", required_argument, 0, 'f'},
		{"animate-ms", required_argument, 0, 'a'},
		{"dump-ppm", required_argument, 0, 'd'},
		{"fullscreen", no_argument, 0, 'F'},
		{0, 0, 0, 0},
	};
	int o;
	while ((o = getopt_long(argc, argv, "w:h:s:o:g:f:a:d:F", opts, NULL)) != -1) {
		switch (o) {
		case 'w': W = atoi(optarg); break;
		case 'h': H = atoi(optarg); break;
		case 's': seam_x = atoi(optarg); break;
		case 'o': output_id = atol(optarg); break;
		case 'g': generation = atol(optarg); break;
		case 'f': frame = atol(optarg); break;
		case 'a': animate_ms = atoi(optarg); break;
		case 'd': dump_ppm = optarg; break;
		case 'F': fullscreen = 1; break;
		default:
			fprintf(stderr, "usage: %s [--width W] [--height H] "
				"[--seam-x X] [--output-id N] [--generation N] "
				"[--frame N] [--animate-ms MS] [--dump-ppm PATH] "
				"[--fullscreen]\n", argv[0]);
			return 2;
		}
	}
	if (seam_x < 0) seam_x = W / 2;

	/* Offscreen mode: render to a malloc'd buffer, write PPM, exit.
	 * No Wayland connection — for the C<->Python contract self-test. */
	if (dump_ppm) {
		uint32_t *fb = calloc((size_t)W * H, sizeof(uint32_t));
		if (!fb) { fprintf(stderr, "calloc failed\n"); return 1; }
		struct band bands[6];
		int nbands = build_bands(bands, W, seam_x);
		uint8_t payload[PAYLOAD_LEN];
		pack_payload(payload, (uint8_t)output_id, (uint16_t)generation,
			     (uint32_t)frame, 0, 0, (int16_t)W, (int16_t)H, 100);
		render_marker(fb, W, H, bands, nbands, payload);
		int rc = write_ppm(dump_ppm, fb, W, H);
		free(fb);
		if (rc) fprintf(stderr, "write_ppm failed: %s\n", dump_ppm);
		return rc;
	}

	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "wl_display_connect failed\n"); return 1; }
	struct ctx c = {0};
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, &c);
	wl_display_roundtrip(display);
	if (!c.compositor || !c.shm || !c.xdg_wm_base) {
		fprintf(stderr, "missing globals: compositor=%p shm=%p xdg=%p\n",
			(void *)c.compositor, (void *)c.shm, (void *)c.xdg_wm_base);
		return 1;
	}

	struct wl_surface *surf = wl_compositor_create_surface(c.compositor);
	struct xdg_surface *xsurf = xdg_wm_base_get_xdg_surface(c.xdg_wm_base, surf);
	xdg_surface_add_listener(xsurf, &xdg_surface_impl, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xsurf);
	xdg_toplevel_add_listener(top, &tl_impl, NULL);
	xdg_toplevel_set_title(top, "qdwin-marker-client");
	xdg_toplevel_set_app_id(top, "qdwin-marker-client");
	if (fullscreen)
		xdg_toplevel_set_fullscreen(top, NULL);  /* fill an output at 0,0 */
	wl_surface_commit(surf);
	wl_display_roundtrip(display);

	struct wl_buffer *buf = NULL;
	uint32_t *fb = map_buffer(c.shm, W, H, &buf);
	if (!fb) { fprintf(stderr, "map_buffer failed\n"); return 1; }

	struct band bands[6];
	int nbands = build_bands(bands, W, seam_x);

	uint8_t payload[PAYLOAD_LEN];
	pack_payload(payload, (uint8_t)output_id, (uint16_t)generation,
		     (uint32_t)frame, 0, 0, (int16_t)W, (int16_t)H, 100);
	render_marker(fb, W, H, bands, nbands, payload);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, W, H);
	wl_surface_commit(surf);
	wl_display_flush(display);

	if (animate_ms <= 0) {
		while (running && wl_display_dispatch(display) != -1) { }
	} else {
		struct timespec ts = {animate_ms / 1000,
				      (long)(animate_ms % 1000) * 1000000L};
		while (running) {
			if (wl_display_dispatch_pending(display) < 0) break;
			nanosleep(&ts, NULL);
			frame++;
			pack_payload(payload, (uint8_t)output_id,
				     (uint16_t)generation, (uint32_t)frame, 0, 0,
				     (int16_t)W, (int16_t)H, 100);
			render_marker(fb, W, H, bands, nbands, payload);
			wl_surface_attach(surf, buf, 0, 0);
			wl_surface_damage_buffer(surf, 0, 0, W, H);
			wl_surface_commit(surf);
			wl_display_flush(display);
		}
	}

	xdg_toplevel_destroy(top);
	xdg_surface_destroy(xsurf);
	wl_surface_destroy(surf);
	wl_buffer_destroy(buf);
	wl_display_disconnect(display);
	return 0;
}
