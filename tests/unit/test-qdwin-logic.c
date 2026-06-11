/* test-qdwin-logic.c — compiled unit test for the pure logic kernels in
 * qdwin/qdwin-logic.c. No weston, no wayland, no live compositor: just
 * the extracted arithmetic / enum maps exercised directly.
 *
 * Wired as meson test 'qdwin-logic-unit' in suite 'logic'. Prints a
 * PASS/FAIL line per check and exits non-zero on any failure.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <libinput.h>

#include "qdwin-logic.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, fmt, ...) do {                                       \
		checks++;                                                \
		if (cond) {                                              \
			printf("PASS: " fmt "\n", ##__VA_ARGS__);        \
		} else {                                                 \
			printf("FAIL: " fmt "\n", ##__VA_ARGS__);        \
			failures++;                                      \
		}                                                        \
	} while (0)

#define CHECK_U(actual, expected, label) \
	CHECK((actual) == (expected), "%s: got %u, want %u", \
	      (label), (unsigned)(actual), (unsigned)(expected))

/* ---- accel-profile enum map (incl. default) ---- */
static void test_accel_profile(void)
{
	CHECK(qdwin_accel_profile_to_libinput(QDWIN_LOGIC_ACCEL_FLAT)
	      == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT,
	      "accel FLAT -> libinput FLAT");
	CHECK(qdwin_accel_profile_to_libinput(QDWIN_LOGIC_ACCEL_ADAPTIVE)
	      == LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE,
	      "accel ADAPTIVE -> libinput ADAPTIVE");
	/* any non-FLAT value falls back to adaptive */
	CHECK(qdwin_accel_profile_to_libinput(99u)
	      == LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE,
	      "accel bogus(99) -> libinput ADAPTIVE (default)");
}

/* ---- scroll-method enum map (all cases incl. default) ---- */
static void test_scroll_method(void)
{
	CHECK(qdwin_scroll_method_to_libinput(QDWIN_LOGIC_SCROLL_NONE)
	      == LIBINPUT_CONFIG_SCROLL_NO_SCROLL,
	      "scroll NONE -> NO_SCROLL");
	CHECK(qdwin_scroll_method_to_libinput(QDWIN_LOGIC_SCROLL_EDGE)
	      == LIBINPUT_CONFIG_SCROLL_EDGE,
	      "scroll EDGE -> EDGE");
	CHECK(qdwin_scroll_method_to_libinput(QDWIN_LOGIC_SCROLL_ON_BUTTON_DOWN)
	      == LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN,
	      "scroll ON_BUTTON_DOWN -> ON_BUTTON_DOWN");
	CHECK(qdwin_scroll_method_to_libinput(QDWIN_LOGIC_SCROLL_TWO_FINGER)
	      == LIBINPUT_CONFIG_SCROLL_2FG,
	      "scroll TWO_FINGER -> 2FG");
	CHECK(qdwin_scroll_method_to_libinput(255u)
	      == LIBINPUT_CONFIG_SCROLL_2FG,
	      "scroll bogus(255) -> 2FG (default)");
}

