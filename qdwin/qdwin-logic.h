/* qdwin-logic.h — pure, side-effect-free logic kernels extracted from
 * qdwin.c so they can be unit-tested in isolation (tests/unit/
 * test-qdwin-logic.c).
 *
 * Everything here takes plain scalars / out-pointers — NO struct qdwin*,
 * NO weston/wayland types, NO global state. The only non-stdlib include
 * is <libinput.h>, used solely for the libinput config enum return types
 * of the two input-config mappers (qdwin.c already links libinput).
 *
 * qdwin.c keeps the original (struct-taking) wrappers; those wrappers
 * read their struct fields and call these kernels, so there is a single
 * source of truth for the arithmetic/enum logic.
 */
#ifndef QDWIN_LOGIC_H
#define QDWIN_LOGIC_H

#include <stdint.h>
#include <stdbool.h>
#include <libinput.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Input-config enum mappers (v28 set_pointer_config).
 *
 * The qdwin-side enum values are mirrored here as plain constants so
 * the kernels can be compiled without qdwin.c's enum definitions; the
 * values MUST match enum qdwin_accel_profile / enum qdwin_scroll_method
 * in qdwin.c.
 * ------------------------------------------------------------------ */
#define QDWIN_LOGIC_ACCEL_ADAPTIVE       0u
#define QDWIN_LOGIC_ACCEL_FLAT           1u

#define QDWIN_LOGIC_SCROLL_NONE          0u
#define QDWIN_LOGIC_SCROLL_TWO_FINGER    1u
#define QDWIN_LOGIC_SCROLL_EDGE          2u
#define QDWIN_LOGIC_SCROLL_ON_BUTTON_DOWN 3u

/* FLAT → flat, anything else → adaptive. */
enum libinput_config_accel_profile
qdwin_accel_profile_to_libinput(uint32_t p);

/* Maps the qdwin scroll-method enum to libinput's; unknown → 2FG. */
enum libinput_config_scroll_method
qdwin_scroll_method_to_libinput(uint32_t m);

/* ------------------------------------------------------------------
 * zwlr_layer_shell anchor / box geometry.
 *
 * Anchor bits mirror the protocol's anchor enum:
 * ------------------------------------------------------------------ */
#define QDWIN_LOGIC_ANCHOR_TOP     1u
#define QDWIN_LOGIC_ANCHOR_BOTTOM  2u
#define QDWIN_LOGIC_ANCHOR_LEFT    4u
#define QDWIN_LOGIC_ANCHOR_RIGHT   8u

/* Derive the exclusive edge. If edge_set is true (edge_val != 0 in the
 * original), edge_val is returned verbatim. Otherwise it is derived from
 * the anchor bits: single-edge anchor → that edge; three-edge anchor →
 * opposite of the free edge; ambiguous → 0 (NONE). */
uint32_t qdwin_layer_exclusive_edge(uint32_t anchor, bool edge_set,
				    uint32_t edge_val);

/* Pure rectangle layout (port of wlroots
 * wlr_scene_layer_surface_v1_configure). Given the anchor bits, the
 * desired size (0 = stretch to fill that axis), the four margins, and a
 * bounding box (bx,by,bw,bh), writes the placed rect to *out_x/_y/_w/_h.
 * Negative widths/heights are clamped to 0. */
void qdwin_layer_compute_box(uint32_t anchor,
			     int32_t desired_w, int32_t desired_h,
			     int32_t margin_top, int32_t margin_right,
			     int32_t margin_bottom, int32_t margin_left,
			     int32_t bx, int32_t by, int32_t bw, int32_t bh,
			     int32_t *out_x, int32_t *out_y,
			     uint32_t *out_w, uint32_t *out_h);

/* ------------------------------------------------------------------
 * wp_fractional_scale: clamp / env-override arithmetic.
 *
 * Scale unit is 120 (120 = 1.0x, 180 = 1.5x, 240 = 2.0x). The valid
 * fractional range is 30..960 inclusive.
 * ------------------------------------------------------------------ */
#define QDWIN_LOGIC_FRACTIONAL_MIN 30u
#define QDWIN_LOGIC_FRACTIONAL_MAX 960u

/* Clamp a raw 120-unit fractional scale into [30, 960].
 *
 * NOTE: this clamp is intentionally NOT wired into qdwin.c's output scale
 * path today. qdwin.c computes its preferred scale as `current_scale * 120u`
 * and sends that UNCLAMPED (an integer output scale times 120 is always
 * in-range by construction). The only clamp-relevant input today is the
 * QDWIN_FRACTIONAL_SCALE env override, and that path is gated by
 * qdwin_fractional_scale_env_valid() (reject-out-of-range) rather than by
 * clamping. Wire this clamp in only when qdwin adopts true fractional
 * (non-integer-derived) output scales; until then do not assume qdwin
 * clamps output-derived scales. */
uint32_t qdwin_clamp_fractional_scale_120(uint32_t raw_120);

/* True when n is a valid in-range fractional scale (30..960), i.e. the
 * QDWIN_FRACTIONAL_SCALE env override should be honoured. Mirrors the
 * original `n >= 30 && n <= 960` accept-or-fall-through gate. */
bool qdwin_fractional_scale_env_valid(long n);

#ifdef __cplusplus
}
#endif

#endif /* QDWIN_LOGIC_H */
