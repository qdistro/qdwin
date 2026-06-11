/* qdwin-logic.c — implementations of the pure logic kernels declared in
 * qdwin-logic.h. Extracted verbatim from qdwin.c so the arithmetic /
 * enum mapping has a single source of truth that is unit-testable in
 * isolation (no weston, no wayland, no global state).
 */
#include "qdwin-logic.h"

enum libinput_config_accel_profile
qdwin_accel_profile_to_libinput(uint32_t p)
{
	return (p == QDWIN_LOGIC_ACCEL_FLAT)
		? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
		: LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
}

enum libinput_config_scroll_method
qdwin_scroll_method_to_libinput(uint32_t m)
{
	switch (m) {
	case QDWIN_LOGIC_SCROLL_NONE:           return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
	case QDWIN_LOGIC_SCROLL_EDGE:           return LIBINPUT_CONFIG_SCROLL_EDGE;
	case QDWIN_LOGIC_SCROLL_ON_BUTTON_DOWN: return LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
	case QDWIN_LOGIC_SCROLL_TWO_FINGER:
	default:                                return LIBINPUT_CONFIG_SCROLL_2FG;
	}
}

uint32_t
qdwin_layer_exclusive_edge(uint32_t anchor, bool edge_set, uint32_t edge_val)
{
	if (edge_set)
		return edge_val;

#define QLS_T QDWIN_LOGIC_ANCHOR_TOP
#define QLS_B QDWIN_LOGIC_ANCHOR_BOTTOM
#define QLS_L QDWIN_LOGIC_ANCHOR_LEFT
#define QLS_R QDWIN_LOGIC_ANCHOR_RIGHT
	switch (anchor) {
	case QLS_T:                          return QLS_T;
	case QLS_B:                          return QLS_B;
	case QLS_L:                          return QLS_L;
	case QLS_R:                          return QLS_R;
	case (QLS_T | QLS_L | QLS_R):        return QLS_T;
	case (QLS_B | QLS_L | QLS_R):        return QLS_B;
	case (QLS_L | QLS_T | QLS_B):        return QLS_L;
	case (QLS_R | QLS_T | QLS_B):        return QLS_R;
	default:                             return 0;
	}
#undef QLS_T
#undef QLS_B
#undef QLS_L
#undef QLS_R
}

void
qdwin_layer_compute_box(uint32_t anchor,
			int32_t desired_w, int32_t desired_h,
			int32_t margin_top, int32_t margin_right,
			int32_t margin_bottom, int32_t margin_left,
			int32_t bx, int32_t by, int32_t bw, int32_t bh,
			int32_t *out_x, int32_t *out_y,
			uint32_t *out_w, uint32_t *out_h)
{
	const uint32_t T = QDWIN_LOGIC_ANCHOR_TOP;
	const uint32_t B = QDWIN_LOGIC_ANCHOR_BOTTOM;
	const uint32_t L = QDWIN_LOGIC_ANCHOR_LEFT;
	const uint32_t R = QDWIN_LOGIC_ANCHOR_RIGHT;
	uint32_t a = anchor;
	int32_t w = desired_w;
	int32_t h = desired_h;
	int32_t x, y;

	/* Horizontal */
	if (w == 0) {
		x = bx + margin_left;
		w = bw - (margin_left + margin_right);
	} else if ((a & L) && (a & R)) {
		x = bx + bw / 2 - w / 2;
	} else if (a & L) {
		x = bx + margin_left;
	} else if (a & R) {
		x = bx + bw - w - margin_right;
	} else {
		x = bx + bw / 2 - w / 2;
	}

	/* Vertical */
	if (h == 0) {
		y = by + margin_top;
		h = bh - (margin_top + margin_bottom);
	} else if ((a & T) && (a & B)) {
		y = by + bh / 2 - h / 2;
	} else if (a & T) {
		y = by + margin_top;
	} else if (a & B) {
		y = by + bh - h - margin_bottom;
	} else {
		y = by + bh / 2 - h / 2;
	}

	if (w < 0) w = 0;
	if (h < 0) h = 0;

	*out_x = x;
	*out_y = y;
	*out_w = (uint32_t)w;
	*out_h = (uint32_t)h;
}

uint32_t
qdwin_clamp_fractional_scale_120(uint32_t raw_120)
{
	if (raw_120 < QDWIN_LOGIC_FRACTIONAL_MIN)
		return QDWIN_LOGIC_FRACTIONAL_MIN;
	if (raw_120 > QDWIN_LOGIC_FRACTIONAL_MAX)
		return QDWIN_LOGIC_FRACTIONAL_MAX;
	return raw_120;
}

bool
qdwin_fractional_scale_env_valid(long n)
{
	return n >= (long)QDWIN_LOGIC_FRACTIONAL_MIN &&
	       n <= (long)QDWIN_LOGIC_FRACTIONAL_MAX;
}