/* ---- exclusive edge: 8 anchor combos + edge_set passthrough ---- */
static void test_exclusive_edge(void)
{
	const uint32_t T = QDWIN_LOGIC_ANCHOR_TOP;
	const uint32_t B = QDWIN_LOGIC_ANCHOR_BOTTOM;
	const uint32_t L = QDWIN_LOGIC_ANCHOR_LEFT;
	const uint32_t R = QDWIN_LOGIC_ANCHOR_RIGHT;

	/* edge_set passthrough: explicit edge wins regardless of anchor */
	CHECK_U(qdwin_layer_exclusive_edge(T | B | L | R, true, R), R,
		"edge_set passthrough returns edge_val verbatim");
	CHECK_U(qdwin_layer_exclusive_edge(0, true, L), L,
		"edge_set passthrough with zero anchor");

	/* single-edge anchors -> that edge */
	CHECK_U(qdwin_layer_exclusive_edge(T, false, 0), T, "anchor TOP -> T");
	CHECK_U(qdwin_layer_exclusive_edge(B, false, 0), B, "anchor BOTTOM -> B");
	CHECK_U(qdwin_layer_exclusive_edge(L, false, 0), L, "anchor LEFT -> L");
	CHECK_U(qdwin_layer_exclusive_edge(R, false, 0), R, "anchor RIGHT -> R");

	/* three-edge anchors -> opposite of the free edge */
	CHECK_U(qdwin_layer_exclusive_edge(T | L | R, false, 0), T,
		"anchor T|L|R (free=B) -> T");
	CHECK_U(qdwin_layer_exclusive_edge(B | L | R, false, 0), B,
		"anchor B|L|R (free=T) -> B");
	CHECK_U(qdwin_layer_exclusive_edge(L | T | B, false, 0), L,
		"anchor L|T|B (free=R) -> L");
	CHECK_U(qdwin_layer_exclusive_edge(R | T | B, false, 0), R,
		"anchor R|T|B (free=L) -> R");

	/* ambiguous combos -> 0 (NONE) */
	CHECK_U(qdwin_layer_exclusive_edge(T | B, false, 0), 0,
		"anchor T|B (two opposite) -> NONE");
	CHECK_U(qdwin_layer_exclusive_edge(T | L, false, 0), 0,
		"anchor T|L (corner) -> NONE");
	CHECK_U(qdwin_layer_exclusive_edge(0, false, 0), 0,
		"anchor 0 -> NONE");
	CHECK_U(qdwin_layer_exclusive_edge(T | B | L | R, false, 0), 0,
		"anchor all four -> NONE");
}

/* helper: run compute_box and assert the rect */
static void expect_box(const char *label, uint32_t anchor,
		       int32_t dw, int32_t dh,
		       int32_t mt, int32_t mr, int32_t mb, int32_t ml,
		       int32_t bx, int32_t by, int32_t bw, int32_t bh,
		       int32_t ex, int32_t ey, uint32_t ew, uint32_t eh)
{
	int32_t x = -12345, y = -12345;
	uint32_t w = 0xDEAD, h = 0xBEEF;
	qdwin_layer_compute_box(anchor, dw, dh, mt, mr, mb, ml,
				bx, by, bw, bh, &x, &y, &w, &h);
	CHECK(x == ex && y == ey && w == ew && h == eh,
	      "%s: got (x=%d y=%d w=%u h=%u) want (x=%d y=%d w=%u h=%u)",
	      label, x, y, (unsigned)w, (unsigned)h,
	      ex, ey, (unsigned)ew, (unsigned)eh);
}

