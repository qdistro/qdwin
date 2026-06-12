/* qdwin-logic.c — implementations of the pure logic kernels declared in
 * qdwin-logic.h. Extracted verbatim from qdwin.c so the arithmetic /
 * enum mapping has a single source of truth that is unit-testable in
 * isolation (no weston, no wayland, no global state).
 */
#include "qdwin-logic.h"

#include <string.h>

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

bool
qdwin_global_visible(enum qdwin_cred_class cred,
		     enum qdwin_global_kind kind)
{
	switch (kind) {
	case QDWIN_GLOBAL_ORDINARY:
		/* Non-privileged inherited globals are visible to everyone. */
		return true;
	case QDWIN_GLOBAL_INPUT_METHOD:
	case QDWIN_GLOBAL_VIRTUAL_KEYBOARD:
		/* Keystroke capture / injection: hidden from sandboxed silo
		 * clients (so one silo cannot keylog or inject into another),
		 * but available to the shell and ordinary session clients. The
		 * IME/VK bind handlers apply the further allowed_ime_uid + exe
		 * pins, so an ordinary client still can't actually grab. */
		return cred != QDWIN_CRED_SECCTX;
	case QDWIN_GLOBAL_WESTON_CAPTURE:
		/* Screen-pixel capture (whole-output). UNLIKE the IME/VK
		 * managers, libweston binds weston_capture_v1 unconditionally
		 * (no bind-time uid/exe pin — access is deferred to a
		 * screenshot-authority callback, which qdwin does not register,
		 * so capture requests default-deny today). A visible-but-
		 * unbound posture is therefore a LATENT hole: the day any
		 * authority is registered, every client that can see the global
		 * regains capture with no pin. So gate it tightest — shell-only
		 * (02/S1, D7) — denying ordinary AND silo clients, not just
		 * silos. qdshell (the bound shell) is the only legitimate
		 * capturer; an ordinary-uid screenshot product, if ever added,
		 * must register an authority that re-checks identity. */
		return cred == QDWIN_CRED_SHELL;
	case QDWIN_GLOBAL_SECCTX_MANAGER:
		/* Minting security contexts: only the bound shell (or the
		 * authorized secctx-exec helper, both QDWIN_CRED_SHELL) may
		 * even see it. Ordinary and silo clients get nothing. */
		return cred == QDWIN_CRED_SHELL;
	}
	/* Unknown kind: fail closed (deny). A new privileged global added
	 * without a policy row must not default to visible. */
	return false;
}

bool
qdwin_om_mutation_allowed(bool client_is_bound_shell,
			  bool shell_bound,
			  pid_t client_pid, uid_t client_uid,
			  pid_t shell_pid, uid_t shell_uid,
			  uid_t allowed_uid)
{
	if (client_is_bound_shell)
		return true;
	if (shell_bound) {
		return client_pid > 0 && client_pid == shell_pid &&
		       client_uid == shell_uid;
	}
	if (allowed_uid == (uid_t)-1)
		return true;
	return client_uid == allowed_uid;
}

bool
qdwin_secctx_root_launcher_attested(uid_t parent_uid,
				    uint64_t parent_start_before,
				    uint64_t parent_start_after,
				    const char *parent_exe_basename)
{
	bool stable_parent = parent_uid == 0 &&
			     parent_start_before != 0 &&
			     parent_start_after != 0 &&
			     parent_start_before == parent_start_after;

	if (!stable_parent)
		return false;
	if (!parent_exe_basename)
		return false;
	if (strcmp(parent_exe_basename, "runuser") == 0 ||
	    strcmp(parent_exe_basename, "su") == 0 ||
	    strcmp(parent_exe_basename, "sudo") == 0 ||
	    strcmp(parent_exe_basename, "pkexec") == 0)
		return true;
	/* Deliberate fail-open: a more-privileged root parent can make
	 * /proc/<pid>/exe unreadable to the unprivileged compositor. The
	 * direct-root-parent + stable-starttime checks above are the pinned
	 * precondition for accepting the structurally unreadable basename. */
	return parent_exe_basename[0] == '\0';
}

bool
qdwin_layershell_pre_shell_uid_allowed(uid_t client_uid, uid_t allowed_uid)
{
	return client_uid == allowed_uid;
}