/* ---- compute_box: representative cases ---- */
static void test_compute_box(void)
{
	const uint32_t T = QDWIN_LOGIC_ANCHOR_TOP;
	const uint32_t B = QDWIN_LOGIC_ANCHOR_BOTTOM;
	const uint32_t L = QDWIN_LOGIC_ANCHOR_LEFT;
	const uint32_t R = QDWIN_LOGIC_ANCHOR_RIGHT;
	/* bounding box: full 1920x1080 output at origin */
	const int32_t BX = 0, BY = 0, BW = 1920, BH = 1080;

	/* Full-width top bar: anchored T|L|R, width 0 (stretch), height 32.
	 * x = BX + ml(0) = 0; w = BW - 0 = 1920; anchored top: y = BY + mt(0)
	 * = 0; h = 32. */
	expect_box("full-width top bar (T|L|R, w=0,h=32)",
		   T | L | R, 0, 32, 0, 0, 0, 0,
		   BX, BY, BW, BH,
		   0, 0, 1920, 32);

	/* Full-width bottom bar with bottom margin 10, height 40.
	 * y = BY + BH - h - mb = 0 + 1080 - 40 - 10 = 1030. */
	expect_box("full-width bottom bar (B|L|R, h=40, mb=10)",
		   B | L | R, 0, 40, 0, 0, 10, 0,
		   BX, BY, BW, BH,
		   0, 1030, 1920, 40);

	/* Anchored top-right corner: 300x200, margins t=8 r=16.
	 * a&R (not L): x = BX + BW - w - mr = 1920 - 300 - 16 = 1604.
	 * a&T: y = BY + mt = 8. */
	expect_box("anchored top-right corner (T|R, 300x200, mt=8 mr=16)",
		   T | R, 300, 200, 8, 16, 0, 0,
		   BX, BY, BW, BH,
		   1604, 8, 300, 200);

	/* Anchored bottom-left corner: 100x100, margins b=5 l=7.
	 * a&L: x = BX + ml = 7. a&B: y = BY + BH - h - mb = 1080-100-5=975. */
	expect_box("anchored bottom-left corner (B|L, 100x100, mb=5 ml=7)",
		   B | L, 100, 100, 0, 0, 5, 7,
		   BX, BY, BW, BH,
		   7, 975, 100, 100);

	/* Centered (no anchors), 400x300.
	 * x = BX + BW/2 - w/2 = 960 - 200 = 760.
	 * y = BY + BH/2 - h/2 = 540 - 150 = 390. */
	expect_box("centered (anchor 0, 400x300)",
		   0, 400, 300, 0, 0, 0, 0,
		   BX, BY, BW, BH,
		   760, 390, 400, 300);

	/* Anchored L|R (both horizontal) with fixed width -> centered horiz.
	 * x = BX + BW/2 - w/2 = 960 - 250 = 710. Vertical: a&T only -> top. */
	expect_box("L|R|T fixed-width centered horiz, top vert (500x60)",
		   L | R | T, 500, 60, 4, 0, 0, 0,
		   BX, BY, BW, BH,
		   710, 4, 500, 60);

	/* Exclusive-margin style full-height left strip: anchored T|B|L,
	 * width 50, height 0 (stretch), margins t=12 b=20.
	 * a&L: x = BX + ml = 0; width fixed 50.
	 * h==0: y = BY + mt = 12; h = BH - (mt+mb) = 1080 - 32 = 1048. */
	expect_box("full-height left strip (T|B|L, w=50,h=0, mt=12 mb=20)",
		   T | B | L, 50, 0, 12, 0, 20, 0,
		   BX, BY, BW, BH,
		   0, 12, 50, 1048);

	/* Negative-clamp: width 0 stretch but margins exceed bounds.
	 * Horizontal: w = bw - (ml+mr) = 100 - 160 = -60 -> clamped to 0;
	 *   x = bx + ml = 80.
	 * Vertical: anchor L only (no T/B), h=10 -> else branch centers:
	 *   y = by + bh/2 - h/2 = 540 - 5 = 535. */
	expect_box("negative width clamps to 0",
		   L, 0, 10, 0, 80, 0, 80,
		   0, 0, 100, 1080,
		   80, 535, 0, 10);

	/* Non-zero bounding origin (usable area offset by exclusive zones).
	 * Top bar anchored T|L|R inside box at (0,40)-1920x1040.
	 * x = bx + ml = 0; w = bw = 1920; y = by + mt = 40; h = 30. */
	expect_box("top bar inside offset bounds (by=40)",
		   T | L | R, 0, 30, 0, 0, 0, 0,
		   0, 40, 1920, 1040,
		   0, 40, 1920, 30);

	/* ODD-dimension centered (no anchors), 301x101. Guards against a
	 * `bw/2 - w/2` vs `(bw-w)/2` off-by-one that EVEN dims hide:
	 * x = bx + bw/2 - w/2 = 0 + 960 - 150 = 810  (301/2 == 150, truncated).
	 * y = by + bh/2 - h/2 = 0 + 540 - 50  = 490  (101/2 == 50, truncated).
	 * Note (bw-w)/2 = (1920-301)/2 = 809 != 810, so the kernel's
	 * round-each-half-then-subtract is distinguishable here. */
	expect_box("ODD centered (anchor 0, 301x101)",
		   0, 301, 101, 0, 0, 0, 0,
		   BX, BY, BW, BH,
		   810, 490, 301, 101);

	/* T|B fixed-height -> vertical CENTERING (a&T && a&B branch), odd
	 * height to exercise the /2 truncation. Anchor also has L so the
	 * horizontal axis is left-anchored (not centered) — this isolates the
	 * vertical-centering path and catches an axis transposition.
	 * Horizontal: a&L (not R) -> x = bx + ml = 15.
	 * Vertical: a&T && a&B -> y = by + bh/2 - h/2 = 0 + 540 - 100 = 440
	 *           (201/2 == 100, truncated). h stays 201. */
	expect_box("T|B|L fixed-height vertical-centered (300x201, ml=15)",
		   T | B | L, 300, 201, 0, 0, 0, 15,
		   BX, BY, BW, BH,
		   15, 440, 300, 201);

	/* Horizontal STRETCH (w==0) with NONZERO left/right margins. Guards
	 * against margins being applied on the wrong axis: the vertical margins
	 * (mt/mb) must NOT influence width/x, and ml/mr must NOT influence y.
	 * Anchor T|L|R; w=0 -> stretch, h=50.
	 * Horizontal: w==0 -> x = bx + ml = 0 + 25 = 25;
	 *             w = bw - (ml + mr) = 1920 - (25 + 35) = 1860.
	 * Vertical: a&T -> y = by + mt = 0 + 12 = 12; h = 50. */
	expect_box("horizontal stretch with L/R margins (w=0,h=50, mt=12 mr=35 ml=25)",
		   T | L | R, 0, 50, 12, 35, 0, 25,
		   BX, BY, BW, BH,
		   25, 12, 1860, 50);
}

/* ---- fractional-scale clamp + env validity ---- */
static void test_fractional_scale(void)
{
	/* clamp: below min */
	CHECK_U(qdwin_clamp_fractional_scale_120(0), 30,
		"clamp 0 -> 30 (min)");
	CHECK_U(qdwin_clamp_fractional_scale_120(29), 30,
		"clamp 29 -> 30 (min)");
	/* clamp: boundaries */
	CHECK_U(qdwin_clamp_fractional_scale_120(30), 30, "clamp 30 -> 30");
	CHECK_U(qdwin_clamp_fractional_scale_120(960), 960, "clamp 960 -> 960");
	/* clamp: above max */
	CHECK_U(qdwin_clamp_fractional_scale_120(961), 960,
		"clamp 961 -> 960 (max)");
	CHECK_U(qdwin_clamp_fractional_scale_120(100000), 960,
		"clamp 100000 -> 960 (max)");
	/* clamp: typical in-range values pass through */
	CHECK_U(qdwin_clamp_fractional_scale_120(120), 120, "clamp 120 (1.0x)");
	CHECK_U(qdwin_clamp_fractional_scale_120(180), 180, "clamp 180 (1.5x)");
	CHECK_U(qdwin_clamp_fractional_scale_120(240), 240, "clamp 240 (2.0x)");

	/* env validity gate */
	CHECK(qdwin_fractional_scale_env_valid(30), "env 30 valid");
	CHECK(qdwin_fractional_scale_env_valid(150), "env 150 valid");
	CHECK(qdwin_fractional_scale_env_valid(960), "env 960 valid");
	CHECK(!qdwin_fractional_scale_env_valid(29), "env 29 invalid");
	CHECK(!qdwin_fractional_scale_env_valid(961), "env 961 invalid");
	CHECK(!qdwin_fractional_scale_env_valid(0), "env 0 invalid");
	CHECK(!qdwin_fractional_scale_env_valid(-5), "env -5 invalid");
}

int main(void)
{
	test_accel_profile();
	test_scroll_method();
	test_exclusive_edge();
	test_compute_box();
	test_fractional_scale();

	printf("\n%d checks, %d failures\n", checks, failures);
	if (failures) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
