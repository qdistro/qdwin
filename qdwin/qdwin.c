/*
 * qdwin — qdistro compositor, libweston shell plugin.
 *
 * Phase 6.1 (S2): qdwin is now the weston shell. Registers
 * weston_desktop_api, tracks per-toplevel state on dedicated layers,
 * and emits qdwin_shell_v1 events so a bound shell client can drive
 * decoration and window state.
 *
 * Surfaces start on the "held" layer (WESTON_LAYER_POSITION_HIDDEN)
 * and stay invisible until set_border_color / attach_decoration
 * moves them to "normal" (release_holding). Minimised toplevels move
 * to minimized_layer, which is deliberately *not* set_position'd so
 * weston_view_move_to_layer() calls weston_view_unmap on entry and
 * remaps on exit — matching desktop-shell's minimised pattern.
 * An opaque black background curtain sits on background_layer so the
 * pixman renderer never leaves stale pixels where a view shrank or
 * was unmapped.
 *
 * Spec: doc/qdistro/the architecture doc,
 *       doc/qdistro/ §S2.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE  /* accept4(), SOCK_CLOEXEC for §6.10 secctx accept loop */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libweston/libweston.h>
#include <libweston/desktop.h>
#include <libweston/shell-utils.h>
#include <libweston/xwayland-api.h>
#include <wayland-server-core.h>
#include <X11/Xcursor/Xcursor.h>

/* Exported by the weston frontend binary; not in the installed plugin
 * header, but resolvable at dlopen time against the executable's
 * symbol table (same trick kiosk-shell uses). */
int screenshooter_create(struct weston_compositor *ec);

#include "qdwin-shell-v1-server-protocol.h"
#include "qdwin-locker-v1-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include "ext-idle-notify-v1-server-protocol.h"
#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "cursor-shape-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "primary-selection-unstable-v1-server-protocol.h"
#include "security-context-v1-server-protocol.h"
#include "qdwin-nested-v1-server-protocol.h"
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"

/* §6.8 S1: nested-mode publisher needs the wl_client side; symbols
 * isolated to qdwin-nested-client.c, opaque-pointer interface here. */
#include "qdwin/qdwin-nested-client.h"
#include <libweston/pipewire-plugin.h>

/* §6.5 S5c: libweston-14 exports these from libweston-14.so but does not
 * declare them in the installed plugin header (they're backend-facing).
 * Declare locally so qdwin-shell can act as a virtual input backend for
 * per-stream seats. Signatures tracked against weston 15.0.0 source
 * (compatible with 14.x ABI). */
void weston_seat_init(struct weston_seat *seat,
		      struct weston_compositor *ec,
		      const char *seat_name);
void weston_seat_release(struct weston_seat *seat);
int  weston_seat_init_pointer(struct weston_seat *seat);
int  weston_seat_init_keyboard(struct weston_seat *seat,
			       struct xkb_keymap *keymap);
int  weston_seat_init_touch(struct weston_seat *seat);
void weston_seat_release_pointer(struct weston_seat *seat);
void weston_seat_release_keyboard(struct weston_seat *seat);
void weston_seat_release_touch(struct weston_seat *seat);
void notify_motion_absolute(struct weston_seat *seat,
			    const struct timespec *time,
			    struct weston_coord_global pos);
void notify_button(struct weston_seat *seat,
		   const struct timespec *time,
		   int32_t button,
		   enum wl_pointer_button_state state);
void notify_axis(struct weston_seat *seat,
		 const struct timespec *time,
		 struct weston_pointer_axis_event *event);
void notify_key(struct weston_seat *seat,
		const struct timespec *time,
		uint32_t key,
		enum wl_keyboard_key_state state,
		enum weston_key_state_update update_state);
struct qdwin;

enum qdwin_side {
	QDWIN_SIDE_N = 0,
	QDWIN_SIDE_E = 1,
	QDWIN_SIDE_S = 2,
	QDWIN_SIDE_W = 3,
	QDWIN_SIDES = 4,
};

struct qdwin_chrome {
	/* One per side. The shell owns the wl_surface; we own the view
	 * and a destroy listener so the view tears down if the client
	 * destroys the surface. The commit listener fires when the shell
	 * attaches a new buffer; we use it to damage_below the previous
	 * view geometry so libweston re-renders the area no longer
	 * covered when the chrome shrinks (e.g. after restore-from-max).
	 * Roleless surfaces don't get this for free the way desktop
	 * surfaces do. */
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;
	struct qdwin_toplevel *tl;   /* back-pointer for the listener */
	int side;
};

struct qdwin_toplevel;
struct qdwin_popup;
struct qdwin_view_stream;

static void qdwin_chrome_detach(struct qdwin_chrome *c);
static void qdwin_toplevel_position_chrome(struct qdwin_toplevel *tl);
static void qdwin_move_grab_end_for(struct qdwin *qdwin, uint32_t handle);
/* §6.8 S1 nested-mode forward decls (definitions near wet_shell_init). */
static void qdwin_nested_publish_toplevel(struct qdwin_toplevel *tl);
static void qdwin_nested_unpublish_toplevel(struct qdwin_toplevel *tl);
/* §6.8 S3 input-sink callbacks (definitions near nested-mode publisher). */
static int qdwin_nested_input_sink_listen_cb(int fd, uint32_t mask, void *data);
static int qdwin_nested_input_sink_peer_cb(int fd, uint32_t mask, void *data);
/* §6.8 S2 outer-proxy forward decls (definitions near wet_shell_init). */
struct qdwin_nested_toplevel;
static struct qdwin_toplevel *
qdwin_nested_proxy_create(struct qdwin *qdwin,
			  struct qdwin_nested_toplevel *owner,
			  const char *app_id, const char *title,
			  uint32_t origin_uid, int w, int h);
static void qdwin_nested_proxy_destroy(struct qdwin_toplevel *tl);
static void qdwin_nested_proxy_set_title(struct qdwin_toplevel *tl,
					 const char *title);
static void qdwin_nested_proxy_set_app_id(struct qdwin_toplevel *tl,
					  const char *app_id);
static void qdwin_nested_proxy_set_geometry(struct qdwin_toplevel *tl,
					    int w, int h);
/* §6.8 S4 admin-gate forward decls. */
static void qdwin_handle_nested_proxy_decision(struct wl_client *client,
					       struct wl_resource *resource,
					       uint32_t handle,
					       uint32_t decision,
					       const char *reason);
static void qdwin_toplevel_autofocus_if_ready(struct qdwin_toplevel *tl);
static void qdwin_toplevel_release_holding(struct qdwin_toplevel *tl,
					   const char *cause);
static struct qdwin_toplevel *
qdwin_toplevel_from_handle(struct qdwin *qdwin, uint32_t handle);
/* §6.8 cursor-sprite full theme forward decl (impl table at L3577 needs
 * the symbol; definition lives near the rest of the cursor-shape code). */
static void qdwin_handle_set_cursor_sprite(struct wl_client *client,
					   struct wl_resource *resource,
					   uint32_t shape,
					   struct wl_resource *surface_resource,
					   int32_t hotspot_x,
					   int32_t hotspot_y);

/* §6.8 S2b bind_proxy_pixels forward decl. */
static void qdwin_handle_bind_proxy_pixels(struct wl_client *client,
					   struct wl_resource *resource,
					   uint32_t handle,
					   struct wl_resource *surface);
/* Send close_requested on the nested-toplevel resource owned by the
 * proxy. Defined alongside the full qdwin_nested_toplevel struct
 * (qdwin_nested_toplevel is opaque at the point this is called from
 * qdwin_handle_request_close). */
static void qdwin_nested_proxy_send_close(struct qdwin_toplevel *tl);
static void qdwin_popup_teardown(struct qdwin_popup *p);
static void qdwin_view_stream_unpin(struct qdwin_view_stream *s);
static void qdwin_stream_seat_init(struct qdwin_view_stream *s);
static void qdwin_stream_seat_release(struct qdwin_view_stream *s);
/* spec/10 clear_selection forward decls — defined in the
 * primary-selection-unstable-v1 block far below, but the qdwin_shell
 * impl table at line ~3600 needs them. */
struct qdwin_primary_seat;
static struct qdwin_primary_seat *
qdwin_primary_seat_find(struct qdwin *qdwin, struct weston_seat *seat);
static void
qdwin_primary_seat_clear_selection(struct qdwin_primary_seat *pseat,
				   int notify_source);
/* spec/09 activation_decision forward decl — defined in the
 * xdg-activation-v1 block far below. */
static void
qdwin_handle_activation_decision(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t handle,
				 uint32_t decision,
				 const char *reason);
struct qdwin;  /* not yet defined here */
static void qdwin_activation_pending_free_all(struct qdwin *qdwin);
static void qdwin_data_offer_pending_free_all(struct qdwin *qdwin);
static void qdwin_data_source_wraps_free_all(struct qdwin *qdwin);
static void qdwin_handle_data_offer_receive_decision(
	struct wl_client *client, struct wl_resource *resource,
	uint32_t handle, const char *decision);
/* §6.10 forward decls — definitions live near wet_shell_init. */
static void qdwin_secctx_destroy_all(struct qdwin *qdwin);
struct qdwin_secctx_client;
static struct qdwin_secctx_client *
qdwin_secctx_client_lookup(struct qdwin *qdwin, struct wl_client *client);
static const char *qdwin_secctx_client_engine(struct qdwin_secctx_client *sc);
static const char *qdwin_secctx_client_app_id(struct qdwin_secctx_client *sc);
static const char *qdwin_secctx_client_instance_id(struct qdwin_secctx_client *sc);
/* Option-B identity accessors — see todo/decisions/secctx-identity-contract.md. */
static uint32_t qdwin_secctx_client_peer_pid(struct qdwin_secctx_client *sc);
static uint64_t qdwin_secctx_client_peer_starttime(struct qdwin_secctx_client *sc);
static uint32_t qdwin_secctx_client_peer_uid(struct qdwin_secctx_client *sc);
static const char *qdwin_secctx_client_peer_exe(struct qdwin_secctx_client *sc);
static const char *qdwin_secctx_client_peer_selinux_label(struct qdwin_secctx_client *sc);
static bool
qdwin_secctx_global_filter(const struct wl_client *client,
			   const struct wl_global *global, void *data);
static void
bind_qdwin_secctx_manager(struct wl_client *client, void *data,
			  uint32_t version, uint32_t id);

/* toplevel_state bits — must match qdwin-shell-v1.xml's event docs. */
#define QDWIN_TS_MAXIMIZED  (1u << 0)
#define QDWIN_TS_FULLSCREEN (1u << 1)
#define QDWIN_TS_MINIMIZED  (1u << 2)
#define QDWIN_TS_URGENT     (1u << 3)
#define QDWIN_TS_FOCUSED    (1u << 4)
#define QDWIN_TS_FLOATING   (1u << 5)

struct qdwin_toplevel {
	struct qdwin *qdwin;
	struct weston_desktop_surface *desktop_surface;
	struct weston_view *view;
	uint32_t handle;
	int mapped;
	int decorated;
	int last_width;
	int last_height;
	/* §6.8 S1 (nested mode): set when this toplevel has been pinned to
	 * a pipewire output and advertised to the outer qdwin. NULL on
	 * outer-mode shells. */
	struct weston_output *nested_pw_output;
	struct qdwin_nested_client_pub *nested_pub;
	struct qdwin_nested_input_sink *nested_input_sink;  /* nested-side */
	struct wl_event_source *nested_input_sink_source;
	struct wl_event_source *nested_input_peer_source;
	int32_t nested_configured_w;
	int32_t nested_configured_h;
	/* §6.8 S3b: per-toplevel weston_seat for inner-client input
	 * delivery. Created lazily on first non-PING packet so PING-only
	 * S3-mvp probes don't pay the seat-init cost. Pointer + keyboard
	 * focus are pinned to tl->view (the inner client's surface). */
	struct weston_seat nested_inner_seat;
	bool nested_inner_seat_inited;
	/* §6.8 S2 (outer mode): set when this toplevel was synthesised
	 * from a qdwin_nested_v1.advertise_toplevel — proxy_curtain owns
	 * tl->view (the placeholder pixels), proxy_app_id / proxy_title
	 * cache the metadata since desktop_surface is NULL for proxies. */
	bool is_nested_proxy;
	struct weston_curtain *proxy_curtain;
	char *proxy_app_id;
	char *proxy_title;
	uint32_t proxy_origin_uid;
	/* §6.8 S2b: when the admin shell calls bind_proxy_pixels, the
	 * compositor swaps the placeholder curtain view for a view of the
	 * bound surface. proxy_pixel_view is the active view (== tl->view),
	 * proxy_pixel_surface is the underlying weston_surface (lifetime
	 * tied to the consumer wl_client). The destroy listener restores
	 * the placeholder curtain when the consumer goes away. */
	struct weston_surface *proxy_pixel_surface;
	struct weston_view *proxy_pixel_view;
	struct wl_listener proxy_pixel_destroy_listener;
	/* §6.8 S3: outer-side fd to the nested-published input sink. -1
	 * when no sink (e.g. pre-S3 advertise with input_sink=""). Used
	 * to send PING + (S3b) motion/button/key packets. */
	int proxy_input_sink_fd;
	/* §6.8 S4: pending admin gating. Proxy stays held until shell
	 * issues nested_proxy_decision. Cleared on any decision (allow
	 * → moves to normal; deny → destroys; defer → stays held with
	 * pending=true). For pre-v8 shells this stays false (auto-allow). */
	bool nested_proxy_pending_decision;
	/* Back-ref so the qdwin_nested_toplevel resource destroy can
	 * tear down the proxy. NULL on non-proxy toplevels. */
	struct qdwin_nested_toplevel *proxy_nested_owner;
	/* Chrome inset, derived from attached chrome surfaces' sizes. */
	int inset_n, inset_e, inset_s, inset_w;
	/* Last outer size we asked the client to configure to, so repeated
	 * attach_decoration calls don't ratchet the window smaller. */
	int outer_width;
	int outer_height;
	/* toplevel_state bitmask (QDWIN_TS_*). */
	uint32_t state;
	/* P05a: per-toplevel chrome border colour (ARGB, big-endian RGBA8888
	 * as set_border_color receives it). Defaults to 0 (= unset → fall
	 * back to qdshell's neutral chrome). qdshell paints the actual
	 * chrome surfaces via qdwin_shell_v1.attach_decoration; this field
	 * captures the per-toplevel value as received so (a) the SSD paint
	 * helper has a single source of truth for the silo-colour, (b) unit
	 * tests can introspect the per-toplevel state instead of a global,
	 * and (c) re-issued attach_decoration calls don't lose the colour
	 * the shell set earlier. */
	uint32_t border_rgba;
	int border_rgba_set;
	/* Restore geometry — captured on entry to maximise, consumed on
	 * un-maximise. Outer dims (chrome-inclusive). */
	int saved_outer_w, saved_outer_h;
	double saved_x, saved_y;
	/* Chrome surfaces/views, indexed by qdwin_side. */
	struct qdwin_chrome chrome[QDWIN_SIDES];
	/* At most one popup anchored to this toplevel; a second
	 * show_popup dismisses the prior. NULL when no popup active. */
	struct qdwin_popup *popup;
	/* Last title we sent via toplevel_added or toplevel_title; we
	 * compare on every commit to detect xdg_toplevel.set_title and
	 * forward the change to qdshell. NULL until the first publish. */
	char *cached_title;
	/* Last app_id observed on this toplevel. xdg_toplevel.set_app_id
	 * post-map currently logs only — no protocol event yet, so
	 * qdshell can't react. Once a toplevel_app_id event lands in
	 * qdwin-shell-v1, send it here on diff. NULL until first
	 * commit. */
	char *cached_app_id;
	struct wl_list link;           /* qdwin::toplevels */
};

struct qdwin_popup {
	struct wl_resource *resource;  /* qdwin_popup_v1 to the shell */
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_listener surface_destroy;
	struct qdwin_toplevel *parent;
	int x, y;                      /* parent-local placement */
	struct weston_pointer_grab grab;  /* §6.4 S5: dismiss-on-outside */
	int grab_active;
};

/* §6.5 S2: per-view forwarding. A stream pins one toplevel's content
 * view onto a pipewire output, so the view renders off the main RDP
 * desktop and onto an out-of-band PipeWire node that an external
 * qdistro-forward process serves to remote RDP clients.
 *
 * S2 scope: pin + emit approved with the pipewire node name.
 * qdistro-forward spawn + input injection are S3/S4. */
struct qdwin_view_stream {
	struct wl_resource *resource;     /* qdwin_view_stream_v1 */
	struct qdwin *qdwin;
	struct qdwin_toplevel *tl;
	struct weston_output *pw_output;  /* pipewire output currently pinned */
	struct weston_output *prev_output; /* restore target on teardown */
	struct weston_coord_global prev_pos;
	int pinned;                       /* 1 while pw_output is in use */

	/* S3: external qdistro-forward proxy lifecycle */
	pid_t forward_pid;                /* 0 if not spawned */
	uint32_t rdp_port;                /* 0 if not spawned */
	char access_token[33];            /* 32 hex chars + NUL */
	char rdp_password[17];            /* 16 hex chars + NUL */

	/* S5: input-injection channel claimed by qdistro-forward */
	int input_claimed;                /* 1 once claim() succeeded */
	struct wl_resource *input_handle; /* qdwin_stream_input_handle_v1, if claimed */

	/* S5c: virtual input device for this view, fed by whichever wl_client
	 * currently holds the stream_input_handle. Independent of the main
	 * RDP-desktop seat so remote input and local input can't race over
	 * focus; focus on this seat is pinned to tl->view. */
	struct weston_seat stream_seat;
	int seat_inited;

	struct wl_list link;              /* qdwin::view_streams */
};

struct qdwin {
	struct weston_compositor *compositor;
	struct weston_desktop *desktop;
	struct wl_listener destroy_listener;

	/* Cached at first use; may be NULL if xwayland.so not loaded. Used
	 * by qdwin_send_toplevel_added to set the is_xwayland flag. */
	const struct weston_xwayland_surface_api *xwayland_surface_api;
	int xwayland_api_resolved;
	int cursor_default_warned;  /* one-shot for "no surface yet" log */

	/* qdwin_shell_v1 */
	struct wl_global *shell_global;
	struct wl_resource *shell_resource;
	uid_t allowed_uid;
	int locked;
	int shell_bound;

	/* qdwin_locker_v1 — peer locker (qdlocker). Same trust shape as
	 * the shell binding but on its own global so the locker is a
	 * separate process from the shell. See doc/locker.md. */
	struct wl_global *locker_global;
	struct wl_resource *locker_resource;
	uid_t allowed_locker_uid;
	/* Which protocol owns the currently-attached lock_resource.
	 * 0 = shell (qdwin_lock_surface_v1), 1 = locker
	 * (qdwin_locker_surface_v1). Used so the destroy/dismiss path
	 * sends the right opcode on the right interface. */
	int lock_resource_is_locker;
	/* Set while we're mid-replace of an attached lock surface (the
	 * locker reattaches). Suppresses the spurious
	 * `locked_changed=0` that the destroy callback would otherwise
	 * fire when the old resource is torn down — the new surface is
	 * about to be attached and the locked state should stay at 1
	 * across the swap. Without this, observers see a 1→0→1 flap. */
	int lock_resource_reattach_in_progress;

	/* Layering (see spec/03). */
	struct weston_layer background_layer;
	struct weston_layer held_layer;       /* HIDDEN until shell acks */
	struct weston_layer normal_layer;
	struct weston_layer minimized_layer;  /* unmapped; minimised views */
	struct weston_layer panel_layer;      /* §6.6 S1: panels/bars above normal */
	struct weston_layer notification_layer; /* §6.6 S2: bubbles above panels */
	struct weston_layer launcher_layer;     /* §6.6 S3/S4: launcher + switcher */
	struct weston_layer lock_layer;         /* §6.6 S5: compositor lock overlay */
	struct weston_layer popup_layer;      /* above normal; chrome menus */
	/* zwlr_layer_shell_v1: BACKGROUND/BOTTOM/TOP/OVERLAY (indices 0..3
	 * matching the protocol enum). Positioned just outside qdwin's own
	 * panel ladder so external panels (waybar et al.) coexist with
	 * qdwin's native panels — see wet_shell_init for the chosen
	 * position values. */
	struct weston_layer layer_shell_layer[4];

	/* §6.6 S5: single lock view (one per shell). `lock_view` is valid
	 * only while a qdwin_lock_surface_v1 resource is alive. */
	struct weston_surface *lock_surface;
	struct weston_view *lock_view;
	struct wl_resource *lock_resource;
	struct wl_listener lock_surface_destroy;
	struct wl_listener lock_surface_commit;

	/* §6.6 S1: active panels. Each entry reserves an exclusive zone on
	 * one output edge; maximise computes its target rect from
	 * output_rect minus the panels' zones. */
	struct wl_list panels;                /* struct qdwin_panel::link */

	/* §6.6 S2: active notification bubbles. Corner-anchored, no
	 * exclusive-zone impact. */
	struct wl_list notifications;         /* struct qdwin_notification::link */

	/* §6.6 S3/S4: launcher + switcher surfaces (same layer). */
	struct wl_list launchers;             /* struct qdwin_launcher::link */

	/* Switcher keyboard grab: Alt+Tab requires watching Alt-release
	 * directly because weston's modifier_binding only fires when the
	 * modifier was pressed alone (Tab pressed during Alt-hold
	 * disqualifies it). The grab's modifiers callback detects the
	 * Alt-released transition and fires switcher_commit. */
	struct weston_keyboard_grab switcher_grab;
	int switcher_grab_active;

	/* Titlebar drag (qdwin_shell_v1.begin_interactive_move).
	 * At most one drag at a time across all seats; second begin
	 * requests while active are silently ignored, and
	 * maximised/fullscreen toplevels refuse silently per XML. */
	struct weston_pointer_grab move_grab;
	int move_grab_active;
	uint32_t move_grab_handle;
	double move_grab_anchor_dx;  /* pointer.x - view.origin.x at start */
	double move_grab_anchor_dy;  /* pointer.y - view.origin.y at start */

	/* v19: shell-driven global hotkeys. Each entry holds a
	 * weston_compositor_add_key_binding registration; the handler
	 * sends qdwin_shell_v1.hotkey_pressed(id) on the shell resource.
	 * Bindings are torn down on shell unbind or unregister_hotkey. */
	struct wl_list hotkeys;             /* struct qdwin_hotkey::link */

	/* Overlay keyboard grab (B3+B4): captures keys while a launcher
	 * (kind=0) or lock_surface is visible and forwards each press to
	 * the shell as overlay_key (qdwin_shell_v1 v17). The shell decodes
	 * keysym + utf8 and routes to its launcher / locker state machine.
	 * role: 0=launcher, 2=locker (1=switcher uses switcher_grab). */
	struct weston_keyboard_grab overlay_grab;
	int overlay_grab_active;
	uint32_t overlay_grab_role;

	/* Opaque black curtain covering the whole output. Ensures pixels
	 * are overwritten every frame — without this, the pixman renderer
	 * only repaints damaged regions, so stale chrome persists after
	 * a view shrinks or is unmapped (chrome shrink + minimise bugs).
	 *
	 * The curtain is hidden when a v18 shell calls attach_background;
	 * the shell-owned background surface takes its place on the
	 * background_layer. The curtain stays around as a fallback for
	 * pre-v18 shells and as a safety net if the shell's background
	 * is destroyed. */
	struct weston_curtain *background;
	/* v18 shell-owned desktop background. NULL until attach_background. */
	struct qdwin_background *shell_background;
	struct wl_listener output_created_listener;
	struct wl_listener output_resized_listener;
	struct wl_listener output_destroyed_listener;
	struct wl_listener seat_created_listener;

	/* One qdwin_seat_tracker per live weston_seat. Each carries a
	 * wl_listener on the seat's destroy_signal so we can fire a
	 * seat_removed event before the seat goes. */
	struct wl_list seat_trackers;     /* struct qdwin_seat_tracker::link */

	struct wl_list toplevels;          /* struct qdwin_toplevel::link */
	uint32_t next_handle;

	/* §6.5 S2: active per-view streams (struct qdwin_view_stream::link) */
	struct wl_list view_streams;
	uint32_t next_stream_port;        /* S3: allocator for qdistro-forward */

	/* §6.5 S5: per-stream input-injection global. Advertised to all
	 * clients; protected by access_token in claim(). */
	struct wl_global *stream_input_global;

	/* §6.7: xdg-activation-v1 (focus-stealing prevention + launcher
	 * tokens). One global advertised to all clients; tokens tracked
	 * on qdwin::activation_tokens. */
	struct wl_global *xdg_activation_global;
	struct wl_list activation_tokens;  /* struct qdwin_activation_token::link */
	uint32_t activation_token_counter; /* monotonic, for debug logging */
	/* spec/09: pending broker-gated activations. Each entry holds the
	 * stalled activate request until the shell calls
	 * activation_decision; on allow we run the original activate
	 * body, on deny we just free the token. */
	struct wl_list activation_pending;  /* qdwin_activation_pending::link */
	uint32_t activation_pending_next_handle;

	/* spec/10 v15 — wl_data_offer.receive per-MIME gating. The
	 * compositor wraps the active selection's data_source.send
	 * callback so receive calls land in our shim; pending entries
	 * hold the destination's pipe fd until the shell decides via
	 * qdwin_shell_v1.data_offer_receive_decision. */
	struct wl_list data_source_wraps;     /* qdwin_data_source_wrap::link */
	struct wl_list data_offer_pending;    /* qdwin_data_offer_pending::link */
	uint32_t data_offer_receive_next_handle;

	/* §6.7: ext-idle-notify-v1 + idle-inhibit-unstable-v1. One global
	 * per interface; notifications tracked for broadcast on weston's
	 * idle_signal / wake_signal. Inhibitors bump ec->idle_inhibit so
	 * weston's own idle timer defers. */
	struct wl_global *idle_notifier_global;
	struct wl_global *idle_inhibit_manager_global;
	struct wl_list idle_notifications;  /* struct qdwin_idle_notification::link */
	struct wl_list idle_inhibitors;     /* struct qdwin_idle_inhibitor::link */
	struct wl_listener idle_signal_listener;
	struct wl_listener wake_signal_listener;
	/* §6.7(a) follow-up: when weston's built-in idle timer is disabled
	 * (`idle-time=0` in weston.ini), idle_signal never fires. We fall
	 * back to a compositor-internal activity tracker: each notification
	 * arms its own wl_event_source for `timeout_ms` on create, and
	 * wake_signal (fired by weston_compositor_wake on input activity)
	 * rearms them. Effective semantics: notification fires exactly
	 * `timeout_ms` after last activity, bypassing weston's coarse
	 * idle state entirely. 1 when compositor->idle_time == 0, else 0. */
	int idle_internal_mode;

	/* §6.7: cursor-shape-v1 (accept + log; visual mapping deferred),
	 * wp_fractional_scale_v1 (spike: advertise, return preferred=120). */
	struct wl_global *cursor_shape_manager_global;
	struct wl_global *fractional_scale_manager_global;

	/* §6.7(b) cursor-shape theme cache. Loaded once at init via
	 * libXcursor; one XcursorImages* per protocol shape. Nominal
	 * cursor size (theme-dependent; we ask for 24). When a shape's
	 * image isn't present in the theme (theme packs vary), the
	 * entry stays NULL and set_shape logs a miss. */
	/* Indexed by shape enum; slot 0 unused (enum starts at 1).
	 * Highest value today is ALL_RESIZE = 36 → size 37. */
	XcursorImages *cursor_images[37];
	char *cursor_theme_name;
	int cursor_size;

	/* §6.8 cursor-sprite full theme: per-shape registered sprite
	 * surfaces (qdwin_shell_v1.set_cursor_sprite, v10). On set_shape
	 * the compositor preferred this cached entry over the solid
	 * fallback. The destroy listener clears the slot when the
	 * shell-owned wl_surface goes away. */
	struct qdwin_cursor_sprite {
		struct weston_surface *surface;
		int32_t hotspot_x, hotspot_y;
		struct wl_listener destroy_listener;
		/* plan3 M2: re-assert the Weston cursor-surface invariant on
		 * every commit (libweston/input.c:pointer_cursor_surface_committed
		 * clears pending+current input on each commit, not only at
		 * install time). Without this, a sprite helper that recommits
		 * with a non-empty pending.input would silently make the cursor
		 * pickable again. */
		struct wl_listener commit_listener;
		struct qdwin *qdwin;
		uint32_t shape;     /* back-ref so the listener can find us */
	} cursor_sprites[37];

	/* §6.7(c) fractional-scale tracking. One qdwin_fractional_scale
	 * per live wp_fractional_scale_v1 resource. */
	struct wl_list fractional_scales;  /* qdwin_fractional_scale::link */

	/* §6.7 primary-selection-unstable-v1. One global for the
	 * device-manager; per-seat selection state lives on
	 * qdwin_primary_seat entries. */
	struct wl_global *primary_selection_manager_global;
	/* §6.10: wp_security_context_manager_v1. One global; one
	 * qdwin_secctx per registered listener; per-client tag for
	 * "this client came through listener X" lookup. */
	struct wl_global *security_context_manager_global;
	struct wl_list secctxs;          /* qdwin_secctx::link */
	struct wl_list secctx_clients;   /* qdwin_secctx_client::link */

	/* §6.8 S0/S1 (2026-04-25): qdwin_nested_v1 — nested-compositor
	 * native-toplevel passthrough. Manager-level global is peer-uid-
	 * filtered (like qdwin_shell_v1). S0 shipped the bind + advertise
	 * stub, S1 wires the publisher side: when QDWIN_NESTED_MODE=1, this
	 * shell is running INSIDE a nested compositor and acts as a
	 * publisher, binding the outer compositor's manager and forwarding
	 * each inner toplevel as a peer toplevel. See
	 * . */
	struct wl_global *nested_manager_global;
	struct wl_list primary_seats;  /* qdwin_primary_seat::link */
	struct wl_list nested_toplevels;  /* qdwin_nested_toplevel::link */

	/* Nested-mode publisher state. Active iff nested_mode is true. */
	bool nested_mode;
	struct qdwin_nested_client *nested_client;
	struct wl_event_source *nested_outer_event_source;
	int nested_next_pw_id;        /* monotonic for output naming */

	/* §6.8 S3b outer-side: which proxy currently has pointer focus
	 * (for QDNI motion/button/axis encoding). NULL when focus is on
	 * a normal toplevel or no view. Updated by the default_pointer_
	 * grab override; cleared on proxy destroy. */
	struct qdwin_toplevel *active_input_proxy;

	/* zwlr_layer_shell_v1 v5: external panels/notifications/lockscreens.
	 * Stub-stage implementation — accepts the protocol and completes
	 * the configure/ack handshake but does not yet lay out, anchor,
	 * or reserve exclusive zones. Goal at this stage: waybar (via
	 * gtk-layer-shell-0) binds the global, sends get_layer_surface +
	 * set_size + set_anchor + initial commit, and receives a configure
	 * back without protocol error. The next iteration ports
	 * wlroots scene/layer_shell_v1.c layout math + integrates with
	 * the existing weston_layer ladder. */
	struct wl_global *layer_shell_global;
	struct wl_list layer_surfaces;  /* qdwin_layer_surface::link */
	uint32_t layer_configure_serial_next;

	/* zxdg_decoration_manager_v1: always-server_side stub. qdshell
	 * draws chrome via qdwin_shell_v1, so toolkits should not try
	 * client-side decorations. See bind_qdwin_xdg_decoration_manager. */
	struct wl_global *xdg_decoration_manager_global;
};

struct qdwin_seat_tracker;

/* §6.7 v2 event forwarders — defined later in this file. Forward-
 * declared here because bind_as_shell() replays them before the
 * definitions are in scope. */
static void qdwin_send_seat_created(struct qdwin *qdwin,
				    struct weston_seat *seat);
static void qdwin_send_output_created_evt(struct qdwin *qdwin,
					  struct weston_output *output);
static struct qdwin_seat_tracker *
qdwin_track_seat(struct qdwin *qdwin, struct weston_seat *seat);
/* spec/10 v14: replay seat_focus_changed for every tracked seat once
 * the shell binds. Defined near the seat_tracker struct. */
static void qdwin_replay_seat_focus_for_shell(struct qdwin *qdwin);
/* B3+B4 overlay grab — defined ~3700 lines below; referenced from
 * qdwin_launcher_resource_destroyed which appears before. */
static void qdwin_overlay_grab_start(struct qdwin *qdwin, uint32_t role);
static void qdwin_overlay_grab_end(struct qdwin *qdwin);
/* B6 default-cursor — defined ~6200 lines below; referenced from the
 * seat-tracker pointer-focus listener installer. */
static void qdwin_install_default_cursor_on_pointer(struct qdwin *qdwin,
						    struct weston_pointer *pointer);
static void qdwin_default_cursor_on_focus_changed(struct wl_listener *l,
						  void *data);
/* spec/10 v14 helpers — bodies live below the seat_tracker struct
 * because they walk seat_trackers via wl_list_for_each on the `link`
 * field which needs the struct to be complete. */
static void qdwin_seat_emit_focus_now(struct qdwin_seat_tracker *tr);
static void qdwin_seat_focus_recover_idle_cb(void *data);
static void qdwin_emit_seat_focus_changed(struct qdwin *qdwin,
					  struct weston_seat *seat,
					  uint32_t handle);
static struct qdwin_seat_tracker *
qdwin_seat_tracker_for_seat(struct qdwin *qdwin, struct weston_seat *seat);
/* spec/10 v16 accessors — forward-declared so set_keyboard_focus_v2
 * (which lives near the rest of the request handlers, above the
 * seat_tracker struct definition) can read/write last_target_silo
 * without seeing the struct internals. Both no-op on NULL tr. */
static const char *qdwin_seat_tracker_silo(struct qdwin_seat_tracker *tr);
static void qdwin_seat_tracker_set_silo(struct qdwin_seat_tracker *tr,
					const char *silo);
/* §6.7(b) cursor-shape theme lifecycle, defined near the cursor-shape
 * block lower in the file. */
static void qdwin_cursor_theme_load(struct qdwin *qdwin);
static void qdwin_cursor_theme_destroy(struct qdwin *qdwin);
/* §6.7(c) fractional-scale re-broadcast on output changes. */
static void qdwin_fractional_scale_broadcast(struct qdwin *qdwin);
/* §6.6 S1 panel work-area helper — forward-declared so request_maximize
 * can call it before the panel block is defined. */
static void qdwin_output_work_area(struct qdwin *qdwin,
				   struct weston_output *out,
				   int *x, int *y, int *w, int *h);
/* Shared window-state cores, called from both the qdshell custom
 * protocol handlers and the standard xdg_toplevel.{set_maximized,
 * set_fullscreen,set_minimized} desktop_api callbacks. Without the
 * desktop_api wiring these requests are silently dropped by libweston
 * (libweston-desktop.c NULL-checks each callback before dispatch), so
 * client-drawn maximize/fullscreen/minimize buttons appear clickable
 * but do nothing. Forward-declared so the desktop_api struct above
 * the protocol handlers can reference them. */
struct qdwin_toplevel;
static void qdwin_toplevel_set_maximized(struct qdwin *qdwin,
					 struct qdwin_toplevel *tl,
					 bool maximized);
static void qdwin_toplevel_set_fullscreen(struct qdwin *qdwin,
					  struct qdwin_toplevel *tl,
					  bool fullscreen,
					  struct weston_output *output);
static void qdwin_toplevel_set_minimized(struct qdwin *qdwin,
					 struct qdwin_toplevel *tl);
static void qdwin_panels_on_output_change(struct qdwin *qdwin);
/* zwlr_layer_shell_v1 zone subtraction — defined after struct
 * qdwin_layer_surface; called from qdwin_output_work_area. */
static void qdwin_layer_shell_subtract_zones(struct qdwin *qdwin,
					     struct weston_output *out,
					     int *x, int *y, int *w, int *h);
struct qdwin_layer_surface;
static struct qdwin_layer_surface *
qdwin_layer_surface_at_pos(struct qdwin *qdwin, struct weston_coord_global pos);
static struct weston_view *
qdwin_layer_surface_view_at_pos(struct qdwin *qdwin,
				struct weston_coord_global pos);
/* plan3 M4: forward-declared so qdwin_proxy_default_grab_button (which
 * sits above the qdwin_layer_surface struct definition) can call it. */
static void
qdwin_layer_surface_handle_on_demand_button(struct qdwin *qdwin,
					    struct weston_pointer *pointer);
/* §6.6 S3/S4 keybinding handlers — forward-declared because they are
 * registered in wet_shell_init which precedes their definitions. */
static void qdwin_on_launcher_key(struct weston_keyboard *kb,
				  const struct timespec *t,
				  uint32_t key, void *data);
static void qdwin_on_switcher_key(struct weston_keyboard *kb,
				  const struct timespec *t,
				  uint32_t key, void *data);
static void qdwin_on_switcher_back_key(struct weston_keyboard *kb,
				       const struct timespec *t,
				       uint32_t key, void *data);
static void qdwin_on_alt_released(struct weston_keyboard *kb,
				  enum weston_keyboard_modifier mod,
				  void *data);
/* §6.6 S5 full — Ctrl+Alt+L manual lock. */
static void qdwin_on_lock_key(struct weston_keyboard *kb,
			      const struct timespec *t,
			      uint32_t key, void *data);

/* B6: retry cursor install when pointer isn't ready at seat creation */
static int qdwin_cursor_retry_timer_cb(void *data);
static int qdwin_cursor_retry_install(struct qdwin *qdwin);

/* ------------------------------------------------------------------
 * Toplevel bookkeeping helpers.
 * ------------------------------------------------------------------ */

static struct qdwin_toplevel *
qdwin_toplevel_from_handle(struct qdwin *qdwin, uint32_t handle)
{
	struct qdwin_toplevel *tl;
	wl_list_for_each(tl, &qdwin->toplevels, link)
		if (tl->handle == handle)
			return tl;
	return NULL;
}

static uid_t
qdwin_client_uid(struct weston_desktop_surface *dsurf)
{
	struct weston_desktop_client *dclient =
		weston_desktop_surface_get_client(dsurf);
	if (!dclient)
		return (uid_t)-1;
	struct wl_client *client =
		weston_desktop_client_get_client(dclient);
	/* libweston-desktop returns dclient->client == NULL for surfaces
	 * spawned by xwayland.so: the X11 toplevel has no wl_client of its
	 * own, only the parent Xwayland server does. wl_client_get_credentials
	 * dereferences its argument, so without this guard the first X11
	 * client connect SIGSEGVs weston (verified via coredumpctl bt). For
	 * XWayland surfaces we currently have no peer-uid signal — return
	 * the compositor's own uid; the shell can attribute via the
	 * forthcoming is_xwayland flag (currently hard-coded 0) once that
	 * lands. */
	if (!client)
		return getuid();
	pid_t pid; uid_t uid; gid_t gid;
	wl_client_get_credentials(client, &pid, &uid, &gid);
	(void)pid; (void)gid;
	return uid;
}

/* §6.10 follow-up: forward a toplevel's secctx tag to the shell as
 * `toplevel_security_context(handle, sandbox_engine, app_id, instance_id)`,
 * but only for shells bound at qdwin_shell_v1@v13 or later. Toplevels
 * whose client has no tag (unsandboxed) emit nothing — absence == not
 * sandboxed, which is the v13 contract.
 *
 * Nested proxies (is_nested_proxy=1) synthesise their metadata from the
 * outer's qdwin_nested_manager_v1.advertise_toplevel call; the proxy's
 * own wl_client is the shell that drove advertise, not the inner app's
 * client, so qdwin_secctx_for_client on the proxy would either return
 * NULL or, worse, the shell's own secctx in pathological reuse.
 * Skip nested proxies for now — when the nested-mode publisher learns
 * to forward inner-client secctx tags through advertise_toplevel
 * (separate task; needs an XML bump on qdwin-nested-v1) we can plumb
 * it back here. */
static void
qdwin_send_toplevel_security_context(struct qdwin *qdwin,
				     struct qdwin_toplevel *tl)
{
	if (!qdwin->shell_bound || !qdwin->shell_resource)
		return;
	if (wl_resource_get_version(qdwin->shell_resource) < 13)
		return;
	if (tl->is_nested_proxy)
		return;
	struct weston_desktop_client *dclient =
		weston_desktop_surface_get_client(tl->desktop_surface);
	struct wl_client *client =
		weston_desktop_client_get_client(dclient);
	struct qdwin_secctx_client *sc = qdwin_secctx_client_lookup(qdwin, client);
	if (!sc)
		return;
	const char *engine   = qdwin_secctx_client_engine(sc);
	const char *app_id   = qdwin_secctx_client_app_id(sc);
	const char *instance = qdwin_secctx_client_instance_id(sc);
	qdwin_shell_v1_send_toplevel_security_context(
		qdwin->shell_resource, tl->handle, engine, app_id, instance);
	weston_log("qdwin: toplevel_security_context handle=%u engine=%s "
		   "app_id=%s instance=%s\n",
		   tl->handle, engine, app_id, instance);
	/* v22 sidecar: stable process identity for broker-side re-verification
	 * (todo/decisions/secctx-identity-contract.md, Option B). Additive —
	 * skipped on shells bound below v22. */
	if (wl_resource_get_version(qdwin->shell_resource) >= 22) {
		uint32_t peer_pid = qdwin_secctx_client_peer_pid(sc);
		uint64_t starttime = qdwin_secctx_client_peer_starttime(sc);
		uint32_t peer_uid = qdwin_secctx_client_peer_uid(sc);
		const char *exe = qdwin_secctx_client_peer_exe(sc);
		const char *label = qdwin_secctx_client_peer_selinux_label(sc);
		uint32_t st_lo = (uint32_t)(starttime & 0xffffffffu);
		uint32_t st_hi = (uint32_t)((starttime >> 32) & 0xffffffffu);
		qdwin_shell_v1_send_toplevel_peer_identity(
			qdwin->shell_resource, tl->handle,
			peer_pid, st_lo, st_hi, peer_uid, exe, label);
		weston_log("qdwin: toplevel_peer_identity handle=%u pid=%u "
			   "starttime=%llu uid=%u exe=%s label=%s\n",
			   tl->handle, peer_pid,
			   (unsigned long long)starttime,
			   peer_uid, exe, label);
	}
}

static int
qdwin_toplevel_is_xwayland(struct qdwin *qdwin, struct qdwin_toplevel *tl)
{
	if (tl->is_nested_proxy || !tl->desktop_surface)
		return 0;
	if (!qdwin->xwayland_api_resolved) {
		qdwin->xwayland_surface_api =
			weston_xwayland_surface_get_api(qdwin->compositor);
		qdwin->xwayland_api_resolved = 1;
	}
	if (!qdwin->xwayland_surface_api)
		return 0;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(tl->desktop_surface);
	if (!surface)
		return 0;
	return qdwin->xwayland_surface_api->is_xwayland_surface(surface) ? 1 : 0;
}

static void
qdwin_send_toplevel_added(struct qdwin *qdwin, struct qdwin_toplevel *tl)
{
	const char *app_id;
	const char *title;
	uid_t uid;
	if (tl->is_nested_proxy) {
		/* Nested-proxy toplevels synthesise their metadata from the
		 * advertise_toplevel call — desktop_surface is NULL. */
		app_id = tl->proxy_app_id ? tl->proxy_app_id : "";
		title  = tl->proxy_title  ? tl->proxy_title  : "";
		uid    = tl->proxy_origin_uid;
	} else {
		app_id = weston_desktop_surface_get_app_id(tl->desktop_surface);
		title  = weston_desktop_surface_get_title(tl->desktop_surface);
		uid    = qdwin_client_uid(tl->desktop_surface);
	}
	/* Seed the caches unconditionally — even when no shell is bound,
	 * the surface_committed diff path uses these to suppress spurious
	 * "title/app_id changed" log spam on every initial commit. */
	free(tl->cached_title);
	tl->cached_title = strdup(title ? title : "");
	free(tl->cached_app_id);
	tl->cached_app_id = strdup(app_id ? app_id : "");
	if (!qdwin->shell_bound || !qdwin->shell_resource)
		return;
	qdwin_shell_v1_send_toplevel_added(qdwin->shell_resource,
					   tl->handle,
					   (uint32_t)uid,
					   app_id ? app_id : "",
					   title  ? title  : "",
					   (uint32_t)qdwin_toplevel_is_xwayland(qdwin, tl));
	qdwin_send_toplevel_security_context(qdwin, tl);
}

static void
qdwin_send_toplevel_removed(struct qdwin *qdwin, struct qdwin_toplevel *tl)
{
	if (!qdwin->shell_bound || !qdwin->shell_resource)
		return;
	qdwin_shell_v1_send_toplevel_removed(qdwin->shell_resource, tl->handle);
}

static void
qdwin_send_toplevel_state(struct qdwin *qdwin, struct qdwin_toplevel *tl)
{
	if (!qdwin->shell_bound || !qdwin->shell_resource)
		return;
	qdwin_shell_v1_send_toplevel_state(qdwin->shell_resource,
					   tl->handle, tl->state);
}

/* Move the content view + every attached chrome view to the same layer.
 * Used by maximise (no layer change today, but kept for future fullscreen
 * lifting), minimise → minimized_layer, and unminimise → normal_layer. */
static void
qdwin_toplevel_move_to_layer(struct qdwin_toplevel *tl,
			     struct weston_layer *layer)
{
	weston_view_move_to_layer(tl->view, &layer->view_list);
	for (int s = 0; s < QDWIN_SIDES; s++) {
		if (tl->chrome[s].view)
			weston_view_move_to_layer(tl->chrome[s].view,
						  &layer->view_list);
	}
}

/* First enabled output, or NULL. Maximise needs the work-area. Single
 * output today; multi-output policy lands with §6.5. */
static struct weston_output *
qdwin_primary_output(struct qdwin *qdwin)
{
	struct weston_output *out;
	wl_list_for_each(out, &qdwin->compositor->output_list, link)
		return out;
	return NULL;
}

/* ------------------------------------------------------------------
 * weston_desktop_api callbacks.
 * ------------------------------------------------------------------ */

static void
qdwin_surface_added(struct weston_desktop_surface *dsurf, void *data)
{
	struct qdwin *qdwin = data;
	struct qdwin_toplevel *tl;
	struct weston_view *view;

	tl = calloc(1, sizeof *tl);
	if (!tl)
		return;

	view = weston_desktop_surface_create_view(dsurf);
	if (!view) {
		free(tl);
		return;
	}

	tl->qdwin = qdwin;
	tl->desktop_surface = dsurf;
	tl->view = view;
	tl->handle = ++qdwin->next_handle;
	tl->mapped = 0;
	tl->decorated = 0;
	for (int s = 0; s < QDWIN_SIDES; s++) {
		tl->chrome[s].side = s;
		tl->chrome[s].tl = tl;
		wl_list_init(&tl->chrome[s].surface_destroy.link);
		wl_list_init(&tl->chrome[s].surface_commit.link);
	}
	wl_list_insert(&qdwin->toplevels, &tl->link);
	weston_desktop_surface_set_user_data(dsurf, tl);

	if (qdwin->nested_mode) {
		/* §6.8 S1: nested mode skips the held-layer + toplevel_added
		 * fanout. Inner toplevels go straight to the normal layer so
		 * they paint into the pipewire output we pin them onto.
		 * No outer chrome — outer qdwin's qdwin_shell_v1 will draw
		 * chrome around the proxy surface (see S2). */
		weston_view_move_to_layer(view, &qdwin->normal_layer.view_list);
		weston_log("qdwin/nested: inner toplevel handle=%u uid=%u "
			   "app_id=%s\n",
			   tl->handle, (unsigned)qdwin_client_uid(dsurf),
			   weston_desktop_surface_get_app_id(dsurf) ?: "(null)");
		/* Defer publish until first commit (so app_id/title are set
		 * by then — most clients set both before the initial commit). */
		return;
	}

	/* Land on the held layer until S3 ships: no pixels visible. */
	weston_view_move_to_layer(view, &qdwin->held_layer.view_list);

	weston_log("qdwin: toplevel_added handle=%u uid=%u app_id=%s\n",
		   tl->handle, (unsigned)qdwin_client_uid(dsurf),
		   weston_desktop_surface_get_app_id(dsurf) ?: "(null)");

	qdwin_send_toplevel_added(qdwin, tl);

	/* qdshell consumes qdwin_shell_v1 to track/focus toplevels, but it
	 * does not attach qdwin SSD chrome for ordinary local applications.
	 * Do not require a shell-side decoration/colour request before a
	 * normal desktop surface can become visible. Nested proxy toplevels
	 * still use their explicit allow/deny path. */
	qdwin_toplevel_release_holding(tl, "default_toplevel_policy");
}

static void
qdwin_surface_removed(struct weston_desktop_surface *dsurf, void *data)
{
	struct qdwin *qdwin = data;
	struct qdwin_toplevel *tl =
		weston_desktop_surface_get_user_data(dsurf);
	if (!tl)
		return;

	if (qdwin->nested_mode) {
		qdwin_nested_unpublish_toplevel(tl);
		weston_log("qdwin/nested: inner toplevel_removed handle=%u\n",
			   tl->handle);
	} else {
		qdwin_send_toplevel_removed(qdwin, tl);
		weston_log("qdwin: toplevel_removed handle=%u\n", tl->handle);
	}

	if (tl->popup) {
		qdwin_popup_v1_send_dismissed(tl->popup->resource);
		qdwin_popup_teardown(tl->popup);
	}
	qdwin_move_grab_end_for(qdwin, tl->handle);
	for (int s = 0; s < QDWIN_SIDES; s++)
		qdwin_chrome_detach(&tl->chrome[s]);

	/* Focus transfer to a surviving sibling on toplevel destroy is
	 * NOT done here. By the time qdwin_surface_removed runs,
	 * libweston has already cleared kbd->focus to NULL (the
	 * weston_surface destroy_signal fires before libweston-desktop's
	 * surface_removed callback). Instead, the recovery is done from
	 * the focus_signal listener in qdwin_seat_emit_focus_now —
	 * whenever focus drops to UINT32_MAX with surviving siblings
	 * present, it re-targets the next mapped toplevel. */

	weston_desktop_surface_unlink_view(tl->view);
	weston_view_destroy(tl->view);
	wl_list_remove(&tl->link);
	weston_desktop_surface_set_user_data(dsurf, NULL);
	free(tl->cached_title);
	free(tl->cached_app_id);
	free(tl);
}

static void
qdwin_surface_committed(struct weston_desktop_surface *dsurf,
			struct weston_coord_surface buf_offset, void *data)
{
	struct qdwin *qdwin = data;
	struct qdwin_toplevel *tl =
		weston_desktop_surface_get_user_data(dsurf);
	struct weston_surface *surface;

	(void)buf_offset; (void)qdwin;
	if (!tl)
		return;

	surface = weston_desktop_surface_get_surface(dsurf);
	if (surface->width == 0)
		return;

	if (qdwin->nested_mode) {
		if (!weston_surface_is_mapped(surface)) {
			weston_surface_map(surface);
			tl->mapped = 1;
			weston_log("qdwin/nested: mapped handle=%u %dx%d\n",
				   tl->handle, surface->width, surface->height);
		}
		/* Publish on first commit so app_id/title are usually set. */
		qdwin_nested_publish_toplevel(tl);
		return;
	}

	/* xdg_toplevel.set_title diff. libweston has no title_changed
	 * callback — clients update the title via set_title and we only
	 * notice on the next commit. cached_title is seeded in
	 * qdwin_send_toplevel_added to the value already shipped, so
	 * this only triggers on *changes*. The wire event needs shell
	 * binding; the log line fires regardless so tests can assert
	 * the diff path even when no shell is connected. Note: surfaces
	 * stuck in the held bystander layer only commit once, so title
	 * updates only propagate after the surface is approved and
	 * migrates to the normal layer where frame-driven commits
	 * resume (set QDWIN_AUTO_APPROVE_TOPLEVELS=1 in dev/test). */
	{
		const char *cur = weston_desktop_surface_get_title(tl->desktop_surface);
		const char *cur_safe = cur ? cur : "";
		if (!tl->cached_title || strcmp(tl->cached_title, cur_safe) != 0) {
			free(tl->cached_title);
			tl->cached_title = strdup(cur_safe);
			weston_log("qdwin: toplevel_title handle=%u title=\"%s\"\n",
				   tl->handle, cur_safe);
			if (qdwin->shell_bound && qdwin->shell_resource)
				qdwin_shell_v1_send_toplevel_title(qdwin->shell_resource,
								   tl->handle, cur_safe);
		}
	}
	/* xdg_toplevel.set_app_id diff. Same shape as the title diff:
	 * detect changes on commit, log them. No protocol event yet
	 * (qdwin_shell_v1 ships app_id only at toplevel_added) so this
	 * is log-only — but the diff path is in place for when a
	 * toplevel_app_id event lands. */
	{
		const char *cur = weston_desktop_surface_get_app_id(tl->desktop_surface);
		const char *cur_safe = cur ? cur : "";
		if (!tl->cached_app_id || strcmp(tl->cached_app_id, cur_safe) != 0) {
			free(tl->cached_app_id);
			tl->cached_app_id = strdup(cur_safe);
			weston_log("qdwin: toplevel_app_id handle=%u app_id=\"%s\" (log-only — no protocol event yet)\n",
				   tl->handle, cur_safe);
		}
	}

	/* First-commit map path. S3 decides whether to migrate to
	 * the normal layer; S2 leaves it held. */
	if (!weston_surface_is_mapped(surface)) {
		weston_surface_map(surface);
		tl->mapped = 1;
		/* Centre content on the primary output so chrome fits
		 * within visible bounds. Without this the view defaults
		 * to (0,0) and left/top chrome ends up at negative
		 * coordinates.
		 *
		 * Cascade: every previously-mapped non-nested toplevel
		 * pushes the new window down-right by 40px. Without this
		 * spawning two foots stacks them at the same coords —
		 * user only ever sees the top one and switching focus to
		 * the bottom one feels broken (text appears on a window
		 * the user can't see). 40 is small enough to keep most of
		 * the older window visible behind, large enough to read
		 * the chrome titlebar through the gap.
		 */
		struct weston_output *out = qdwin_primary_output(qdwin);
		if (out) {
			int siblings = 0;
			struct qdwin_toplevel *t;
			wl_list_for_each(t, &qdwin->toplevels, link) {
				if (t == tl || t->is_nested_proxy)
					continue;
				if (t->mapped)
					siblings++;
			}
			int offset = (siblings * 40) % 200;  /* wrap at 5 */
			/* Pixel-coord math is int throughout; cast pos.c.{x,y}
			 * (double) to int so clang-tidy doesn't flag the
			 * intermediate int divisions as occurring in a float
			 * context. Position values are whole pixels in
			 * practice. */
			int cx = (int)out->pos.c.x + (out->width  - surface->width)  / 2 + offset;
			int cy = (int)out->pos.c.y + (out->height - surface->height) / 2 + offset;
			if (cx < 0) cx = 0;
			if (cy < 0) cy = 0;
			struct weston_coord_global p = { .c = weston_coord(cx, cy) };
			weston_view_set_position(tl->view, p);
			weston_view_update_transform(tl->view);
		}
		weston_log("qdwin: mapped handle=%u size=%dx%d (%s)\n",
			   tl->handle, surface->width, surface->height,
			   tl->decorated ? "normal" : "held");
		/* If approval (release_holding) already fired before this
		 * first commit, qdwin_toplevel_release_holding's autofocus
		 * call shortcircuited because the surface wasn't yet
		 * mapped. Re-arm now that both conditions hold. */
		qdwin_toplevel_autofocus_if_ready(tl);
	}

	if (tl->last_width != surface->width ||
	    tl->last_height != surface->height) {
		tl->last_width = surface->width;
		tl->last_height = surface->height;
		if (qdwin->shell_bound && qdwin->shell_resource) {
			struct weston_coord_global op =
				weston_view_get_pos_offset_global(tl->view);
			qdwin_shell_v1_send_toplevel_geometry(
				qdwin->shell_resource, tl->handle,
				(int)(op.c.x - tl->inset_w),
				(int)(op.c.y - tl->inset_n),
				(uint32_t)surface->width,
				(uint32_t)surface->height);
		}
		if (tl->decorated)
			qdwin_toplevel_position_chrome(tl);
	}
}

static void
qdwin_surface_move(struct weston_desktop_surface *dsurf,
		   struct weston_seat *seat, uint32_t serial, void *data)
{
	struct qdwin *qdwin = data;
	struct qdwin_toplevel *tl =
		weston_desktop_surface_get_user_data(dsurf);
	(void)seat;
	if (!tl)
		return;
	weston_log("qdwin: desktop_surface_move handle=%u serial=%u (stub)\n",
		   tl->handle, serial);
	/* S2: no-op. The archive validated that the shell, not the
	 * compositor, initiates drag via begin_interactive_move when
	 * the user grabs the chrome — this callback only fires for
	 * client-side move grabs (xdg_toplevel.move), which qdwin
	 * doesn't route through in the normal flow. */
	(void)qdwin;
}

static void
qdwin_surface_resize(struct weston_desktop_surface *dsurf,
		     struct weston_seat *seat, uint32_t serial,
		     enum weston_desktop_surface_edge edges, void *data)
{
	struct qdwin *qdwin = data;
	struct qdwin_toplevel *tl =
		weston_desktop_surface_get_user_data(dsurf);
	(void)seat; (void)edges;
	if (!tl)
		return;
	weston_log("qdwin: desktop_surface_resize handle=%u serial=%u (stub)\n",
		   tl->handle, serial);
	(void)qdwin;
}

/* xdg_toplevel.set_maximized / .set_fullscreen / .set_minimized
 * handlers. Without these libweston drops the requests silently
 * (libweston-desktop.c NULL-checks each callback before dispatch), so
 * client-side titlebar buttons (weston-terminal, foot+libdecor, GTK
 * CSD apps) appear clickable but do nothing. Each routes through the
 * same shared core the qdshell custom protocol uses so both paths
 * produce identical state. */
static void
qdwin_surface_maximized_requested(struct weston_desktop_surface *dsurf,
				  bool maximized, void *data)
{
	struct qdwin *qdwin = data;
	struct qdwin_toplevel *tl =
		weston_desktop_surface_get_user_data(dsurf);
	if (!tl || qdwin->locked)
		return;
	qdwin_toplevel_set_maximized(qdwin, tl, maximized);
}

static void
qdwin_surface_fullscreen_requested(struct weston_desktop_surface *dsurf,
				   bool fullscreen,
				   struct weston_output *output,
				   void *data)
{
	struct qdwin *qdwin = data;
	struct qdwin_toplevel *tl =
		weston_desktop_surface_get_user_data(dsurf);
	if (!tl || qdwin->locked)
		return;
	qdwin_toplevel_set_fullscreen(qdwin, tl, fullscreen, output);
}

static void
qdwin_surface_minimized_requested(struct weston_desktop_surface *dsurf,
				  void *data)
{
	struct qdwin *qdwin = data;
	struct qdwin_toplevel *tl =
		weston_desktop_surface_get_user_data(dsurf);
	if (!tl || qdwin->locked)
		return;
	qdwin_toplevel_set_minimized(qdwin, tl);
}

static const struct weston_desktop_api qdwin_desktop_api = {
	.struct_size = sizeof(struct weston_desktop_api),
	.surface_added = qdwin_surface_added,
	.surface_removed = qdwin_surface_removed,
	.committed = qdwin_surface_committed,
	.move = qdwin_surface_move,
	.resize = qdwin_surface_resize,
	.maximized_requested = qdwin_surface_maximized_requested,
	.fullscreen_requested = qdwin_surface_fullscreen_requested,
	.minimized_requested = qdwin_surface_minimized_requested,
};

/* ------------------------------------------------------------------
 * qdwin_shell_v1 request handlers.
 * ------------------------------------------------------------------ */

static int
qdwin_shell_require_bound(struct qdwin *qdwin, struct wl_resource *resource)
{
	if (qdwin->shell_bound && qdwin->shell_resource == resource)
		return 1;
	wl_resource_post_error(resource,
			       QDWIN_SHELL_V1_ERROR_NOT_BOUND,
			       "request issued before bind_as_shell");
	return 0;
}

static void
qdwin_handle_bind_as_shell(struct wl_client *client,
			   struct wl_resource *resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl;
	pid_t pid; uid_t uid; gid_t gid;

	wl_client_get_credentials(client, &pid, &uid, &gid);

	if (qdwin->shell_bound && qdwin->shell_resource != resource) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_ALREADY_BOUND,
				       "qdwin_shell_v1: shell role already claimed");
		return;
	}
	qdwin->shell_bound = 1;
	qdwin->shell_resource = resource;
	weston_log("qdwin: shell bound (uid=%u pid=%d); replaying %u toplevels\n",
		   (unsigned)uid, (int)pid,
		   (unsigned)wl_list_length(&qdwin->toplevels));

	wl_list_for_each(tl, &qdwin->toplevels, link)
		qdwin_send_toplevel_added(qdwin, tl);

	/* v2 replay: seats and outputs that existed before the shell
	 * bound. seat_removed / output_removed follow the normal signal
	 * path once these go away. */
	if (wl_resource_get_version(resource) >= 2) {
		struct weston_seat *seat;
		struct weston_output *output;
		wl_list_for_each(seat, &qdwin->compositor->seat_list, link)
			qdwin_send_seat_created(qdwin, seat);
		wl_list_for_each(output, &qdwin->compositor->output_list, link)
			qdwin_send_output_created_evt(qdwin, output);
	}
	/* v14 replay: seat_focus_changed for every seat we track, so the
	 * shell starts with a coherent focus map. Defined lower (the seat
	 * tracker struct lives below); forward-declared at the top. */
	if (wl_resource_get_version(resource) >= 14)
		qdwin_replay_seat_focus_for_shell(qdwin);
}

static void
qdwin_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_toplevel_release_holding(struct qdwin_toplevel *tl, const char *cause)
{
	if (tl->decorated)
		return;
	/* §6.8 S4: nested-proxy gating overrides chrome-driven release.
	 * Until nested_proxy_decision lands with allow, chrome calls
	 * (set_border_color, attach_decoration) preserve the held layer
	 * so the user doesn't see an unauthorised inner toplevel briefly
	 * flash before the broker decides. The decision-handler clears
	 * the pending flag and re-enters this function. */
	if (tl->nested_proxy_pending_decision &&
	    strncmp(cause, "nested_proxy_decision/", 22) != 0) {
		weston_log("qdwin: holding_released handle=%u via %s deferred — "
			   "nested-proxy waiting for admin decision\n",
			   tl->handle, cause);
		return;
	}
	tl->decorated = 1;
	weston_view_move_to_layer(tl->view,
				  &tl->qdwin->normal_layer.view_list);
	/* Chrome views migrate alongside the content when they exist at
	 * this point. If set_border_color fires before attach_decoration
	 * (qdshell's order), chrome views don't exist yet — migration of
	 * each chrome view happens in qdwin_chrome_attach_side instead,
	 * which checks tl->decorated and skips the held layer. */
	for (int s = 0; s < QDWIN_SIDES; s++) {
		if (tl->chrome[s].view)
			weston_view_move_to_layer(tl->chrome[s].view,
				&tl->qdwin->normal_layer.view_list);
	}
	weston_log("qdwin: holding_released handle=%u via %s (held → normal)\n",
		   tl->handle, cause);

	/* Auto-focus is wired in qdwin_toplevel_autofocus_if_ready, called
	 * from BOTH the release-holding path (here, in case the surface
	 * already committed a buffer) and the first-commit map path (in
	 * case release fires before first commit). See the helper's
	 * docblock for why both call sites are needed. */
	qdwin_toplevel_autofocus_if_ready(tl);
}

/* Assign keyboard focus to this toplevel on every seat with a keyboard,
 * but only when the surface is BOTH mapped (has a buffer; safe to
 * render) AND decorated (on the normal layer, not still held in the
 * bystander layer awaiting approval). Idempotent — re-arming with the
 * same focus is cheap and the kbd->focus equality check shortcircuits.
 *
 * Without this, the qdshell side has no path to drive focus
 * (Services/Qdwin/Qdwin.qml::focusWindow is a TODO stub), leaving
 * keyboard focus pinned at UINT32_MAX indefinitely — every newly-
 * spawned window is unfocused until the user clicks it. This matches
 * the default behaviour of sway/hyprland/labwc/etc.
 *
 * Called from both:
 *   - qdwin_toplevel_release_holding (held→normal transition): handles
 *     the common case where first_commit/map fires before approval.
 *   - qdwin_surface_committed first-map branch: handles the case
 *     where approval (release_holding) fires before the surface has
 *     committed any buffer (e.g. QDWIN_AUTO_APPROVE_TOPLEVELS env).
 * Only one call ends up actually emitting focus_signal; the other
 * shortcircuits on kbd->focus equality.
 *
 * Skipped when the session is locked — the locker's EXCLUSIVE layer
 * surface (ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE in
 * qdwin_layer_surface_apply) owns focus. Without the guard a
 * background app spawning during a lock could briefly steal focus
 * before the next exclusive-grab re-assertion.
 *
 * See todo/qdshell-no-focus-driving.md.
 */
static void
qdwin_toplevel_autofocus_if_ready(struct qdwin_toplevel *tl)
{
	if (!tl || !tl->qdwin || tl->qdwin->locked)
		return;
	if (!tl->decorated)  /* still in held bystander layer */
		return;
	if (!tl->view || !tl->view->surface)
		return;
	if (!weston_surface_is_mapped(tl->view->surface))
		return;
	struct weston_seat *seat;
	wl_list_for_each(seat, &tl->qdwin->compositor->seat_list, link) {
		struct weston_keyboard *kbd = weston_seat_get_keyboard(seat);
		if (kbd && kbd->focus != tl->view->surface) {
			weston_keyboard_set_focus(kbd, tl->view->surface);
			qdwin_seat_emit_focus_now(
				qdwin_seat_tracker_for_seat(tl->qdwin, seat));
		}
	}
}

static void
qdwin_handle_set_border_color(struct wl_client *client,
			      struct wl_resource *resource,
			      uint32_t handle, uint32_t rgba)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl;
	(void)client;
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	tl = qdwin_toplevel_from_handle(qdwin, handle);
	if (!tl) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_INVALID_HANDLE,
				       "set_border_color: unknown handle %u",
				       handle);
		return;
	}
	/* P05a: store per-toplevel so the SSD paint helper has a single
	 * source of truth for the silo chrome colour. Pre-P05a the rgba
	 * arg was logged + discarded; that meant a re-issued
	 * attach_decoration call (qdshell legitimately does this on
	 * state changes) silently dropped the colour. Now the per-tl
	 * state survives, and qdwin_toplevel_border_rgba(tl) is the
	 * accessor the paint helper + unit tests read. */
	tl->border_rgba = rgba;
	tl->border_rgba_set = 1;
	weston_log("qdwin: set_border_color handle=%u rgba=%#010x\n",
		   handle, rgba);
	qdwin_toplevel_release_holding(tl, "set_border_color");
}

/* P05a: accessor for the per-toplevel chrome border rgba.
 *
 * Returns the value last set by qdwin_shell_v1.set_border_color for
 * `tl`. When no colour has been set (border_rgba_set == 0), returns
 * `fallback` so callers don't need a separate "is-set" probe — pass
 * 0 for "transparent / use qdshell default", or a packed RGBA for a
 * compositor-side default.
 *
 * The split between `border_rgba` and `border_rgba_set` exists so a
 * legitimate set_border_color(0x000000ff) (opaque black, R=G=B=0,
 * A=0xff in RGBA8888) is not indistinguishable from "never set".
 * The SSD paint helper uses this
 * accessor instead of reading the field directly so the "unset →
 * fallback" semantics live in one place.
 */
uint32_t
qdwin_toplevel_border_rgba(struct qdwin_toplevel *tl, uint32_t fallback)
{
	if (!tl || !tl->border_rgba_set)
		return fallback;
	return tl->border_rgba;
}

/* ------------------------------------------------------------------
 * Chrome surface/view management.
 * ------------------------------------------------------------------ */

static void
qdwin_chrome_detach(struct qdwin_chrome *c)
{
	if (!c->surface)
		return;
	if (c->view) {
		weston_view_destroy(c->view);
		c->view = NULL;
	}
	wl_list_remove(&c->surface_destroy.link);
	wl_list_remove(&c->surface_commit.link);
	c->surface = NULL;
}

static void
qdwin_chrome_surface_destroyed(struct wl_listener *listener, void *data)
{
	struct qdwin_chrome *c = wl_container_of(listener, c, surface_destroy);
	(void)data;
	/* Shell's wl_surface went away before we did; drop the view. */
	if (c->view) {
		weston_view_destroy(c->view);
		c->view = NULL;
	}
	wl_list_remove(&c->surface_destroy.link);
	wl_list_init(&c->surface_destroy.link);
	wl_list_remove(&c->surface_commit.link);
	wl_list_init(&c->surface_commit.link);
	c->surface = NULL;
	weston_log("qdwin: chrome side=%d surface destroyed by client\n",
		   c->side);
}

static void
qdwin_chrome_surface_committed(struct wl_listener *listener, void *data)
{
	struct qdwin_chrome *c = wl_container_of(listener, c, surface_commit);
	(void)data;
	if (!c->view)
		return;
	/* Mark geometry dirty so the next repaint re-derives the view
	 * bbox from the (possibly resized) surface. damage_all covers the
	 * old pixels — the newer, smaller chrome won't otherwise damage
	 * the region it no longer occupies. Cheap on single-window
	 * headless tests; can be tightened to the union of old+new bounds
	 * once production scenes exercise chrome resize. */
	weston_view_geometry_dirty(c->view);
	weston_compositor_damage_all(c->tl->qdwin->compositor);
}

static void
qdwin_toplevel_position_chrome(struct qdwin_toplevel *tl)
{
	struct weston_coord_global content_pos =
		weston_view_get_pos_offset_global(tl->view);
	double x = content_pos.c.x;
	double y = content_pos.c.y;
	int cw, ch;
	if (tl->is_nested_proxy) {
		/* Nested-proxy: the curtain has no weston_desktop_surface, but
		 * we know the rect from last_width/last_height. */
		cw = tl->last_width  > 0 ? tl->last_width  : 800;
		ch = tl->last_height > 0 ? tl->last_height : 600;
	} else {
		struct weston_surface *content =
			weston_desktop_surface_get_surface(tl->desktop_surface);
		cw = content->width;
		ch = content->height;
	}

	for (int s = 0; s < QDWIN_SIDES; s++) {
		struct qdwin_chrome *c = &tl->chrome[s];
		if (!c->view)
			continue;
		double cx = x, cy = y;
		switch (s) {
		case QDWIN_SIDE_N: cx = x - tl->inset_w; cy = y - tl->inset_n; break;
		case QDWIN_SIDE_S: cx = x - tl->inset_w; cy = y + ch;          break;
		case QDWIN_SIDE_W: cx = x - tl->inset_w; cy = y;               break;
		case QDWIN_SIDE_E: cx = x + cw;          cy = y;               break;
		}
		struct weston_coord_global p = { .c = weston_coord(cx, cy) };
		weston_view_set_position(c->view, p);
		weston_view_update_transform(c->view);
	}
}

static int
qdwin_chrome_attach_side(struct qdwin_toplevel *tl, int side,
			 struct wl_resource *surface_res)
{
	struct qdwin_chrome *c = &tl->chrome[side];

	/* NULL means "detach this side". */
	if (!surface_res) {
		qdwin_chrome_detach(c);
		return 0;
	}

	struct weston_surface *ws = wl_resource_get_user_data(surface_res);
	if (!ws)
		return -1;

	/* If already attached to the same surface, leave in place —
	 * position update below handles geometry changes. */
	if (c->surface == ws && c->view)
		return 0;

	/* Otherwise drop the old, install the new. */
	qdwin_chrome_detach(c);

	c->surface = ws;
	c->side = side;
	c->tl = tl;
	c->surface_destroy.notify = qdwin_chrome_surface_destroyed;
	wl_signal_add(&ws->destroy_signal, &c->surface_destroy);
	c->surface_commit.notify = qdwin_chrome_surface_committed;
	wl_signal_add(&ws->commit_signal, &c->surface_commit);

	c->view = weston_view_create(ws);
	if (!c->view) {
		wl_list_remove(&c->surface_destroy.link);
		c->surface = NULL;
		return -1;
	}
	/* Land on the same layer as the content view. When holding hasn't
	 * released yet (rare — attach_decoration before set_border_color)
	 * we stash on held and release_holding will migrate us. Otherwise
	 * (set_border_color already fired) the content is already on
	 * normal, and we must follow so the chrome actually composites. */
	struct weston_layer *target = tl->decorated
		? &tl->qdwin->normal_layer
		: &tl->qdwin->held_layer;
	weston_view_move_to_layer(c->view, &target->view_list);
	/* Mark the roleless chrome surface as mapped so it actually
	 * composites. Shell-owned surfaces have no weston_desktop life-
	 * cycle to drive this for us. */
	if (!weston_surface_is_mapped(ws))
		weston_surface_map(ws);

	return 0;
}

static void
qdwin_toplevel_apply_inset(struct qdwin_toplevel *tl)
{
	int outer_w, outer_h;
	int inner_w, inner_h;

	/* §6.8 S2: nested-proxy toplevels have no desktop_surface — the
	 * curtain is the content. Use last_width/last_height as the
	 * authoritative outer size and skip the desktop-surface set_size
	 * round-trip (the inner client gets resized via S3 input + the
	 * nested compositor's own xdg_toplevel.configure path). */
	if (tl->is_nested_proxy) {
		outer_w = tl->last_width  > 0 ? tl->last_width  : 800;
		outer_h = tl->last_height > 0 ? tl->last_height : 600;
		inner_w = outer_w - tl->inset_w - tl->inset_e;
		inner_h = outer_h - tl->inset_n - tl->inset_s;
		if (inner_w < 1) inner_w = 1;
		if (inner_h < 1) inner_h = 1;
		weston_log("qdwin: apply_inset handle=%u (nested-proxy) "
			   "outer=%dx%d inset=N%d E%d S%d W%d -> inner=%dx%d "
			   "(curtain stays at outer; inner client resize "
			   "deferred to S3)\n",
			   tl->handle, outer_w, outer_h,
			   tl->inset_n, tl->inset_e, tl->inset_s, tl->inset_w,
			   inner_w, inner_h);
		return;
	}

	struct weston_surface *surface =
		weston_desktop_surface_get_surface(tl->desktop_surface);

	/* Outer size = whatever we previously asked the client for, or
	 * (first time through) the current content size it sent us. This
	 * keeps repeated attach_decoration calls stable — we only shrink
	 * from outer to inner once per outer size.
	 *
	 * Holding-state chicken-and-egg: clients in qdwin's holding layer
	 * may not have committed a sized buffer yet when qdshell calls
	 * attach_decoration (qdshell paints chrome at its own fallback
	 * size; client is still waiting for an initial configure()). In
	 * that case surface->width/height are both 0; without a fallback
	 * here we'd compute outer=0, inner=clamp(-inset,1)=1, and ship
	 * configure(1,1) to the client, locking it to 1×1 forever. Pick
	 * a sensible default outer (800×600) when the client hasn't
	 * spoken yet — matches qdshell's chrome fallback (640+insets,
	 * 400+insets) closely enough that the first repaint after the
	 * client commits a real size produces a stable window.
	 */
	if (tl->outer_width == 0 || tl->outer_height == 0) {
		tl->outer_width  = surface->width  > 0 ? surface->width  : 800;
		tl->outer_height = surface->height > 0 ? surface->height : 600;
	}
	outer_w = tl->outer_width;
	outer_h = tl->outer_height;

	inner_w = outer_w - tl->inset_w - tl->inset_e;
	inner_h = outer_h - tl->inset_n - tl->inset_s;
	if (inner_w < 1) inner_w = 1;
	if (inner_h < 1) inner_h = 1;

	weston_log("qdwin: apply_inset handle=%u outer=%dx%d inset=N%d E%d S%d W%d -> inner=%dx%d\n",
		   tl->handle, outer_w, outer_h,
		   tl->inset_n, tl->inset_e, tl->inset_s, tl->inset_w,
		   inner_w, inner_h);

	weston_desktop_surface_set_size(tl->desktop_surface, inner_w, inner_h);
}

static void
qdwin_handle_attach_decoration(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t handle,
			       struct wl_resource *north,
			       struct wl_resource *east,
			       struct wl_resource *south,
			       struct wl_resource *west)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl;
	(void)client;

	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	tl = qdwin_toplevel_from_handle(qdwin, handle);
	if (!tl) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_INVALID_HANDLE,
				       "attach_decoration: unknown handle %u",
				       handle);
		return;
	}

	struct wl_resource *sides[QDWIN_SIDES] = { north, east, south, west };
	for (int s = 0; s < QDWIN_SIDES; s++) {
		if (qdwin_chrome_attach_side(tl, s, sides[s]) < 0) {
			weston_log("qdwin: attach_decoration handle=%u side=%d failed\n",
				   handle, s);
		}
	}

	/* Derive insets from the chrome surfaces' committed sizes.
	 * Shell is required to have committed before calling us. */
	tl->inset_n = tl->chrome[QDWIN_SIDE_N].surface
		? tl->chrome[QDWIN_SIDE_N].surface->height : 0;
	tl->inset_e = tl->chrome[QDWIN_SIDE_E].surface
		? tl->chrome[QDWIN_SIDE_E].surface->width  : 0;
	tl->inset_s = tl->chrome[QDWIN_SIDE_S].surface
		? tl->chrome[QDWIN_SIDE_S].surface->height : 0;
	tl->inset_w = tl->chrome[QDWIN_SIDE_W].surface
		? tl->chrome[QDWIN_SIDE_W].surface->width  : 0;

	/* P05a: read the per-toplevel border rgba via the accessor so a
	 * re-issued attach_decoration emits a journal trail confirming the
	 * silo colour survived the re-bind. This is the load-bearing
	 * consumer of qdwin_toplevel_border_rgba(); without it the field
	 * was inert. The shell paints chrome surfaces itself (qdwin SSD is
	 * stub today) but the stored rgba is the single source of truth a
	 * future SSD paint helper / focus ring will read, and logging it
	 * here means a regression that drops the field flips this line. */
	uint32_t border_rgba_seen = qdwin_toplevel_border_rgba(tl, 0u);
	weston_log("qdwin: attach_decoration handle=%u inset=N%d E%d S%d W%d "
		   "border_rgba=%#010x\n",
		   handle, tl->inset_n, tl->inset_e, tl->inset_s, tl->inset_w,
		   border_rgba_seen);

	qdwin_toplevel_release_holding(tl, "attach_decoration");

	if (tl->inset_n || tl->inset_e || tl->inset_s || tl->inset_w)
		qdwin_toplevel_apply_inset(tl);

	qdwin_toplevel_position_chrome(tl);
}

static void
qdwin_handle_request_close(struct wl_client *client,
			   struct wl_resource *resource, uint32_t handle)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl;
	(void)client;

	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	if (qdwin->locked) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_LOCKED, "locked");
		return;
	}
	tl = qdwin_toplevel_from_handle(qdwin, handle);
	if (!tl) {
		weston_log("qdwin: request_close unknown handle=%u\n", handle);
		return;
	}
	if (tl->is_nested_proxy) {
		/* §6.8 S2: close on a proxy fires close_requested on the
		 * nested toplevel resource — the nested compositor propagates
		 * to its inner client via xdg_toplevel.close. */
		qdwin_nested_proxy_send_close(tl);
		weston_log("qdwin: request_close handle=%u (nested-proxy: "
			   "fired close_requested)\n", handle);
		return;
	}
	weston_desktop_surface_close(tl->desktop_surface);
	weston_log("qdwin: request_close handle=%u dispatched\n", handle);
}

static void
qdwin_handle_request_minimize(struct wl_client *client,
			      struct wl_resource *resource, uint32_t handle)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl;
	(void)client;
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	if (qdwin->locked) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_LOCKED, "locked");
		return;
	}
	tl = qdwin_toplevel_from_handle(qdwin, handle);
	if (!tl) {
		weston_log("qdwin: request_minimize unknown handle=%u\n", handle);
		return;
	}
	qdwin_toplevel_set_minimized(qdwin, tl);
}

static void
qdwin_toplevel_set_minimized(struct qdwin *qdwin, struct qdwin_toplevel *tl)
{
	if (tl->state & QDWIN_TS_MINIMIZED)
		return;
	qdwin_toplevel_move_to_layer(tl, &qdwin->minimized_layer);
	tl->state |= QDWIN_TS_MINIMIZED;
	weston_log("qdwin: set_minimized handle=%u (state=%#x)\n",
		   tl->handle, tl->state);
	qdwin_send_toplevel_state(qdwin, tl);
	weston_compositor_schedule_repaint(qdwin->compositor);
}

/* Fullscreen: like maximize but fills the entire output (covers panels
 * and layer-shell exclusive zones). On unset, restore to the saved
 * pre-fullscreen geometry. The output param may be NULL — pick the
 * primary in that case. */
static void
qdwin_toplevel_set_fullscreen(struct qdwin *qdwin,
			      struct qdwin_toplevel *tl,
			      bool fullscreen,
			      struct weston_output *output)
{
	int want_fs = fullscreen ? 1 : 0;
	int is_fs   = (tl->state & QDWIN_TS_FULLSCREEN) ? 1 : 0;
	if (want_fs == is_fs) {
		weston_log("qdwin: set_fullscreen handle=%u fs=%d noop\n",
			   tl->handle, want_fs);
		return;
	}

	if (want_fs) {
		struct weston_output *out = output ? output
						   : qdwin_primary_output(qdwin);
		if (!out) {
			weston_log("qdwin: set_fullscreen handle=%u: no output\n",
				   tl->handle);
			return;
		}
		struct weston_coord_global pos =
			weston_view_get_pos_offset_global(tl->view);
		tl->saved_x = pos.c.x;
		tl->saved_y = pos.c.y;
		if (tl->outer_width == 0 || tl->outer_height == 0) {
			struct weston_surface *surface = tl->is_nested_proxy
				? NULL
				: weston_desktop_surface_get_surface(tl->desktop_surface);
			int sw = surface ? surface->width  : tl->last_width;
			int sh = surface ? surface->height : tl->last_height;
			if (sw <= 0) sw = 800;
			if (sh <= 0) sh = 600;
			tl->outer_width  = sw;
			tl->outer_height = sh;
		}
		tl->saved_outer_w = tl->outer_width;
		tl->saved_outer_h = tl->outer_height;

		int ox = out->pos.c.x, oy = out->pos.c.y;
		int ow = out->width,   oh = out->height;
		tl->outer_width  = ow;
		tl->outer_height = oh;
		if (tl->is_nested_proxy) {
			qdwin_nested_proxy_set_geometry(tl, ow, oh);
		} else {
			weston_desktop_surface_set_fullscreen(tl->desktop_surface,
							      true);
		}
		struct weston_coord_global origin = {
			.c = weston_coord(ox + tl->inset_w,
					  oy + tl->inset_n),
		};
		weston_view_set_position(tl->view, origin);
		qdwin_toplevel_apply_inset(tl);
		qdwin_toplevel_position_chrome(tl);

		tl->state |= QDWIN_TS_FULLSCREEN;
		weston_log("qdwin: set_fullscreen handle=%u fs=1 outer=%dx%d at (%d,%d) "
			   "saved=%dx%d@(%.0f,%.0f)\n",
			   tl->handle, tl->outer_width, tl->outer_height, ox, oy,
			   tl->saved_outer_w, tl->saved_outer_h,
			   tl->saved_x, tl->saved_y);
	} else {
		if (tl->is_nested_proxy) {
			qdwin_nested_proxy_set_geometry(tl, tl->saved_outer_w,
							tl->saved_outer_h);
		} else {
			weston_desktop_surface_set_fullscreen(tl->desktop_surface,
							      false);
		}
		tl->outer_width  = tl->saved_outer_w;
		tl->outer_height = tl->saved_outer_h;
		struct weston_coord_global pos = {
			.c = weston_coord(tl->saved_x, tl->saved_y),
		};
		weston_view_set_position(tl->view, pos);
		qdwin_toplevel_apply_inset(tl);
		qdwin_toplevel_position_chrome(tl);

		tl->state &= ~QDWIN_TS_FULLSCREEN;
		weston_log("qdwin: set_fullscreen handle=%u fs=0 restored=%dx%d@(%.0f,%.0f)\n",
			   tl->handle, tl->outer_width, tl->outer_height,
			   tl->saved_x, tl->saved_y);
	}
	qdwin_send_toplevel_state(qdwin, tl);
	weston_compositor_schedule_repaint(qdwin->compositor);
}

static void
qdwin_handle_request_maximize(struct wl_client *client,
			      struct wl_resource *resource,
			      uint32_t handle, uint32_t maximized)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl;
	(void)client;
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	if (qdwin->locked) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_LOCKED, "locked");
		return;
	}
	tl = qdwin_toplevel_from_handle(qdwin, handle);
	if (!tl) {
		weston_log("qdwin: request_maximize unknown handle=%u\n", handle);
		return;
	}
	qdwin_toplevel_set_maximized(qdwin, tl, maximized != 0);
}

static void
qdwin_toplevel_set_maximized(struct qdwin *qdwin,
			     struct qdwin_toplevel *tl,
			     bool maximized)
{
	int want_max = maximized ? 1 : 0;
	int is_max   = (tl->state & QDWIN_TS_MAXIMIZED) ? 1 : 0;
	if (want_max == is_max) {
		weston_log("qdwin: set_maximized handle=%u max=%d noop\n",
			   tl->handle, want_max);
		return;
	}

	if (want_max) {
		struct weston_output *out = qdwin_primary_output(qdwin);
		if (!out) {
			weston_log("qdwin: set_maximized handle=%u: no output\n",
				   tl->handle);
			return;
		}
		struct weston_coord_global pos =
			weston_view_get_pos_offset_global(tl->view);
		tl->saved_x = pos.c.x;
		tl->saved_y = pos.c.y;
		/* outer_width / outer_height are 0 until the first apply_inset
		 * call (attach_decoration / set_border_color don't seed them).
		 * If we save 0 here, restore() walks back to outer=0 → apply_
		 * inset's fallback branch picks up surface->width/height —
		 * which by then is the *maximised* size the client just
		 * committed, not the pre-max size. Result: "restore" leaves
		 * the window at maximised dimensions at the saved (top-left)
		 * position. Seed outer_* from the real committed surface size
		 * so the saved-restore round-trip is symmetric. */
		if (tl->outer_width == 0 || tl->outer_height == 0) {
			struct weston_surface *surface = tl->is_nested_proxy
				? NULL
				: weston_desktop_surface_get_surface(tl->desktop_surface);
			int sw = surface ? surface->width  : tl->last_width;
			int sh = surface ? surface->height : tl->last_height;
			if (sw <= 0) sw = 800;
			if (sh <= 0) sh = 600;
			tl->outer_width  = sw;
			tl->outer_height = sh;
		}
		tl->saved_outer_w = tl->outer_width;
		tl->saved_outer_h = tl->outer_height;

		/* §6.6 S1: maximise fills the work area (output minus panel
		 * exclusive zones), not the full output. */
		int wx, wy, ww, wh;
		qdwin_output_work_area(qdwin, out, &wx, &wy, &ww, &wh);
		tl->outer_width  = ww;
		tl->outer_height = wh;
		if (tl->is_nested_proxy) {
			/* Nested-proxy: resize the curtain via set_geometry.
			 * Inner client gets resized via S3 input flow. */
			qdwin_nested_proxy_set_geometry(tl, ww, wh);
		} else {
			weston_desktop_surface_set_maximized(tl->desktop_surface,
							    true);
		}
		/* Place the CONTENT view inside the work area, leaving room
		 * for the chrome (N titlebar, side borders). Without this
		 * offset chrome ends up at negative coords (off-screen) and
		 * the close/maximize/minimize glyphs paint above the visible
		 * area, making the maximised window indistinguishable from
		 * fullscreen. The (wx + inset_w, wy + inset_n) anchor keeps
		 * the titlebar at (wx, wy) — the top of the work area — so
		 * the panel stays visible at the bottom and the chrome stays
		 * visible at the top. */
		struct weston_coord_global origin = {
			.c = weston_coord(wx + tl->inset_w,
					  wy + tl->inset_n),
		};
		weston_view_set_position(tl->view, origin);
		qdwin_toplevel_apply_inset(tl);
		qdwin_toplevel_position_chrome(tl);

		tl->state |= QDWIN_TS_MAXIMIZED;
		weston_log("qdwin: set_maximized handle=%u max=1 outer=%dx%d at (%d,%d) "
			   "content_at=(%d,%d) saved=%dx%d@(%.0f,%.0f)\n",
			   tl->handle, tl->outer_width, tl->outer_height, wx, wy,
			   (int)origin.c.x, (int)origin.c.y,
			   tl->saved_outer_w, tl->saved_outer_h,
			   tl->saved_x, tl->saved_y);
	} else {
		if (tl->is_nested_proxy) {
			qdwin_nested_proxy_set_geometry(tl, tl->saved_outer_w,
							tl->saved_outer_h);
		} else {
			weston_desktop_surface_set_maximized(tl->desktop_surface,
							    false);
		}
		tl->outer_width  = tl->saved_outer_w;
		tl->outer_height = tl->saved_outer_h;
		struct weston_coord_global pos = {
			.c = weston_coord(tl->saved_x, tl->saved_y),
		};
		weston_view_set_position(tl->view, pos);
		qdwin_toplevel_apply_inset(tl);
		qdwin_toplevel_position_chrome(tl);

		tl->state &= ~QDWIN_TS_MAXIMIZED;
		weston_log("qdwin: set_maximized handle=%u max=0 restored=%dx%d@(%.0f,%.0f)\n",
			   tl->handle, tl->outer_width, tl->outer_height,
			   tl->saved_x, tl->saved_y);
	}
	qdwin_send_toplevel_state(qdwin, tl);
	weston_compositor_schedule_repaint(qdwin->compositor);
}

static void
qdwin_handle_request_raise(struct wl_client *client,
			   struct wl_resource *resource, uint32_t handle)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl;
	(void)client;
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	if (qdwin->locked) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_LOCKED, "locked");
		return;
	}
	tl = qdwin_toplevel_from_handle(qdwin, handle);
	if (!tl) {
		weston_log("qdwin: request_raise unknown handle=%u\n", handle);
		return;
	}

	/* For now request_raise doubles as un-minimise. Stack-raise for
	 * non-minimised toplevels is a Phase 6.4/6.5 concern. */
	if (tl->state & QDWIN_TS_MINIMIZED) {
		qdwin_toplevel_move_to_layer(tl, &qdwin->normal_layer);
		tl->state &= ~QDWIN_TS_MINIMIZED;
		weston_log("qdwin: request_raise handle=%u unminimised (state=%#x)\n",
			   handle, tl->state);
		qdwin_send_toplevel_state(qdwin, tl);
		weston_compositor_schedule_repaint(qdwin->compositor);
		return;
	}
	/* Stack-raise: re-insert the view at the front of normal_layer's
	 * view list. weston_view_move_to_layer with the current layer
	 * re-stacks; the side-effect-free no-op had to wait for the
	 * shell client to evolve a real raise contract. Without this,
	 * Alt+Tab moves keyboard focus correctly but the raised window
	 * stays buried under the previously-focused one — user can type
	 * into a hidden window but only sees the typed text once the
	 * obscuring window is closed. */
	qdwin_toplevel_move_to_layer(tl, &qdwin->normal_layer);
	weston_log("qdwin: request_raise handle=%u re-stacked\n", handle);
	weston_compositor_schedule_repaint(qdwin->compositor);
}

/* ------------------------------------------------------------------
 * Titlebar move-drag grab.
 *
 * Activated by the shell calling begin_interactive_move(handle,
 * serial) in response to a press on a chrome side. While the grab is
 * active, pointer motion translates the toplevel's content view + all
 * attached chrome views by the same delta; any button release ends
 * the grab.
 *
 * No focus changes during drag, no wl_pointer.motion forwarded to the
 * surface under the cursor (avoids phantom hover on other windows
 * while the user is mid-drag).
 * ------------------------------------------------------------------ */

static void
qdwin_move_grab_focus(struct weston_pointer_grab *grab)
{
	(void)grab;
}

static void
qdwin_move_grab_motion(struct weston_pointer_grab *grab,
		       const struct timespec *time,
		       struct weston_pointer_motion_event *event)
{
	(void)time;
	struct weston_pointer *pointer = grab->pointer;
	weston_pointer_move(pointer, event);

	struct qdwin *qd = wl_container_of(grab, qd, move_grab);
	if (!qd->move_grab_active)
		return;
	struct qdwin_toplevel *tl =
		qdwin_toplevel_from_handle(qd, qd->move_grab_handle);
	if (!tl || !tl->view)
		return;

	double nx = pointer->pos.c.x - qd->move_grab_anchor_dx;
	double ny = pointer->pos.c.y - qd->move_grab_anchor_dy;
	struct weston_coord_global p = { .c = weston_coord(nx, ny) };
	weston_view_set_position(tl->view, p);
	weston_view_update_transform(tl->view);
	qdwin_toplevel_position_chrome(tl);
	weston_compositor_schedule_repaint(qd->compositor);
}

static void
qdwin_move_grab_button(struct weston_pointer_grab *grab,
		       const struct timespec *time,
		       uint32_t button, uint32_t state)
{
	(void)time; (void)button;
	struct weston_pointer *pointer = grab->pointer;
	struct qdwin *qd = wl_container_of(grab, qd, move_grab);

	/* End on any button release. The protocol expects the user to
	 * release the same button that initiated the drag, but we don't
	 * gate on which button — clients sometimes lose the original
	 * press serial across re-grabs and ending defensively is safer. */
	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (qd->move_grab_active) {
			uint32_t handle = qd->move_grab_handle;
			struct qdwin_toplevel *tl =
				qdwin_toplevel_from_handle(qd, handle);
			weston_log("qdwin: end_interactive_move handle=%u\n",
				   handle);
			qd->move_grab_active = 0;
			weston_pointer_end_grab(pointer);
			/* Tell the shell where the toplevel ended up so it
			 * can update its tracked geometry; without this
			 * tl.x/tl.y stay frozen at the pre-drag position. */
			if (tl && tl->view && qd->shell_bound &&
			    qd->shell_resource) {
				struct weston_coord_global op =
					weston_view_get_pos_offset_global(tl->view);
				qdwin_shell_v1_send_toplevel_geometry(
					qd->shell_resource, handle,
					(int)(op.c.x - tl->inset_w),
					(int)(op.c.y - tl->inset_n),
					(uint32_t)tl->last_width,
					(uint32_t)tl->last_height);
			}
		}
	}
}

static void
qdwin_move_grab_axis(struct weston_pointer_grab *grab,
		     const struct timespec *time,
		     struct weston_pointer_axis_event *event)
{
	(void)grab; (void)time; (void)event;
}

static void
qdwin_move_grab_axis_source(struct weston_pointer_grab *grab, uint32_t source)
{
	(void)grab; (void)source;
}

static void
qdwin_move_grab_frame(struct weston_pointer_grab *grab)
{
	(void)grab;
}

static void
qdwin_move_grab_cancel(struct weston_pointer_grab *grab)
{
	struct qdwin *qd = wl_container_of(grab, qd, move_grab);
	qd->move_grab_active = 0;
}

static const struct weston_pointer_grab_interface qdwin_move_grab_iface = {
	qdwin_move_grab_focus,
	qdwin_move_grab_motion,
	qdwin_move_grab_button,
	qdwin_move_grab_axis,
	qdwin_move_grab_axis_source,
	qdwin_move_grab_frame,
	qdwin_move_grab_cancel,
};

/* End any active move-drag targeting `handle`. Called from
 * qdwin_surface_removed and from shell unbind so a destroyed toplevel
 * (or a vanished shell) doesn't leave a dangling grab. */
static void
qdwin_move_grab_end_for(struct qdwin *qdwin, uint32_t handle)
{
	if (!qdwin->move_grab_active)
		return;
	if (qdwin->move_grab_handle != handle)
		return;
	weston_log("qdwin: cancelling move-drag handle=%u "
		   "(toplevel/shell gone)\n", handle);
	qdwin->move_grab_active = 0;
	if (qdwin->move_grab.pointer)
		weston_pointer_end_grab(qdwin->move_grab.pointer);
}

static void
qdwin_handle_begin_interactive_move(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t handle, uint32_t serial)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client; (void)serial;
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	if (qdwin->locked) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_LOCKED, "locked");
		return;
	}

	/* One drag at a time. Silently drop a second begin request. */
	if (qdwin->move_grab_active) {
		weston_log("qdwin: begin_interactive_move handle=%u "
			   "ignored — drag already active on handle=%u\n",
			   handle, qdwin->move_grab_handle);
		return;
	}

	struct qdwin_toplevel *tl = qdwin_toplevel_from_handle(qdwin, handle);
	if (!tl || !tl->view) {
		weston_log("qdwin: begin_interactive_move handle=%u "
			   "ignored — no such toplevel\n", handle);
		return;
	}

	/* Per XML: tiled / maximised / fullscreen refuse silently. */
	if (tl->state & (QDWIN_TS_MAXIMIZED | QDWIN_TS_FULLSCREEN)) {
		weston_log("qdwin: begin_interactive_move handle=%u "
			   "ignored — toplevel is maximised/fullscreen\n",
			   handle);
		return;
	}

	/* Find a seat with an active pointer + start the grab on it.
	 * MVP: first seat wins. Multi-seat drag is uncommon and adds
	 * complexity that nothing currently exercises. */
	struct weston_seat *seat;
	struct weston_pointer *pointer = NULL;
	wl_list_for_each(seat, &qdwin->compositor->seat_list, link) {
		pointer = weston_seat_get_pointer(seat);
		if (pointer)
			break;
	}
	if (!pointer) {
		weston_log("qdwin: begin_interactive_move handle=%u "
			   "ignored — no pointer on any seat\n", handle);
		return;
	}

	struct weston_coord_global origin =
		weston_view_get_pos_offset_global(tl->view);
	qdwin->move_grab_handle = handle;
	qdwin->move_grab_anchor_dx = pointer->pos.c.x - origin.c.x;
	qdwin->move_grab_anchor_dy = pointer->pos.c.y - origin.c.y;
	qdwin->move_grab.interface = &qdwin_move_grab_iface;
	qdwin->move_grab_active = 1;
	weston_pointer_start_grab(pointer, &qdwin->move_grab);
	weston_log("qdwin: begin_interactive_move handle=%u anchor=(%.1f,%.1f)\n",
		   handle,
		   qdwin->move_grab_anchor_dx,
		   qdwin->move_grab_anchor_dy);
}

static void
qdwin_handle_begin_interactive_resize(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t handle, uint32_t serial,
				      uint32_t edges)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	if (qdwin->locked) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_LOCKED, "locked");
		return;
	}
	weston_log("qdwin: begin_interactive_resize handle=%u serial=%u edges=%#x (stub)\n",
		   handle, serial, edges);
}

/* ------------------------------------------------------------------
 * Popups (§6.4).
 *
 * A qdwin_popup is one shell-owned wl_surface placed on popup_layer
 * as a weston_view. The shell is responsible for painting; qdwin
 * handles placement, stacking, and teardown.
 *
 * S5: while a popup is up, qdwin owns a seat pointer grab. Button
 * presses inside the popup bbox pass through to the shell (so menu
 * items still get clicked); presses outside fire `dismissed` and
 * tear the popup down. Motion/axis/frame pass through so hover
 * highlighting on the popup surface keeps working.
 * ------------------------------------------------------------------ */

static void qdwin_popup_teardown(struct qdwin_popup *p);

static int
qdwin_popup_bbox_contains(struct qdwin_popup *p,
			  struct weston_coord_global pos)
{
	if (!p->view || !p->surface)
		return 0;
	struct weston_coord_global vp =
		weston_view_get_pos_offset_global(p->view);
	int w = p->surface->width;
	int h = p->surface->height;
	return pos.c.x >= vp.c.x && pos.c.x < vp.c.x + w &&
	       pos.c.y >= vp.c.y && pos.c.y < vp.c.y + h;
}

static void
qdwin_popup_grab_focus(struct weston_pointer_grab *grab)
{
	(void)grab;
}

static void
qdwin_popup_grab_motion(struct weston_pointer_grab *grab,
			const struct timespec *time,
			struct weston_pointer_motion_event *event)
{
	struct weston_pointer *pointer = grab->pointer;
	weston_pointer_move(pointer, event);

	/* Re-pick focus so the popup surface receives enter/motion
	 * while the grab is active — hover highlighting on the menu
	 * depends on it. */
	struct weston_view *view = weston_compositor_pick_view(
		pointer->seat->compositor, pointer->pos);
	if (view != pointer->focus)
		weston_pointer_set_focus(pointer, view);

	weston_pointer_send_motion(pointer, time, event);
}

static void
qdwin_popup_grab_button(struct weston_pointer_grab *grab,
			const struct timespec *time,
			uint32_t button, uint32_t state)
{
	struct qdwin_popup *p = wl_container_of(grab, p, grab);
	struct weston_pointer *pointer = grab->pointer;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
	    !qdwin_popup_bbox_contains(p, pointer->pos)) {
		weston_log("qdwin: popup dismissed by outside click "
			   "at (%.0f,%.0f)\n",
			   pointer->pos.c.x, pointer->pos.c.y);
		/* Don't free `p` synchronously from inside the grab
		 * callback — libweston may still dereference the grab
		 * pointer after we return. End the grab first so
		 * pointer->grab is rebound to the default grab, then
		 * notify the client; qdshell destroys the qdwin_popup_v1
		 * proxy in response, which triggers teardown through
		 * qdwin_popup_resource_destroyed on the next dispatch. */
		if (p->grab_active) {
			weston_pointer_end_grab(pointer);
			p->grab_active = 0;
		}
		if (p->resource)
			qdwin_popup_v1_send_dismissed(p->resource);
		return;
	}
	weston_pointer_send_button(pointer, time, button, state);

	/* qdwin_shell_v1@v21 popup_button — forward the click to the
	 * shell as a typed event with popup-local coords. Same fix as
	 * chrome_button: weston_pointer_send_button targets the shell's
	 * own popup wl_surface, but libweston's same-client suppression
	 * means qdshell never sees the wl_pointer.button event there,
	 * making popup menu items inert. We bypass it by delivering a
	 * shell-protocol event the shell binding does receive. */
	struct qdwin *qd = (p->parent && p->parent->qdwin) ?
		p->parent->qdwin : NULL;
	if (qd && qd->shell_resource && p->parent && p->view &&
	    wl_resource_get_version(qd->shell_resource) >= 21 &&
	    qdwin_popup_bbox_contains(p, pointer->pos)) {
		struct weston_coord_global vp =
			weston_view_get_pos_offset_global(p->view);
		double sx = pointer->pos.c.x - vp.c.x;
		double sy = pointer->pos.c.y - vp.c.y;
		qdwin_shell_v1_send_popup_button(
			qd->shell_resource,
			p->parent->handle,
			wl_fixed_from_double(sx),
			wl_fixed_from_double(sy),
			button,
			state);
	}
}

static void
qdwin_popup_grab_axis(struct weston_pointer_grab *grab,
		      const struct timespec *time,
		      struct weston_pointer_axis_event *event)
{
	weston_pointer_send_axis(grab->pointer, time, event);
}

static void
qdwin_popup_grab_axis_source(struct weston_pointer_grab *grab, uint32_t source)
{
	weston_pointer_send_axis_source(grab->pointer, source);
}

static void
qdwin_popup_grab_frame(struct weston_pointer_grab *grab)
{
	weston_pointer_send_frame(grab->pointer);
}

static void
qdwin_popup_grab_cancel(struct weston_pointer_grab *grab)
{
	struct qdwin_popup *p = wl_container_of(grab, p, grab);
	/* libweston has cancelled the grab; mark it inactive so teardown
	 * doesn't try to end it again. Defer actual teardown to the
	 * client resource-destroy path (same reasoning as button). */
	p->grab_active = 0;
	if (p->resource)
		qdwin_popup_v1_send_dismissed(p->resource);
}

/* ------------------------------------------------------------------
 * §6.8 S3b — default-pointer-grab override for nested-proxy input
 * forwarding.
 *
 * `weston_compositor_set_default_pointer_grab` lets a shell replace
 * the compositor-wide default grab interface with its own. Our
 * interface delegates fully to the standard delivery helpers so
 * normal client wl_pointer routing keeps working, AND additionally
 * encodes events into QDNI packets when the focused view is a
 * nested-proxy. Other grabs (popup, view-stream) still take over
 * via `weston_pointer_start_grab`; when they `end_grab`, weston
 * restores the default — i.e. our interface — so we never need to
 * re-arm.
 *
 * Single-process singleton: qdwin owns this; lookup via the
 * `qdwin_singleton` static (one shell per weston instance). The
 * grab struct itself is owned by libweston, so we can't attach
 * per-grab state.
 * ------------------------------------------------------------------ */

static struct qdwin *qdwin_singleton = NULL;  /* set in wet_shell_init */

static struct qdwin_toplevel *
qdwin_proxy_for_view(struct qdwin *qdwin, struct weston_view *v)
{
	if (!qdwin || !v)
		return NULL;
	struct qdwin_toplevel *tl;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		if (tl->is_nested_proxy && tl->view == v)
			return tl;
	}
	return NULL;
}

static uint32_t
qdwin_proxy_time_msec(const struct timespec *ts)
{
	if (!ts) return 0;
	return (uint32_t)(ts->tv_sec * 1000u +
			  (uint32_t)(ts->tv_nsec / 1000000L));
}

/* Update qdwin->active_input_proxy to match the pointer's current
 * focus. Sends focus_leave on the previous proxy, focus_enter on the
 * new one. No-op when the active proxy is unchanged. */
static void
qdwin_proxy_pointer_track_focus(struct qdwin *qdwin,
				struct weston_pointer *pointer)
{
	struct qdwin_toplevel *new_proxy =
		qdwin_proxy_for_view(qdwin, pointer->focus);
	if (new_proxy == qdwin->active_input_proxy)
		return;
	struct qdwin_toplevel *old = qdwin->active_input_proxy;
	if (old && old->proxy_input_sink_fd >= 0)
		qdwin_nested_input_sink_send_focus(old->proxy_input_sink_fd, 0);
	qdwin->active_input_proxy = new_proxy;
	if (new_proxy && new_proxy->proxy_input_sink_fd >= 0)
		qdwin_nested_input_sink_send_focus(
			new_proxy->proxy_input_sink_fd, 1);
}

static void
qdwin_proxy_default_grab_focus(struct weston_pointer_grab *grab)
{
	struct weston_pointer *pointer = grab->pointer;
	struct weston_view *view = weston_compositor_pick_view(
		pointer->seat->compositor, pointer->pos);
	if (view != pointer->focus)
		weston_pointer_set_focus(pointer, view);
	qdwin_proxy_pointer_track_focus(qdwin_singleton, pointer);
}

static void
qdwin_proxy_default_grab_motion(struct weston_pointer_grab *grab,
				const struct timespec *time,
				struct weston_pointer_motion_event *event)
{
	struct weston_pointer *pointer = grab->pointer;
	weston_pointer_move(pointer, event);
	struct weston_view *view = weston_compositor_pick_view(
		pointer->seat->compositor, pointer->pos);
	/* plan3 post-review: override the picked view with the layer-shell
	 * view ONLY when the picker resolved to a cursor sprite (a view
	 * whose surface holds the wl_pointer-cursor role). With the M2
	 * commit listener keeping cursor input regions empty, the picker
	 * should already skip cursor sprites — but if a sprite leaks a
	 * non-empty input region mid-commit, this override prevents the
	 * focus-on-cursor regression. Avoid unconditional override: the
	 * SIGABRT trace observed on the daily VM was through this exact
	 * code path, and forcing a focus change to a layer view that the
	 * picker didn't pick can hit weston_view_set_position_with_offset
	 * assertions when the layer view is mid-commit. */
	if (view && view->surface) {
		const char *role = weston_surface_get_role(view->surface);
		if (role && strcmp(role, "wl_pointer-cursor") == 0) {
			struct weston_view *layer_view =
				qdwin_layer_surface_view_at_pos(qdwin_singleton,
								 pointer->pos);
			if (layer_view)
				view = layer_view;
		}
	}
	if (view != pointer->focus)
		weston_pointer_set_focus(pointer, view);
	qdwin_proxy_pointer_track_focus(qdwin_singleton, pointer);
	/* B6: belt-and-braces — if the pointer has no sprite (no client
	 * owns the cursor over the focused area), install our default. */
	if (!pointer->sprite && qdwin_singleton)
		qdwin_install_default_cursor_on_pointer(qdwin_singleton, pointer);
	weston_pointer_send_motion(pointer, time, event);

	struct qdwin_toplevel *tl = qdwin_singleton ?
		qdwin_singleton->active_input_proxy : NULL;
	if (tl && tl->view && tl->view->surface &&
	    tl->proxy_input_sink_fd >= 0) {
		weston_view_update_transform(tl->view);
		struct weston_coord_global gpos = pointer->pos;
		struct weston_coord_surface cs =
			weston_coord_global_to_surface(tl->view, gpos);
		qdwin_nested_input_sink_send_motion(
			tl->proxy_input_sink_fd,
			qdwin_proxy_time_msec(time),
			wl_fixed_from_double(cs.c.x),
			wl_fixed_from_double(cs.c.y));
	}
}

/* Hit-test a global pointer position against every chrome side of every
 * toplevel and return the first match (ordered by toplevel iteration —
 * with z-stack mattering little for chrome since each toplevel's chrome
 * is co-located with its content).
 *
 * Why this exists: weston_compositor_pick_view returns the cursor
 * sprite view (role=wl_pointer-cursor) when the cursor is composited
 * into the framebuffer (pixman renderer + cursor-sprites helper, the
 * post-task(135) configuration). pointer->focus is therefore the
 * cursor view, not the chrome surface underneath, breaking any
 * click handling that relies on pointer->focus. Doing the hit-test
 * ourselves against tracked chrome views sidesteps the picker and
 * removes the dependency on whatever weston's view-ordering happens
 * to be after a cursor-set transition. */
static struct qdwin_toplevel *
qdwin_chrome_at_pos(struct qdwin *qdwin, struct weston_coord_global pos,
		    int *side_out, double *sx_out, double *sy_out)
{
	if (!qdwin)
		return NULL;
	struct qdwin_toplevel *tl;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		for (int s = 0; s < QDWIN_SIDES; s++) {
			struct weston_view *cv = tl->chrome[s].view;
			struct weston_surface *cs = tl->chrome[s].surface;
			if (!cv || !cs || cs->width <= 0 || cs->height <= 0)
				continue;
			weston_view_update_transform(cv);
			struct weston_coord_global vp =
				weston_view_get_pos_offset_global(cv);
			double x0 = vp.c.x, y0 = vp.c.y;
			double x1 = x0 + cs->width, y1 = y0 + cs->height;
			if (pos.c.x < x0 || pos.c.x >= x1 ||
			    pos.c.y < y0 || pos.c.y >= y1)
				continue;
			if (side_out) *side_out = s;
			if (sx_out) *sx_out = pos.c.x - x0;
			if (sy_out) *sy_out = pos.c.y - y0;
			return tl;
		}
	}
	return NULL;
}

/* Same as qdwin_chrome_at_pos but matches the content view too. Returns
 * the toplevel whose content OR chrome bbox contains `pos`. Used by
 * click-to-focus (it doesn't care which side, only which toplevel). */
static struct qdwin_toplevel *
qdwin_toplevel_at_pos(struct qdwin *qdwin, struct weston_coord_global pos)
{
	if (!qdwin)
		return NULL;
	int dummy;
	struct qdwin_toplevel *tl =
		qdwin_chrome_at_pos(qdwin, pos, &dummy, NULL, NULL);
	if (tl)
		return tl;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		struct weston_view *v = tl->view;
		struct weston_surface *cs = v ? v->surface : NULL;
		if (!v || !cs || cs->width <= 0 || cs->height <= 0)
			continue;
		weston_view_update_transform(v);
		struct weston_coord_global vp =
			weston_view_get_pos_offset_global(v);
		if (pos.c.x >= vp.c.x &&
		    pos.c.x < vp.c.x + cs->width &&
		    pos.c.y >= vp.c.y &&
		    pos.c.y < vp.c.y + cs->height)
			return tl;
	}
	return NULL;
}

static void
qdwin_proxy_default_grab_button(struct weston_pointer_grab *grab,
				const struct timespec *time,
				uint32_t button, uint32_t state)
{
	struct weston_pointer *pointer = grab->pointer;
	struct weston_view *layer_view =
		qdwin_layer_surface_view_at_pos(qdwin_singleton, pointer->pos);
	if (layer_view && pointer->focus != layer_view)
		weston_pointer_set_focus(pointer, layer_view);

	/* plan3 H1 (post-deep-review): a press that landed on a layer-shell
	 * surface is governed by that surface's keyboard-interactivity (NONE
	 * leaves focus alone, ON_DEMAND transfers, EXCLUSIVE was already
	 * granted at map time). The toplevel click-to-focus block below
	 * would otherwise steal keyboard focus to the toplevel behind the
	 * layer surface — that broke Quickshell text fields whose layer
	 * panel overlaps a window. */
	int press_on_layer =
		(state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_LEFT &&
		 layer_view != NULL);

	/* plan3 M4: zwlr_layer_surface_v1 ON_DEMAND keyboard interactivity.
	 * On left-button press over a layer-shell surface that requested
	 * ON_DEMAND, transfer keyboard focus to that surface so that text
	 * fields inside Quickshell popups can receive input. The helper
	 * is defined later (the qdwin_layer_surface struct is incomplete
	 * here) — see qdwin_layer_surface_handle_on_demand_button. */
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_LEFT)
		qdwin_layer_surface_handle_on_demand_button(qdwin_singleton,
							    pointer);
	/* Click-to-focus: on left-button press, raise + focus the toplevel
	 * under the cursor (whether the pointer is on its content surface
	 * or on a chrome side). Without this, clicking a background window
	 * delivers the button event but doesn't change z-order or
	 * keyboard focus — the user can't pick a window with the mouse.
	 *
	 * Done BEFORE delivering the button so the click also lands on
	 * the now-focused window in the same dispatch (matches gnome /
	 * kwin / kiosk-shell behaviour).
	 *
	 * Skip this block when the press landed on a layer-shell view —
	 * layer-shell focus is owned by the layer-shell interactivity mode,
	 * not by click-to-raise. See press_on_layer above. */
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_LEFT &&
	    !press_on_layer && qdwin_singleton) {
		/* Look up the toplevel under the pointer by position rather
		 * than by pointer->focus — see qdwin_chrome_at_pos comment
		 * for why pointer->focus is unreliable when the cursor is
		 * composited into the framebuffer. */
		struct qdwin_toplevel *tl_under =
			qdwin_toplevel_at_pos(qdwin_singleton, pointer->pos);
		if (tl_under) {
			/* Raise on the normal layer (re-stack to top). */
			qdwin_toplevel_move_to_layer(tl_under,
						     &qdwin_singleton->normal_layer);
			/* Move keyboard focus if it isn't already here. */
			struct weston_keyboard *kb =
				weston_seat_get_keyboard(pointer->seat);
			struct weston_surface *content = tl_under->view ?
				tl_under->view->surface : NULL;
			if (kb && content && kb->focus != content) {
				weston_keyboard_set_focus(kb, content);
				/* Tell the shell so it can update its
				 * internal seat-focus state. The emit
				 * helper self-checks shell version. */
				qdwin_emit_seat_focus_changed(
					qdwin_singleton, pointer->seat,
					tl_under->handle);
			}
			weston_compositor_schedule_repaint(
				qdwin_singleton->compositor);
		}
	}
	weston_pointer_send_button(pointer, time, button, state);

	/* qdwin_shell_v1@v20 chrome_button — forward button events on
	 * shell-owned chrome surfaces to the shell directly, bypassing two
	 * libweston-side gotchas:
	 *
	 * (1) The same-client suppression: pointer events libweston tries
	 *     to deliver to surfaces owned by the shell client are not
	 *     reliably routed through wl_pointer (see
	 *     ), so qdshell never sees the
	 *     wl_pointer.button event for clicks on its own chrome.
	 *
	 * (2) The cursor-sprite picker shadow: with pixman renderer +
	 *     post-task(135) cursor-sprites helper, weston composites the
	 *     cursor into the primary framebuffer as a regular weston_view
	 *     and weston_compositor_pick_view returns it as the topmost
	 *     view at the pointer position. So pointer->focus is the
	 *     cursor sprite (role=wl_pointer-cursor), not the chrome
	 *     underneath — using pointer->focus to look up the chrome
	 *     toplevel here would miss every click. We hit-test against
	 *     tracked chrome bboxes directly instead.
	 *
	 * Without this whole path, clicks on the close / maximize /
	 * minimize glyphs that qdshell paints on the titlebar are silently
	 * dropped; see the chrome_button XML event docs for the contract.
	 */
	if (qdwin_singleton && qdwin_singleton->shell_resource &&
	    wl_resource_get_version(qdwin_singleton->shell_resource) >= 20) {
		int side = -1;
		double sx = 0, sy = 0;
		struct qdwin_toplevel *chrome_tl =
			qdwin_chrome_at_pos(qdwin_singleton, pointer->pos,
					    &side, &sx, &sy);
		if (chrome_tl && side >= 0) {
			qdwin_shell_v1_send_chrome_button(
				qdwin_singleton->shell_resource,
				chrome_tl->handle,
				(uint32_t)side,
				wl_fixed_from_double(sx),
				wl_fixed_from_double(sy),
				button,
				state);
		}
	}

	struct qdwin_toplevel *tl = qdwin_singleton ?
		qdwin_singleton->active_input_proxy : NULL;
	if (tl && tl->proxy_input_sink_fd >= 0)
		qdwin_nested_input_sink_send_button(
			tl->proxy_input_sink_fd,
			qdwin_proxy_time_msec(time), button, state);
}

static void
qdwin_proxy_default_grab_axis(struct weston_pointer_grab *grab,
			      const struct timespec *time,
			      struct weston_pointer_axis_event *event)
{
	weston_pointer_send_axis(grab->pointer, time, event);
	struct qdwin_toplevel *tl = qdwin_singleton ?
		qdwin_singleton->active_input_proxy : NULL;
	if (tl && tl->proxy_input_sink_fd >= 0)
		qdwin_nested_input_sink_send_axis(
			tl->proxy_input_sink_fd,
			qdwin_proxy_time_msec(time),
			event->axis,
			wl_fixed_from_double(event->value));
}

static void
qdwin_proxy_default_grab_axis_source(struct weston_pointer_grab *grab,
				     uint32_t source)
{
	weston_pointer_send_axis_source(grab->pointer, source);
}

static void
qdwin_proxy_default_grab_frame(struct weston_pointer_grab *grab)
{
	weston_pointer_send_frame(grab->pointer);
}

static void
qdwin_proxy_default_grab_cancel(struct weston_pointer_grab *grab)
{
	(void)grab;
	/* libweston cancels the default grab when shutting down; nothing
	 * to free since we own no per-grab state. */
}

static const struct weston_pointer_grab_interface
qdwin_proxy_default_pointer_grab_iface = {
	qdwin_proxy_default_grab_focus,
	qdwin_proxy_default_grab_motion,
	qdwin_proxy_default_grab_button,
	qdwin_proxy_default_grab_axis,
	qdwin_proxy_default_grab_axis_source,
	qdwin_proxy_default_grab_frame,
	qdwin_proxy_default_grab_cancel,
};

/* ------------------------------------------------------------------
 * §6.8 S3c — always-active keyboard grab for nested-proxy input
 * forwarding.
 *
 * libweston has no `weston_compositor_set_default_keyboard_grab`
 * mirror of the pointer one, but `weston_keyboard.default_grab` is a
 * public field whose `interface` pointer can be swapped per seat.
 * We do that for every weston_keyboard at seat creation (and on caps
 * changes for hot-pluggable keyboards). When other grabs (popup,
 * input-method) start and end via weston_keyboard_start_grab, the
 * keyboard's grab pointer flips to them and back to default — and
 * because we mutated the default's interface itself, our grab is
 * always what "default" means.
 *
 * Methods delegate to weston_keyboard_send_key/send_modifiers for
 * normal client routing, and additionally encode QDNI key events
 * when active_input_proxy is set (the same pointer-driven state
 * machine S3b uses). Modifiers don't need a separate QDNI message:
 * the nested side calls notify_key with STATE_UPDATE_AUTOMATIC and
 * the inner xkb tracker derives mod state from keycodes alone.
 * ------------------------------------------------------------------ */

static void
qdwin_proxy_default_grab_key(struct weston_keyboard_grab *grab,
			     const struct timespec *time,
			     uint32_t key, uint32_t state)
{
	struct weston_keyboard *kb = grab->keyboard;
	weston_keyboard_send_key(kb, time, key, state);

	struct qdwin_toplevel *tl = qdwin_singleton ?
		qdwin_singleton->active_input_proxy : NULL;
	if (tl && tl->proxy_input_sink_fd >= 0)
		qdwin_nested_input_sink_send_key(
			tl->proxy_input_sink_fd,
			qdwin_proxy_time_msec(time), key, state);
}

static void
qdwin_proxy_default_grab_modifiers(struct weston_keyboard_grab *grab,
				   uint32_t serial,
				   uint32_t mods_depressed,
				   uint32_t mods_latched,
				   uint32_t mods_locked,
				   uint32_t group)
{
	weston_keyboard_send_modifiers(grab->keyboard, serial,
				       mods_depressed, mods_latched,
				       mods_locked, group);
	/* No QDNI modifiers event — see comment block above. */
}

static void
qdwin_proxy_default_grab_kb_cancel(struct weston_keyboard_grab *grab)
{
	(void)grab;
}

static const struct weston_keyboard_grab_interface
qdwin_proxy_default_keyboard_grab_iface = {
	qdwin_proxy_default_grab_key,
	qdwin_proxy_default_grab_modifiers,
	qdwin_proxy_default_grab_kb_cancel,
};

/* Swap the keyboard's default-grab interface for ours. Idempotent
 * (calling repeatedly is a no-op once the iface ptr matches). The
 * field is part of weston_keyboard's public ABI; libweston-14 has
 * carried it stable across the 14.x point releases. */
static void
qdwin_install_default_keyboard_grab(struct weston_seat *seat)
{
	struct weston_keyboard *kb = weston_seat_get_keyboard(seat);
	if (!kb)
		return;
	if (kb->default_grab.interface ==
	    &qdwin_proxy_default_keyboard_grab_iface)
		return;
	kb->default_grab.interface =
		&qdwin_proxy_default_keyboard_grab_iface;
	weston_log("qdwin/nested-proxy: default keyboard grab installed "
		   "on seat '%s'\n",
		   seat->seat_name ? seat->seat_name : "(unnamed)");
}

static const struct weston_pointer_grab_interface qdwin_popup_grab_iface = {
	qdwin_popup_grab_focus,
	qdwin_popup_grab_motion,
	qdwin_popup_grab_button,
	qdwin_popup_grab_axis,
	qdwin_popup_grab_axis_source,
	qdwin_popup_grab_frame,
	qdwin_popup_grab_cancel,
};

static void
qdwin_popup_start_grab(struct qdwin_popup *p, struct weston_compositor *ec)
{
	struct weston_seat *seat;
	wl_list_for_each(seat, &ec->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);
		if (!pointer)
			continue;
		p->grab.interface = &qdwin_popup_grab_iface;
		weston_pointer_start_grab(pointer, &p->grab);
		p->grab_active = 1;
		return;  /* MVP: first seat with a pointer wins */
	}
}

static void
qdwin_popup_surface_destroyed(struct wl_listener *listener, void *data)
{
	struct qdwin_popup *p =
		wl_container_of(listener, p, surface_destroy);
	(void)data;
	weston_log("qdwin: popup surface destroyed by client — dismissing\n");
	qdwin_popup_teardown(p);
}

static void
qdwin_popup_teardown(struct qdwin_popup *p)
{
	if (p->grab_active) {
		weston_pointer_end_grab(p->grab.pointer);
		p->grab_active = 0;
	}
	if (p->parent && p->parent->popup == p)
		p->parent->popup = NULL;
	if (p->view) {
		weston_view_destroy(p->view);
		p->view = NULL;
	}
	if (p->surface) {
		wl_list_remove(&p->surface_destroy.link);
		p->surface = NULL;
	}
	if (p->resource) {
		/* Detach the listener slot so the resource-destroy path
		 * stops calling back into a freed qdwin_popup. The
		 * resource itself is destroyed by wl_resource_destroy
		 * (called below or by libwayland on client teardown). */
		wl_resource_set_user_data(p->resource, NULL);
	}
	free(p);
}

static void
qdwin_popup_handle_destroy(struct wl_client *client,
			   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_popup_resource_destroyed(struct wl_resource *resource)
{
	struct qdwin_popup *p = wl_resource_get_user_data(resource);
	if (!p)
		return;
	/* Client dropped the popup object — tear down the view. */
	p->resource = NULL;
	qdwin_popup_teardown(p);
}

static const struct qdwin_popup_v1_interface qdwin_popup_impl = {
	.destroy = qdwin_popup_handle_destroy,
};

static void
qdwin_handle_show_popup(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t popup_id,
			uint32_t parent,
			struct wl_resource *surface_res,
			int32_t x, int32_t y)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);

	if (!qdwin_shell_require_bound(qdwin, resource))
		return;

	struct qdwin_toplevel *tl = qdwin_toplevel_from_handle(qdwin, parent);
	if (!tl) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_INVALID_HANDLE,
				       "show_popup: unknown parent handle %u",
				       parent);
		return;
	}
	struct weston_surface *surface = wl_resource_get_user_data(surface_res);
	if (!surface) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_INVALID_HANDLE,
				       "show_popup: surface has no weston state");
		return;
	}

	/* One popup per toplevel. If another is already active, dismiss
	 * it before creating the new one. */
	if (tl->popup) {
		qdwin_popup_v1_send_dismissed(tl->popup->resource);
		qdwin_popup_teardown(tl->popup);
	}

	struct qdwin_popup *p = calloc(1, sizeof *p);
	if (!p) {
		wl_client_post_no_memory(client);
		return;
	}
	p->parent = tl;
	p->surface = surface;
	p->x = x;
	p->y = y;

	p->resource = wl_resource_create(client, &qdwin_popup_v1_interface, 1,
					 popup_id);
	if (!p->resource) {
		free(p);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(p->resource, &qdwin_popup_impl,
				       p, qdwin_popup_resource_destroyed);

	p->view = weston_view_create(surface);
	if (!p->view) {
		wl_resource_destroy(p->resource);
		/* resource_destroyed ran free(p) already */
		return;
	}

	/* Position in output-absolute coords. The shell expresses x,y
	 * relative to the outer rect of the parent (origin = N chrome
	 * top-left). Our chrome N view sits at exactly that origin, so
	 * anchor there; if chrome hasn't been attached yet, fall back to
	 * the content view origin. */
	double anchor_x, anchor_y;
	struct weston_view *anchor = tl->chrome[QDWIN_SIDE_N].view
		? tl->chrome[QDWIN_SIDE_N].view
		: tl->view;
	struct weston_coord_global ap =
		weston_view_get_pos_offset_global(anchor);
	anchor_x = ap.c.x;
	anchor_y = ap.c.y;
	struct weston_coord_global pos = {
		.c = weston_coord(anchor_x + x, anchor_y + y),
	};
	weston_view_set_position(p->view, pos);
	weston_view_update_transform(p->view);
	weston_view_move_to_layer(p->view, &qdwin->popup_layer.view_list);

	p->surface_destroy.notify = qdwin_popup_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &p->surface_destroy);

	if (!weston_surface_is_mapped(surface))
		weston_surface_map(surface);

	tl->popup = p;
	qdwin_popup_start_grab(p, qdwin->compositor);
	weston_compositor_damage_all(qdwin->compositor);
	weston_log("qdwin: show_popup parent=%u at (%d,%d) -> view placed at (%.0f,%.0f) grab=%d\n",
		   parent, x, y, pos.c.x, pos.c.y, p->grab_active);
}

/* ------------------------------------------------------------------
 * §6.5 S2: per-view stream subscribe/destroy handlers.
 *
 * MVP scope: pin one toplevel's content+chrome views onto a
 * pipewire output, emit approved with the pipewire node name so
 * an external consumer can bind. RDP fields in approved are stubs
 * until qdistro-forward lands in S3.
 * ------------------------------------------------------------------ */

static int
qdwin_output_has_stream(struct qdwin *qdwin, struct weston_output *o)
{
	struct qdwin_view_stream *s;
	wl_list_for_each(s, &qdwin->view_streams, link)
		if (s->pw_output == o)
			return 1;
	return 0;
}

static struct weston_output *
qdwin_find_free_pipewire_output(struct qdwin *qdwin)
{
	struct weston_output *o;
	wl_list_for_each(o, &qdwin->compositor->output_list, link) {
		if (strncmp(o->name, "pipewire", 8) != 0)
			continue;
		if (qdwin_output_has_stream(qdwin, o))
			continue;
		return o;
	}
	return NULL;
}

static struct weston_output *
qdwin_find_non_pipewire_output(struct qdwin *qdwin)
{
	struct weston_output *o;
	wl_list_for_each(o, &qdwin->compositor->output_list, link)
		if (strncmp(o->name, "pipewire", 8) != 0)
			return o;
	return NULL;
}

static void
qdwin_view_stream_pin(struct qdwin_view_stream *s)
{
	struct qdwin_toplevel *tl = s->tl;
	struct weston_output *pw = s->pw_output;
	if (!tl || !tl->view || !pw)
		return;

	/* Remember where the view was so we can restore on teardown. */
	s->prev_pos = weston_view_get_pos_offset_global(tl->view);
	s->prev_output = tl->view->output;

	/* Move view inside the pipewire output's bounding rect so it stops
	 * intersecting the primary output and therefore stops rendering
	 * there. Position flush to the pipewire output origin plus a small
	 * inset so chrome N has room above. */
	struct weston_coord_global pos = {
		.c = weston_coord(pw->pos.c.x + 32, pw->pos.c.y + 32),
	};
	weston_view_set_position(tl->view, pos);
	weston_view_set_output(tl->view, pw);
	for (int side = 0; side < QDWIN_SIDES; side++)
		if (tl->chrome[side].view)
			weston_view_set_output(tl->chrome[side].view, pw);
	qdwin_toplevel_position_chrome(tl);
	weston_view_update_transform(tl->view);
	/* Damage the surface(s) so the next pipewire-output repaint
	 * actually composites the view bytes — without this, the first
	 * PipeWire frame can be the pre-pin empty framebuffer of the
	 * pipewire output, which the consumer captures and shows as
	 * mostly black. */
	if (tl->view->surface)
		weston_surface_damage(tl->view->surface);
	for (int side = 0; side < QDWIN_SIDES; side++)
		if (tl->chrome[side].view && tl->chrome[side].view->surface)
			weston_surface_damage(tl->chrome[side].view->surface);
	weston_output_schedule_repaint(pw);
	s->pinned = 1;
}

static void
qdwin_view_stream_unpin(struct qdwin_view_stream *s)
{
	if (!s->pinned)
		return;
	s->pinned = 0;
	struct qdwin_toplevel *tl = s->tl;
	if (!tl || !tl->view)
		return;

	struct weston_output *target = s->prev_output;
	if (!target)
		target = qdwin_find_non_pipewire_output(s->qdwin);
	if (target) {
		weston_view_set_output(tl->view, target);
		for (int side = 0; side < QDWIN_SIDES; side++)
			if (tl->chrome[side].view)
				weston_view_set_output(tl->chrome[side].view, target);
	}
	weston_view_set_position(tl->view, s->prev_pos);
	qdwin_toplevel_position_chrome(tl);
	weston_view_update_transform(tl->view);
	if (target)
		weston_output_schedule_repaint(target);
}

/* Hex-encode n random bytes into out (out_len must be 2*n+1). */
static int
qdwin_hex_token(char *out, size_t out_len, size_t n_bytes)
{
	unsigned char buf[32];
	if (n_bytes > sizeof buf)
		n_bytes = sizeof buf;
	ssize_t got = getrandom(buf, n_bytes, 0);
	if (got != (ssize_t)n_bytes) {
		out[0] = '\0';
		return -1;
	}
	static const char hex[] = "0123456789abcdef";
	size_t slots = (out_len - 1) / 2;
	if (slots > n_bytes)
		slots = n_bytes;
	for (size_t i = 0; i < slots; i++) {
		out[2*i]     = hex[buf[i] >> 4];
		out[2*i + 1] = hex[buf[i] & 0xf];
	}
	out[2*slots] = '\0';
	return 0;
}

/* Reject QDWIN_FORWARD_BIN unless it is a regular file owned by root
 * with no group/world write bits — matches qdlocker config-hardening
 * predicate. Compositor process may be reached by the session user, so
 * a writable forwarder would let them inject code into the compositor's
 * forked child. */
static int
qdwin_forward_bin_is_trusted(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) {
		weston_log("qdwin: WARN QDWIN_FORWARD_BIN=%s lstat failed: %m; "
			   "falling back to compiled-in path\n", path);
		return 0;
	}
	if (!S_ISREG(st.st_mode)) {
		weston_log("qdwin: WARN QDWIN_FORWARD_BIN=%s is not a regular "
			   "file; falling back to compiled-in path\n", path);
		return 0;
	}
	if (st.st_uid != 0) {
		weston_log("qdwin: WARN QDWIN_FORWARD_BIN=%s not owned by root "
			   "(uid=%u); falling back to compiled-in path\n",
			   path, (unsigned)st.st_uid);
		return 0;
	}
	if (st.st_mode & (S_IWGRP | S_IWOTH)) {
		weston_log("qdwin: WARN QDWIN_FORWARD_BIN=%s is group/world-"
			   "writable; falling back to compiled-in path\n", path);
		return 0;
	}
	return 1;
}

static const char *
qdwin_forward_bin_path(void)
{
	const char *env = getenv("QDWIN_FORWARD_BIN");
	if (env && *env && qdwin_forward_bin_is_trusted(env))
		return env;
	return "/usr/bin/qdistro-forward";
}

static int
qdwin_forward_write_secret(int fd, const char *secret)
{
	const char *p = secret ? secret : "";
	size_t left = strlen(p);

	while (left > 0) {
		ssize_t n = write(fd, p, left);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		left -= (size_t)n;
	}
	return 0;
}

static int
qdwin_view_stream_spawn_forward(struct qdwin_view_stream *s,
				const char *pw_node_name,
				int view_width, int view_height)
{
	int token_pipe[2] = { -1, -1 };
	int password_pipe[2] = { -1, -1 };
	if (pipe(token_pipe) != 0) {
		weston_log("qdwin: pipe failed for qdistro-forward token: %m\n");
		return -1;
	}
	if (pipe(password_pipe) != 0) {
		weston_log("qdwin: pipe failed for qdistro-forward password: %m\n");
		close(token_pipe[0]);
		close(token_pipe[1]);
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		weston_log("qdwin: fork failed for qdistro-forward: %m\n");
		close(token_pipe[0]);
		close(token_pipe[1]);
		close(password_pipe[0]);
		close(password_pipe[1]);
		return -1;
	}
	if (pid == 0) {
		close(token_pipe[1]);
		close(password_pipe[1]);
		/* Child. Reset signal handlers weston installed on our behalf. */
		signal(SIGPIPE, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		/* Weston's mainloop blocks SIGTERM/SIGINT/SIGCHLD/SIGUSR1/SIGUSR2
		 * (they arrive via a signalfd drained by the event loop). The
		 * signal mask is inherited across fork+execve, so without this
		 * the child can install a SIGTERM handler but never see it
		 * delivered — waitpid of the eventual reap blocks forever.
		 * Clear the full mask so the child runs with the standard
		 * "no signals blocked" state. */
		sigset_t empty;
		sigemptyset(&empty);
		sigprocmask(SIG_SETMASK, &empty, NULL);

		char port_arg[16];
		char token_fd_arg[16];
		char password_fd_arg[16];
		char width_arg[16];
		char height_arg[16];
		snprintf(port_arg,   sizeof port_arg,   "%u", s->rdp_port);
		snprintf(token_fd_arg, sizeof token_fd_arg, "%d", token_pipe[0]);
		snprintf(password_fd_arg, sizeof password_fd_arg, "%d", password_pipe[0]);
		snprintf(width_arg,  sizeof width_arg,  "%d", view_width  > 0 ? view_width  : 640);
		snprintf(height_arg, sizeof height_arg, "%d", view_height > 0 ? view_height : 480);

		const char *bin = qdwin_forward_bin_path();
		execl(bin, bin,
		      "--pipewire-node", pw_node_name,
		      "--access-token-fd", token_fd_arg,
		      "--rdp-port", port_arg,
		      "--rdp-password-fd", password_fd_arg,
		      "--wayland-display", "wayland-0",
		      "--width", width_arg,
		      "--height", height_arg,
		      (char *)NULL);
		fprintf(stderr, "exec %s failed: %m\n", bin);
		_exit(127);
	}
	int secret_write_failed = 0;
	if (qdwin_forward_write_secret(token_pipe[1], s->access_token) < 0) {
		weston_log("qdwin: writing qdistro-forward token failed: %m\n");
		secret_write_failed = 1;
	}
	if (qdwin_forward_write_secret(password_pipe[1], s->rdp_password) < 0) {
		weston_log("qdwin: writing qdistro-forward password failed: %m\n");
		secret_write_failed = 1;
	}
	close(token_pipe[1]);
	close(password_pipe[1]);
	close(token_pipe[0]);
	close(password_pipe[0]);
	if (secret_write_failed) {
		kill(pid, SIGTERM);
		waitpid(pid, NULL, WNOHANG);
		return -1;
	}
	s->forward_pid = pid;
	weston_log("qdwin: spawned qdistro-forward pid=%d port=%u node=%s\n",
		   (int)pid, s->rdp_port, pw_node_name);
	return 0;
}

static void
qdwin_view_stream_reap_forward(struct qdwin_view_stream *s)
{
	if (s->forward_pid <= 0)
		return;
	if (kill(s->forward_pid, SIGTERM) != 0 && errno != ESRCH)
		weston_log("qdwin: SIGTERM qdistro-forward pid=%d failed: %m\n",
			   (int)s->forward_pid);
	/* Non-blocking reap; let weston's SIGCHLD-or-idle loop catch it.
	 * If we block here we stall the wayland dispatch. */
	int status;
	pid_t got = waitpid(s->forward_pid, &status, WNOHANG);
	if (got == s->forward_pid) {
		weston_log("qdwin: qdistro-forward pid=%d reaped status=%d\n",
			   (int)s->forward_pid, status);
	}
	s->forward_pid = 0;
}

static void
qdwin_stream_resource_destroyed(struct wl_resource *resource)
{
	struct qdwin_view_stream *s = wl_resource_get_user_data(resource);
	if (!s)
		return;
	/* If qdistro-forward was holding an input handle, post-destroy it
	 * via wl_resource_destroy — this also fires the handle's
	 * resource_destroyed which clears s->input_handle. We do this
	 * BEFORE freeing s so the handle's destroyed callback can still
	 * read its user_data. */
	if (s->input_handle) {
		struct wl_resource *h = s->input_handle;
		s->input_handle = NULL;
		s->input_claimed = 0;
		wl_resource_set_user_data(h, NULL);
		wl_resource_destroy(h);
	}
	qdwin_view_stream_reap_forward(s);
	qdwin_view_stream_unpin(s);
	qdwin_stream_seat_release(s);
	wl_list_remove(&s->link);
	free(s);
}

static void
qdwin_stream_handle_destroy(struct wl_client *client,
			    struct wl_resource *resource)
{
	(void)client;
	struct qdwin_view_stream *s = wl_resource_get_user_data(resource);
	if (s)
		qdwin_view_stream_v1_send_torn_down(resource, "client destroy");
	wl_resource_destroy(resource);
}

static const struct qdwin_view_stream_v1_interface qdwin_stream_impl = {
	.destroy = qdwin_stream_handle_destroy,
};

static void
qdwin_handle_subscribe_view_stream(struct wl_client *client,
				   struct wl_resource *shell_resource,
				   uint32_t stream_id,
				   uint32_t handle,
				   const char *peer_label,
				   int32_t desired_width,
				   int32_t desired_height,
				   uint32_t allow_input)
{
	struct qdwin *qdwin = wl_resource_get_user_data(shell_resource);
	(void)allow_input;

	if (!qdwin_shell_require_bound(qdwin, shell_resource))
		return;

	struct qdwin_toplevel *tl = qdwin_toplevel_from_handle(qdwin, handle);
	if (!tl) {
		wl_resource_post_error(shell_resource,
				       QDWIN_SHELL_V1_ERROR_INVALID_HANDLE,
				       "subscribe_view_stream: unknown handle %u",
				       handle);
		return;
	}

	struct wl_resource *stream_resource =
		wl_resource_create(client, &qdwin_view_stream_v1_interface,
				   1, stream_id);
	if (!stream_resource) {
		wl_client_post_no_memory(client);
		return;
	}

	struct weston_output *pw = qdwin_find_free_pipewire_output(qdwin);
	if (!pw) {
		/* Attach a minimal impl so destroy still frees the resource;
		 * then immediately emit denied. No stream struct allocated. */
		wl_resource_set_implementation(stream_resource,
					       &qdwin_stream_impl,
					       NULL, NULL);
		qdwin_view_stream_v1_send_denied(
			stream_resource,
			"no free pipewire output "
			"(increase [pipewire] num-outputs in weston.ini)");
		weston_log("qdwin: subscribe_view_stream denied "
			   "handle=%u peer_label=\"%s\" (no pw output)\n",
			   handle, peer_label ? peer_label : "");
		return;
	}

	struct qdwin_view_stream *s = calloc(1, sizeof *s);
	if (!s) {
		wl_resource_destroy(stream_resource);
		wl_client_post_no_memory(client);
		return;
	}
	s->resource = stream_resource;
	s->qdwin = qdwin;
	s->tl = tl;
	s->pw_output = pw;
	wl_list_insert(&qdwin->view_streams, &s->link);

	wl_resource_set_implementation(stream_resource, &qdwin_stream_impl,
				       s, qdwin_stream_resource_destroyed);

	qdwin_view_stream_pin(s);

	char node_name[128];
	snprintf(node_name, sizeof node_name, "weston.%s", pw->name);

	/* S3: spawn the external proxy. Generate token + password and
	 * reserve a port; the proxy uses these for its RDP listener. The
	 * forward must agree with weston's backend-pipewire on the stream
	 * dimensions — backend-pipewire offers a fixed-size raw video
	 * format at the output's mode, so pass the pipewire-output's
	 * mode (NOT the toplevel's surface size). desired_* from the
	 * subscriber is treated as a hint only and ignored if it doesn't
	 * fit; for now we always use the pipewire output's size. */
	(void)desired_width; (void)desired_height;
	s->rdp_port = qdwin->next_stream_port++;
	if (qdwin_hex_token(s->access_token, sizeof s->access_token, 16) < 0 ||
	    qdwin_hex_token(s->rdp_password, sizeof s->rdp_password, 8) < 0) {
		qdwin_view_stream_v1_send_denied(
			stream_resource,
			"kernel random source unavailable");
		weston_log("qdwin: subscribe_view_stream denied "
			   "handle=%u peer_label=\"%s\" (getrandom failed)\n",
			   handle, peer_label ? peer_label : "");
		wl_resource_destroy(stream_resource);
		return;
	}

	/* Bring the per-stream virtual input device up before we advertise
	 * the stream to the subscriber — once the wl_client sees the
	 * approved event it may connect and start pushing events
	 * immediately (via claim on qdwin_stream_input_v1). */
	qdwin_stream_seat_init(s);
	int spawn_w = (pw->current_mode && pw->current_mode->width  > 0)
		? pw->current_mode->width  : 640;
	int spawn_h = (pw->current_mode && pw->current_mode->height > 0)
		? pw->current_mode->height : 480;
	if (qdwin_view_stream_spawn_forward(s, node_name, spawn_w, spawn_h) < 0) {
		qdwin_view_stream_v1_send_denied(
			stream_resource,
			"failed to start qdistro-forward");
		wl_resource_destroy(stream_resource);
		return;
	}

	/* Cert path: reuse qdwin's main RDP cert for now; per-stream certs
	 * are a hardening item. */
	const char *cert_path = getenv("QDWIN_RDP_CERT")
		? getenv("QDWIN_RDP_CERT")
		: "";
	qdwin_view_stream_v1_send_approved(stream_resource,
					   node_name,
					   s->rdp_port,
					   cert_path,
					   s->rdp_password);
	weston_log("qdwin: view_stream approved handle=%u peer_label=\"%s\" "
		   "pw=%s output_pos=(%.0f,%.0f) rdp_port=%u "
		   "forward_pid=%d allow_input=%u\n",
		   handle, peer_label ? peer_label : "",
		   pw->name, pw->pos.c.x, pw->pos.c.y,
		   s->rdp_port, (int)s->forward_pid,
		   (unsigned)allow_input);
}

/* ------------------------------------------------------------------
 * §6.5 S5: qdwin_stream_input_v1 — a wl_client (qdistro-forward today,
 * potentially an AI agent or test harness tomorrow) injects pointer/
 * keyboard events into one specific view through this private channel.
 * claim() validates the access_token (one-shot) issued by the spawn
 * argv; inject_* are dispatched through a per-stream weston_seat whose
 * pointer/keyboard focus is pinned to tl->view. The per-stream seat is
 * the "virtual input device" abstraction — the compositor doesn't care
 * whether the events originated from RDP, an AI policy, or a fuzzer.
 * ------------------------------------------------------------------ */

static void
qdwin_stream_seat_init(struct qdwin_view_stream *s)
{
	if (s->seat_inited)
		return;

	char seat_name[64];
	snprintf(seat_name, sizeof seat_name,
		 "qdwin-stream-%u", s->rdp_port);
	weston_seat_init(&s->stream_seat, s->qdwin->compositor, seat_name);

	if (weston_seat_init_pointer(&s->stream_seat) < 0) {
		weston_log("qdwin: stream seat init_pointer failed (port=%u)\n",
			   s->rdp_port);
	}

	/* Inherit the compositor-wide xkb keymap — per-stream layouts are
	 * out of scope; if the remote user wants a different layout they
	 * can set it on their RDP client (which sends scan codes we
	 * translate identically regardless of layout). */
	struct xkb_keymap *keymap = NULL;
	if (s->qdwin->compositor->xkb_info)
		keymap = s->qdwin->compositor->xkb_info->keymap;
	if (weston_seat_init_keyboard(&s->stream_seat, keymap) < 0) {
		weston_log("qdwin: stream seat init_keyboard failed (port=%u)\n",
			   s->rdp_port);
	}

	/* Advertise touch capability even though qdistro-forward doesn't
	 * translate RDP touch today. A client sees caps=pointer|keyboard|
	 * touch and can react to the presence/absence of touch support
	 * without needing a protocol bump when we later add
	 * inject_touch_* requests. Cost of no-op touch state is negligible. */
	if (weston_seat_init_touch(&s->stream_seat) < 0) {
		weston_log("qdwin: stream seat init_touch failed (port=%u)\n",
			   s->rdp_port);
	}

	s->seat_inited = 1;
	weston_log("qdwin: stream seat '%s' created\n", seat_name);
}

static void
qdwin_stream_seat_release(struct qdwin_view_stream *s)
{
	if (!s->seat_inited)
		return;
	weston_seat_release_touch(&s->stream_seat);
	weston_seat_release_keyboard(&s->stream_seat);
	weston_seat_release_pointer(&s->stream_seat);
	weston_seat_release(&s->stream_seat);
	s->seat_inited = 0;
}

/* Pin (or re-pin) pointer/keyboard focus to the source view. Called on
 * the first inject and on every pointer_motion, so transient focus
 * changes triggered elsewhere in libweston can't let events leak to
 * other surfaces. Cheap when focus is already correct. */
static void
qdwin_stream_seat_assert_focus(struct qdwin_view_stream *s)
{
	if (!s->seat_inited || !s->tl || !s->tl->view || !s->tl->view->surface)
		return;

	struct weston_pointer *ptr = weston_seat_get_pointer(&s->stream_seat);
	if (ptr && ptr->focus != s->tl->view)
		weston_pointer_set_focus(ptr, s->tl->view);

	struct weston_keyboard *kbd = weston_seat_get_keyboard(&s->stream_seat);
	if (kbd && kbd->focus != s->tl->view->surface)
		weston_keyboard_set_focus(kbd, s->tl->view->surface);
}

static inline struct timespec
qdwin_ts_from_msec(uint32_t time_msec)
{
	struct timespec ts;
	ts.tv_sec  = time_msec / 1000u;
	ts.tv_nsec = (long)(time_msec % 1000u) * 1000000L;
	return ts;
}

static struct qdwin_view_stream *
qdwin_view_stream_by_token(struct qdwin *qdwin, const char *token)
{
	if (!token)
		return NULL;
	struct qdwin_view_stream *s;
	wl_list_for_each(s, &qdwin->view_streams, link) {
		if (strncmp(s->access_token, token,
			    sizeof s->access_token) == 0)
			return s;
	}
	return NULL;
}

/* qdwin_stream_input_handle_v1 destructor + inject_* */

static void
qdwin_stream_input_handle_resource_destroyed(struct wl_resource *resource)
{
	struct qdwin_view_stream *s = wl_resource_get_user_data(resource);
	if (!s)
		return;
	if (s->input_handle == resource) {
		s->input_handle = NULL;
		s->input_claimed = 0;
		weston_log("qdwin: stream_input handle released "
			   "(rdp_port=%u)\n", s->rdp_port);
	}
}

static void
qdwin_stream_input_handle_destroy(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void
qdwin_stream_input_inject_pointer_motion(
	struct wl_client *c, struct wl_resource *r,
	uint32_t time_msec, wl_fixed_t x, wl_fixed_t y)
{
	(void)c;
	struct qdwin_view_stream *s = wl_resource_get_user_data(r);
	if (!s || !s->seat_inited ||
	    !s->tl || !s->tl->view || !s->tl->view->surface)
		return;

	qdwin_stream_seat_assert_focus(s);

	/* x,y arrive in surface-local logical pixels (wl_fixed). Translate
	 * to compositor-global space via the view's transform. The view's
	 * cached matrix may be dirty between paint cycles if the view
	 * moved, so force an update before the transform call —
	 * weston_coord_surface_to_global asserts !transform.dirty. */
	weston_view_update_transform(s->tl->view);
	struct weston_coord_surface cs = {
		.c = { .x = wl_fixed_to_double(x),
		       .y = wl_fixed_to_double(y) },
		.coordinate_space_id = s->tl->view->surface,
	};
	struct weston_coord_global gpos =
		weston_coord_surface_to_global(s->tl->view, cs);

	struct timespec ts = qdwin_ts_from_msec(time_msec);
	notify_motion_absolute(&s->stream_seat, &ts, gpos);

	if (getenv("QDWIN_STREAM_INPUT_DEBUG"))
		weston_log("qdwin: notify_motion_absolute stream=%u "
			   "surf=(%.1f,%.1f) global=(%.1f,%.1f)\n",
			   s->rdp_port,
			   wl_fixed_to_double(x), wl_fixed_to_double(y),
			   gpos.c.x, gpos.c.y);
}

static void
qdwin_stream_input_inject_pointer_button(
	struct wl_client *c, struct wl_resource *r,
	uint32_t time_msec, uint32_t button, uint32_t state)
{
	(void)c;
	struct qdwin_view_stream *s = wl_resource_get_user_data(r);
	if (!s || !s->seat_inited)
		return;
	struct timespec ts = qdwin_ts_from_msec(time_msec);
	notify_button(&s->stream_seat, &ts, (int32_t)button,
		      state ? WL_POINTER_BUTTON_STATE_PRESSED
			    : WL_POINTER_BUTTON_STATE_RELEASED);
	if (getenv("QDWIN_STREAM_INPUT_DEBUG"))
		weston_log("qdwin: notify_button stream=%u btn=0x%x state=%u\n",
			   s->rdp_port, button, state);
}

static void
qdwin_stream_input_inject_pointer_axis(
	struct wl_client *c, struct wl_resource *r,
	uint32_t time_msec, uint32_t axis, wl_fixed_t value)
{
	(void)c;
	struct qdwin_view_stream *s = wl_resource_get_user_data(r);
	if (!s || !s->seat_inited)
		return;
	struct weston_pointer_axis_event ev = {
		.axis = axis,  /* 0=vertical, 1=horizontal (wl_pointer.axis) */
		.value = wl_fixed_to_double(value),
		.has_discrete = false,
		.discrete = 0,
	};
	struct timespec ts = qdwin_ts_from_msec(time_msec);
	notify_axis(&s->stream_seat, &ts, &ev);
}

static void
qdwin_stream_input_inject_key(
	struct wl_client *c, struct wl_resource *r,
	uint32_t time_msec, uint32_t key, uint32_t state)
{
	(void)c;
	struct qdwin_view_stream *s = wl_resource_get_user_data(r);
	if (!s || !s->seat_inited)
		return;

	qdwin_stream_seat_assert_focus(s);

	struct timespec ts = qdwin_ts_from_msec(time_msec);
	notify_key(&s->stream_seat, &ts, key,
		   state ? WL_KEYBOARD_KEY_STATE_PRESSED
			 : WL_KEYBOARD_KEY_STATE_RELEASED,
		   STATE_UPDATE_AUTOMATIC);
	if (getenv("QDWIN_STREAM_INPUT_DEBUG"))
		weston_log("qdwin: notify_key stream=%u key=%u state=%u\n",
			   s->rdp_port, key, state);
}

static void
qdwin_stream_input_inject_modifiers(
	struct wl_client *c, struct wl_resource *r,
	uint32_t depressed, uint32_t latched,
	uint32_t locked, uint32_t group)
{
	(void)c;
	/* RDP "synchronize" conveys LED state (NumLock/CapsLock/ScrollLock)
	 * on keyboard-activation events; it is NOT a xkb modifier mask.
	 * Real modifier keys (Shift/Ctrl/Alt/Super) flow through inject_key
	 * as regular press/release events, which update xkb state via
	 * notify_key. So this request is advisory — we log and drop. If a
	 * future claimant needs explicit modifier overrides, the right
	 * route is to add a new request that takes evdev codes, not masks. */
	struct qdwin_view_stream *s = wl_resource_get_user_data(r);
	weston_log("qdwin: inject modifiers (advisory) stream=%u "
		   "dep=0x%x lat=0x%x lock=0x%x grp=%u\n",
		   s ? s->rdp_port : 0, depressed, latched, locked, group);
}

/* §6.5 S3c iter3: tie PipeWire frame production to RDP encoder demand.
 * Weston's backend-pipewire only emits a frame when the compositor
 * finishes a paint on the pipewire output, and paints only on damage.
 * A static source view + an RDP client pulling frames at 30 Hz means
 * the client sees exactly one frame forever. request_frame damages
 * the view's surface + schedules a repaint of the pipewire output; a
 * consumer (qdistro-forward's shadow encoder) calls it on its own
 * cadence to drive continuous frame flow. */
static void
qdwin_stream_input_handle_request_frame(
	struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	struct qdwin_view_stream *s = wl_resource_get_user_data(r);
	if (!s || !s->pinned || !s->pw_output)
		return;
	if (s->tl && s->tl->view && s->tl->view->surface)
		weston_surface_damage(s->tl->view->surface);
	for (int side = 0; side < QDWIN_SIDES; side++) {
		struct weston_view *cv = s->tl ? s->tl->chrome[side].view : NULL;
		if (cv && cv->surface)
			weston_surface_damage(cv->surface);
	}
	weston_output_schedule_repaint(s->pw_output);
}

static const struct qdwin_stream_input_handle_v1_interface
qdwin_stream_input_handle_impl = {
	.destroy = qdwin_stream_input_handle_destroy,
	.inject_pointer_motion = qdwin_stream_input_inject_pointer_motion,
	.inject_pointer_button = qdwin_stream_input_inject_pointer_button,
	.inject_pointer_axis   = qdwin_stream_input_inject_pointer_axis,
	.inject_key            = qdwin_stream_input_inject_key,
	.inject_modifiers      = qdwin_stream_input_inject_modifiers,
	.request_frame         = qdwin_stream_input_handle_request_frame,
};

/* qdwin_stream_input_v1.claim — token check + handle resource bind. */

static void
qdwin_stream_input_handle_claim(
	struct wl_client *client, struct wl_resource *input_resource,
	uint32_t handle_id, const char *access_token)
{
	struct qdwin *qdwin = wl_resource_get_user_data(input_resource);

	/* Inherit the bound interface version so request_frame (v2) is
	 * callable iff the client asked for v2. */
	uint32_t version = wl_resource_get_version(input_resource);
	struct wl_resource *handle_res = wl_resource_create(
		client, &qdwin_stream_input_handle_v1_interface,
		(int)version, handle_id);
	if (!handle_res) {
		wl_client_post_no_memory(client);
		return;
	}

	struct qdwin_view_stream *s =
		qdwin_view_stream_by_token(qdwin, access_token);
	if (!s) {
		/* Bind handle so destroy works, then post error. */
		wl_resource_set_implementation(
			handle_res, &qdwin_stream_input_handle_impl,
			NULL, NULL);
		wl_resource_post_error(
			input_resource,
			QDWIN_STREAM_INPUT_V1_ERROR_INVALID_TOKEN,
			"claim: token did not match any live stream");
		weston_log("qdwin: stream_input claim INVALID_TOKEN\n");
		return;
	}
	if (s->input_claimed) {
		wl_resource_set_implementation(
			handle_res, &qdwin_stream_input_handle_impl,
			NULL, NULL);
		wl_resource_post_error(
			input_resource,
			QDWIN_STREAM_INPUT_V1_ERROR_ALREADY_CLAIMED,
			"claim: token already consumed (rdp_port=%u)",
			s->rdp_port);
		weston_log("qdwin: stream_input claim ALREADY_CLAIMED "
			   "rdp_port=%u\n", s->rdp_port);
		return;
	}

	pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
	wl_client_get_credentials(client, &pid, &uid, &gid);
	if (s->forward_pid <= 0 || pid != s->forward_pid) {
		wl_resource_set_implementation(
			handle_res, &qdwin_stream_input_handle_impl,
			NULL, NULL);
		wl_resource_post_error(
			input_resource,
			QDWIN_STREAM_INPUT_V1_ERROR_INVALID_TOKEN,
			"claim: token is reserved for qdistro-forward pid=%d",
			(int)s->forward_pid);
		weston_log("qdwin: stream_input claim PID_MISMATCH "
			   "rdp_port=%u expected=%d peer pid=%d uid=%u\n",
			   s->rdp_port, (int)s->forward_pid, (int)pid,
			   (unsigned)uid);
		return;
	}

	wl_resource_set_implementation(
		handle_res, &qdwin_stream_input_handle_impl, s,
		qdwin_stream_input_handle_resource_destroyed);
	s->input_claimed = 1;
	s->input_handle = handle_res;

	weston_log("qdwin: stream_input claim OK rdp_port=%u peer pid=%d "
		   "uid=%u\n", s->rdp_port, (int)pid, (unsigned)uid);
}

static const struct qdwin_stream_input_v1_interface qdwin_stream_input_impl = {
	.claim = qdwin_stream_input_handle_claim,
};

static void
bind_qdwin_stream_input(struct wl_client *client, void *data,
			uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *r = wl_resource_create(
		client, &qdwin_stream_input_v1_interface,
		(int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &qdwin_stream_input_impl,
				       qdwin, NULL);
}

/* §6.6 S1 — panel role (layer, placement, work-area exclusive zone).
 *
 * A panel is a shell-owned wl_surface pinned to one output edge,
 * composited above normal toplevels but below popups, reserving an
 * exclusive zone on its edge so xdg-toplevel maximise does not cover
 * it. One qdwin_panel per live `qdwin_panel_v1` resource; panels live
 * on `qdwin::panels`. Panel placement is recomputed on each surface
 * commit (so buffer-size changes reflow the view) and on output
 * add/resize (see qdwin_panels_reposition_all).
 *
 * The exclusive rectangle contributed to work-area accounting:
 *   edge 0 (top)    : reserves a band at output_y .. output_y+thickness
 *   edge 1 (bottom) : reserves a band at output_y+output_h-thickness ..
 *                     output_y+output_h
 *   edge 2 (left)   : reserves a band at output_x .. output_x+thickness
 *   edge 3 (right)  : reserves a band at output_x+output_w-thickness ..
 *                     output_x+output_w
 *
 * Work-area helper qdwin_output_work_area() is used by request_maximize
 * so the client's maximised size excludes all panels on the relevant
 * output.
 */

struct qdwin_panel {
	struct qdwin *qdwin;
	struct wl_resource *resource;   /* qdwin_panel_v1 */
	struct weston_surface *surface;
	struct weston_view *view;
	struct weston_output *output;
	uint32_t edge;
	uint32_t thickness;
	uint32_t exclusive;
	int last_x, last_y, last_w, last_h;
	int geometry_sent;
	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;
	struct wl_list link;            /* qdwin::panels */
};

static void
qdwin_panel_compute_rect(struct qdwin_panel *p,
			 int *ox, int *oy, int *ow, int *oh)
{
	struct weston_output *out = p->output;
	int x = (int)out->pos.c.x;
	int y = (int)out->pos.c.y;
	int w = out->width;
	int h = out->height;
	int t = (int)p->thickness;
	if (t < 0) t = 0;
	switch (p->edge) {
	case 0: /* top */
		*ox = x; *oy = y; *ow = w; *oh = t; break;
	case 1: /* bottom */
		*ox = x; *oy = y + h - t; *ow = w; *oh = t; break;
	case 2: /* left */
		*ox = x; *oy = y; *ow = t; *oh = h; break;
	case 3: /* right */
		*ox = x + w - t; *oy = y; *ow = t; *oh = h; break;
	default:
		*ox = x; *oy = y; *ow = w; *oh = t; break;
	}
}

static void
qdwin_panel_send_geometry(struct qdwin_panel *p, int x, int y, int w, int h)
{
	if (!p->resource)
		return;
	if (p->geometry_sent && p->last_x == x && p->last_y == y &&
	    p->last_w == w && p->last_h == h)
		return;
	p->last_x = x; p->last_y = y; p->last_w = w; p->last_h = h;
	p->geometry_sent = 1;
	qdwin_panel_v1_send_geometry(p->resource, x, y, w, h);
}

static void
qdwin_panel_place(struct qdwin_panel *p)
{
	if (!p->view || !p->output)
		return;
	int x, y, w, h;
	qdwin_panel_compute_rect(p, &x, &y, &w, &h);
	struct weston_coord_global pos = { .c = weston_coord(x, y) };
	weston_view_set_position(p->view, pos);
	/* Map on first commit or after a rebuild. */
	if (p->surface && !weston_surface_is_mapped(p->surface))
		weston_surface_map(p->surface);
	qdwin_panel_send_geometry(p, x, y, w, h);
}

static void
qdwin_panel_surface_commit(struct wl_listener *l, void *data)
{
	struct qdwin_panel *p = wl_container_of(l, p, surface_commit);
	(void)data;
	qdwin_panel_place(p);
	weston_compositor_schedule_repaint(p->qdwin->compositor);
}

static void
qdwin_panel_drop(struct qdwin_panel *p, int send_dismissed)
{
	if (!p) return;
	if (send_dismissed && p->resource)
		qdwin_panel_v1_send_dismissed(p->resource);
	if (p->view) {
		weston_view_destroy(p->view);
		p->view = NULL;
	}
	if (p->surface) {
		wl_list_remove(&p->surface_destroy.link);
		wl_list_remove(&p->surface_commit.link);
		p->surface = NULL;
	}
	wl_list_remove(&p->link);
	wl_list_init(&p->link);
}

static void
qdwin_panel_surface_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_panel *p = wl_container_of(l, p, surface_destroy);
	(void)data;
	/* Surface went away — view is invalid. Drop view + listeners but
	 * keep the qdwin_panel_v1 resource alive until the client destroys
	 * it; fire dismissed so the shell knows. */
	if (p->view) {
		weston_view_destroy(p->view);
		p->view = NULL;
	}
	wl_list_remove(&p->surface_commit.link);
	wl_list_init(&p->surface_commit.link);
	wl_list_remove(&p->surface_destroy.link);
	wl_list_init(&p->surface_destroy.link);
	p->surface = NULL;
	if (p->resource)
		qdwin_panel_v1_send_dismissed(p->resource);
	qdwin_panels_on_output_change(p->qdwin);
}

static void
qdwin_panel_destroy_req(struct wl_client *client,
			struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct qdwin_panel_v1_interface qdwin_panel_impl = {
	.destroy = qdwin_panel_destroy_req,
};

static void
qdwin_panel_resource_destroyed(struct wl_resource *resource)
{
	struct qdwin_panel *p = wl_resource_get_user_data(resource);
	if (!p) return;
	struct qdwin *qdwin = p->qdwin;
	p->resource = NULL;
	qdwin_panel_drop(p, 0);
	free(p);
	/* Maximised toplevels may need to grow into the reclaimed zone. */
	qdwin_panels_on_output_change(qdwin);
}

static void
qdwin_handle_attach_panel(struct wl_client *client,
			  struct wl_resource *resource,
			  uint32_t id,
			  struct wl_resource *surface_resource,
			  uint32_t edge,
			  uint32_t thickness,
			  uint32_t exclusive)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct wl_resource *panel_res;
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	panel_res = wl_resource_create(client, &qdwin_panel_v1_interface,
				       wl_resource_get_version(resource), id);
	if (!panel_res) {
		wl_client_post_no_memory(client);
		return;
	}
	if (edge > 3) {
		weston_log("qdwin: attach_panel invalid edge=%u\n", edge);
		wl_resource_destroy(panel_res);
		return;
	}
	struct weston_surface *surface = surface_resource
		? wl_resource_get_user_data(surface_resource) : NULL;
	if (!surface) {
		weston_log("qdwin: attach_panel missing surface\n");
		wl_resource_destroy(panel_res);
		return;
	}
	struct weston_output *out = qdwin_primary_output(qdwin);
	if (!out) {
		weston_log("qdwin: attach_panel no output yet — deferring\n");
		/* Still create the resource; we just don't have an output to
		 * place on. On first output_created we'll reflow. */
	}
	struct qdwin_panel *p = calloc(1, sizeof *p);
	if (!p) {
		wl_client_post_no_memory(client);
		wl_resource_destroy(panel_res);
		return;
	}
	p->qdwin = qdwin;
	p->resource = panel_res;
	p->surface = surface;
	p->output = out;
	p->edge = edge;
	p->thickness = thickness;
	p->exclusive = exclusive;
	wl_list_init(&p->link);
	wl_list_init(&p->surface_destroy.link);
	wl_list_init(&p->surface_commit.link);
	p->surface_destroy.notify = qdwin_panel_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &p->surface_destroy);
	p->surface_commit.notify = qdwin_panel_surface_commit;
	wl_signal_add(&surface->commit_signal, &p->surface_commit);

	p->view = weston_view_create(surface);
	if (!p->view) {
		weston_log("qdwin: attach_panel weston_view_create failed\n");
		wl_list_remove(&p->surface_destroy.link);
		wl_list_remove(&p->surface_commit.link);
		free(p);
		wl_resource_destroy(panel_res);
		return;
	}
	weston_view_move_to_layer(p->view, &qdwin->panel_layer.view_list);
	wl_list_insert(&qdwin->panels, &p->link);
	wl_resource_set_implementation(panel_res, &qdwin_panel_impl,
				       p, qdwin_panel_resource_destroyed);
	if (out)
		qdwin_panel_place(p);
	weston_log("qdwin: attach_panel edge=%u thickness=%u exclusive=%u "
		   "output=%s\n", edge, thickness, exclusive,
		   out ? out->name : "(none)");
	/* Let existing maximised toplevels re-fit. */
	qdwin_panels_on_output_change(qdwin);
}

/* Compute the work area for `out` = output rect minus all panel
 * exclusive zones on this output (or all panels if their output is
 * not yet assigned, to stay conservative). */
static void
qdwin_output_work_area(struct qdwin *qdwin, struct weston_output *out,
		       int *ox, int *oy, int *ow, int *oh)
{
	if (!out) { *ox = *oy = 0; *ow = *oh = 0; return; }
	int x = (int)out->pos.c.x, y = (int)out->pos.c.y;
	int w = out->width, h = out->height;

	/* Native qdwin panels (qdwin_attach_panel API). */
	struct qdwin_panel *p;
	wl_list_for_each(p, &qdwin->panels, link) {
		if (p->output && p->output != out) continue;
		int t = (int)p->exclusive;
		if (t <= 0) continue;
		switch (p->edge) {
		case 0: y += t; h -= t; break; /* top */
		case 1: h -= t; break;         /* bottom */
		case 2: x += t; w -= t; break; /* left */
		case 3: w -= t; break;         /* right */
		default: break;
		}
	}

	/* zwlr_layer_shell_v1 panels (Phase 1.3). See
	 * qdwin_layer_shell_subtract_zones(), defined later in the file
	 * (after struct qdwin_layer_surface). Stacking-order subtraction
	 * is not modelled — for xdg-toplevel maximize the order is
	 * irrelevant; we just need the union of reserved space removed. */
	qdwin_layer_shell_subtract_zones(qdwin, out, &x, &y, &w, &h);

	if (w < 1) w = 1;
	if (h < 1) h = 1;
	*ox = x; *oy = y; *ow = w; *oh = h;
}

/* Called when outputs change geometry or panels are added/removed.
 * Repositions every panel + nudges maximised toplevels back into the
 * current work area so their visible rect stays correct. */
static void
qdwin_panels_on_output_change(struct qdwin *qdwin)
{
	struct qdwin_panel *p;
	wl_list_for_each(p, &qdwin->panels, link) {
		if (!p->output)
			p->output = qdwin_primary_output(qdwin);
		if (p->output)
			qdwin_panel_place(p);
	}
	/* Re-apply max/restore geometry for any currently-maximised
	 * toplevel. */
	struct qdwin_toplevel *tl;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		if (!(tl->state & QDWIN_TS_MAXIMIZED))
			continue;
		struct weston_output *out = qdwin_primary_output(qdwin);
		if (!out) continue;
		int wx, wy, ww, wh;
		qdwin_output_work_area(qdwin, out, &wx, &wy, &ww, &wh);
		tl->outer_width = ww;
		tl->outer_height = wh;
		struct weston_coord_global pos = {
			.c = weston_coord(wx, wy),
		};
		weston_view_set_position(tl->view, pos);
		weston_desktop_surface_set_size(tl->desktop_surface,
						ww - tl->inset_w - tl->inset_e,
						wh - tl->inset_n - tl->inset_s);
		qdwin_toplevel_position_chrome(tl);
	}
	weston_compositor_schedule_repaint(qdwin->compositor);
}

/* §6.6 S2 — notification bubble role.
 *
 * A notification is a shell-owned wl_surface placed on
 * notification_layer (above panels, below popups). Corner-anchored
 * to the primary output with a (offset_x, offset_y) offset. No
 * exclusive zone — bubbles overlay content. Lifecycle is shell-
 * driven: compositor fires `dismissed` on surface destroy or output
 * removal, never on a timeout of its own. */

struct qdwin_notification {
	struct qdwin *qdwin;
	struct wl_resource *resource;
	struct weston_surface *surface;
	struct weston_view *view;
	struct weston_output *output;
	uint32_t anchor;        /* 0=TR 1=TL 2=BR 3=BL */
	uint32_t offset_x;
	uint32_t offset_y;
	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;
	struct wl_list link;
};

static void
qdwin_notification_place(struct qdwin_notification *n)
{
	if (!n->view || !n->output || !n->surface)
		return;
	int ow = n->output->width;
	int oh = n->output->height;
	int ox = (int)n->output->pos.c.x;
	int oy = (int)n->output->pos.c.y;
	int bw = n->surface->width;
	int bh = n->surface->height;
	if (bw <= 0) bw = 1;
	if (bh <= 0) bh = 1;
	int x = ox, y = oy;
	switch (n->anchor) {
	case 0: /* top-right */
		x = ox + ow - bw - (int)n->offset_x;
		y = oy + (int)n->offset_y;
		break;
	case 1: /* top-left */
		x = ox + (int)n->offset_x;
		y = oy + (int)n->offset_y;
		break;
	case 2: /* bottom-right */
		x = ox + ow - bw - (int)n->offset_x;
		y = oy + oh - bh - (int)n->offset_y;
		break;
	case 3: /* bottom-left */
		x = ox + (int)n->offset_x;
		y = oy + oh - bh - (int)n->offset_y;
		break;
	}
	struct weston_coord_global pos = { .c = weston_coord(x, y) };
	weston_view_set_position(n->view, pos);
	if (!weston_surface_is_mapped(n->surface))
		weston_surface_map(n->surface);
}

static void
qdwin_notification_commit(struct wl_listener *l, void *data)
{
	struct qdwin_notification *n = wl_container_of(l, n, surface_commit);
	(void)data;
	qdwin_notification_place(n);
	weston_compositor_schedule_repaint(n->qdwin->compositor);
}

static void
qdwin_notification_surface_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_notification *n =
		wl_container_of(l, n, surface_destroy);
	(void)data;
	if (n->view) {
		weston_view_destroy(n->view);
		n->view = NULL;
	}
	wl_list_remove(&n->surface_commit.link);
	wl_list_init(&n->surface_commit.link);
	wl_list_remove(&n->surface_destroy.link);
	wl_list_init(&n->surface_destroy.link);
	n->surface = NULL;
	if (n->resource)
		qdwin_notification_v1_send_dismissed(n->resource);
}

static void
qdwin_notification_destroy_req(struct wl_client *client,
			       struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct qdwin_notification_v1_interface qdwin_notification_impl = {
	.destroy = qdwin_notification_destroy_req,
};

static void
qdwin_notification_resource_destroyed(struct wl_resource *resource)
{
	struct qdwin_notification *n = wl_resource_get_user_data(resource);
	if (!n) return;
	if (n->view) {
		weston_view_destroy(n->view);
		n->view = NULL;
	}
	if (n->surface) {
		wl_list_remove(&n->surface_commit.link);
		wl_list_remove(&n->surface_destroy.link);
		n->surface = NULL;
	}
	wl_list_remove(&n->link);
	free(n);
}

static void
qdwin_handle_attach_notification(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t id,
				 struct wl_resource *surface_resource,
				 uint32_t anchor,
				 uint32_t offset_x,
				 uint32_t offset_y)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	struct wl_resource *note_res = wl_resource_create(
		client, &qdwin_notification_v1_interface,
		wl_resource_get_version(resource), id);
	if (!note_res) {
		wl_client_post_no_memory(client);
		return;
	}
	if (anchor > 3) {
		weston_log("qdwin: attach_notification invalid anchor=%u\n",
			   anchor);
		wl_resource_destroy(note_res);
		return;
	}
	struct weston_surface *surface = surface_resource
		? wl_resource_get_user_data(surface_resource) : NULL;
	if (!surface) {
		weston_log("qdwin: attach_notification missing surface\n");
		wl_resource_destroy(note_res);
		return;
	}
	struct qdwin_notification *n = calloc(1, sizeof *n);
	if (!n) {
		wl_client_post_no_memory(client);
		wl_resource_destroy(note_res);
		return;
	}
	n->qdwin = qdwin;
	n->resource = note_res;
	n->surface = surface;
	n->output = qdwin_primary_output(qdwin);
	n->anchor = anchor;
	n->offset_x = offset_x;
	n->offset_y = offset_y;
	wl_list_init(&n->link);
	wl_list_init(&n->surface_commit.link);
	wl_list_init(&n->surface_destroy.link);
	n->surface_destroy.notify = qdwin_notification_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &n->surface_destroy);
	n->surface_commit.notify = qdwin_notification_commit;
	wl_signal_add(&surface->commit_signal, &n->surface_commit);
	n->view = weston_view_create(surface);
	if (!n->view) {
		wl_list_remove(&n->surface_destroy.link);
		wl_list_remove(&n->surface_commit.link);
		free(n);
		wl_resource_destroy(note_res);
		return;
	}
	weston_view_move_to_layer(n->view, &qdwin->notification_layer.view_list);
	wl_list_insert(&qdwin->notifications, &n->link);
	wl_resource_set_implementation(note_res, &qdwin_notification_impl,
				       n, qdwin_notification_resource_destroyed);
	qdwin_notification_place(n);
	weston_log("qdwin: attach_notification anchor=%u offset=%u,%u\n",
		   anchor, offset_x, offset_y);
}

/* §6.6 S3/S4 — launcher + switcher role. */

struct qdwin_launcher {
	struct qdwin *qdwin;
	struct wl_resource *resource;
	struct weston_surface *surface;
	struct weston_view *view;
	struct weston_output *output;
	uint32_t kind;             /* 0=launcher 1=switcher */
	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;
	struct wl_list link;
};

static void
qdwin_launcher_place(struct qdwin_launcher *ln)
{
	if (!ln->view || !ln->output || !ln->surface)
		return;
	int bw = ln->surface->width;
	int bh = ln->surface->height;
	if (bw <= 0 || bh <= 0)
		return;
	int ox = (int)ln->output->pos.c.x;
	int oy = (int)ln->output->pos.c.y;
	int x = ox + (ln->output->width - bw) / 2;
	int y = oy + (ln->output->height - bh) / 2;
	struct weston_coord_global pos = { .c = weston_coord(x, y) };
	weston_view_set_position(ln->view, pos);
	if (!weston_surface_is_mapped(ln->surface))
		weston_surface_map(ln->surface);
}

static void
qdwin_launcher_commit(struct wl_listener *l, void *data)
{
	struct qdwin_launcher *ln = wl_container_of(l, ln, surface_commit);
	(void)data;
	qdwin_launcher_place(ln);
	weston_compositor_schedule_repaint(ln->qdwin->compositor);
}

static void
qdwin_launcher_surface_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_launcher *ln = wl_container_of(l, ln, surface_destroy);
	(void)data;
	if (ln->view) { weston_view_destroy(ln->view); ln->view = NULL; }
	wl_list_remove(&ln->surface_commit.link);
	wl_list_init(&ln->surface_commit.link);
	wl_list_remove(&ln->surface_destroy.link);
	wl_list_init(&ln->surface_destroy.link);
	ln->surface = NULL;
	if (ln->resource)
		qdwin_launcher_v1_send_dismissed(ln->resource);
}

static void
qdwin_launcher_destroy_req(struct wl_client *client,
			   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct qdwin_launcher_v1_interface qdwin_launcher_impl = {
	.destroy = qdwin_launcher_destroy_req,
};

static void
qdwin_launcher_resource_destroyed(struct wl_resource *resource)
{
	struct qdwin_launcher *ln = wl_resource_get_user_data(resource);
	if (!ln) return;
	struct qdwin *qdwin = ln->qdwin;
	uint32_t kind = ln->kind;
	if (ln->view) { weston_view_destroy(ln->view); ln->view = NULL; }
	if (ln->surface) {
		wl_list_remove(&ln->surface_commit.link);
		wl_list_remove(&ln->surface_destroy.link);
		ln->surface = NULL;
	}
	wl_list_remove(&ln->link);
	free(ln);
	/* B3: drop the keyboard grab when the launcher overlay goes
	 * away. Locker takes a separate grab on its own attach path. */
	if (qdwin && kind == 0 &&
	    qdwin->overlay_grab_active &&
	    qdwin->overlay_grab_role == 0)
		qdwin_overlay_grab_end(qdwin);
}

/* §6.6 S3/S4 keybinding handlers. The compositor owns the key grab
 * and fires shell events; the shell turns events into attach_launcher
 * calls. If no shell is bound, the bindings are a logged no-op. */

static void
qdwin_on_launcher_key(struct weston_keyboard *kb,
		      const struct timespec *t,
		      uint32_t key, void *data)
{
	struct qdwin *qdwin = data;
	(void)kb; (void)t; (void)key;
	if (!qdwin->shell_resource) {
		weston_log("qdwin: launcher key pressed; no shell bound\n");
		return;
	}
	weston_log("qdwin: launcher_requested\n");
	qdwin_shell_v1_send_launcher_requested(qdwin->shell_resource);
}

/* §6.6 S3/S4 switcher keyboard grab.
 *
 * Why a grab and not the original modifier-release binding:
 *   weston_compositor_add_modifier_binding(MODIFIER_ALT, ...) only fires
 *   when the modifier was pressed alone for the entire hold. Pressing
 *   Tab during Alt-hold disqualifies the modifier-release fire — exactly
 *   the case we need for Alt+Tab. Verified 2026-04-30 by capturing
 *   /dev/input/event0 with evtest while user pressed Alt+Tab+release:
 *   all four events arrived in order, but the binding never fired.
 *
 * The grab is installed from qdwin_on_switcher_key the first time the
 * user presses Alt+Tab, and stays active until Alt is released
 * (modifiers callback) or any non-Tab key is pressed (key callback,
 * cancel path). While active:
 *   - Tab presses → switcher_next event (cycle)
 *   - Shift+Tab presses → switcher_next(-1)
 *   - Esc → switcher_dismiss (cancel without commit) — TODO if needed
 *   - Other keys → end grab + commit (mirrors gnome behaviour)
 *   - Modifier change with Alt no longer set → end grab + commit
 *
 * The grab eats Tab events so apps don't see them; other keys are
 * delivered after the grab ends + commit fires.
 */
static void qdwin_switcher_grab_end(struct qdwin *qdwin);

static void
qdwin_switcher_grab_key(struct weston_keyboard_grab *grab,
			const struct timespec *t,
			uint32_t key, uint32_t state_w)
{
	struct qdwin *qdwin = wl_container_of(grab, qdwin, switcher_grab);
	(void)t;
	if (!qdwin->shell_resource)
		return;
	if (state_w != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;
	if (key == KEY_TAB) {
		struct weston_keyboard *kb = grab->keyboard;
		uint32_t shift_mask = (kb->xkb_info && kb->xkb_info->shift_mod != XKB_MOD_INVALID)
			? (1u << kb->xkb_info->shift_mod) : 0;
		int dir = (shift_mask && (kb->modifiers.mods_depressed & shift_mask))
			? -1 : 1;
		weston_log("qdwin: switcher_next dir=%d\n", dir);
		qdwin_shell_v1_send_switcher_next(qdwin->shell_resource, dir);
		return;
	}
	/* Any non-Tab key cancels with commit (matches gnome behaviour). */
	qdwin_switcher_grab_end(qdwin);
	weston_log("qdwin: switcher_commit cause=non-tab-key\n");
	qdwin_shell_v1_send_switcher_commit(qdwin->shell_resource);
}

static void
qdwin_switcher_grab_modifiers(struct weston_keyboard_grab *grab,
			      uint32_t serial,
			      uint32_t mods_depressed,
			      uint32_t mods_latched,
			      uint32_t mods_locked,
			      uint32_t group)
{
	struct qdwin *qdwin = wl_container_of(grab, qdwin, switcher_grab);
	struct weston_keyboard *kb = grab->keyboard;
	(void)serial; (void)mods_latched; (void)mods_locked; (void)group;

	/* Forward modifier changes to clients (default behaviour) so
	 * downstream apps see Shift/Ctrl/etc. transitions. */
	weston_keyboard_send_modifiers(kb, serial, mods_depressed,
				       mods_latched, mods_locked, group);

	if (!kb->xkb_info || kb->xkb_info->alt_mod == XKB_MOD_INVALID)
		return;
	uint32_t alt_mask = 1u << kb->xkb_info->alt_mod;
	if (mods_depressed & alt_mask)
		return;     /* Alt still held */
	/* Alt released → commit + end grab. */
	weston_log("qdwin: switcher_commit cause=alt-released\n");
	if (qdwin->shell_resource)
		qdwin_shell_v1_send_switcher_commit(qdwin->shell_resource);
	qdwin_switcher_grab_end(qdwin);
}

static void
qdwin_switcher_grab_cancel(struct weston_keyboard_grab *grab)
{
	struct qdwin *qdwin = wl_container_of(grab, qdwin, switcher_grab);
	qdwin->switcher_grab_active = 0;
}

static const struct weston_keyboard_grab_interface qdwin_switcher_grab_iface = {
	qdwin_switcher_grab_key,
	qdwin_switcher_grab_modifiers,
	qdwin_switcher_grab_cancel,
};

static void
qdwin_switcher_grab_start(struct qdwin *qdwin, struct weston_keyboard *kb)
{
	if (qdwin->switcher_grab_active || !kb)
		return;
	qdwin->switcher_grab.interface = &qdwin_switcher_grab_iface;
	qdwin->switcher_grab.keyboard = kb;
	weston_keyboard_start_grab(kb, &qdwin->switcher_grab);
	qdwin->switcher_grab_active = 1;
}

static void
qdwin_switcher_grab_end(struct qdwin *qdwin)
{
	if (!qdwin->switcher_grab_active)
		return;
	struct weston_keyboard *kb = qdwin->switcher_grab.keyboard;
	qdwin->switcher_grab_active = 0;
	if (kb)
		weston_keyboard_end_grab(kb);
}

/* B3+B4 overlay grab — keyboard input for visible launcher / locker.
 * The compositor grabs keys while the overlay is alive and forwards
 * each press as qdwin_shell_v1.overlay_key(role, sym, utf8, state).
 * The shell decodes utf8 for printable chars (Compose/dead-key safe)
 * and matches sym for named keys.
 */
static void
qdwin_overlay_grab_key(struct weston_keyboard_grab *grab,
		       const struct timespec *t,
		       uint32_t key, uint32_t state_w)
{
	struct qdwin *qdwin = wl_container_of(grab, qdwin, overlay_grab);
	(void)t;
	if (state_w != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;     /* releases absorbed; v17 forwards press only */
	struct weston_keyboard *kb = grab->keyboard;
	if (!kb || !kb->xkb_state.state)
		return;
	/* keycode in evdev space; xkb adds the 8 X11 offset. */
	xkb_keycode_t kc = key + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(kb->xkb_state.state, kc);
	char utf8[16] = {0};
	xkb_state_key_get_utf8(kb->xkb_state.state, kc, utf8, sizeof utf8);
	weston_log("qdwin: overlay_key role=%u sym=%u utf8=\"%s\" state=PRESSED\n",
		   qdwin->overlay_grab_role, (uint32_t)sym, utf8);
	/* Security boundary: locker keystrokes go to the locker process,
	 * NOT the shell. The shell never observes the password buffer.
	 * See qdlocker/tests/gui/05-keystroke-isolation.md. The fallback
	 * to the shell remains for launcher/switcher overlays (roles 0/1)
	 * which qdshell still owns. */
	if (qdwin->overlay_grab_role == 2 /* locker */ && qdwin->locker_resource) {
		qdwin_locker_v1_send_overlay_key(qdwin->locker_resource,
						 (uint32_t)sym, utf8);
		return;
	}
	if (qdwin->shell_resource)
		qdwin_shell_v1_send_overlay_key(qdwin->shell_resource,
						qdwin->overlay_grab_role,
						(uint32_t)sym, utf8,
						WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void
qdwin_overlay_grab_modifiers(struct weston_keyboard_grab *grab,
			     uint32_t serial,
			     uint32_t mods_depressed,
			     uint32_t mods_latched,
			     uint32_t mods_locked,
			     uint32_t group)
{
	/* Forward modifier state to clients so e.g. Shift on the locker
	 * password field follows the right xkb path. */
	weston_keyboard_send_modifiers(grab->keyboard, serial,
				       mods_depressed, mods_latched,
				       mods_locked, group);
}

static void
qdwin_overlay_grab_cancel(struct weston_keyboard_grab *grab)
{
	struct qdwin *qdwin = wl_container_of(grab, qdwin, overlay_grab);
	qdwin->overlay_grab_active = 0;
}

static const struct weston_keyboard_grab_interface qdwin_overlay_grab_iface = {
	qdwin_overlay_grab_key,
	qdwin_overlay_grab_modifiers,
	qdwin_overlay_grab_cancel,
};

static void
qdwin_overlay_grab_start(struct qdwin *qdwin, uint32_t role)
{
	/* Pick the default seat's keyboard. Multi-seat overlays would
	 * need per-seat grabs; out of scope for v17. */
	struct weston_seat *seat;
	struct weston_keyboard *kb = NULL;
	wl_list_for_each(seat, &qdwin->compositor->seat_list, link) {
		kb = weston_seat_get_keyboard(seat);
		if (kb) break;
	}
	if (!kb)
		return;
	if (qdwin->overlay_grab_active) {
		/* Already grabbed — just update the role (e.g. launcher
		 * → locker takeover). The grab struct itself stays. */
		qdwin->overlay_grab_role = role;
		return;
	}
	qdwin->overlay_grab.interface = &qdwin_overlay_grab_iface;
	qdwin->overlay_grab.keyboard = kb;
	qdwin->overlay_grab_role = role;
	weston_keyboard_start_grab(kb, &qdwin->overlay_grab);
	qdwin->overlay_grab_active = 1;
}

static void
qdwin_overlay_grab_end(struct qdwin *qdwin)
{
	if (!qdwin->overlay_grab_active)
		return;
	struct weston_keyboard *kb = qdwin->overlay_grab.keyboard;
	qdwin->overlay_grab_active = 0;
	if (kb)
		weston_keyboard_end_grab(kb);
}

static void
qdwin_on_switcher_key(struct weston_keyboard *kb,
		      const struct timespec *t,
		      uint32_t key, void *data)
{
	struct qdwin *qdwin = data;
	(void)t; (void)key;
	if (!qdwin->shell_resource) {
		weston_log("qdwin: switcher key pressed; no shell bound\n");
		return;
	}
	/* Just install the grab. The grab's key callback fires
	 * switcher_next for this Tab press AND subsequent ones — sending
	 * switcher_next from here too would double-fire (one from
	 * binding, one from grab) and net-cancel on a 2-window setup. */
	qdwin_switcher_grab_start(qdwin, kb);
}

static void
qdwin_on_switcher_back_key(struct weston_keyboard *kb,
			   const struct timespec *t,
			   uint32_t key, void *data)
{
	struct qdwin *qdwin = data;
	(void)t; (void)key;
	if (!qdwin->shell_resource) {
		weston_log("qdwin: switcher back-key pressed; no shell bound\n");
		return;
	}
	/* See qdwin_on_switcher_key — grab's key callback also handles
	 * Shift+Tab via the modifier check, so don't send here. */
	qdwin_switcher_grab_start(qdwin, kb);
}

static void
qdwin_on_alt_released(struct weston_keyboard *kb,
		      enum weston_keyboard_modifier mod, void *data)
{
	/* Kept as a fallback for the alone-Alt press-release path
	 * (user dismisses an idle switcher with a tap). With the
	 * keyboard grab installed on first Alt+Tab, the typical chord
	 * path is handled by qdwin_switcher_grab_modifiers. */
	struct qdwin *qdwin = data;
	(void)kb;
	if (mod != MODIFIER_ALT || !qdwin->shell_resource)
		return;
	weston_log("qdwin: switcher_commit cause=mod-fallback\n");
	qdwin_shell_v1_send_switcher_commit(qdwin->shell_resource);
}

/* §6.6 S5 full — Ctrl+Alt+L fires the manual-lock event. The shell
 * owns the lock decision (spec/04 §PyQt-locker: "locker is a subsystem
 * of the admin compositor"); the compositor only relays the keystroke
 * so the shell can show an auth UI and call set_locked(1) itself. */
static void
qdwin_on_lock_key(struct weston_keyboard *kb,
		  const struct timespec *t,
		  uint32_t key, void *data)
{
	struct qdwin *qdwin = data;
	(void)kb; (void)t; (void)key;
	/* `reason=3` is the manual hotkey (XML lock_requested enum).
	 * Include in the log so tests that grep for the reason can
	 * distinguish this from the idle / lid / suspend paths once
	 * those are wired. */
	weston_log("qdwin: lock_requested reason=3=manual\n");
	/* Fan out to whichever lock client is bound. Locker is the new
	 * path (qdlocker); shell is the deprecated path. When a locker
	 * is bound it takes the event exclusively — sending to both
	 * causes both to call set_locked(1) and double-fires the state
	 * machine. Until qdshell's Modules/LockScreen is deleted, the
	 * shell-only branch remains as a fallback for sessions without
	 * a peer locker. */
	if (qdwin->locker_resource) {
		qdwin_locker_v1_send_lock_requested(qdwin->locker_resource,
			3 /* reason=manual, per XML */);
	} else if (qdwin->shell_resource &&
		   wl_resource_get_version(qdwin->shell_resource) >= 7) {
		qdwin_shell_v1_send_lock_requested(qdwin->shell_resource);
	} else {
		weston_log("qdwin: lock key pressed; no locker or shell bound\n");
	}
}

static void
qdwin_handle_attach_launcher(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t id,
			     struct wl_resource *surface_resource,
			     uint32_t kind)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	struct wl_resource *ln_res = wl_resource_create(
		client, &qdwin_launcher_v1_interface,
		wl_resource_get_version(resource), id);
	if (!ln_res) { wl_client_post_no_memory(client); return; }
	struct weston_surface *surface = surface_resource
		? wl_resource_get_user_data(surface_resource) : NULL;
	if (!surface) {
		wl_resource_destroy(ln_res);
		return;
	}
	struct qdwin_launcher *ln = calloc(1, sizeof *ln);
	if (!ln) {
		wl_client_post_no_memory(client);
		wl_resource_destroy(ln_res);
		return;
	}
	ln->qdwin = qdwin;
	ln->resource = ln_res;
	ln->surface = surface;
	ln->output = qdwin_primary_output(qdwin);
	ln->kind = kind;
	wl_list_init(&ln->link);
	wl_list_init(&ln->surface_commit.link);
	wl_list_init(&ln->surface_destroy.link);
	ln->surface_destroy.notify = qdwin_launcher_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &ln->surface_destroy);
	ln->surface_commit.notify = qdwin_launcher_commit;
	wl_signal_add(&surface->commit_signal, &ln->surface_commit);
	ln->view = weston_view_create(surface);
	if (!ln->view) {
		wl_list_remove(&ln->surface_destroy.link);
		wl_list_remove(&ln->surface_commit.link);
		free(ln);
		wl_resource_destroy(ln_res);
		return;
	}
	weston_view_move_to_layer(ln->view, &qdwin->launcher_layer.view_list);
	wl_list_insert(&qdwin->launchers, &ln->link);
	wl_resource_set_implementation(ln_res, &qdwin_launcher_impl,
				       ln, qdwin_launcher_resource_destroyed);
	qdwin_launcher_place(ln);
	weston_log("qdwin: attach_launcher kind=%u\n", kind);
	/* B3: install overlay keyboard grab for kind=0 (launcher). The
	 * switcher (kind=1) already has its own switcher_grab driven by
	 * Alt+Tab; double-grabbing would conflict. v17+ clients handle
	 * the overlay_key event we'll start emitting; older shells just
	 * see the grab eat their input — same effective behaviour as
	 * before this change (overlay was never typeable).
	 */
	if (kind == 0 &&
	    wl_resource_get_version(resource) >= 17) {
		qdwin_overlay_grab_start(qdwin, /* role=launcher */ 0);
	}
}

/* §6.6 S5 — lock surface + set_locked. */

static void
qdwin_lock_surface_place(struct qdwin *qdwin)
{
	if (!qdwin->lock_view || !qdwin->lock_surface)
		return;
	struct weston_output *out = qdwin_primary_output(qdwin);
	if (!out) return;
	struct weston_coord_global pos = { .c = weston_coord(out->pos.c.x,
							     out->pos.c.y) };
	weston_view_set_position(qdwin->lock_view, pos);
	if (!weston_surface_is_mapped(qdwin->lock_surface))
		weston_surface_map(qdwin->lock_surface);
}

static void
qdwin_lock_surface_commit_cb(struct wl_listener *l, void *data)
{
	struct qdwin *qdwin = wl_container_of(l, qdwin, lock_surface_commit);
	(void)data;
	qdwin_lock_surface_place(qdwin);
	weston_compositor_schedule_repaint(qdwin->compositor);
}

static void
qdwin_lock_surface_destroyed_cb(struct wl_listener *l, void *data)
{
	struct qdwin *qdwin = wl_container_of(l, qdwin, lock_surface_destroy);
	(void)data;
	if (qdwin->lock_view) {
		weston_view_destroy(qdwin->lock_view);
		qdwin->lock_view = NULL;
	}
	wl_list_remove(&qdwin->lock_surface_commit.link);
	wl_list_init(&qdwin->lock_surface_commit.link);
	wl_list_remove(&qdwin->lock_surface_destroy.link);
	wl_list_init(&qdwin->lock_surface_destroy.link);
	qdwin->lock_surface = NULL;
	/* Send the dismiss opcode on whichever interface owns this
	 * lock_resource. Sending the shell's dismissed event on a
	 * qdwin_locker_surface_v1 resource posts a protocol error and
	 * tears down the locker connection. */
	if (qdwin->lock_resource) {
		if (qdwin->lock_resource_is_locker) {
			/* qdwin_locker_surface_v1 has no dismissed event;
			 * the client observes the destroy via wl_resource
			 * tracking + the next locked_changed=0. */
		} else {
			qdwin_lock_surface_v1_send_dismissed(qdwin->lock_resource);
		}
	}
	if (qdwin->locked && !qdwin->lock_resource_reattach_in_progress) {
		qdwin->locked = 0;
		weston_log("qdwin: locked_changed=0 cause=lock-surface-destroy\n");
		if (qdwin->shell_resource)
			qdwin_shell_v1_send_locked_changed(
				qdwin->shell_resource, 0);
		if (qdwin->locker_resource)
			qdwin_locker_v1_send_locked_changed(
				qdwin->locker_resource, 0);
	}
	/* B4: drop overlay keyboard grab when locker goes away. */
	if (qdwin->overlay_grab_active && qdwin->overlay_grab_role == 2)
		qdwin_overlay_grab_end(qdwin);
}

static void
qdwin_lock_surface_destroy_req(struct wl_client *client,
			       struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct qdwin_lock_surface_v1_interface qdwin_lock_surface_impl = {
	.destroy = qdwin_lock_surface_destroy_req,
};

/* Locker-side surface vtable. The locker XML declares a distinct
 * qdwin_locker_surface_v1 type (separate from the shell's
 * qdwin_lock_surface_v1) so wayland-scanner doesn't emit duplicate
 * struct definitions when both protocols are compiled in. The
 * underlying state is the same — both vtables share the
 * qdwin->lock_* fields and the qdwin_lock_surface_resource_destroyed
 * teardown. */
static void
qdwin_locker_surface_ack_configure(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t serial)
{
	(void)client; (void)resource; (void)serial;
}

static const struct qdwin_locker_surface_v1_interface qdwin_locker_surface_impl = {
	.destroy = qdwin_lock_surface_destroy_req,
	.ack_configure = qdwin_locker_surface_ack_configure,
};

static void
qdwin_lock_surface_resource_destroyed(struct wl_resource *resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (!qdwin) return;
	qdwin->lock_resource = NULL;
	if (qdwin->lock_view) {
		weston_view_destroy(qdwin->lock_view);
		qdwin->lock_view = NULL;
	}
	if (qdwin->lock_surface) {
		wl_list_remove(&qdwin->lock_surface_commit.link);
		wl_list_remove(&qdwin->lock_surface_destroy.link);
		qdwin->lock_surface = NULL;
	}
	if (qdwin->locked && !qdwin->lock_resource_reattach_in_progress) {
		qdwin->locked = 0;
		weston_log("qdwin: locked_changed=0 cause=lock-resource-destroy\n");
		if (qdwin->shell_resource)
			qdwin_shell_v1_send_locked_changed(
				qdwin->shell_resource, 0);
		if (qdwin->locker_resource)
			qdwin_locker_v1_send_locked_changed(
				qdwin->locker_resource, 0);
	}
	/* Suppress the analogous flap on the surface-destroy callback
	 * by leaving `locked` untouched when a reattach is in flight;
	 * see lock_resource_reattach_in_progress on struct qdwin. */
	qdwin->lock_resource_is_locker = 0;
	/* B4 fix: when the shell calls proxy.destroy() on the lock_surface,
	 * the resource-destroyed handler fires (this one). Without ending
	 * the overlay grab here, the keyboard stays grabbed by an
	 * already-dead role=2 grab; subsequent keys generate overlay_key
	 * events that qdshell drops (lk.locked is False), so input goes
	 * nowhere visible until another grab cycle. */
	if (qdwin->overlay_grab_active && qdwin->overlay_grab_role == 2)
		qdwin_overlay_grab_end(qdwin);
}

/* v18 attach_background — shell-owned fullscreen surface that
 * replaces the compositor curtain on the background_layer. The shell
 * sets a default cursor on pointer.enter so the SPICE viewer always
 * has cursor data to render. */
struct qdwin_background {
	struct qdwin *qdwin;
	struct wl_resource *resource;
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_listener surface_destroy;
};

static void
qdwin_background_send_geometry(struct qdwin_background *bg)
{
	struct weston_output *out = qdwin_primary_output(bg->qdwin);
	if (!out || !bg->resource)
		return;
	qdwin_background_v1_send_geometry(bg->resource,
					  (int)out->pos.c.x,
					  (int)out->pos.c.y,
					  out->width, out->height);
}

static void
qdwin_background_place(struct qdwin_background *bg)
{
	struct weston_output *out = qdwin_primary_output(bg->qdwin);
	if (!out || !bg->view)
		return;
	struct weston_coord_global pos = { .c = weston_coord(out->pos.c.x,
							     out->pos.c.y) };
	weston_view_set_position(bg->view, pos);
	if (!weston_surface_is_mapped(bg->surface))
		weston_surface_map(bg->surface);
	weston_view_update_transform(bg->view);
}

static void
qdwin_background_surface_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_background *bg =
		wl_container_of(l, bg, surface_destroy);
	(void)data;
	if (bg->view) {
		weston_view_destroy(bg->view);
		bg->view = NULL;
	}
	wl_list_remove(&bg->surface_destroy.link);
	wl_list_init(&bg->surface_destroy.link);
	bg->surface = NULL;
	if (bg->resource)
		qdwin_background_v1_send_dismissed(bg->resource);
}

static void
qdwin_background_resource_destroyed(struct wl_resource *resource)
{
	struct qdwin_background *bg = wl_resource_get_user_data(resource);
	if (!bg)
		return;
	if (bg->view) {
		weston_view_destroy(bg->view);
		bg->view = NULL;
	}
	if (bg->surface) {
		wl_list_remove(&bg->surface_destroy.link);
		bg->surface = NULL;
	}
	struct qdwin *qdwin = bg->qdwin;
	if (qdwin->shell_background == bg)
		qdwin->shell_background = NULL;
	free(bg);
	/* Bring the compositor curtain back so the desktop isn't
	 * transparent if the shell never reattaches. */
	if (qdwin->background && qdwin->background->view) {
		weston_view_move_to_layer(qdwin->background->view,
					  &qdwin->background_layer.view_list);
		weston_compositor_schedule_repaint(qdwin->compositor);
	}
}

static void
qdwin_background_destroy_req(struct wl_client *client,
			     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct qdwin_background_v1_interface qdwin_background_impl = {
	.destroy = qdwin_background_destroy_req,
};

static void
qdwin_handle_attach_background(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t id,
			       struct wl_resource *surface_resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	struct wl_resource *bg_res = wl_resource_create(
		client, &qdwin_background_v1_interface,
		wl_resource_get_version(resource), id);
	if (!bg_res) { wl_client_post_no_memory(client); return; }
	struct weston_surface *surface = surface_resource
		? wl_resource_get_user_data(surface_resource) : NULL;
	if (!surface) {
		wl_resource_destroy(bg_res);
		return;
	}
	if (qdwin->shell_background) {
		/* Replace any previous background. */
		wl_resource_destroy(qdwin->shell_background->resource);
	}
	struct qdwin_background *bg = calloc(1, sizeof *bg);
	if (!bg) {
		wl_client_post_no_memory(client);
		wl_resource_destroy(bg_res);
		return;
	}
	bg->qdwin = qdwin;
	bg->resource = bg_res;
	bg->surface = surface;
	wl_list_init(&bg->surface_destroy.link);
	bg->surface_destroy.notify = qdwin_background_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &bg->surface_destroy);
	bg->view = weston_view_create(surface);
	if (!bg->view) {
		wl_list_remove(&bg->surface_destroy.link);
		free(bg);
		wl_resource_destroy(bg_res);
		return;
	}
	weston_view_move_to_layer(bg->view,
				  &qdwin->background_layer.view_list);
	/* Hide the compositor curtain — its weston_view is opaque and
	 * compositor-owned, so weston_compositor_pick_view picks it for
	 * pointer focus instead of our shell-owned bg surface. Detach
	 * from any layer so it isn't composited or focusable. The
	 * curtain stays allocated; we re-add it to the background layer
	 * if the shell background goes away. */
	if (qdwin->background && qdwin->background->view) {
		weston_view_move_to_layer(qdwin->background->view, NULL);
	}
	wl_resource_set_implementation(bg_res, &qdwin_background_impl,
				       bg, qdwin_background_resource_destroyed);
	qdwin->shell_background = bg;
	qdwin_background_place(bg);
	qdwin_background_send_geometry(bg);
	weston_log("qdwin: attach_background output=%s\n",
		   qdwin_primary_output(qdwin) ?
		   qdwin_primary_output(qdwin)->name : "(none)");
}

static void
qdwin_handle_attach_lock_surface(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t id,
				 struct wl_resource *surface_resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	struct wl_resource *ls_res = wl_resource_create(
		client, &qdwin_lock_surface_v1_interface,
		wl_resource_get_version(resource), id);
	if (!ls_res) { wl_client_post_no_memory(client); return; }
	struct weston_surface *surface = surface_resource
		? wl_resource_get_user_data(surface_resource) : NULL;
	if (!surface) { wl_resource_destroy(ls_res); return; }
	/* Drop any previous lock surface. */
	if (qdwin->lock_resource)
		wl_resource_destroy(qdwin->lock_resource);

	qdwin->lock_resource = ls_res;
	qdwin->lock_resource_is_locker = 0;
	qdwin->lock_surface = surface;
	qdwin->lock_view = weston_view_create(surface);
	if (!qdwin->lock_view) {
		qdwin->lock_surface = NULL;
		qdwin->lock_resource = NULL;
		wl_resource_destroy(ls_res);
		return;
	}
	weston_view_move_to_layer(qdwin->lock_view,
				  &qdwin->lock_layer.view_list);
	wl_list_init(&qdwin->lock_surface_destroy.link);
	wl_list_init(&qdwin->lock_surface_commit.link);
	qdwin->lock_surface_destroy.notify = qdwin_lock_surface_destroyed_cb;
	wl_signal_add(&surface->destroy_signal,
		      &qdwin->lock_surface_destroy);
	qdwin->lock_surface_commit.notify = qdwin_lock_surface_commit_cb;
	wl_signal_add(&surface->commit_signal,
		      &qdwin->lock_surface_commit);
	wl_resource_set_implementation(ls_res, &qdwin_lock_surface_impl,
				       qdwin,
				       qdwin_lock_surface_resource_destroyed);
	qdwin_lock_surface_place(qdwin);
	weston_log("qdwin: attach_lock_surface\n");
	/* B4: install overlay keyboard grab so the password field can
	 * accept typing. role=2=locker. v17+ shells handle overlay_key. */
	if (wl_resource_get_version(resource) >= 17)
		qdwin_overlay_grab_start(qdwin, /* role=locker */ 2);
}

static void
qdwin_handle_set_locked(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t locked)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	int want = locked ? 1 : 0;
	if (qdwin->locked == want) return;
	if (want && !qdwin->lock_surface) {
		weston_log("qdwin: set_locked(1) without lock surface — "
			   "attach_lock_surface first\n");
		return;
	}
	qdwin->locked = want;
	weston_log("qdwin: set_locked=%d (lock_surface=%p)\n",
		   want, (void*)qdwin->lock_surface);
	weston_log("qdwin: locked_changed=%d cause=set_locked\n", want);
	if (qdwin->shell_resource)
		qdwin_shell_v1_send_locked_changed(qdwin->shell_resource,
						   want);
	weston_compositor_schedule_repaint(qdwin->compositor);
}

/* spec/10: shell rejected the just-fired selection_set. Drop the seat's
 * selection (and the primary selection, when is_primary=1). */
static void
qdwin_handle_clear_selection(struct wl_client *client,
			     struct wl_resource *resource,
			     const char *seat_name,
			     uint32_t is_primary)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct weston_seat *seat;
	(void)client;
	if (!qdwin || !seat_name)
		return;
	seat = NULL;
	struct weston_seat *s;
	wl_list_for_each(s, &qdwin->compositor->seat_list, link) {
		if (s->seat_name && strcmp(s->seat_name, seat_name) == 0) {
			seat = s;
			break;
		}
	}
	if (!seat) {
		weston_log("qdwin: clear_selection unknown seat=%s\n", seat_name);
		return;
	}
	if (is_primary) {
		struct qdwin_primary_seat *pseat =
			qdwin_primary_seat_find(qdwin, seat);
		if (pseat)
			qdwin_primary_seat_clear_selection(pseat, 1);
		weston_log("qdwin: clear_selection seat=%s primary cleared\n",
			   seat_name);
	} else {
		weston_seat_set_selection(seat, NULL,
					  wl_display_next_serial(
						  qdwin->compositor->wl_display));
		weston_log("qdwin: clear_selection seat=%s clipboard cleared\n",
			   seat_name);
	}
}

/* spec/10 v14: admin shell injects keyboard focus on a seat. Used by
 * the qdshell ctrl-socket bats hooks (no real input under sdl-freerdp
 * dummy) and by future click-to-focus on chrome.
 *
 * Two effects:
 *
 *   1. Drops the seat selection (and its primary equivalent) via
 *      weston_seat_set_selection(NULL, fresh_serial) so that a
 *      follow-up set_selection at any serial wins. This dual-
 *      purpose:
 *      - Defense-in-depth on top of qdshell's cross-silo clear.
 *      - Bumps seat->selection_serial. Test/probe sources use
 *        serial=0 in their set_selection calls (no real input
 *        serial available); without the bump, libweston's stale-
 *        serial guard rejects subsequent serial=0 sets after the
 *        first round. The focus_aware_clear from qdshell DOES
 *        clear via shell.clear_selection, but that's a one-shot;
 *        without this bump the seat's serial monotonically grows
 *        each set_selection and quickly outpaces serial=0.
 *
 *   2. Calls weston_keyboard_set_focus(kbd, target_surface). This is
 *      load-bearing: qdwin_emit_selection_set's first source-handle
 *      resolution is `qdwin_toplevel_for_keyboard_focus`, so without
 *      a real focus shift the silo's selection_set lands at admin's
 *      compositor with handle=UINT32_MAX (the wl_client / secctx
 *      fallbacks miss when the source comes via waypipe and the
 *      destination toplevel lives behind a different wl_client).
 *      The previous version of this handler tripped an infinite loop
 *      in libweston — but that was the listener double-add bug fixed
 *      in qdwin_on_seat_updated_caps (one tracker had its
 *      kbd_focus_listener.link relinked into a fresh focus_signal
 *      list while still belonging to the old one). With the listener
 *      tracked exactly once, weston_keyboard_set_focus's
 *      wl_signal_emit walks our listener once and returns.
 *
 * spec/10 v15 limitation, RESOLVED in v16: when a same-silo sink
 * takes focus via this v1 request, the unconditional clear kills the
 * active selection along with bumping the serial. The clean fix lives
 * in `set_keyboard_focus_v2` (since=16): the shell passes the target
 * toplevel's silo identity, qdwin compares with the per-seat last
 * v2 silo, and only clears on cross-silo. See task 050 (a clean v1-
 * level removal attempt that was reverted because it broke phase7-
 * clipboard-gate's serial=0 set chain — the load-bearing role is
 * actually clearing data_source so subsequent serial=0 sets pass
 * weston's stale-serial guard, not the serial bump itself). v1
 * callers keep the unconditional behaviour for backward compat.
 *
 * The focus_signal listener emits seat_focus_changed; if for some
 * reason it didn't fire (no keyboard at install time) we force-emit.
 */
static void
qdwin_handle_set_keyboard_focus(struct wl_client *client,
				struct wl_resource *resource,
				const char *seat_name,
				uint32_t target_handle)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (!qdwin || !seat_name)
		return;
	struct weston_seat *seat = NULL;
	struct weston_seat *s;
	wl_list_for_each(s, &qdwin->compositor->seat_list, link) {
		if (s->seat_name && strcmp(s->seat_name, seat_name) == 0) {
			seat = s; break;
		}
	}
	if (!seat) {
		weston_log("qdwin: set_keyboard_focus unknown seat=%s\n",
			   seat_name);
		return;
	}
	struct weston_keyboard *kbd = weston_seat_get_keyboard(seat);
	struct weston_surface *target = NULL;
	if (target_handle != UINT32_MAX) {
		struct qdwin_toplevel *tl;
		wl_list_for_each(tl, &qdwin->toplevels, link) {
			if (tl->handle == target_handle) {
				if (tl->view && tl->view->surface)
					target = tl->view->surface;
				break;
			}
		}
		if (!target) {
			weston_log("qdwin: set_keyboard_focus handle=%u "
				   "not mapped\n", target_handle);
			return;
		}
	}
	/* Defense-in-depth + serial bump. See the comment block above
	 * for why this is unconditional even when qdshell's cross-silo
	 * focus_aware_clear path also fires. */
	weston_seat_set_selection(seat, NULL,
				  wl_display_next_serial(
					  qdwin->compositor->wl_display));
	struct qdwin_primary_seat *pseat = qdwin_primary_seat_find(qdwin, seat);
	if (pseat)
		qdwin_primary_seat_clear_selection(pseat, 1);
	if (kbd) {
		weston_keyboard_set_focus(kbd, target);
	} else {
		weston_log("qdwin: set_keyboard_focus seat=%s has no keyboard "
			   "— wire-only emit\n", seat_name);
	}
	weston_log("qdwin: set_keyboard_focus seat=%s handle=%u\n",
		   seat_name, target_handle);
	/* Always force-emit seat_focus_changed for the target. The
	 * focus_signal listener may have already fired from
	 * weston_keyboard_set_focus, in which case this is a redundant
	 * emit (qdshell's on_seat_focus_changed is idempotent for the
	 * same focused_silo). When kbd is NULL or the focus_signal
	 * iteration didn't run our listener yet, this guarantees the
	 * shell sees the new state. */
	qdwin_emit_seat_focus_changed(qdwin, seat, target_handle);
}

/* spec/10 v16: silo-aware focus injection. See protocol XML and the
 * docblock above qdwin_handle_set_keyboard_focus for the load-bearing
 * role of the unconditional clear in v1.
 *
 * The v2 handler skips the clear when target_silo matches the seat
 * tracker's last_target_silo (same-silo focus move). Both must be
 * non-empty for a same-silo determination — empty target_silo or
 * empty stored silo counts as cross-silo for safety, which preserves
 * the v14 behaviour for callers who don't yet know the silo.
 *
 * UINT32_MAX target_handle (clear focus) resets last_target_silo to
 * NULL regardless of the passed string, so the next non-clear v2 call
 * always counts as cross-silo and clears.
 *
 * The handler intentionally shares everything below the gate with the
 * v1 path (focus change + force-emit seat_focus_changed). On the v15
 * sink test path, the source's seat selection survives the focus
 * move; weston's existing code in weston_keyboard_set_focus then
 * sends the active wl_data_device.selection event to the new focus
 * client, which is exactly what the sink needs to call
 * wl_data_offer.receive against. */
static void
qdwin_handle_set_keyboard_focus_v2(struct wl_client *client,
				   struct wl_resource *resource,
				   const char *seat_name,
				   uint32_t target_handle,
				   const char *target_silo)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (!qdwin || !seat_name)
		return;
	struct weston_seat *seat = NULL;
	struct weston_seat *s;
	wl_list_for_each(s, &qdwin->compositor->seat_list, link) {
		if (s->seat_name && strcmp(s->seat_name, seat_name) == 0) {
			seat = s; break;
		}
	}
	if (!seat) {
		weston_log("qdwin: set_keyboard_focus_v2 unknown seat=%s\n",
			   seat_name);
		return;
	}
	struct weston_keyboard *kbd = weston_seat_get_keyboard(seat);
	struct weston_surface *target = NULL;
	if (target_handle != UINT32_MAX) {
		struct qdwin_toplevel *tl;
		wl_list_for_each(tl, &qdwin->toplevels, link) {
			if (tl->handle == target_handle) {
				if (tl->view && tl->view->surface)
					target = tl->view->surface;
				break;
			}
		}
		if (!target) {
			weston_log("qdwin: set_keyboard_focus_v2 handle=%u "
				   "not mapped\n", target_handle);
			return;
		}
	}

	struct qdwin_seat_tracker *tr =
		qdwin_seat_tracker_for_seat(qdwin, seat);
	const char *prev_silo = qdwin_seat_tracker_silo(tr);
	const char *new_silo = target_silo ? target_silo : "";
	int cross_silo = 1;
	if (target_handle == UINT32_MAX) {
		/* Clearing focus always counts as cross-silo. */
		cross_silo = 1;
	} else if (*new_silo && *prev_silo &&
		   strcmp(new_silo, prev_silo) == 0) {
		cross_silo = 0;
	}

	if (cross_silo) {
		weston_seat_set_selection(seat, NULL,
					  wl_display_next_serial(
						  qdwin->compositor->wl_display));
		struct qdwin_primary_seat *pseat =
			qdwin_primary_seat_find(qdwin, seat);
		if (pseat)
			qdwin_primary_seat_clear_selection(pseat, 1);
	}

	if (kbd) {
		/* Use weston_seat_set_keyboard_focus (the public helper) so
		 * the new focus client receives the active selection event
		 * via wl_data_device_set_keyboard_focus. v1 used
		 * weston_keyboard_set_focus directly because the
		 * unconditional clear immediately above made a re-send
		 * unnecessary (selection was NULL). On the v2 same-silo
		 * path we DON'T clear — the source's offer must reach the
		 * new focus client so its sink path can call
		 * wl_data_offer.receive. */
		weston_seat_set_keyboard_focus(seat, target);
	} else {
		weston_log("qdwin: set_keyboard_focus_v2 seat=%s has no "
			   "keyboard — wire-only emit\n", seat_name);
	}

	/* Update per-seat last_target_silo. UINT32_MAX clear resets to
	 * NULL so the next non-clear call is cross-silo. */
	qdwin_seat_tracker_set_silo(
		tr, (target_handle == UINT32_MAX || !*new_silo)
		    ? NULL
		    : new_silo);

	weston_log("qdwin: set_keyboard_focus_v2 seat=%s handle=%u "
		   "silo='%s' cross_silo=%d (prev='%s')\n",
		   seat_name, target_handle, new_silo, cross_silo, prev_silo);

	qdwin_emit_seat_focus_changed(qdwin, seat, target_handle);
}

/* ------------------------------------------------------------------
 * v19: shell-driven global hotkeys (register_hotkey/unregister_hotkey/
 * hotkey_pressed). Backed by weston_compositor_add_key_binding which
 * is already excluded by every active grab — overlays, switcher, and
 * lock all naturally suppress hotkey delivery.
 * ------------------------------------------------------------------ */

struct qdwin_hotkey {
	uint32_t id;
	uint32_t modifiers;          /* qdwin_shell_v1.modifier bitmask */
	uint32_t key;                /* linux input keycode */
	struct weston_binding *binding;
	struct qdwin *qdwin;
	struct wl_list link;
};

static struct qdwin_hotkey *
qdwin_hotkey_find(struct qdwin *qdwin, uint32_t id)
{
	struct qdwin_hotkey *h;
	wl_list_for_each(h, &qdwin->hotkeys, link) {
		if (h->id == id) return h;
	}
	return NULL;
}

static void
qdwin_hotkey_destroy(struct qdwin_hotkey *h)
{
	if (h->binding) weston_binding_destroy(h->binding);
	wl_list_remove(&h->link);
	free(h);
}

static void
qdwin_hotkey_handler(struct weston_keyboard *keyboard,
		     const struct timespec *time,
		     uint32_t key, void *data)
{
	struct qdwin_hotkey *h = data;
	(void)keyboard; (void)time; (void)key;
	weston_log("qdwin: hotkey_pressed id=%u\n", h->id);
	if (h->qdwin->shell_resource) {
		qdwin_shell_v1_send_hotkey_pressed(h->qdwin->shell_resource,
						   h->id);
	}
}

/* qdwin_shell_v1.modifier bits → enum weston_keyboard_modifier. */
static enum weston_keyboard_modifier
qdwin_hotkey_mods_to_weston(uint32_t mods)
{
	enum weston_keyboard_modifier wmods = 0;
	if (mods & 1) wmods |= MODIFIER_CTRL;
	if (mods & 2) wmods |= MODIFIER_ALT;
	if (mods & 4) wmods |= MODIFIER_SUPER;
	if (mods & 8) wmods |= MODIFIER_SHIFT;
	return wmods;
}

static void
qdwin_handle_register_hotkey(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t id,
			     uint32_t modifiers,
			     uint32_t key)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (!qdwin || key == 0) return;

	/* Idempotent re-register: drop any existing entry under this id. */
	struct qdwin_hotkey *existing = qdwin_hotkey_find(qdwin, id);
	if (existing) qdwin_hotkey_destroy(existing);

	struct qdwin_hotkey *h = calloc(1, sizeof(*h));
	if (!h) return;
	h->id = id;
	h->modifiers = modifiers;
	h->key = key;
	h->qdwin = qdwin;
	h->binding = weston_compositor_add_key_binding(
		qdwin->compositor, key,
		qdwin_hotkey_mods_to_weston(modifiers),
		qdwin_hotkey_handler, h);
	if (!h->binding) {
		free(h);
		weston_log("qdwin: register_hotkey id=%u failed (binding alloc)\n",
			   id);
		return;
	}
	wl_list_insert(&qdwin->hotkeys, &h->link);
	weston_log("qdwin: register_hotkey id=%u mods=0x%x key=%u\n",
		   id, modifiers, key);
}

static void
qdwin_handle_unregister_hotkey(struct wl_client *client,
			       struct wl_resource *resource, uint32_t id)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (!qdwin) return;
	struct qdwin_hotkey *h = qdwin_hotkey_find(qdwin, id);
	if (h) {
		qdwin_hotkey_destroy(h);
		weston_log("qdwin: unregister_hotkey id=%u\n", id);
	}
}

static void
qdwin_hotkeys_purge(struct qdwin *qdwin)
{
	struct qdwin_hotkey *h, *tmp;
	wl_list_for_each_safe(h, tmp, &qdwin->hotkeys, link) {
		qdwin_hotkey_destroy(h);
	}
}

static const struct qdwin_shell_v1_interface qdwin_shell_impl = {
	.bind_as_shell = qdwin_handle_bind_as_shell,
	.destroy = qdwin_handle_destroy,
	.set_border_color = qdwin_handle_set_border_color,
	.attach_decoration = qdwin_handle_attach_decoration,
	.request_close = qdwin_handle_request_close,
	.request_minimize = qdwin_handle_request_minimize,
	.request_maximize = qdwin_handle_request_maximize,
	.request_raise = qdwin_handle_request_raise,
	.begin_interactive_move = qdwin_handle_begin_interactive_move,
	.begin_interactive_resize = qdwin_handle_begin_interactive_resize,
	.show_popup = qdwin_handle_show_popup,
	.subscribe_view_stream = qdwin_handle_subscribe_view_stream,
	.attach_panel = qdwin_handle_attach_panel,
	.attach_notification = qdwin_handle_attach_notification,
	.attach_launcher = qdwin_handle_attach_launcher,
	.attach_lock_surface = qdwin_handle_attach_lock_surface,
	.set_locked = qdwin_handle_set_locked,
	.nested_proxy_decision = qdwin_handle_nested_proxy_decision,
	.bind_proxy_pixels = qdwin_handle_bind_proxy_pixels,
	.set_cursor_sprite = qdwin_handle_set_cursor_sprite,
	.clear_selection = qdwin_handle_clear_selection,
	.activation_decision = qdwin_handle_activation_decision,
	.set_keyboard_focus = qdwin_handle_set_keyboard_focus,
	.data_offer_receive_decision = qdwin_handle_data_offer_receive_decision,
	.set_keyboard_focus_v2 = qdwin_handle_set_keyboard_focus_v2,
	.attach_background = qdwin_handle_attach_background,
	.register_hotkey = qdwin_handle_register_hotkey,
	.unregister_hotkey = qdwin_handle_unregister_hotkey,
};

static void
qdwin_shell_resource_destroy(struct wl_resource *resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (qdwin->shell_resource == resource) {
		qdwin_hotkeys_purge(qdwin);
		if (qdwin->move_grab_active) {
			qdwin->move_grab_active = 0;
			if (qdwin->move_grab.pointer)
				weston_pointer_end_grab(qdwin->move_grab.pointer);
		}
		qdwin->shell_resource = NULL;
		qdwin->shell_bound = 0;
		weston_log("qdwin: shell unbound\n");
	}
}

static void
bind_qdwin_shell(struct wl_client *client, void *data,
		 uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource;
	pid_t pid; uid_t uid; gid_t gid;

	wl_client_get_credentials(client, &pid, &uid, &gid);

	weston_log("qdwin: bind attempt pid=%d uid=%u (allowed_uid=%u)\n",
		   (int)pid, (unsigned)uid, (unsigned)qdwin->allowed_uid);

	if (uid != qdwin->allowed_uid) {
		wl_client_post_implementation_error(
			client,
			"qdwin_shell_v1: uid %u not permitted (allowed uid=%u)",
			(unsigned)uid, (unsigned)qdwin->allowed_uid);
		return;
	}

	resource = wl_resource_create(client, &qdwin_shell_v1_interface,
				      version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &qdwin_shell_impl,
				       qdwin, qdwin_shell_resource_destroy);

	qdwin_shell_v1_send_hello(resource, (uint32_t)uid);
	weston_log("qdwin: bind accepted for uid=%u, hello sent\n",
		   (unsigned)uid);
}

/* ------------------------------------------------------------------
 * qdwin_locker_v1 — peer locker binding. See doc/locker.md.
 *
 * Mirrors the shell-side attach_lock_surface / set_locked handlers
 * above, sharing the lock_surface / lock_view / lock_resource state.
 * The shell-side variants stay as a deprecation shim until qdshell
 * drops its Modules/LockScreen QML; once that lands they can be
 * removed and the version of qdwin_shell_v1 bumped.
 * ------------------------------------------------------------------ */

static void
qdwin_locker_resource_destroy(struct wl_resource *resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (qdwin->locker_resource == resource) {
		qdwin->locker_resource = NULL;
		weston_log("qdwin: locker unbound\n");
		/* End the role=2 overlay keyboard grab so the next press
		 * isn't forwarded with the locker role still set; without
		 * this, keystrokes briefly fall through into the shell
		 * overlay_key path with role=2 — a small but real leak
		 * window during locker restart. The lock_surface itself
		 * tears down separately via qdwin_lock_surface_resource_destroyed
		 * (when the client connection closes wl_resources). */
		if (qdwin->overlay_grab_active && qdwin->overlay_grab_role == 2)
			qdwin_overlay_grab_end(qdwin);
		/* Fail-safe: if the locker disappears while the compositor
		 * is locked, keep the LOCK layer composited (the surface
		 * is owned by the (now-defunct) client and will tear down
		 * naturally). The screen stays black until a fresh locker
		 * binds and attaches a new surface. We do NOT auto-unlock
		 * on locker death — see doc/locker.md §Lifecycle. */
	}
}

static void
qdwin_handle_bind_as_locker(struct wl_client *client,
			    struct wl_resource *resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (qdwin->locker_resource == resource) {
		/* Same resource binding twice. Use the dedicated
		 * already_bound error code (was using `role` which is for
		 * "surface already has a role" — wrong category). */
		wl_resource_post_error(resource,
			QDWIN_LOCKER_V1_ERROR_ALREADY_BOUND,
			"qdwin_locker_v1: bind_as_locker already called on this resource");
		return;
	}
	if (qdwin->locker_resource) {
		/* A fresh bind from a different client (or a different
		 * resource of the same client) means either the prior
		 * locker is dead or a duplicate is starting. Per the XML
		 * "only one locker may exist at a time" — destroy the old
		 * binding so the new one is authoritative. */
		wl_resource_destroy(qdwin->locker_resource);
	}
	qdwin->locker_resource = resource;
	weston_log("qdwin: locker bound (initially_locked=%d)\n", qdwin->locked);
	qdwin_locker_v1_send_ready(resource, qdwin->locked ? 1u : 0u);
}

static void
qdwin_handle_locker_attach_lock_surface(struct wl_client *client,
					struct wl_resource *resource,
					uint32_t id,
					struct wl_resource *surface_resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (qdwin->locker_resource != resource) {
		wl_resource_post_error(resource,
			QDWIN_LOCKER_V1_ERROR_NOT_BOUND,
			"qdwin_locker_v1: not bound (call bind_as_locker first)");
		return;
	}
	struct wl_resource *ls_res = wl_resource_create(
		client, &qdwin_locker_surface_v1_interface,
		wl_resource_get_version(resource), id);
	if (!ls_res) { wl_client_post_no_memory(client); return; }
	struct weston_surface *surface = surface_resource
		? wl_resource_get_user_data(surface_resource) : NULL;
	if (!surface) { wl_resource_destroy(ls_res); return; }
	/* Drop any previously-attached lock surface (whether from the
	 * shell-deprecated path or a prior locker attach). Set
	 * `reattach_in_progress` around the destroy so the resource-
	 * destroyed callback doesn't fire a spurious locked_changed=0
	 * between the old surface tearing down and the new one being
	 * committed — without this, clients observe a 1→0→1 flap on
	 * every locker reattach mid-lock. */
	if (qdwin->lock_resource) {
		qdwin->lock_resource_reattach_in_progress = 1;
		wl_resource_destroy(qdwin->lock_resource);
		qdwin->lock_resource_reattach_in_progress = 0;
	}

	qdwin->lock_resource = ls_res;
	qdwin->lock_resource_is_locker = 1;
	qdwin->lock_surface = surface;
	qdwin->lock_view = weston_view_create(surface);
	if (!qdwin->lock_view) {
		qdwin->lock_surface = NULL;
		qdwin->lock_resource = NULL;
		qdwin->lock_resource_is_locker = 0;
		wl_resource_destroy(ls_res);
		return;
	}
	weston_view_move_to_layer(qdwin->lock_view,
				  &qdwin->lock_layer.view_list);
	wl_list_init(&qdwin->lock_surface_destroy.link);
	wl_list_init(&qdwin->lock_surface_commit.link);
	qdwin->lock_surface_destroy.notify = qdwin_lock_surface_destroyed_cb;
	wl_signal_add(&surface->destroy_signal, &qdwin->lock_surface_destroy);
	qdwin->lock_surface_commit.notify = qdwin_lock_surface_commit_cb;
	wl_signal_add(&surface->commit_signal, &qdwin->lock_surface_commit);
	wl_resource_set_implementation(ls_res, &qdwin_locker_surface_impl,
				       qdwin,
				       qdwin_lock_surface_resource_destroyed);
	qdwin_lock_surface_place(qdwin);
	weston_log("qdwin: locker attach_lock_surface\n");
	/* Install overlay keyboard grab, locker role=2. Same dispatch
	 * site as the shell path; the overlay_key router now checks for
	 * locker_resource first. */
	qdwin_overlay_grab_start(qdwin, /* role=locker */ 2);
}

static void
qdwin_handle_locker_set_locked(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t locked)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (qdwin->locker_resource != resource) {
		wl_resource_post_error(resource,
			QDWIN_LOCKER_V1_ERROR_NOT_BOUND,
			"qdwin_locker_v1: not bound (call bind_as_locker first)");
		return;
	}
	int want = locked ? 1 : 0;
	if (qdwin->locked == want) return;
	if (want && !qdwin->lock_surface) {
		wl_resource_post_error(resource,
			QDWIN_LOCKER_V1_ERROR_NO_SURFACE,
			"qdwin_locker_v1: set_locked(1) without attach_lock_surface");
		return;
	}
	qdwin->locked = want;
	weston_log("qdwin: locker set_locked=%d (lock_surface=%p)\n",
		   want, (void*)qdwin->lock_surface);
	weston_log("qdwin: locked_changed=%d cause=locker_set_locked\n", want);
	/* Fan out to both protocols so the shell sees the transition
	 * even though the locker drove it. */
	if (qdwin->shell_resource)
		qdwin_shell_v1_send_locked_changed(qdwin->shell_resource, want);
	if (qdwin->locker_resource)
		qdwin_locker_v1_send_locked_changed(qdwin->locker_resource, want);
	weston_compositor_schedule_repaint(qdwin->compositor);
}

static void
qdwin_handle_lock_acknowledged(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t reason)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	if (qdwin->locker_resource != resource) {
		wl_resource_post_error(resource,
			QDWIN_LOCKER_V1_ERROR_NOT_BOUND,
			"qdwin_locker_v1: lock_acknowledged before bind_as_locker");
		return;
	}
	if (reason > 3) {
		wl_resource_post_error(resource,
			QDWIN_LOCKER_V1_ERROR_INVALID_REASON,
			"qdwin_locker_v1: lock_acknowledged reason=%u out of range", reason);
		return;
	}
	weston_log("qdwin: lock_acknowledged reason=%u\n", reason);
}

static const struct qdwin_locker_v1_interface qdwin_locker_impl = {
	.bind_as_locker = qdwin_handle_bind_as_locker,
	.attach_lock_surface = qdwin_handle_locker_attach_lock_surface,
	.set_locked = qdwin_handle_locker_set_locked,
	.lock_acknowledged = qdwin_handle_lock_acknowledged,
};

/* §P10: callback referenced only when role=host; mark unused to avoid
 * -Wunused-function under role=guest builds without rewrapping every
 * forward declaration. */
__attribute__((unused))
static void
bind_qdwin_locker(struct wl_client *client, void *data,
		  uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource;
	pid_t pid; uid_t uid; gid_t gid;

	wl_client_get_credentials(client, &pid, &uid, &gid);
	weston_log("qdwin: locker bind attempt pid=%d uid=%u "
		   "(allowed_locker_uid=%u)\n",
		   (int)pid, (unsigned)uid,
		   (unsigned)qdwin->allowed_locker_uid);

	/* TODO production hardening: see qdlocker/protocol/qdwin-locker-v1.xml — additional exe/SELinux checks intentionally out of scope here. */
	if (uid != qdwin->allowed_locker_uid) {
		wl_client_post_implementation_error(client,
			"qdwin_locker_v1: uid %u not permitted "
			"(allowed locker uid=%u)",
			(unsigned)uid, (unsigned)qdwin->allowed_locker_uid);
		return;
	}

	resource = wl_resource_create(client, &qdwin_locker_v1_interface,
				      version, id);
	if (!resource) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(resource, &qdwin_locker_impl, qdwin,
				       qdwin_locker_resource_destroy);
}

/* ------------------------------------------------------------------
 * Background curtain.
 * ------------------------------------------------------------------ */

/* Install/resize an opaque black curtain covering every enabled output.
 * Without it, the pixman renderer leaves stale pixels wherever nothing
 * redraws — old chrome stays after a shrink, minimised views stay
 * visible, etc. Called on init and whenever outputs change. */
static void
qdwin_refresh_background(struct qdwin *qdwin)
{
	int max_x = 0, max_y = 0;
	struct weston_output *out;

	wl_list_for_each(out, &qdwin->compositor->output_list, link) {
		int right = out->pos.c.x + out->width;
		int bottom = out->pos.c.y + out->height;
		if (right > max_x)
			max_x = right;
		if (bottom > max_y)
			max_y = bottom;
	}
	if (max_x == 0 || max_y == 0)
		return;

	if (qdwin->background) {
		weston_shell_utils_curtain_destroy(qdwin->background);
		qdwin->background = NULL;
	}
	struct weston_curtain_params params = {
		.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f,
		.pos = { .c = weston_coord(0, 0) },
		.width = max_x,
		.height = max_y,
	};
	qdwin->background =
		weston_shell_utils_curtain_create(qdwin->compositor, &params);
	if (!qdwin->background) {
		weston_log("qdwin: curtain_create failed\n");
		return;
	}
	weston_view_move_to_layer(qdwin->background->view,
				  &qdwin->background_layer.view_list);
	weston_log("qdwin: background curtain %dx%d installed\n",
		   max_x, max_y);
}

/* ------------------------------------------------------------------
 * §6.7 seat / output lifecycle forwarders.
 *
 * Forward `weston_compositor` signals onto the bound shell as v2
 * events. Version-gated on the bound resource: a v1 shell silently
 * does not get them. Replay of existing seats/outputs happens in
 * bind_as_shell.
 * ------------------------------------------------------------------ */

struct qdwin_seat_tracker {
	struct qdwin *qdwin;
	struct weston_seat *seat;
	char *seat_name;
	struct wl_listener destroy_listener;
	struct wl_listener updated_caps_listener;  /* §6.8 S3c */
	struct wl_listener selection_listener;     /* spec/10 */
	struct wl_listener kbd_focus_listener;     /* spec/10 v14 focus-aware */
	int kbd_focus_listener_installed;
	struct wl_listener pointer_focus_listener; /* B6 default-cursor restore */
	int pointer_focus_listener_installed;
	uint32_t last_focused_handle;              /* dedup seat_focus_changed */
	/* spec/10 v16: last `target_silo` from set_keyboard_focus_v2.
	 * NULL or empty means "unknown". A v2 call with target_silo equal
	 * to this string skips the unconditional pre-focus selection-
	 * clear so a same-silo sink toplevel can still receive an active
	 * data_offer. UINT32_MAX-handle (clear-focus) calls reset to NULL
	 * so the next non-clear v2 call always counts as cross-silo. */
	char *last_target_silo;
	/* Deferred focus-transfer source: scheduled when focus drops to
	 * UINT32_MAX so the recovery runs on the next event-loop pass,
	 * after any concurrent toplevel destroys have finished tearing
	 * down. Doing the transfer synchronously inside the focus_signal
	 * listener loops infinitely when the focused window is itself
	 * being destroyed (its wl_list_remove hasn't fired yet, so the
	 * recovery picks it back up, only for libweston to clear focus
	 * again as the destroy continues). The idle defers past the
	 * destroy chain. NULL when nothing scheduled. */
	struct wl_event_source *focus_recover_idle;
	struct wl_list link;               /* qdwin::seat_trackers */
};

/* Forward decl — defined below near the can_receive helpers. */
static int qdwin_shell_can_receive_v11(struct qdwin *qdwin);
static struct qdwin_toplevel *
qdwin_toplevel_for_keyboard_focus(struct qdwin *qdwin,
				  struct weston_seat *seat);

static int
qdwin_shell_can_receive_v2(struct qdwin *qdwin)
{
	return qdwin->shell_bound && qdwin->shell_resource &&
	       wl_resource_get_version(qdwin->shell_resource) >= 2;
}

static int
qdwin_shell_can_receive_v11(struct qdwin *qdwin)
{
	return qdwin->shell_bound && qdwin->shell_resource &&
	       wl_resource_get_version(qdwin->shell_resource) >= 11;
}

static int
qdwin_shell_can_receive_v14(struct qdwin *qdwin)
{
	return qdwin->shell_bound && qdwin->shell_resource &&
	       wl_resource_get_version(qdwin->shell_resource) >= 14;
}

static int
qdwin_shell_can_receive_v15(struct qdwin *qdwin)
{
	return qdwin->shell_bound && qdwin->shell_resource &&
	       wl_resource_get_version(qdwin->shell_resource) >= 15;
}

/* Walk qdwin->toplevels for the one whose desktop_surface's main view is
 * the seat's keyboard focus. NULL when the focus surface has no qdwin
 * toplevel (compositor-owned chrome/popup, lock surface, or unmapped).
 * Used by selection-set to identify the source toplevel — Wayland only
 * permits clients with keyboard focus to set selection. */
static struct qdwin_toplevel *
qdwin_toplevel_for_keyboard_focus(struct qdwin *qdwin,
				  struct weston_seat *seat)
{
	struct weston_keyboard *kbd = seat ? seat->keyboard_state : NULL;
	struct weston_surface *focus = kbd ? kbd->focus : NULL;
	struct qdwin_toplevel *tl;
	if (!focus)
		return NULL;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		if (tl->view && tl->view->surface == focus)
			return tl;
		/* §6.8 S2b: a proxy may have its pixel surface as focus. */
		if (tl->proxy_pixel_surface && tl->proxy_pixel_surface == focus)
			return tl;
	}
	return NULL;
}

/* Forward decls — secctx accessors live further down. */
struct qdwin_secctx_client;
static struct qdwin_secctx_client *
qdwin_secctx_client_lookup(struct qdwin *qdwin, struct wl_client *client);
static const char *qdwin_secctx_client_app_id(struct qdwin_secctx_client *sc);

/* Fallback when the selection source's wl_client owns one of qdwin's
 * tracked toplevels but that toplevel isn't currently keyboard-focused
 * (e.g. an RDP backend with no real input delivery, a compositor-grab
 * popup, an off-screen scratch surface). Returns the FIRST matching
 * toplevel — for the broker gate the silo identity is per-wl_client
 * not per-toplevel, so any toplevel from the same wl_client carries
 * the same secctx tag. NULL when no toplevel matches. */
static struct qdwin_toplevel *
qdwin_toplevel_for_client(struct qdwin *qdwin, struct wl_client *client)
{
	if (!client)
		return NULL;
	struct qdwin_toplevel *tl;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		struct weston_surface *ws = NULL;
		if (tl->desktop_surface)
			ws = weston_desktop_surface_get_surface(
				tl->desktop_surface);
		if (!ws || !ws->resource)
			continue;
		if (wl_resource_get_client(ws->resource) == client)
			return tl;
	}
	return NULL;
}

/* Last-chance fallback: when the selection source comes from a different
 * wl_client than every tracked toplevel (typical of waypipe-tier3 where
 * each app launch creates a fresh waypipe-client wl_client, and the
 * test source-helper is a separate launch from the test window), match
 * on the *secctx app_id* instead. Same silo → same app_id → returns the
 * first toplevel from any wl_client tagged with the same app_id.
 * NULL when the source has no secctx or no toplevel shares it. */
static struct qdwin_toplevel *
qdwin_toplevel_for_secctx_app_id(struct qdwin *qdwin,
				 struct wl_client *client)
{
	if (!client)
		return NULL;
	struct qdwin_secctx_client *src_sc =
		qdwin_secctx_client_lookup(qdwin, client);
	const char *src_app_id = qdwin_secctx_client_app_id(src_sc);
	if (!src_app_id || !*src_app_id)
		return NULL;
	struct qdwin_toplevel *tl;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		struct weston_surface *ws = NULL;
		if (tl->desktop_surface)
			ws = weston_desktop_surface_get_surface(
				tl->desktop_surface);
		if (!ws || !ws->resource)
			continue;
		struct wl_client *tlc = wl_resource_get_client(ws->resource);
		struct qdwin_secctx_client *tl_sc =
			qdwin_secctx_client_lookup(qdwin, tlc);
		const char *tl_app_id = qdwin_secctx_client_app_id(tl_sc);
		if (tl_app_id && *tl_app_id &&
		    strcmp(tl_app_id, src_app_id) == 0)
			return tl;
	}
	return NULL;
}

/* Cross-process fallback removed: investigated 2026-04-27 evening on
 * qdwin-final-tier45-260427-1419 with diagnostic logging. The waypipe
 * silo→admin set_selection path arrives at qdwin's selection_signal
 * with src->resource == NULL (the data_source carried by the seat is
 * actually weston's own clipboard.c synthetic clipboard_source after
 * the original is read+cached, OR — more importantly — weston's stale-
 * serial guard in weston_seat_set_selection rejects waypipe's serial=0
 * second set when the admin helper just set at serial=0). Either way
 * src_client is NULL by the time qdwin sees it, so a per-PID fallback
 * doesn't run. Fix needs to land before the serial guard — out of
 * scope for a shell plugin. spec/10 §"e2e gate hits weston serial
 * guard" carries the analysis. */

/* Pack mime types from a wl_array of char* into an LF-joined buffer.
 * Caller frees the returned string (NULL when input is empty). The
 * choice of LF (not NUL or comma) keeps the wire arg as a plain
 * Wayland string without escaping; mime types never contain whitespace
 * per RFC 6838.
 *
 * Mime types are ASCII per RFC 6838 §4.2 — the wire is UTF-8 (Wayland
 * strings) and pywayland validates UTF-8 strictly. Some Wayland
 * clients (wl-clipboard, GTK in some configurations) advertise types
 * with non-printable or non-ASCII bytes. We silently sanitize: any
 * byte outside printable-ASCII (0x20..0x7E) gets replaced with '?',
 * and an entry that would collapse to empty is dropped. Logging the
 * substitutions would be noisy and adds no admin value — the gate is
 * still applied, just on a sanitized type name. */
static char *
qdwin_pack_mime_types(struct wl_array *types)
{
	if (!types || types->size == 0)
		return strdup("");
	size_t total = 0;
	char **p;
	wl_array_for_each(p, types) {
		const char *s = p ? *p : NULL;
		if (s)
			total += strlen(s) + 1;  /* +1 for LF or terminating NUL */
	}
	if (total == 0)
		return strdup("");
	char *out = malloc(total + 1);
	if (!out)
		return NULL;
	size_t off = 0;
	int first = 1;
	wl_array_for_each(p, types) {
		const char *s = p ? *p : NULL;
		if (!s)
			continue;
		size_t n = strlen(s);
		if (n == 0)
			continue;
		if (!first)
			out[off++] = '\n';
		for (size_t i = 0; i < n; i++) {
			unsigned char b = (unsigned char)s[i];
			out[off++] = (b >= 0x20 && b <= 0x7E) ? (char)b : '?';
		}
		first = 0;
	}
	out[off] = '\0';
	return out;
}

static void
qdwin_emit_selection_set(struct qdwin *qdwin, struct weston_seat *seat,
			 struct wl_array *mime_types, int is_primary,
			 struct wl_client *src_client_fallback)
{
	if (!qdwin_shell_can_receive_v11(qdwin))
		return;
	struct qdwin_toplevel *tl =
		qdwin_toplevel_for_keyboard_focus(qdwin, seat);
	/* spec/10 fallback: when no toplevel has keyboard focus (RDP
	 * dummy backend, compositor-grab popup, off-screen scratch
	 * surface), fall back to ANY toplevel owned by the data source's
	 * wl_client. The broker gate is per-wl_client not per-toplevel
	 * (silo identity is carried by the security-context tag attached
	 * to the wl_client), so any matching toplevel produces a usable
	 * source_handle. Closes the long-standing s39 e2e hole where
	 * sdl-freerdp /v: dummy never delivered wl_keyboard.enter. */
	if (!tl)
		tl = qdwin_toplevel_for_client(qdwin, src_client_fallback);
	/* Last-chance: a different wl_client may share the source's silo
	 * (typical of waypipe-tier3 where each app launch is a fresh
	 * waypipe-client wl_client). Match on secctx app_id. */
	if (!tl)
		tl = qdwin_toplevel_for_secctx_app_id(qdwin,
						      src_client_fallback);
	uint32_t handle = tl ? tl->handle : UINT32_MAX;
	char *joined = qdwin_pack_mime_types(mime_types);
	const char *name = (seat && seat->seat_name) ? seat->seat_name : "";
	/* v23 sidecar (selection_set_source_identity): when the wl_client
	 * that issued set_selection carries a wp_security_context_v1 tag,
	 * emit its (engine, app_id, instance_id) tuple IMMEDIATELY BEFORE
	 * selection_set. Lets the shell derive src_silo from the wire
	 * (ClipboardGate.qml) instead of from the keyboard-focused
	 * toplevel handle — which collapses to "admin shell" when a
	 * tagged client emits set_selection without owning a focused
	 * toplevel (XWayland helper, waypipe-tier3 bridge, browser-extension
	 * silo proxy). Untagged sources skip the sidecar entirely and the
	 * shell falls back to the v11 focus-handle path. */
	if (wl_resource_get_version(qdwin->shell_resource) >= 23 &&
	    src_client_fallback) {
		struct qdwin_secctx_client *src_sc =
			qdwin_secctx_client_lookup(qdwin, src_client_fallback);
		if (src_sc) {
			const char *src_engine   = qdwin_secctx_client_engine(src_sc);
			const char *src_app_id   = qdwin_secctx_client_app_id(src_sc);
			const char *src_instance = qdwin_secctx_client_instance_id(src_sc);
			qdwin_shell_v1_send_selection_set_source_identity(
				qdwin->shell_resource,
				src_engine   ? src_engine   : "",
				src_app_id   ? src_app_id   : "",
				src_instance ? src_instance : "");
			weston_log("qdwin: selection_set_source_identity "
				   "engine=%s app_id=%s instance=%s\n",
				   src_engine   ? src_engine   : "",
				   src_app_id   ? src_app_id   : "",
				   src_instance ? src_instance : "");
		}
	}
	qdwin_shell_v1_send_selection_set(qdwin->shell_resource,
					  name, handle,
					  joined ? joined : "",
					  is_primary ? 1u : 0u);
	weston_log("qdwin: selection_set seat=%s handle=%u "
		   "primary=%d mimes='%s'\n",
		   name, handle, is_primary,
		   joined ? joined : "");
	free(joined);
}

/* spec/10 v15 — wl_data_offer.receive per-MIME / per-recipient gating.
 *
 * v14's `seat_focus_changed` lets the shell drop the seat selection on
 * cross-silo focus moves — binary, all-or-nothing. v15 adds receive-
 * time policy so the shell can authorise a specific (mime_type,
 * destination) pair against the rules engine while leaving other
 * mime types denied.
 *
 * Mechanism (no clean upstream hook in libweston-14): when
 * selection_signal fires we wrap the new data_source's `send` callback
 * with our shim. libweston later invokes `send(source, mime, fd)` on
 * every wl_data_offer.receive call from a destination. The shim
 * defers the original `send` until the shell answers via
 * `data_offer_receive_decision`. On allow → original_send(fd); on deny
 * → close(fd) (destination sees EOF → empty paste).
 *
 * The wrap is per-source (one weston_data_source = one wrap entry,
 * one libweston `destroy_signal` listener for cleanup). Pre-v15 shells
 * cause the install to be skipped → libweston's original send is
 * never overwritten → v14 semantics preserved.
 */

struct qdwin_data_source_wrap {
	struct qdwin *qdwin;
	struct weston_data_source *source;
	struct weston_seat *seat;
	void (*original_send)(struct weston_data_source *,
			      const char *, int32_t);
	struct wl_listener destroy_listener;
	struct wl_list link;  /* qdwin::data_source_wraps */
};

struct qdwin_data_offer_pending {
	struct qdwin *qdwin;
	struct qdwin_data_source_wrap *wrap;  /* NULL after src destroyed */
	uint32_t handle;
	int fd;        /* -1 once handed off / closed */
	char *mime;
	struct wl_event_source *timeout_source;
	struct wl_list link;  /* qdwin::data_offer_pending */
};

static struct qdwin_data_source_wrap *
qdwin_data_source_wrap_find(struct qdwin *qdwin,
			    struct weston_data_source *src)
{
	struct qdwin_data_source_wrap *w;
	wl_list_for_each(w, &qdwin->data_source_wraps, link) {
		if (w->source == src)
			return w;
	}
	return NULL;
}

static void
qdwin_data_offer_pending_close(struct qdwin_data_offer_pending *p, int allow)
{
	if (allow && p->wrap && p->wrap->original_send && p->fd >= 0) {
		p->wrap->original_send(p->wrap->source, p->mime, p->fd);
	} else if (p->fd >= 0) {
		close(p->fd);
	}
	p->fd = -1;
}

static void
qdwin_data_offer_pending_free(struct qdwin_data_offer_pending *p)
{
	wl_list_remove(&p->link);
	if (p->timeout_source)
		wl_event_source_remove(p->timeout_source);
	if (p->fd >= 0)
		close(p->fd);
	free(p->mime);
	free(p);
}

static void
qdwin_data_offer_pending_free_all(struct qdwin *qdwin)
{
	struct qdwin_data_offer_pending *p, *tmp;
	wl_list_for_each_safe(p, tmp,
			      &qdwin->data_offer_pending, link) {
		if (p->fd >= 0)
			close(p->fd);
		wl_list_remove(&p->link);
		if (p->timeout_source)
			wl_event_source_remove(p->timeout_source);
		free(p->mime);
		free(p);
	}
}

static int
qdwin_data_offer_pending_timeout_cb(void *data)
{
	struct qdwin_data_offer_pending *p = data;
	weston_log("qdwin: data_offer_receive_pending handle=%u "
		   "timed out → deny\n", p->handle);
	p->timeout_source = NULL;  /* one-shot; libwl frees on return */
	qdwin_data_offer_pending_close(p, 0);
	qdwin_data_offer_pending_free(p);
	return 0;
}

static void
qdwin_data_source_wrap_on_source_destroy(struct wl_listener *l, void *data)
{
	struct qdwin_data_source_wrap *w =
		wl_container_of(l, w, destroy_listener);
	(void)data;
	struct qdwin_data_offer_pending *p, *tmp;
	wl_list_for_each_safe(p, tmp,
			      &w->qdwin->data_offer_pending, link) {
		if (p->wrap != w)
			continue;
		/* Source is going away; we cannot honour an allow now.
		 * Close fd → destination sees EOF (empty paste). */
		if (p->fd >= 0)
			close(p->fd);
		p->fd = -1;
		p->wrap = NULL;
		wl_list_remove(&p->link);
		if (p->timeout_source)
			wl_event_source_remove(p->timeout_source);
		free(p->mime);
		free(p);
	}
	wl_list_remove(&w->link);
	wl_list_remove(&w->destroy_listener.link);
	free(w);
}

static void
qdwin_data_source_wraps_free_all(struct qdwin *qdwin)
{
	struct qdwin_data_source_wrap *w, *tmp;
	wl_list_for_each_safe(w, tmp, &qdwin->data_source_wraps, link) {
		wl_list_remove(&w->link);
		wl_list_remove(&w->destroy_listener.link);
		if (w->source && w->original_send)
			w->source->send = w->original_send;
		free(w);
	}
}

static void qdwin_data_source_send_shim(struct weston_data_source *source,
					 const char *mime, int32_t fd);

/* Install the send-shim on src->send. Idempotent + version-gated. */
static void
qdwin_install_data_source_wrap(struct qdwin *qdwin,
			       struct weston_seat *seat,
			       struct weston_data_source *src)
{
	if (!qdwin || !src)
		return;
	if (!qdwin_shell_can_receive_v15(qdwin))
		return;
	if (qdwin_data_source_wrap_find(qdwin, src))
		return;
	if (!src->send)
		return;  /* nothing to wrap */
	struct qdwin_data_source_wrap *w = calloc(1, sizeof(*w));
	if (!w)
		return;
	w->qdwin = qdwin;
	w->source = src;
	w->seat = seat;
	w->original_send = src->send;
	src->send = qdwin_data_source_send_shim;
	w->destroy_listener.notify = qdwin_data_source_wrap_on_source_destroy;
	wl_signal_add(&src->destroy_signal, &w->destroy_listener);
	wl_list_insert(&qdwin->data_source_wraps, &w->link);
	weston_log("qdwin: v15 data_source wrap installed src=%p seat=%s\n",
		   src,
		   (seat && seat->seat_name) ? seat->seat_name : "(none)");
}

/* libweston invokes our shim on every wl_data_offer.receive against the
 * wrapped source. We hold the fd, advertise to the shell, and wait. */
static void
qdwin_data_source_send_shim(struct weston_data_source *source,
			    const char *mime, int32_t fd)
{
	struct qdwin *qdwin = qdwin_singleton;
	struct qdwin_data_source_wrap *w = qdwin ?
		qdwin_data_source_wrap_find(qdwin, source) : NULL;
	if (!w || !qdwin || !qdwin_shell_can_receive_v15(qdwin)) {
		/* Wrap missing or shell version dropped — pass through if
		 * we still know the original; otherwise close fd to avoid
		 * leak. */
		if (w && w->original_send)
			w->original_send(source, mime, fd);
		else
			close(fd);
		return;
	}

	struct qdwin_data_offer_pending *p = calloc(1, sizeof(*p));
	if (!p) {
		close(fd);
		return;
	}
	p->qdwin = qdwin;
	p->wrap = w;
	p->fd = fd;
	p->mime = strdup(mime ? mime : "");
	if (!p->mime) {
		close(fd);
		free(p);
		return;
	}
	p->handle = ++qdwin->data_offer_receive_next_handle;
	if (p->handle == 0)
		p->handle = ++qdwin->data_offer_receive_next_handle;
	wl_list_insert(&qdwin->data_offer_pending, &p->link);

	struct wl_client *src_client = source->resource ?
		wl_resource_get_client(source->resource) : NULL;
	struct qdwin_toplevel *src_tl =
		qdwin_toplevel_for_keyboard_focus(qdwin, w->seat);
	if (!src_tl)
		src_tl = qdwin_toplevel_for_client(qdwin, src_client);
	if (!src_tl)
		src_tl = qdwin_toplevel_for_secctx_app_id(qdwin, src_client);
	struct qdwin_toplevel *dst_tl =
		qdwin_toplevel_for_keyboard_focus(qdwin, w->seat);
	uint32_t source_handle = src_tl ? src_tl->handle : UINT32_MAX;
	uint32_t target_handle = dst_tl ? dst_tl->handle : UINT32_MAX;
	const char *seat_name = (w->seat && w->seat->seat_name) ?
		w->seat->seat_name : "";

	qdwin_shell_v1_send_data_offer_receive_pending(
		qdwin->shell_resource, p->handle, seat_name,
		source_handle, target_handle, p->mime);
	weston_log("qdwin: data_offer_receive_pending handle=%u seat=%s "
		   "src=%u dst=%u mime='%s' fd=%d\n",
		   p->handle, seat_name, source_handle, target_handle,
		   p->mime, p->fd);

	struct wl_event_loop *loop =
		wl_display_get_event_loop(qdwin->compositor->wl_display);
	p->timeout_source = wl_event_loop_add_timer(
		loop, qdwin_data_offer_pending_timeout_cb, p);
	if (p->timeout_source)
		wl_event_source_timer_update(p->timeout_source, 2000);
}

static struct qdwin_data_offer_pending *
qdwin_data_offer_pending_find(struct qdwin *qdwin, uint32_t handle)
{
	struct qdwin_data_offer_pending *p;
	wl_list_for_each(p, &qdwin->data_offer_pending, link)
		if (p->handle == handle)
			return p;
	return NULL;
}

static void
qdwin_handle_data_offer_receive_decision(struct wl_client *client,
					 struct wl_resource *resource,
					 uint32_t handle,
					 const char *decision)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	struct qdwin_data_offer_pending *p =
		qdwin_data_offer_pending_find(qdwin, handle);
	if (!p) {
		weston_log("qdwin: data_offer_receive_decision unknown "
			   "handle=%u\n", handle);
		return;
	}
	int allow = (decision && strcmp(decision, "allow") == 0);
	weston_log("qdwin: data_offer_receive_decision handle=%u → %s\n",
		   handle, allow ? "allow" : "deny");
	qdwin_data_offer_pending_close(p, allow);
	qdwin_data_offer_pending_free(p);
}

/* weston_seat::selection_signal fires after weston_seat_set_selection.
 * data is the seat. We read seat->selection_data_source — NULL on
 * clear, populated on a real set. We only emit on a real set; clears
 * are tracked by the shell separately (it knows it just called
 * clear_selection). */
static void
qdwin_on_seat_selection_changed(struct wl_listener *listener, void *data)
{
	struct qdwin_seat_tracker *tr =
		wl_container_of(listener, tr, selection_listener);
	struct weston_seat *seat = data;
	if (!seat)
		return;
	struct weston_data_source *src = seat->selection_data_source;
	if (!src) {
		/* Selection was cleared — don't emit; the shell either
		 * triggered the clear itself (via clear_selection request)
		 * or the focused client called set_selection(NULL)
		 * voluntarily, in which case there's no policy to apply. */
		return;
	}
	/* spec/10 v15: install the per-source send-shim BEFORE we tell
	 * the shell about the selection. Once the shell starts processing
	 * selection_set, destinations may already begin calling
	 * wl_data_offer.receive; the shim must already be in place to
	 * intercept. No-op when shell version <15. */
	qdwin_install_data_source_wrap(tr->qdwin, seat, src);
	/* Pass the data source's wl_client so emit_selection_set can
	 * fall back to a toplevel owned by that client when no toplevel
	 * has keyboard focus. */
	struct wl_client *src_client = src->resource ?
		wl_resource_get_client(src->resource) : NULL;
	qdwin_emit_selection_set(tr->qdwin, seat, &src->mime_types, 0,
				 src_client);
}

/* spec/10 v14: shell observes keyboard-focus moves so it can drop the
 * seat selection on cross-silo focus transitions ("Qubes-style"). The
 * compositor side emits `seat_focus_changed` whenever weston_keyboard's
 * focus surface changes; the shell maps surfaces to silos via the
 * toplevel handle and decides whether to call `clear_selection`. */
static void
qdwin_emit_seat_focus_changed(struct qdwin *qdwin, struct weston_seat *seat,
			      uint32_t handle)
{
	if (!qdwin_shell_can_receive_v14(qdwin))
		return;
	const char *name = (seat && seat->seat_name) ? seat->seat_name : "";
	qdwin_shell_v1_send_seat_focus_changed(qdwin->shell_resource,
					       name, handle);
	weston_log("qdwin: seat_focus_changed seat=%s handle=%u\n",
		   name, handle);
}

/* Walk toplevels for one whose primary view surface == s. NULL if s is
 * a panel/lock/popup/proxy or not tracked. */
static struct qdwin_toplevel *
qdwin_toplevel_by_surface(struct qdwin *qdwin, struct weston_surface *s)
{
	if (!s)
		return NULL;
	struct qdwin_toplevel *tl;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		if (tl->view && tl->view->surface == s)
			return tl;
		if (tl->proxy_pixel_surface && tl->proxy_pixel_surface == s)
			return tl;
	}
	return NULL;
}

static void
qdwin_seat_emit_focus_now(struct qdwin_seat_tracker *tr)
{
	if (!tr || !tr->seat)
		return;
	struct weston_keyboard *kbd = weston_seat_get_keyboard(tr->seat);
	struct weston_surface *focus = kbd ? kbd->focus : NULL;
	struct qdwin_toplevel *tl = qdwin_toplevel_by_surface(tr->qdwin, focus);
	uint32_t handle = tl ? tl->handle : UINT32_MAX;
	if (handle == tr->last_focused_handle)
		return;
	uint32_t prev = tr->last_focused_handle;
	tr->last_focused_handle = handle;
	/* Unconditional ground-truth log — independent of shell binding state
	 * so focus transitions are observable in the journal even before
	 * qdshell has bound qdwin_shell_v1 at v14+ (see
	 * todo/qdwin-focus-events.md). The shell-facing protocol emit below
	 * is still gated on v14 inside qdwin_emit_seat_focus_changed. */
	weston_log("qdwin: focus handle=%u (was %u) seat=%s\n",
		   handle, prev,
		   (tr->seat && tr->seat->seat_name) ? tr->seat->seat_name : "");
	qdwin_emit_seat_focus_changed(tr->qdwin, tr->seat, handle);

	/* Focus-transfer recovery: when focus drops to "no toplevel"
	 * (handle == UINT32_MAX) while live siblings exist, auto-recover
	 * to the next mapped+decorated toplevel. Deferred to the next
	 * event-loop pass via wl_event_loop_add_idle — see the docblock
	 * on focus_recover_idle for why synchronous recovery here
	 * infinite-loops when the dropping window is itself mid-destroy.
	 *
	 * Skipped when locked (locker grab owns focus). One idle queued
	 * per tracker at a time; if focus oscillates we'd over-schedule
	 * but the de-dupe at the top of this function makes the second
	 * idle a no-op. */
	if (handle == UINT32_MAX && kbd && !tr->qdwin->locked &&
	    !tr->focus_recover_idle) {
		struct wl_event_loop *loop = wl_display_get_event_loop(
			tr->qdwin->compositor->wl_display);
		if (loop)
			tr->focus_recover_idle = wl_event_loop_add_idle(
				loop, qdwin_seat_focus_recover_idle_cb, tr);
	}
}

/* Idle callback firing one event-loop iteration after focus dropped
 * to UINT32_MAX. Re-checks state (focus might have legitimately moved
 * elsewhere in the meantime) and assigns focus to the head-most
 * mapped+decorated toplevel if any remain. See the focus_recover_idle
 * docblock for the rationale on deferring rather than recovering
 * synchronously. */
static void
qdwin_seat_focus_recover_idle_cb(void *data)
{
	struct qdwin_seat_tracker *tr = data;
	tr->focus_recover_idle = NULL;
	if (!tr->seat || !tr->qdwin || tr->qdwin->locked)
		return;
	struct weston_keyboard *kbd = weston_seat_get_keyboard(tr->seat);
	if (!kbd || kbd->focus != NULL)
		return;  /* focus already moved elsewhere, nothing to do */
	struct qdwin_toplevel *succ = NULL;
	struct qdwin_toplevel *cand;
	wl_list_for_each(cand, &tr->qdwin->toplevels, link) {
		if (!cand->decorated)
			continue;
		if (cand->view && cand->view->surface &&
		    weston_surface_is_mapped(cand->view->surface)) {
			succ = cand;
			break;
		}
	}
	if (succ)
		weston_keyboard_set_focus(kbd, succ->view->surface);
}

static void
qdwin_on_keyboard_focus_changed(struct wl_listener *listener, void *data)
{
	struct qdwin_seat_tracker *tr =
		wl_container_of(listener, tr, kbd_focus_listener);
	(void)data;
	qdwin_seat_emit_focus_now(tr);
}

static void
qdwin_replay_seat_focus_for_shell(struct qdwin *qdwin)
{
	/* Emit one seat_focus_changed per tracked seat, unconditionally,
	 * so the shell starts with a coherent focus map. We bypass
	 * `qdwin_seat_emit_focus_now`'s dedup guard since a freshly bound
	 * shell has no prior focus state and needs the full snapshot
	 * (including the "no toplevel focused" / UINT32_MAX case). */
	struct qdwin_seat_tracker *tr;
	wl_list_for_each(tr, &qdwin->seat_trackers, link) {
		struct weston_keyboard *kbd =
			weston_seat_get_keyboard(tr->seat);
		struct weston_surface *focus = kbd ? kbd->focus : NULL;
		struct qdwin_toplevel *tl =
			qdwin_toplevel_by_surface(tr->qdwin, focus);
		uint32_t handle = tl ? tl->handle : UINT32_MAX;
		tr->last_focused_handle = handle;
		qdwin_emit_seat_focus_changed(qdwin, tr->seat, handle);
	}
}

/* Lookup a seat tracker by weston_seat. Currently unused — kept as a
 * forward-compatible helper for future code paths that need the seat
 * tracker without walking from a wl_listener via wl_container_of. */
static struct qdwin_seat_tracker *
qdwin_seat_tracker_for_seat(struct qdwin *qdwin, struct weston_seat *seat)
{
	struct qdwin_seat_tracker *tr;
	wl_list_for_each(tr, &qdwin->seat_trackers, link)
		if (tr->seat == seat)
			return tr;
	return NULL;
}

/* spec/10 v16: accessor helpers that let request handlers above the
 * struct definition (e.g. qdwin_handle_set_keyboard_focus_v2) read
 * and write last_target_silo without seeing the struct layout. */
static const char *
qdwin_seat_tracker_silo(struct qdwin_seat_tracker *tr)
{
	if (!tr || !tr->last_target_silo)
		return "";
	return tr->last_target_silo;
}

static void
qdwin_seat_tracker_set_silo(struct qdwin_seat_tracker *tr, const char *silo)
{
	if (!tr)
		return;
	free(tr->last_target_silo);
	tr->last_target_silo = (silo && *silo) ? strdup(silo) : NULL;
}

static void
qdwin_install_focus_listener_if_needed(struct qdwin_seat_tracker *tr)
{
	if (!tr || tr->kbd_focus_listener_installed)
		return;
	struct weston_keyboard *kbd = weston_seat_get_keyboard(tr->seat);
	if (!kbd)
		return;
	tr->kbd_focus_listener.notify = qdwin_on_keyboard_focus_changed;
	wl_signal_add(&kbd->focus_signal, &tr->kbd_focus_listener);
	tr->kbd_focus_listener_installed = 1;
	/* Emit current state so the shell starts coherent — this is also
	 * what bind_as_shell relies on for late-binding shells. */
	tr->last_focused_handle = UINT32_MAX;  /* force a fresh emit */
	qdwin_seat_emit_focus_now(tr);
}

/* B6: install a pointer-focus listener so we can restore the default
 * cursor sprite whenever pointer focus leaves a client surface. Plus
 * install the default sprite immediately on the pointer if it has no
 * sprite yet (e.g. cursor-sprites helper already registered the
 * default by the time the seat tracker exists). */
static void
qdwin_install_pointer_focus_listener_if_needed(struct qdwin_seat_tracker *tr)
{
	if (!tr || tr->pointer_focus_listener_installed)
		return;
	struct weston_pointer *p = weston_seat_get_pointer(tr->seat);
	if (!p)
		return;
	tr->pointer_focus_listener.notify = qdwin_default_cursor_on_focus_changed;
	wl_signal_add(&p->focus_signal, &tr->pointer_focus_listener);
	tr->pointer_focus_listener_installed = 1;
	if (!p->sprite)
		qdwin_install_default_cursor_on_pointer(tr->qdwin, p);
}

static void
qdwin_send_seat_created(struct qdwin *qdwin, struct weston_seat *seat)
{
	if (!qdwin_shell_can_receive_v2(qdwin))
		return;
	qdwin_shell_v1_send_seat_created(qdwin->shell_resource,
					 seat->seat_name ? seat->seat_name : "");
}

static void
qdwin_send_seat_removed(struct qdwin *qdwin, const char *seat_name)
{
	if (!qdwin_shell_can_receive_v2(qdwin))
		return;
	qdwin_shell_v1_send_seat_removed(qdwin->shell_resource,
					 seat_name ? seat_name : "");
}

static void
qdwin_seat_tracker_destroy(struct qdwin_seat_tracker *tr, int emit_removed)
{
	if (!tr)
		return;
	if (emit_removed)
		qdwin_send_seat_removed(tr->qdwin, tr->seat_name);
	wl_list_remove(&tr->destroy_listener.link);
	wl_list_remove(&tr->updated_caps_listener.link);
	wl_list_remove(&tr->selection_listener.link);
	if (tr->kbd_focus_listener_installed) {
		wl_list_remove(&tr->kbd_focus_listener.link);
		tr->kbd_focus_listener_installed = 0;
	}
	if (tr->pointer_focus_listener_installed) {
		wl_list_remove(&tr->pointer_focus_listener.link);
		tr->pointer_focus_listener_installed = 0;
	}
	if (tr->focus_recover_idle) {
		wl_event_source_remove(tr->focus_recover_idle);
		tr->focus_recover_idle = NULL;
	}
	wl_list_remove(&tr->link);
	tr->seat = NULL;
	free(tr->last_target_silo);
	free(tr->seat_name);
	free(tr);
}

static void
qdwin_send_output_created_evt(struct qdwin *qdwin, struct weston_output *output)
{
	if (!qdwin_shell_can_receive_v2(qdwin))
		return;
	qdwin_shell_v1_send_output_created(qdwin->shell_resource,
					   output->name ? output->name : "");
}

static void
qdwin_send_output_removed(struct qdwin *qdwin, struct weston_output *output)
{
	if (!qdwin_shell_can_receive_v2(qdwin))
		return;
	qdwin_shell_v1_send_output_removed(qdwin->shell_resource,
					   output->name ? output->name : "");
}

static void
qdwin_on_seat_destroyed(struct wl_listener *listener, void *data)
{
	struct qdwin_seat_tracker *tr =
		wl_container_of(listener, tr, destroy_listener);
	(void)data;
	qdwin_seat_tracker_destroy(tr, 1);
}

/* §6.8 S3c: caps just changed on this seat — a keyboard may have just
 * appeared (RDP backend's keyboard arrives after seat creation, USB
 * keyboards hot-plug). Reinstall the default keyboard grab if so. */
static void
qdwin_on_seat_updated_caps(struct wl_listener *listener, void *data)
{
	struct qdwin_seat_tracker *tr =
		wl_container_of(listener, tr, updated_caps_listener);
	struct weston_seat *seat = data;
	if (seat == tr->seat) {
		qdwin_install_default_keyboard_grab(seat);
		/* spec/10 v14: keyboard may have just appeared; install the
		 * focus listener now if we hadn't yet. If we previously
		 * installed on an older keyboard, remove that link first
		 * before re-arming so the listener can never be on two
		 * focus_signal lists at once (which would have wl_signal_emit
		 * iterating a corrupted list — and on at least one
		 * weston-rdp keyboard re-init the result was a wedged
		 * compositor at 100% CPU). */
		if (tr->kbd_focus_listener_installed) {
			wl_list_remove(&tr->kbd_focus_listener.link);
			tr->kbd_focus_listener_installed = 0;
		}
		qdwin_install_focus_listener_if_needed(tr);
		/* B6: same drill for the pointer focus signal. */
		if (tr->pointer_focus_listener_installed) {
			wl_list_remove(&tr->pointer_focus_listener.link);
			tr->pointer_focus_listener_installed = 0;
		}
		qdwin_install_pointer_focus_listener_if_needed(tr);
	}
}

static struct qdwin_seat_tracker *
qdwin_track_seat(struct qdwin *qdwin, struct weston_seat *seat)
{
	struct qdwin_seat_tracker *tr = calloc(1, sizeof *tr);
	if (!tr)
		return NULL;
	tr->qdwin = qdwin;
	tr->seat = seat;
	tr->seat_name = strdup(seat->seat_name ? seat->seat_name : "");
	if (!tr->seat_name) {
		free(tr);
		return NULL;
	}
	tr->destroy_listener.notify = qdwin_on_seat_destroyed;
	wl_signal_add(&seat->destroy_signal, &tr->destroy_listener);
	tr->updated_caps_listener.notify = qdwin_on_seat_updated_caps;
	wl_signal_add(&seat->updated_caps_signal, &tr->updated_caps_listener);
	tr->selection_listener.notify = qdwin_on_seat_selection_changed;
	wl_signal_add(&seat->selection_signal, &tr->selection_listener);
	tr->last_focused_handle = UINT32_MAX;
	wl_list_insert(&qdwin->seat_trackers, &tr->link);
	/* §6.8 S3c: install on whatever keyboard is already present. */
	qdwin_install_default_keyboard_grab(seat);
	/* spec/10 v14: install kbd focus listener now if a keyboard exists.
	 * For RDP backends without a keyboard at seat-creation time, the
	 * updated_caps handler re-tries when the keyboard appears. */
	qdwin_install_focus_listener_if_needed(tr);
	/* B6: pointer focus listener for default-cursor restoration. */
	qdwin_install_pointer_focus_listener_if_needed(tr);
	return tr;
}

static void
qdwin_on_seat_created(struct wl_listener *listener, void *data)
{
	struct qdwin *qdwin =
		wl_container_of(listener, qdwin, seat_created_listener);
	struct weston_seat *seat = data;
	qdwin_track_seat(qdwin, seat);
	qdwin_send_seat_created(qdwin, seat);
}

static void
qdwin_on_output_destroyed(struct wl_listener *listener, void *data)
{
	struct qdwin *qdwin =
		wl_container_of(listener, qdwin, output_destroyed_listener);
	struct weston_output *output = data;
	qdwin_send_output_removed(qdwin, output);
}

static void
qdwin_on_output_changed(struct wl_listener *listener, void *data)
{
	struct qdwin *qdwin =
		wl_container_of(listener, qdwin, output_created_listener);
	struct weston_output *output = data;
	qdwin_refresh_background(qdwin);
	qdwin_fractional_scale_broadcast(qdwin);
	qdwin_panels_on_output_change(qdwin);
	if (output)
		qdwin_send_output_created_evt(qdwin, output);
}

static void
qdwin_on_output_resized(struct wl_listener *listener, void *data)
{
	struct qdwin *qdwin =
		wl_container_of(listener, qdwin, output_resized_listener);
	(void)data;
	qdwin_refresh_background(qdwin);
	qdwin_fractional_scale_broadcast(qdwin);
	qdwin_panels_on_output_change(qdwin);
}

/* ------------------------------------------------------------------
 * Lifecycle.
 * ------------------------------------------------------------------ */

static void
qdwin_destroy(struct wl_listener *listener, void *data)
{
	struct qdwin *qdwin = wl_container_of(listener, qdwin,
					      destroy_listener);
	(void)data;
	if (qdwin->background) {
		weston_shell_utils_curtain_destroy(qdwin->background);
		qdwin->background = NULL;
	}
	wl_list_remove(&qdwin->output_created_listener.link);
	wl_list_remove(&qdwin->output_resized_listener.link);
	wl_list_remove(&qdwin->output_destroyed_listener.link);
	wl_list_remove(&qdwin->seat_created_listener.link);
	{
		struct qdwin_seat_tracker *tr, *tmp;
		wl_list_for_each_safe(tr, tmp, &qdwin->seat_trackers, link)
			qdwin_seat_tracker_destroy(tr, 0);
	}
	qdwin_activation_pending_free_all(qdwin);
	qdwin_data_offer_pending_free_all(qdwin);
	qdwin_data_source_wraps_free_all(qdwin);
	if (qdwin->desktop)
		weston_desktop_destroy(qdwin->desktop);
	if (qdwin->shell_global)
		wl_global_destroy(qdwin->shell_global);
	if (qdwin->locker_global)
		wl_global_destroy(qdwin->locker_global);
	if (qdwin->stream_input_global)
		wl_global_destroy(qdwin->stream_input_global);
	if (qdwin->xdg_activation_global)
		wl_global_destroy(qdwin->xdg_activation_global);
	if (qdwin->idle_notifier_global)
		wl_global_destroy(qdwin->idle_notifier_global);
	if (qdwin->idle_inhibit_manager_global)
		wl_global_destroy(qdwin->idle_inhibit_manager_global);
	if (qdwin->cursor_shape_manager_global)
		wl_global_destroy(qdwin->cursor_shape_manager_global);
	if (qdwin->fractional_scale_manager_global)
		wl_global_destroy(qdwin->fractional_scale_manager_global);
	if (qdwin->primary_selection_manager_global)
		wl_global_destroy(qdwin->primary_selection_manager_global);
	if (qdwin->security_context_manager_global)
		wl_global_destroy(qdwin->security_context_manager_global);
	qdwin_secctx_destroy_all(qdwin);
	if (qdwin->nested_manager_global)
		wl_global_destroy(qdwin->nested_manager_global);
	if (qdwin->layer_shell_global)
		wl_global_destroy(qdwin->layer_shell_global);
	if (qdwin->xdg_decoration_manager_global)
		wl_global_destroy(qdwin->xdg_decoration_manager_global);
	if (qdwin->nested_outer_event_source) {
		wl_event_source_remove(qdwin->nested_outer_event_source);
		qdwin->nested_outer_event_source = NULL;
	}
	if (qdwin->nested_client) {
		qdwin_nested_client_destroy(qdwin->nested_client);
		qdwin->nested_client = NULL;
	}
	if (qdwin->idle_signal_listener.notify)
		wl_list_remove(&qdwin->idle_signal_listener.link);
	if (qdwin->wake_signal_listener.notify)
		wl_list_remove(&qdwin->wake_signal_listener.link);
	qdwin_cursor_theme_destroy(qdwin);
	weston_layer_fini(&qdwin->background_layer);
	weston_layer_fini(&qdwin->held_layer);
	weston_layer_fini(&qdwin->normal_layer);
	weston_layer_fini(&qdwin->minimized_layer);
	weston_layer_fini(&qdwin->panel_layer);
	weston_layer_fini(&qdwin->notification_layer);
	weston_layer_fini(&qdwin->launcher_layer);
	weston_layer_fini(&qdwin->lock_layer);
	weston_layer_fini(&qdwin->popup_layer);
	for (int i = 0; i < 4; i++)
		weston_layer_fini(&qdwin->layer_shell_layer[i]);
	wl_list_remove(&qdwin->destroy_listener.link);
	free(qdwin);
}

/* ------------------------------------------------------------------
 * §6.7 idle-inhibit-unstable-v1 + ext-idle-notify-v1.
 *
 * Two protocols that share the idle-state concept, so they live in
 * one block.
 *
 * idle-inhibit: each zwp_idle_inhibitor_v1 bumps ec->idle_inhibit
 * while its backing wl_surface is alive; weston's own idle timer
 * defers while the counter is non-zero. Spec allows ignoring the
 * inhibitor while the surface is occluded / unmapped; we keep it
 * simple here and inhibit as long as the resource + surface exist.
 *
 * ext-idle-notify: every notification is linked from
 * qdwin::idle_notifications. Firing strategy (§6.7(a)):
 *
 *   - If `timeout_ms <= ec->idle_time * 1000`, we fire `idled` on
 *     weston's idle_signal directly. The notification actually fires
 *     at weston's coarser idle, which is later than the client asked
 *     for — spec-compliant ("at least timeout").
 *   - If `timeout_ms >  ec->idle_time * 1000`, we arm a per-
 *     notification wl_event_source timer at idle_signal for
 *     `timeout_ms - idle_time*1000` ms, so `idled` fires at roughly
 *     the right absolute offset. Without this, long-timeout clients
 *     would fire ~idle_time early, which *is* a spec violation.
 *
 * On wake_signal we always disarm the timer and send `resumed` if
 * the notification was idle.
 *
 * If weston is configured with idle_time=0 (built-in idle timer
 * disabled), idle_signal never fires and notifications never fire —
 * a deployment choice outside this plugin's scope.
 * ------------------------------------------------------------------ */

struct qdwin_idle_notification {
	struct qdwin *qdwin;
	struct wl_resource *resource;
	uint32_t timeout_ms;
	int is_idle;
	int ignore_inhibit;   /* v2 get_input_idle_notification */
	/* §6.7(a): per-notification delay timer. Armed on weston idle_signal
	 * when timeout_ms > weston idle_time*1000, so the notification fires
	 * at its requested offset instead of at weston's coarse idle. */
	struct wl_event_source *timer;
	struct wl_list link;  /* qdwin::idle_notifications */
};

struct qdwin_idle_inhibitor {
	struct qdwin *qdwin;
	struct wl_resource *resource;
	struct weston_surface *surface;
	struct wl_listener surface_destroy_listener;
	int active;           /* currently contributing to ec->idle_inhibit */
	struct wl_list link;  /* qdwin::idle_inhibitors */
};

static void
qdwin_idle_inhibitor_activate(struct qdwin_idle_inhibitor *inh)
{
	if (inh->active)
		return;
	inh->active = 1;
	inh->qdwin->compositor->idle_inhibit++;
}

static void
qdwin_idle_inhibitor_deactivate(struct qdwin_idle_inhibitor *inh)
{
	if (!inh->active)
		return;
	inh->active = 0;
	if (inh->qdwin->compositor->idle_inhibit > 0)
		inh->qdwin->compositor->idle_inhibit--;
}

static void
qdwin_idle_inhibitor_surface_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_idle_inhibitor *inh =
		wl_container_of(l, inh, surface_destroy_listener);
	(void)data;
	qdwin_idle_inhibitor_deactivate(inh);
	wl_list_remove(&inh->surface_destroy_listener.link);
	wl_list_init(&inh->surface_destroy_listener.link);
	inh->surface = NULL;
}

static void
qdwin_idle_inhibitor_destroy(struct wl_client *client,
			     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_idle_inhibitor_v1_interface qdwin_idle_inhibitor_impl = {
	.destroy = qdwin_idle_inhibitor_destroy,
};

static void
qdwin_idle_inhibitor_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_idle_inhibitor *inh = wl_resource_get_user_data(resource);
	if (!inh)
		return;
	qdwin_idle_inhibitor_deactivate(inh);
	if (inh->surface) {
		wl_list_remove(&inh->surface_destroy_listener.link);
		inh->surface = NULL;
	}
	wl_list_remove(&inh->link);
	free(inh);
}

static void
qdwin_idle_inhibit_manager_destroy(struct wl_client *client,
				   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_idle_inhibit_create_inhibitor(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id,
				    struct wl_resource *surface_resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct weston_surface *surface =
		surface_resource ? wl_resource_get_user_data(surface_resource)
				 : NULL;
	struct qdwin_idle_inhibitor *inh;
	struct wl_resource *inh_resource;

	inh = calloc(1, sizeof *inh);
	if (!inh) {
		wl_client_post_no_memory(client);
		return;
	}
	inh_resource = wl_resource_create(
		client, &zwp_idle_inhibitor_v1_interface,
		wl_resource_get_version(resource), id);
	if (!inh_resource) {
		free(inh);
		wl_client_post_no_memory(client);
		return;
	}
	inh->qdwin = qdwin;
	inh->resource = inh_resource;
	inh->surface = surface;
	wl_list_init(&inh->surface_destroy_listener.link);
	if (surface) {
		inh->surface_destroy_listener.notify =
			qdwin_idle_inhibitor_surface_destroyed;
		wl_signal_add(&surface->destroy_signal,
			      &inh->surface_destroy_listener);
	}
	wl_list_insert(&qdwin->idle_inhibitors, &inh->link);
	wl_resource_set_implementation(inh_resource, &qdwin_idle_inhibitor_impl,
				       inh,
				       qdwin_idle_inhibitor_resource_destroy);
	qdwin_idle_inhibitor_activate(inh);
	weston_log("qdwin: idle-inhibit created (count=%u)\n",
		   (unsigned)qdwin->compositor->idle_inhibit);
}

static const struct zwp_idle_inhibit_manager_v1_interface
qdwin_idle_inhibit_manager_impl = {
	.destroy         = qdwin_idle_inhibit_manager_destroy,
	.create_inhibitor= qdwin_idle_inhibit_create_inhibitor,
};

static void
bind_qdwin_idle_inhibit_manager(struct wl_client *client, void *data,
				uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource = wl_resource_create(
		client, &zwp_idle_inhibit_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource,
				       &qdwin_idle_inhibit_manager_impl,
				       qdwin, NULL);
}

/* --- zwlr_layer_shell_v1 (stub) ------------------------------------ */
/*
 * Stub implementation. Goals at this layer:
 *   1. Accept the protocol cleanly: bind global, dispatch every request
 *      defined in the v5 XML, store all state on per-surface objects.
 *   2. Drive the configure/ack handshake so a client (waybar via
 *      gtk-layer-shell-0) can complete protocol setup.
 *   3. Set the wl_surface role to "zwlr_layer_surface_v1" so weston
 *      enforces commit-without-role / role-conflict per spec.
 *
 * NOT yet implemented (next iteration):
 *   - Layout: anchor / exclusive_zone / margin → output rect math.
 *     Direct port of wlroots types/scene/layer_shell_v1.c (~150 lines)
 *     calling weston_view_set_position instead of scene_node_set_position.
 *   - Mapping: creating weston_views on the per-zwlr_layer
 *     weston_layer (BACKGROUND/BOTTOM_UI/UI/TOP_UI ladder slots).
 *   - Exclusive-zone propagation into qdwin's xdg-toplevel maximize.
 *   - get_popup integration (xdg_popup positioner against layer bounds).
 *   - Keyboard interactivity modes (none / exclusive / on_demand).
 *
 * Reference: $HOME/doc/quickshell/src/wayland/wlr_layershell/
 *            wlr-layer-shell-unstable-v1.xml (vendored to
 *            qdwin/wlr-layer-shell-unstable-v1.xml).
 *            wlroots/types/wlr_layer_shell_v1.c is the protocol-glue
 *            blueprint, but with libweston API substitutions.
 */

#define QDWIN_LAYER_SURFACE_ROLE "zwlr_layer_surface_v1"

struct qdwin_layer_surface {
	struct qdwin *qdwin;
	struct wl_resource *resource;          /* zwlr_layer_surface_v1 */
	struct weston_surface *surface;
	struct weston_output *output;          /* may be NULL → compositor-pick */
	char *namespace;
	uint32_t layer;                        /* zwlr_layer_shell_v1.layer */

	/* Per-surface state, pending vs current (applied on commit). */
	struct {
		uint32_t desired_w, desired_h;
		uint32_t anchor;
		int32_t  exclusive_zone;
		struct { int32_t top, right, bottom, left; } margin;
		uint32_t kbd_interactivity;
		uint32_t exclusive_edge;
	} pending, current;

	int initial_commit_seen;               /* first commit (no buffer) */
	int initial_configure_sent;            /* sent first configure event */
	uint32_t last_configure_serial;
	uint32_t last_acked_serial;

	/* Layout outputs of the most recent compute (used to detect when a
	 * fresh configure is needed and to position the view). */
	int32_t  box_x, box_y;
	uint32_t box_w, box_h;

	struct weston_view *view;              /* NULL until first post-ack
					        * commit with a buffer */
	int mapped;

	struct wl_listener commit_listener;
	struct wl_listener surface_destroy_listener;
	struct wl_list popups;                 /* qdwin_layer_popup::link */
	struct wl_list link;                   /* qdwin::layer_surfaces */
};

struct qdwin_layer_popup {
	struct qdwin_layer_surface *parent;
	struct wl_resource *popup_resource;    /* xdg_popup */
	struct weston_desktop_surface *desktop_surface;
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_listener surface_commit_listener;
	struct wl_listener surface_destroy_listener;
	/* plan3 H2 (deep-review): the underlying wl_surface destroy listener
	 * above only fires when the client destroys the wl_surface. A client
	 * can destroy the xdg_popup *role* resource independently (per the
	 * xdg-shell protocol) and libweston's resource-destroy path tears
	 * the desktop_surface down without touching the wl_surface. Without
	 * this listener, a destroyed xdg_popup would leave qdwin_layer_popup
	 * linked under ls->popups with a stale popup_resource — later
	 * commits/dismissals would deref freed libweston state. */
	struct wl_listener popup_resource_destroy_listener;
	struct wl_list link;                   /* qdwin_layer_surface::popups */

	/* plan3 H1: layer-popup grab state. Set when the client calls
	 * xdg_popup.grab and the vendored libweston layer-grab handler
	 * delegates to qdwin. One pointer grab at a time per qdwin
	 * instance (mirrors upstream xdg_popup grab serialisation). */
	struct weston_pointer_grab grab;
	int grab_active;
	/* plan3 H3 (deep-review): seat destroy listener so we know the
	 * cached grab.pointer became dangling before we attempt to end the
	 * grab on it during destroy/dismiss. */
	struct wl_listener seat_destroy_listener;
	struct weston_seat *grab_seat;
};

static void qdwin_layer_popup_destroy(struct qdwin_layer_popup *lp);

/* Pick a default output if the client passed NULL: first in the
 * compositor's output_list. Returns NULL if no outputs are connected
 * (client gets configure with desired or 0×0; we'll re-resolve on next
 * commit when an output may have appeared). */
static struct weston_output *
qdwin_layer_surface_resolve_output(struct qdwin_layer_surface *ls)
{
	if (ls->output)
		return ls->output;
	if (wl_list_empty(&ls->qdwin->compositor->output_list))
		return NULL;
	struct weston_output *o = NULL;
	o = wl_container_of(ls->qdwin->compositor->output_list.next, o, link);
	return o;
}

static int
qdwin_layer_surface_contains(struct qdwin_layer_surface *ls,
			     struct weston_coord_global pos)
{
	if (!ls || !ls->mapped || !ls->view || !ls->surface ||
	    ls->surface->width <= 0 || ls->surface->height <= 0)
		return 0;

	struct weston_coord_global vp =
		weston_view_get_pos_offset_global(ls->view);
	return pos.c.x >= vp.c.x && pos.c.x < vp.c.x + ls->surface->width &&
	       pos.c.y >= vp.c.y && pos.c.y < vp.c.y + ls->surface->height;
}

static struct qdwin_layer_surface *
qdwin_layer_surface_at_pos(struct qdwin *qdwin, struct weston_coord_global pos)
{
	if (!qdwin)
		return NULL;

	struct qdwin_layer_surface *ls;
	wl_list_for_each(ls, &qdwin->layer_surfaces, link) {
		if (qdwin_layer_surface_contains(ls, pos))
			return ls;
	}
	return NULL;
}

static struct weston_view *
qdwin_layer_surface_view_at_pos(struct qdwin *qdwin,
				struct weston_coord_global pos)
{
	struct qdwin_layer_surface *ls =
		qdwin_layer_surface_at_pos(qdwin, pos);
	return ls ? ls->view : NULL;
}

/* plan3 M4: ON_DEMAND keyboard interactivity handler. Called from the
 * default pointer grab on every left-button press. If the press lands
 * on a layer-shell surface that requested ON_DEMAND, transfer keyboard
 * focus there. EXCLUSIVE focus is handled at map time in
 * qdwin_layer_surface_apply; NONE never changes focus.
 *
 * plan3 NEW-H2: do NOT steal focus from an EXCLUSIVE layer surface.
 * A lock screen / OSK / on-call panel that requested EXCLUSIVE expects
 * its keyboard focus to be sticky; a click into a panel/overlay that
 * happens to request ON_DEMAND would otherwise silently take focus. We
 * walk every mapped layer surface and abort the transfer if any other
 * one with EXCLUSIVE currently owns kb->focus. */
static void
qdwin_layer_surface_handle_on_demand_button(struct qdwin *qdwin,
					    struct weston_pointer *pointer)
{
	if (!qdwin || !pointer)
		return;
	struct qdwin_layer_surface *ls =
		qdwin_layer_surface_at_pos(qdwin, pointer->pos);
	if (!ls || !ls->surface)
		return;
	if (ls->current.kbd_interactivity !=
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)
		return;
	struct weston_keyboard *kb = weston_seat_get_keyboard(pointer->seat);
	if (!kb || kb->focus == ls->surface)
		return;

	/* NEW-H2 guard: refuse if an EXCLUSIVE layer surface currently
	 * owns keyboard focus. Compare by ls->surface so a mapped
	 * EXCLUSIVE layer surface that re-mapped into a different
	 * weston_surface (rare) does not match accidentally. */
	struct qdwin_layer_surface *other;
	wl_list_for_each(other, &qdwin->layer_surfaces, link) {
		if (other == ls || !other->surface)
			continue;
		if (other->current.kbd_interactivity !=
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
			continue;
		if (kb->focus == other->surface) {
			weston_log("qdwin: layer-shell ON_DEMAND focus "
				   "skipped (EXCLUSIVE held by ns=%s) "
				   "target ns=%s\n",
				   other->namespace ? other->namespace
						    : "(null)",
				   ls->namespace ? ls->namespace : "(null)");
			return;
		}
	}

	weston_keyboard_set_focus(kb, ls->surface);
	weston_log("qdwin: layer-shell ON_DEMAND focus -> ns=%s seat=%s\n",
		   ls->namespace ? ls->namespace : "(null)",
		   (pointer->seat && pointer->seat->seat_name)
			   ? pointer->seat->seat_name : "");
}

/* Per spec: when exclusive_edge is unset (0), derive from anchor.
 * - Anchored to one edge → that edge.
 * - Anchored to three edges (one "free" edge) → the opposite of the
 *   free edge.
 * - Otherwise: NONE (no zone reserved).
 * Encoded as the same bitfield as anchor: TOP/BOTTOM/LEFT/RIGHT. */
#define QLS_T ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
#define QLS_B ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
#define QLS_L ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
#define QLS_R ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
static uint32_t
qdwin_layer_surface_get_exclusive_edge(const struct qdwin_layer_surface *ls)
{
	if (ls->current.exclusive_edge != 0)
		return ls->current.exclusive_edge;

	switch (ls->current.anchor) {
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
}
#undef QLS_T
#undef QLS_B
#undef QLS_L
#undef QLS_R

/* Phase 1.3: forward-declared at top of file; called by
 * qdwin_output_work_area. Subtracts every mapped layer-shell surface's
 * exclusive zone from the working rect. exclusive_zone == -1 means
 * "occupies full output, doesn't reserve" — handled by the
 * exclusive_zone <= 0 skip. Margin on the anchored edge is added to
 * the zone (matches wlroots layer_surface_exclusive_zone semantics). */
static void
qdwin_layer_shell_subtract_zones(struct qdwin *qdwin,
				 struct weston_output *out,
				 int *x, int *y, int *w, int *h)
{
	struct qdwin_layer_surface *ls;
	wl_list_for_each(ls, &qdwin->layer_surfaces, link) {
		if (!ls->mapped) continue;
		if (ls->output && ls->output != out) continue;
		if (ls->current.exclusive_zone <= 0) continue;
		uint32_t edge = qdwin_layer_surface_get_exclusive_edge(ls);
		int t = ls->current.exclusive_zone;
		switch (edge) {
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
			t += ls->current.margin.top;
			*y += t; *h -= t; break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
			t += ls->current.margin.bottom;
			*h -= t; break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
			t += ls->current.margin.left;
			*x += t; *w -= t; break;
		case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
			t += ls->current.margin.right;
			*w -= t; break;
		default: break; /* ambiguous → no shrink */
		}
	}
}

/* Port of wlroots types/scene/layer_shell_v1.c
 * wlr_scene_layer_surface_v1_configure (commit a8bb88b cross-ref).
 * Computes the surface rect inside `bounds` (full output rect when
 * exclusive_zone == -1, otherwise the usable area shrunk by previously
 * placed exclusive zones). Writes result to ls->box_{x,y,w,h}. */
static void
qdwin_layer_surface_compute_box(struct qdwin_layer_surface *ls,
				int32_t bx, int32_t by,
				int32_t bw, int32_t bh)
{
	const uint32_t T = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	const uint32_t B = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	const uint32_t L = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	const uint32_t R = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	uint32_t a = ls->current.anchor;
	int32_t w = (int32_t)ls->current.desired_w;
	int32_t h = (int32_t)ls->current.desired_h;
	int32_t x, y;

	/* Horizontal */
	if (w == 0) {
		x = bx + ls->current.margin.left;
		w = bw - (ls->current.margin.left + ls->current.margin.right);
	} else if ((a & L) && (a & R)) {
		x = bx + bw / 2 - w / 2;
	} else if (a & L) {
		x = bx + ls->current.margin.left;
	} else if (a & R) {
		x = bx + bw - w - ls->current.margin.right;
	} else {
		x = bx + bw / 2 - w / 2;
	}

	/* Vertical */
	if (h == 0) {
		y = by + ls->current.margin.top;
		h = bh - (ls->current.margin.top + ls->current.margin.bottom);
	} else if ((a & T) && (a & B)) {
		y = by + bh / 2 - h / 2;
	} else if (a & T) {
		y = by + ls->current.margin.top;
	} else if (a & B) {
		y = by + bh - h - ls->current.margin.bottom;
	} else {
		y = by + bh / 2 - h / 2;
	}

	if (w < 0) w = 0;
	if (h < 0) h = 0;

	ls->box_x = x;
	ls->box_y = y;
	ls->box_w = (uint32_t)w;
	ls->box_h = (uint32_t)h;
}

static void
qdwin_layer_surface_send_configure(struct qdwin_layer_surface *ls)
{
	struct weston_output *out = qdwin_layer_surface_resolve_output(ls);
	int32_t fx = 0, fy = 0, fw = 1920, fh = 1080;
	if (out) {
		fx = out->pos.c.x;
		fy = out->pos.c.y;
		fw = out->width;
		fh = out->height;
	}
	/* Phase 1.1: full_area = output rect. Per-output usable_area
	 * (Phase 1.3) will subtract previously placed exclusive zones. */
	qdwin_layer_surface_compute_box(ls, fx, fy, fw, fh);

	uint32_t serial = ++ls->qdwin->layer_configure_serial_next;
	ls->last_configure_serial = serial;
	zwlr_layer_surface_v1_send_configure(ls->resource, serial,
					     ls->box_w, ls->box_h);
	weston_log("qdwin: layer-shell configure ns=%s layer=%u "
		   "box=%d,%d %ux%u serial=%u\n",
		   ls->namespace ? ls->namespace : "(null)",
		   ls->layer, ls->box_x, ls->box_y,
		   ls->box_w, ls->box_h, serial);
}

/* Lazily create the weston_view + place it on the right layer + map.
 * Idempotent across commits; subsequent commits just re-position. */
static void
qdwin_layer_surface_apply(struct qdwin_layer_surface *ls)
{
	struct qdwin *qdwin = ls->qdwin;
	if (ls->layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)
		return;

	if (!ls->view) {
		ls->view = weston_view_create(ls->surface);
		if (!ls->view) {
			weston_log("qdwin: layer-shell view_create failed "
				   "ns=%s\n",
				   ls->namespace ? ls->namespace : "(null)");
			return;
		}
		weston_view_move_to_layer(ls->view,
			&qdwin->layer_shell_layer[ls->layer].view_list);
	}

	struct weston_coord_global pos = {
		.c = weston_coord(ls->box_x, ls->box_y)
	};
	weston_view_set_position(ls->view, pos);

	if (!ls->mapped) {
		if (!weston_surface_is_mapped(ls->surface))
			weston_surface_map(ls->surface);
		ls->mapped = 1;
		/* Log "mapped" only on the !mapped→mapped transition (i.e.
		 * an actual map event), not on every subsequent same-shape
		 * commit. Otherwise a client that repaints at 60 Hz (clock
		 * widget, animation) floods the journal with one "mapped"
		 * line per frame, drowning real qdwin signal — see
		 * todo/qdshell-bar-remap-storm.md. Reconfigures still log
		 * separately in qdwin_layer_surface_on_commit. */
		weston_log("qdwin: layer-shell mapped ns=%s layer=%u "
			   "at %d,%d %ux%u\n",
			   ls->namespace ? ls->namespace : "(null)",
			   ls->layer, ls->box_x, ls->box_y,
			   ls->box_w, ls->box_h);
		/* Phase 1.3: a newly mapped panel with exclusive_zone > 0
		 * shrinks the work area; reflow any maximised toplevels.
		 * Cheap to call unconditionally — qdwin_panels_on_output_change
		 * is a no-op when nothing is maximised. */
		qdwin_panels_on_output_change(qdwin);
	}

	/* Phase 1.5: if the client requested EXCLUSIVE keyboard
	 * interactivity (e.g. swaylock-style overlays), grant focus to
	 * this layer surface on every apply while still EXCLUSIVE. The
	 * shell's own focus orchestration via set_keyboard_focus_v2 keeps
	 * working for normal toplevels — EXCLUSIVE is a compositor-side
	 * override that mirrors wlroots's behaviour for OVERLAY/TOP locker
	 * surfaces.
	 *
	 * ON_DEMAND (focus-on-click) is wired in plan3 M4 via
	 * qdwin_layer_surface_handle_on_demand_button (called from
	 * qdwin_proxy_default_grab_button). EXCLUSIVE is granted here at
	 * map/apply time; ON_DEMAND is granted at left-button-press time
	 * and ON_DEMAND will not steal focus from a held EXCLUSIVE
	 * (see post-deep-review NEW-H2 guard). NONE leaves focus alone. */
	if (ls->current.kbd_interactivity ==
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
		struct weston_seat *seat;
		wl_list_for_each(seat, &qdwin->compositor->seat_list, link) {
			if (weston_seat_get_keyboard(seat))
				weston_seat_set_keyboard_focus(seat, ls->surface);
		}
	}
}

static void
qdwin_layer_surface_on_commit(struct wl_listener *l, void *data)
{
	struct qdwin_layer_surface *ls =
		wl_container_of(l, ls, commit_listener);
	(void)data;

	/* Promote pending → current. The state is "double buffered" per
	 * spec: setters store into pending, commit applies to current. */
	ls->current = ls->pending;

	if (!ls->initial_configure_sent) {
		/* Spec: on the initial commit (no buffer attached) the
		 * compositor sends its first configure. */
		ls->initial_commit_seen = 1;
		ls->initial_configure_sent = 1;
		qdwin_layer_surface_send_configure(ls);
		return;
	}

	/* Subsequent commit. Recompute layout (anchor/size/margin may have
	 * changed since the last configure) and either send a new
	 * configure if the rect changed, or accept the current rect and
	 * map the view. */
	int32_t prev_x = ls->box_x, prev_y = ls->box_y;
	uint32_t prev_w = ls->box_w, prev_h = ls->box_h;

	struct weston_output *out = qdwin_layer_surface_resolve_output(ls);
	int32_t fx = 0, fy = 0, fw = 1920, fh = 1080;
	if (out) {
		fx = out->pos.c.x; fy = out->pos.c.y;
		fw = out->width;   fh = out->height;
	}
	qdwin_layer_surface_compute_box(ls, fx, fy, fw, fh);

	if (ls->box_w != prev_w || ls->box_h != prev_h) {
		uint32_t serial = ++ls->qdwin->layer_configure_serial_next;
		ls->last_configure_serial = serial;
		zwlr_layer_surface_v1_send_configure(ls->resource, serial,
						     ls->box_w, ls->box_h);
		weston_log("qdwin: layer-shell reconfigure ns=%s "
			   "box=%d,%d %ux%u serial=%u (was %ux%u)\n",
			   ls->namespace ? ls->namespace : "(null)",
			   ls->box_x, ls->box_y,
			   ls->box_w, ls->box_h, serial, prev_w, prev_h);
		(void)prev_x; (void)prev_y;
		return;
	}

	qdwin_layer_surface_apply(ls);
	/* "mapped" is logged inside qdwin_layer_surface_apply on the
	 * unmapped→mapped transition only — not on every subsequent
	 * same-shape commit. See todo/qdshell-bar-remap-storm.md. */
}

static void
qdwin_layer_surface_on_surface_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_layer_surface *ls =
		wl_container_of(l, ls, surface_destroy_listener);
	(void)data;
	wl_list_remove(&ls->surface_destroy_listener.link);
	wl_list_init(&ls->surface_destroy_listener.link);
	wl_list_remove(&ls->commit_listener.link);
	wl_list_init(&ls->commit_listener.link);
	int was_reserving = ls->mapped &&
			    ls->current.exclusive_zone > 0;
	if (ls->view) {
		if (ls->mapped)
			weston_view_unmap(ls->view);
		weston_view_destroy(ls->view);
		ls->view = NULL;
		ls->mapped = 0;
	}
	struct qdwin_layer_popup *lp, *tmp;
	wl_list_for_each_safe(lp, tmp, &ls->popups, link)
		qdwin_layer_popup_destroy(lp);
	ls->surface = NULL;
	if (was_reserving)
		qdwin_panels_on_output_change(ls->qdwin);
	/* Per spec: closed event when surface is forcibly destroyed. */
	if (ls->resource)
		zwlr_layer_surface_v1_send_closed(ls->resource);
}

/* --- per-layer-surface request handlers --- */

static void
qdwin_layer_surface_set_size(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t width, uint32_t height)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	ls->pending.desired_w = width;
	ls->pending.desired_h = height;
}

static void
qdwin_layer_surface_set_anchor(struct wl_client *client,
			       struct wl_resource *resource, uint32_t anchor)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	uint32_t valid = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
		       | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
		       | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
		       | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (anchor & ~valid) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
			"invalid anchor bitfield 0x%x", anchor);
		return;
	}
	ls->pending.anchor = anchor;
}

static void
qdwin_layer_surface_set_exclusive_zone(struct wl_client *client,
				       struct wl_resource *resource,
				       int32_t zone)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	ls->pending.exclusive_zone = zone;
}

static void
qdwin_layer_surface_set_margin(struct wl_client *client,
			       struct wl_resource *resource,
			       int32_t top, int32_t right,
			       int32_t bottom, int32_t left)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	ls->pending.margin.top    = top;
	ls->pending.margin.right  = right;
	ls->pending.margin.bottom = bottom;
	ls->pending.margin.left   = left;
}

static void
qdwin_layer_surface_set_keyboard_interactivity(
	struct wl_client *client, struct wl_resource *resource,
	uint32_t interactivity)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	if (interactivity > ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
			"invalid kbd interactivity %u", interactivity);
		return;
	}
	/* Phase 1.5: NONE (default — waybar's default) leaves qdwin's
	 * shell-driven focus model alone. EXCLUSIVE is wired in
	 * qdwin_layer_surface_apply: every apply while EXCLUSIVE grants
	 * keyboard focus to this surface across all seats with a keyboard
	 * (matches wlroots semantics for swaylock-style OVERLAY surfaces).
	 * ON_DEMAND (focus-on-click) is wired in plan3 M4 via
	 * qdwin_proxy_default_grab_button → qdwin_layer_surface_handle_on_demand_button;
	 * NEW-H2 guards it against stealing focus from an EXCLUSIVE
	 * holder. The pending value is the source of truth for both paths. */
	ls->pending.kbd_interactivity = interactivity;
}

typedef bool (*qdwin_xdg_popup_attach_layer_parent_fn)(
	struct wl_resource *popup_resource,
	struct weston_surface *parent_surface);
typedef bool (*qdwin_xdg_popup_get_geometry_fn)(
	struct wl_resource *popup_resource,
	struct weston_geometry *geometry);

/* plan3 H1: dlsym typedefs for the new layer-popup grab helpers in
 * vendored libweston. Kept as soft references so qdwin still links and
 * runs against an unpatched libweston (the grab call will then
 * gracefully fall back to libweston's INVALID_GRAB error path). */
typedef bool (*qdwin_xdg_popup_layer_grab_handler_t)(
	struct wl_resource *popup_resource,
	struct weston_surface *popup_surface,
	struct weston_surface *layer_parent_surface,
	struct weston_seat *seat,
	uint32_t serial,
	void *user_data);

typedef void (*qdwin_xdg_popup_set_layer_grab_handler_fn)(
	qdwin_xdg_popup_layer_grab_handler_t handler,
	void *user_data);

typedef void (*qdwin_xdg_popup_dismiss_layer_grab_fn)(
	struct wl_resource *popup_resource);

static qdwin_xdg_popup_attach_layer_parent_fn
qdwin_xdg_popup_attach_layer_parent_sym(void)
{
	static qdwin_xdg_popup_attach_layer_parent_fn fn;
	static int looked_up;
	if (!looked_up) {
		fn = (qdwin_xdg_popup_attach_layer_parent_fn)dlsym(
			RTLD_DEFAULT,
			"weston_desktop_xdg_popup_attach_layer_parent");
		looked_up = 1;
	}
	return fn;
}

static qdwin_xdg_popup_get_geometry_fn
qdwin_xdg_popup_get_geometry_sym(void)
{
	static qdwin_xdg_popup_get_geometry_fn fn;
	static int looked_up;
	if (!looked_up) {
		fn = (qdwin_xdg_popup_get_geometry_fn)dlsym(
			RTLD_DEFAULT,
			"weston_desktop_xdg_popup_get_geometry");
		looked_up = 1;
	}
	return fn;
}

static qdwin_xdg_popup_set_layer_grab_handler_fn
qdwin_xdg_popup_set_layer_grab_handler_sym(void)
{
	static qdwin_xdg_popup_set_layer_grab_handler_fn fn;
	static int looked_up;
	if (!looked_up) {
		fn = (qdwin_xdg_popup_set_layer_grab_handler_fn)dlsym(
			RTLD_DEFAULT,
			"weston_desktop_xdg_popup_set_layer_grab_handler");
		looked_up = 1;
	}
	return fn;
}

static qdwin_xdg_popup_dismiss_layer_grab_fn
qdwin_xdg_popup_dismiss_layer_grab_sym(void)
{
	static qdwin_xdg_popup_dismiss_layer_grab_fn fn;
	static int looked_up;
	if (!looked_up) {
		fn = (qdwin_xdg_popup_dismiss_layer_grab_fn)dlsym(
			RTLD_DEFAULT,
			"weston_desktop_xdg_popup_dismiss_layer_grab");
		looked_up = 1;
	}
	return fn;
}

static void
qdwin_layer_popup_update_position(struct qdwin_layer_popup *lp)
{
	if (!lp || !lp->parent || !lp->view)
		return;

	struct weston_geometry g = { 0 };
	qdwin_xdg_popup_get_geometry_fn get_geometry =
		qdwin_xdg_popup_get_geometry_sym();
	if (!get_geometry || !get_geometry(lp->popup_resource, &g))
		return;

	struct weston_coord_global pos = {
		.c = weston_coord(lp->parent->box_x + g.x,
				  lp->parent->box_y + g.y)
	};
	weston_view_set_position(lp->view, pos);
	weston_view_update_transform(lp->view);
}

static void
qdwin_layer_popup_destroy(struct qdwin_layer_popup *lp)
{
	if (!lp)
		return;

	/* plan3 NEW-H1 + deep-review H3/H5: end any active pointer grab so
	 * libweston rebinds the default grab before we free the
	 * qdwin_layer_popup. Subtleties:
	 *
	 * - weston_pointer_end_grab re-enters our cancel op which calls
	 *   dismiss_layer_grab(lp->popup_resource). To avoid sending
	 *   xdg_popup.popup_done on the resource we are tearing down, we
	 *   null popup_resource and clear grab_active BEFORE end_grab.
	 *
	 * - lp->grab.pointer is the pointer cached at start time. We must
	 *   not dereference it if its owning seat was destroyed since (the
	 *   seat_destroy_listener nulls lp->grab_seat for that case; we
	 *   skip end_grab and just release our state). */
	if (lp->grab_active && lp->grab_seat && lp->grab.pointer &&
	    lp->grab.pointer->grab == &lp->grab) {
		struct weston_pointer *p = lp->grab.pointer;
		lp->popup_resource = NULL;        /* suppresses cancel→dismiss */
		lp->grab_active = 0;
		weston_pointer_end_grab(p);
	} else {
		lp->grab_active = 0;
	}
	if (lp->seat_destroy_listener.link.next)
		wl_list_remove(&lp->seat_destroy_listener.link);
	lp->grab_seat = NULL;

	wl_list_remove(&lp->link);
	if (lp->surface_commit_listener.link.next)
		wl_list_remove(&lp->surface_commit_listener.link);
	if (lp->surface_destroy_listener.link.next)
		wl_list_remove(&lp->surface_destroy_listener.link);
	if (lp->popup_resource_destroy_listener.link.next)
		wl_list_remove(&lp->popup_resource_destroy_listener.link);

	if (lp->view) {
		weston_desktop_surface_unlink_view(lp->view);
		weston_view_destroy(lp->view);
	}
	free(lp);
}

/* plan3 H2 (deep-review): xdg_popup resource was destroyed — qdwin must
 * release its qdwin_layer_popup before any further commit/dismiss path
 * dereferences stale libweston data. lp->popup_resource is nulled first
 * to suppress any outgoing dismiss event aimed at the dying resource. */
static void
qdwin_layer_popup_on_popup_resource_destroyed(struct wl_listener *listener,
					      void *data)
{
	struct qdwin_layer_popup *lp =
		wl_container_of(listener, lp, popup_resource_destroy_listener);
	(void)data;
	lp->popup_resource = NULL;
	qdwin_layer_popup_destroy(lp);
}

/* plan3 H3 (deep-review): the grab pointer's owning seat was destroyed.
 * Null the cached lp->grab_seat so qdwin_layer_popup_destroy skips its
 * weston_pointer_end_grab call. The pointer struct itself may be freed
 * already — we never deref it from here.
 *
 * deep-review-2 DPF2-H1: also unlink the listener from the (now
 * destroyed) seat's destroy_signal list and re-init the link node so
 * later cleanup paths (qdwin_layer_popup_destroy, or a follow-up grab
 * on a different seat) can safely call wl_list_remove on a quiescent
 * listener. libwayland's destroy-signal dispatch does not detach
 * listeners after firing; the listener.link prev/next still reference
 * the dying signal's storage until we unlink. */
static void
qdwin_layer_popup_on_grab_seat_destroyed(struct wl_listener *listener,
					 void *data)
{
	struct qdwin_layer_popup *lp =
		wl_container_of(listener, lp, seat_destroy_listener);
	(void)data;
	weston_log("qdwin: layer-popup grab seat destroyed; clearing cached "
		   "pointer\n");
	wl_list_remove(&lp->seat_destroy_listener.link);
	wl_list_init(&lp->seat_destroy_listener.link);
	lp->grab_seat = NULL;
	lp->grab_active = 0;
	/* lp->grab.pointer is now stale; do not deref. weston_pointer was
	 * freed alongside the seat. */
	lp->grab.pointer = NULL;
}

/* plan3 H1: layer-popup grab interface. xdg_popup.grab on a layer-
 * parented popup cannot use libweston's weston_desktop_seat_popup_grab
 * machinery (it requires desktop_surface parents). We install our own
 * weston_pointer grab that dismisses the popup on outside-click via
 * weston_desktop_xdg_popup_dismiss_layer_grab (which sends
 * xdg_popup.popup_done). Inside-popup events go through normal pointer
 * delivery. */

static int
qdwin_layer_popup_bbox_contains(struct qdwin_layer_popup *lp,
				struct weston_coord_global pos)
{
	if (!lp || !lp->view || !lp->surface ||
	    lp->surface->width <= 0 || lp->surface->height <= 0)
		return 0;
	struct weston_coord_global vp =
		weston_view_get_pos_offset_global(lp->view);
	return pos.c.x >= vp.c.x && pos.c.x < vp.c.x + lp->surface->width &&
	       pos.c.y >= vp.c.y && pos.c.y < vp.c.y + lp->surface->height;
}

/* deep-review-2 H2: shared client-gated focus filter. Returns the
 * grab's popup wl_client, or NULL when the popup_resource is gone
 * (cancelled / outside-click-dismissed). */
static struct wl_client *
qdwin_layer_popup_grab_client(struct qdwin_layer_popup *lp)
{
	if (!lp || !lp->popup_resource)
		return NULL;
	return wl_resource_get_client(lp->popup_resource);
}

/* deep-review-2 H2: repick the topmost view at the pointer and clamp
 * pointer->focus to "popup or same-client view"; clear focus
 * otherwise. Called by every grab event hook before sending events so
 * stale pre-grab focus cannot receive button/axis/frame.
 *
 * Side effect: pointer->focus reflects the filter result on return.
 * Callers can compare against NULL to decide whether to forward the
 * current event to a downstream client. */
static void
qdwin_layer_popup_grab_refilter_focus(struct qdwin_layer_popup *lp,
				      struct weston_pointer *pointer)
{
	if (!pointer)
		return;
	struct weston_view *view = weston_compositor_pick_view(
		pointer->seat->compositor, pointer->pos);
	struct wl_client *grab_client = qdwin_layer_popup_grab_client(lp);
	int deliver = 0;
	if (view && view->surface) {
		if (view->surface == lp->surface) {
			deliver = 1;
		} else if (view->surface->resource && grab_client &&
			   wl_resource_get_client(view->surface->resource) ==
			       grab_client) {
			deliver = 1;
		}
	}
	if (deliver) {
		if (view != pointer->focus)
			weston_pointer_set_focus(pointer, view);
	} else if (pointer->focus) {
		/* libweston accepts NULL view → clears focus + sends leave. */
		weston_pointer_set_focus(pointer, NULL);
	}
}

/* deep-review-2 H2: weston_pointer_start_grab invokes the grab's .focus
 * op synchronously. Without filtering here, the pre-grab focus survives
 * until the first motion event, so a button/axis/frame received in
 * between would be delivered to the prior focus client. */
static void
qdwin_layer_popup_grab_focus(struct weston_pointer_grab *grab)
{
	struct qdwin_layer_popup *lp = wl_container_of(grab, lp, grab);
	qdwin_layer_popup_grab_refilter_focus(lp, grab->pointer);
}

static void
qdwin_layer_popup_grab_motion(struct weston_pointer_grab *grab,
			      const struct timespec *time,
			      struct weston_pointer_motion_event *event)
{
	struct qdwin_layer_popup *lp = wl_container_of(grab, lp, grab);
	struct weston_pointer *pointer = grab->pointer;
	weston_pointer_move(pointer, event);
	qdwin_layer_popup_grab_refilter_focus(lp, pointer);
	if (pointer->focus)
		weston_pointer_send_motion(pointer, time, event);
}

static void
qdwin_layer_popup_grab_button(struct weston_pointer_grab *grab,
			      const struct timespec *time,
			      uint32_t button, uint32_t state)
{
	struct qdwin_layer_popup *lp = wl_container_of(grab, lp, grab);
	struct weston_pointer *pointer = grab->pointer;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
	    !qdwin_layer_popup_bbox_contains(lp, pointer->pos)) {
		weston_log("qdwin: layer-popup dismissed by outside click "
			   "at (%.0f,%.0f)\n",
			   pointer->pos.c.x, pointer->pos.c.y);
		/* plan3 H5 (deep-review): dismiss exactly once. The cancel op
		 * we install via weston_pointer_end_grab also calls
		 * dismiss_layer_grab on lp->popup_resource — without ownership
		 * of the dismiss event, popup_done would fire twice. Send the
		 * dismiss ourselves first, then null lp->popup_resource so the
		 * cancel-driven dismiss is a no-op. */
		struct wl_resource *r = lp->popup_resource;
		lp->popup_resource = NULL;
		if (r) {
			qdwin_xdg_popup_dismiss_layer_grab_fn dismiss =
				qdwin_xdg_popup_dismiss_layer_grab_sym();
			if (dismiss)
				dismiss(r);
		}
		if (lp->grab_active) {
			lp->grab_active = 0;
			weston_pointer_end_grab(pointer);
		}
		return;
	}
	/* deep-review-2 H2: clamp focus before delivering the button so a
	 * press that arrived right after start_grab (no motion yet) cannot
	 * use stale focus. send_button is a no-op when focus is NULL. */
	qdwin_layer_popup_grab_refilter_focus(lp, pointer);
	if (pointer->focus)
		weston_pointer_send_button(pointer, time, button, state);
}

static void
qdwin_layer_popup_grab_axis(struct weston_pointer_grab *grab,
			    const struct timespec *time,
			    struct weston_pointer_axis_event *event)
{
	struct qdwin_layer_popup *lp = wl_container_of(grab, lp, grab);
	struct weston_pointer *pointer = grab->pointer;
	qdwin_layer_popup_grab_refilter_focus(lp, pointer);
	if (pointer->focus)
		weston_pointer_send_axis(pointer, time, event);
}

static void
qdwin_layer_popup_grab_axis_source(struct weston_pointer_grab *grab,
				   uint32_t source)
{
	struct qdwin_layer_popup *lp = wl_container_of(grab, lp, grab);
	struct weston_pointer *pointer = grab->pointer;
	qdwin_layer_popup_grab_refilter_focus(lp, pointer);
	if (pointer->focus)
		weston_pointer_send_axis_source(pointer, source);
}

static void
qdwin_layer_popup_grab_frame(struct weston_pointer_grab *grab)
{
	struct qdwin_layer_popup *lp = wl_container_of(grab, lp, grab);
	struct weston_pointer *pointer = grab->pointer;
	qdwin_layer_popup_grab_refilter_focus(lp, pointer);
	if (pointer->focus)
		weston_pointer_send_frame(pointer);
}

static void
qdwin_layer_popup_grab_cancel(struct weston_pointer_grab *grab)
{
	struct qdwin_layer_popup *lp = wl_container_of(grab, lp, grab);
	lp->grab_active = 0;
	if (lp->popup_resource) {
		qdwin_xdg_popup_dismiss_layer_grab_fn dismiss =
			qdwin_xdg_popup_dismiss_layer_grab_sym();
		if (dismiss)
			dismiss(lp->popup_resource);
	}
}

static const struct weston_pointer_grab_interface
qdwin_layer_popup_grab_iface = {
	.focus       = qdwin_layer_popup_grab_focus,
	.motion      = qdwin_layer_popup_grab_motion,
	.button      = qdwin_layer_popup_grab_button,
	.axis        = qdwin_layer_popup_grab_axis,
	.axis_source = qdwin_layer_popup_grab_axis_source,
	.frame       = qdwin_layer_popup_grab_frame,
	.cancel      = qdwin_layer_popup_grab_cancel,
};

/* Forward declaration: defined later to find the qdwin_layer_popup that
 * owns a given xdg_popup resource. */
static struct qdwin_layer_popup *
qdwin_layer_popup_for_resource(struct qdwin *qdwin,
			       struct wl_resource *popup_resource);

/* plan3 H1: layer-popup grab handler called from vendored libweston's
 * weston_desktop_xdg_popup_protocol_grab when the popup is layer-
 * parented and qdwin has registered this handler. Returning false makes
 * libweston post XDG_POPUP_ERROR_INVALID_GRAB to the client. */
static bool
qdwin_layer_popup_layer_grab_handler(struct wl_resource *popup_resource,
				     struct weston_surface *popup_surface,
				     struct weston_surface *layer_parent_surface,
				     struct weston_seat *wseat,
				     uint32_t serial,
				     void *user_data)
{
	struct qdwin *qdwin = user_data;
	(void)popup_surface;
	(void)layer_parent_surface;
	if (!qdwin || !wseat || !popup_resource)
		return false;

	/* Validate that one of the input devices on this seat has a grab
	 * serial matching the request. Mirrors libweston's serial check
	 * in weston_desktop_seat_popup_grab_start. */
	struct weston_pointer *pointer = weston_seat_get_pointer(wseat);
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(wseat);
	struct weston_touch *touch = weston_seat_get_touch(wseat);
	int serial_ok =
		(pointer && pointer->grab_serial == serial) ||
		(keyboard && keyboard->grab_serial == serial) ||
		(touch && touch->grab_serial == serial);
	if (!serial_ok) {
		weston_log("qdwin: layer-popup grab refused: stale serial=%u "
			   "(p=%u k=%u t=%u)\n", serial,
			   pointer ? pointer->grab_serial : 0,
			   keyboard ? keyboard->grab_serial : 0,
			   touch ? touch->grab_serial : 0);
		return false;
	}

	struct qdwin_layer_popup *lp =
		qdwin_layer_popup_for_resource(qdwin, popup_resource);
	if (!lp) {
		weston_log("qdwin: layer-popup grab refused: popup not "
			   "tracked\n");
		return false;
	}

	if (!pointer) {
		weston_log("qdwin: layer-popup grab refused: no pointer on "
			   "seat\n");
		return false;
	}
	if (lp->grab_active) {
		weston_log("qdwin: layer-popup grab already active for "
			   "popup=%p\n", (void *)popup_resource);
		return true;
	}
	lp->grab.interface = &qdwin_layer_popup_grab_iface;
	weston_pointer_start_grab(pointer, &lp->grab);
	lp->grab_active = 1;
	/* plan3 H3 (deep-review): subscribe to the seat's destroy signal so
	 * that if the seat goes away mid-grab, the cached lp->grab.pointer
	 * is recognised as stale and qdwin_layer_popup_destroy skips its
	 * end_grab. The listener.link was wl_list_init'd at get_popup time;
	 * re-init defensively in case a prior grab on a different seat left
	 * it attached, then add to the new seat's signal. */
	if (lp->seat_destroy_listener.link.next)
		wl_list_remove(&lp->seat_destroy_listener.link);
	wl_signal_add(&wseat->destroy_signal, &lp->seat_destroy_listener);
	lp->grab_seat = wseat;
	weston_log("qdwin: layer-popup grab started popup=%p seat=%s\n",
		   (void *)popup_resource,
		   wseat->seat_name ? wseat->seat_name : "");
	return true;
}

static void
qdwin_layer_popup_on_commit(struct wl_listener *listener, void *data)
{
	struct qdwin_layer_popup *lp =
		wl_container_of(listener, lp, surface_commit_listener);
	(void)data;
	qdwin_layer_popup_update_position(lp);
}

static void
qdwin_layer_popup_on_surface_destroyed(struct wl_listener *listener,
				       void *data)
{
	struct qdwin_layer_popup *lp =
		wl_container_of(listener, lp, surface_destroy_listener);
	(void)data;
	qdwin_layer_popup_destroy(lp);
}

static void
qdwin_layer_surface_get_popup(struct wl_client *client,
			      struct wl_resource *resource,
			      struct wl_resource *popup)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;

	qdwin_xdg_popup_attach_layer_parent_fn attach_layer_parent =
		qdwin_xdg_popup_attach_layer_parent_sym();
	if (!attach_layer_parent || !attach_layer_parent(popup, ls->surface)) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
			"get_popup requires patched libweston xdg_popup support");
		return;
	}

	struct weston_desktop_surface *dsurf = wl_resource_get_user_data(popup);
	struct qdwin_layer_popup *lp = calloc(1, sizeof *lp);
	if (!lp) {
		wl_client_post_no_memory(client);
		return;
	}

	lp->parent = ls;
	lp->popup_resource = popup;
	lp->desktop_surface = dsurf;
	lp->surface = weston_desktop_surface_get_surface(dsurf);
	lp->view = weston_desktop_surface_create_view(dsurf);
	if (!lp->view) {
		free(lp);
		wl_client_post_no_memory(client);
		return;
	}

	weston_view_move_to_layer(lp->view,
		&ls->qdwin->layer_shell_layer[ls->layer].view_list);
	qdwin_layer_popup_update_position(lp);

	lp->surface_commit_listener.notify = qdwin_layer_popup_on_commit;
	wl_signal_add(&lp->surface->commit_signal, &lp->surface_commit_listener);
	lp->surface_destroy_listener.notify =
		qdwin_layer_popup_on_surface_destroyed;
	wl_signal_add(&lp->surface->destroy_signal, &lp->surface_destroy_listener);
	/* plan3 H2: also subscribe to xdg_popup resource destruction so a
	 * client that drops the popup role (without destroying the
	 * wl_surface) does not leave a dangling qdwin_layer_popup. */
	lp->popup_resource_destroy_listener.notify =
		qdwin_layer_popup_on_popup_resource_destroyed;
	wl_resource_add_destroy_listener(popup,
					 &lp->popup_resource_destroy_listener);
	wl_list_init(&lp->seat_destroy_listener.link);
	lp->seat_destroy_listener.notify =
		qdwin_layer_popup_on_grab_seat_destroyed;
	wl_list_insert(&ls->popups, &lp->link);

	weston_log("qdwin: layer-shell get_popup ns=%s popup=%p attached\n",
		   ls->namespace ? ls->namespace : "(null)", (void *)popup);
}

/* plan3 H1: walk every tracked layer-surface and its popups for the one
 * owning a given xdg_popup resource. Layer popups are rare and short-
 * lived; a linear scan is cheaper than threading a back-pointer through
 * the libweston-side struct. */
static struct qdwin_layer_popup *
qdwin_layer_popup_for_resource(struct qdwin *qdwin,
			       struct wl_resource *popup_resource)
{
	if (!qdwin || !popup_resource)
		return NULL;
	struct qdwin_layer_surface *ls;
	wl_list_for_each(ls, &qdwin->layer_surfaces, link) {
		struct qdwin_layer_popup *lp;
		wl_list_for_each(lp, &ls->popups, link) {
			if (lp->popup_resource == popup_resource)
				return lp;
		}
	}
	return NULL;
}

static void
qdwin_layer_surface_ack_configure(struct wl_client *client,
				  struct wl_resource *resource, uint32_t serial)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	ls->last_acked_serial = serial;
}

static void
qdwin_layer_surface_destroy_req(struct wl_client *client,
				struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_layer_surface_set_layer(struct wl_client *client,
			      struct wl_resource *resource, uint32_t layer)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
			"invalid layer %u", layer);
		return;
	}
	uint32_t old = ls->layer;
	ls->layer = layer;
	/* If the view exists, move it onto the new layer's view_list.
	 * weston_view_move_to_layer handles removal from the old layer. */
	if (ls->view && old != layer) {
		weston_view_move_to_layer(ls->view,
			&ls->qdwin->layer_shell_layer[layer].view_list);
	}
}

static void
qdwin_layer_surface_set_exclusive_edge(struct wl_client *client,
				       struct wl_resource *resource,
				       uint32_t edge)
{
	(void)client;
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	uint32_t valid = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
		       | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
		       | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
		       | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (edge != 0 && (edge & ~valid)) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
			"invalid exclusive_edge 0x%x", edge);
		return;
	}
	ls->pending.exclusive_edge = edge;
}

static const struct zwlr_layer_surface_v1_interface
qdwin_layer_surface_impl = {
	.set_size                    = qdwin_layer_surface_set_size,
	.set_anchor                  = qdwin_layer_surface_set_anchor,
	.set_exclusive_zone          = qdwin_layer_surface_set_exclusive_zone,
	.set_margin                  = qdwin_layer_surface_set_margin,
	.set_keyboard_interactivity  = qdwin_layer_surface_set_keyboard_interactivity,
	.get_popup                   = qdwin_layer_surface_get_popup,
	.ack_configure               = qdwin_layer_surface_ack_configure,
	.destroy                     = qdwin_layer_surface_destroy_req,
	.set_layer                   = qdwin_layer_surface_set_layer,
	.set_exclusive_edge          = qdwin_layer_surface_set_exclusive_edge,
};

static void
qdwin_layer_surface_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_layer_surface *ls = wl_resource_get_user_data(resource);
	if (!ls) return;
	struct qdwin *qdwin = ls->qdwin;
	int was_reserving = ls->mapped &&
			    ls->current.exclusive_zone > 0;
	if (ls->surface) {
		wl_list_remove(&ls->commit_listener.link);
		wl_list_remove(&ls->surface_destroy_listener.link);
	}
	if (ls->view) {
		if (ls->mapped)
			weston_view_unmap(ls->view);
		weston_view_destroy(ls->view);
		ls->view = NULL;
	}
	struct qdwin_layer_popup *lp, *tmp;
	wl_list_for_each_safe(lp, tmp, &ls->popups, link)
		qdwin_layer_popup_destroy(lp);
	wl_list_remove(&ls->link);
	free(ls->namespace);
	free(ls);
	if (was_reserving)
		qdwin_panels_on_output_change(qdwin);
}

/* --- manager (zwlr_layer_shell_v1) --- */

static void
qdwin_layer_shell_get_layer_surface(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id,
				    struct wl_resource *surface_resource,
				    struct wl_resource *output_resource,
				    uint32_t layer,
				    const char *namespace_str)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);

	if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
			"invalid layer %u", layer);
		return;
	}
	if (weston_surface_set_role(surface, QDWIN_LAYER_SURFACE_ROLE,
				    resource,
				    ZWLR_LAYER_SHELL_V1_ERROR_ROLE) < 0) {
		/* set_role posts the protocol error itself. */
		return;
	}

	struct qdwin_layer_surface *ls = calloc(1, sizeof *ls);
	if (!ls) {
		wl_client_post_no_memory(client);
		return;
	}
	ls->qdwin     = qdwin;
	ls->surface   = surface;
	ls->layer     = layer;
	ls->namespace = namespace_str ? strdup(namespace_str) : NULL;
	ls->pending.exclusive_zone = 0;
	ls->pending.kbd_interactivity =
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	wl_list_init(&ls->popups);

	if (output_resource) {
		struct weston_head *head =
			weston_head_from_resource(output_resource);
		ls->output = head ? head->output : NULL;
	}

	ls->resource = wl_resource_create(client,
		&zwlr_layer_surface_v1_interface,
		wl_resource_get_version(resource), id);
	if (!ls->resource) {
		free(ls->namespace);
		free(ls);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(ls->resource,
		&qdwin_layer_surface_impl, ls,
		qdwin_layer_surface_resource_destroy);

	ls->commit_listener.notify = qdwin_layer_surface_on_commit;
	wl_signal_add(&surface->commit_signal, &ls->commit_listener);
	ls->surface_destroy_listener.notify =
		qdwin_layer_surface_on_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &ls->surface_destroy_listener);

	wl_list_insert(&qdwin->layer_surfaces, &ls->link);
	weston_log("qdwin: layer-shell get_layer_surface ns=%s layer=%u "
		   "output=%p\n",
		   ls->namespace ? ls->namespace : "(null)",
		   layer, (void *)ls->output);
}

static void
qdwin_layer_shell_destroy(struct wl_client *client,
			  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwlr_layer_shell_v1_interface qdwin_layer_shell_impl = {
	.get_layer_surface = qdwin_layer_shell_get_layer_surface,
	.destroy           = qdwin_layer_shell_destroy,
};

static void
bind_qdwin_layer_shell(struct wl_client *client, void *data,
		       uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource = wl_resource_create(
		client, &zwlr_layer_shell_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &qdwin_layer_shell_impl,
				       qdwin, NULL);
}

/* --- ext-idle-notify-v1 -------------------------------------------- */

static void
qdwin_idle_notification_destroy(struct wl_client *client,
				struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_idle_notification_v1_interface
qdwin_idle_notification_impl = {
	.destroy = qdwin_idle_notification_destroy,
};

static void
qdwin_idle_notification_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_idle_notification *n = wl_resource_get_user_data(resource);
	if (!n)
		return;
	if (n->timer)
		wl_event_source_remove(n->timer);
	wl_list_remove(&n->link);
	free(n);
}

static int
qdwin_idle_notification_timer_fire(void *data)
{
	struct qdwin_idle_notification *n = data;
	if (n->is_idle)
		return 0;
	if (!n->ignore_inhibit && n->qdwin->compositor->idle_inhibit > 0)
		return 0;
	n->is_idle = 1;
	ext_idle_notification_v1_send_idled(n->resource);
	return 0;
}

static struct qdwin_idle_notification *
qdwin_idle_notification_create(struct qdwin *qdwin,
			       struct wl_client *client,
			       uint32_t id,
			       uint32_t timeout_ms,
			       int ignore_inhibit,
			       uint32_t version)
{
	struct qdwin_idle_notification *n = calloc(1, sizeof *n);
	struct wl_resource *resource;
	if (!n) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	resource = wl_resource_create(
		client, &ext_idle_notification_v1_interface, version, id);
	if (!resource) {
		free(n);
		wl_client_post_no_memory(client);
		return NULL;
	}
	n->qdwin = qdwin;
	n->resource = resource;
	n->timeout_ms = timeout_ms;
	n->ignore_inhibit = ignore_inhibit;
	n->timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(qdwin->compositor->wl_display),
		qdwin_idle_notification_timer_fire, n);
	wl_list_insert(&qdwin->idle_notifications, &n->link);
	wl_resource_set_implementation(resource,
				       &qdwin_idle_notification_impl,
				       n,
				       qdwin_idle_notification_resource_destroy);
	/* §6.7(a) follow-up: in internal-idle mode, arm the timer at
	 * creation for the full timeout. wake_signal will rearm it as
	 * activity arrives; timer fire sends `idled`. Outside internal
	 * mode, the timer stays disarmed until idle_signal fires. */
	if (qdwin->idle_internal_mode && n->timer && n->timeout_ms > 0)
		wl_event_source_timer_update(n->timer, (int)n->timeout_ms);
	return n;
}

static void
qdwin_idle_notifier_destroy(struct wl_client *client,
			    struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_idle_notifier_get_idle_notification(struct wl_client *client,
					  struct wl_resource *resource,
					  uint32_t id,
					  uint32_t timeout,
					  struct wl_resource *seat)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)seat;
	qdwin_idle_notification_create(qdwin, client, id, timeout,
				       0 /* honour inhibit */,
				       wl_resource_get_version(resource));
}

static void
qdwin_idle_notifier_get_input_idle_notification(struct wl_client *client,
						struct wl_resource *resource,
						uint32_t id,
						uint32_t timeout,
						struct wl_resource *seat)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)seat;
	qdwin_idle_notification_create(qdwin, client, id, timeout,
				       1 /* ignore inhibit */,
				       wl_resource_get_version(resource));
}

static const struct ext_idle_notifier_v1_interface
qdwin_idle_notifier_impl = {
	.destroy                       = qdwin_idle_notifier_destroy,
	.get_idle_notification         = qdwin_idle_notifier_get_idle_notification,
	.get_input_idle_notification   = qdwin_idle_notifier_get_input_idle_notification,
};

static void
bind_qdwin_idle_notifier(struct wl_client *client, void *data,
			 uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource = wl_resource_create(
		client, &ext_idle_notifier_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &qdwin_idle_notifier_impl,
				       qdwin, NULL);
}

static void
qdwin_on_idle_signal(struct wl_listener *listener, void *data)
{
	struct qdwin *qdwin =
		wl_container_of(listener, qdwin, idle_signal_listener);
	struct qdwin_idle_notification *n;
	uint32_t weston_idle_ms;
	(void)data;
	/* weston idle_time is configured in seconds; 0 disables the
	 * built-in idle timer, in which case idle_signal won't fire at
	 * all and this handler is a no-op. */
	weston_idle_ms = (uint32_t)qdwin->compositor->idle_time * 1000u;
	wl_list_for_each(n, &qdwin->idle_notifications, link) {
		if (n->is_idle)
			continue;
		if (!n->ignore_inhibit && qdwin->compositor->idle_inhibit > 0)
			continue;
		if (n->timeout_ms <= weston_idle_ms) {
			/* Client asked for ≤ weston idle — fire now. */
			n->is_idle = 1;
			ext_idle_notification_v1_send_idled(n->resource);
		} else if (n->timer) {
			/* Client asked for longer than weston idle — arm a
			 * secondary timer so we fire at approx the right
			 * offset. Timer is disarmed on wake_signal below. */
			wl_event_source_timer_update(
				n->timer,
				(int)(n->timeout_ms - weston_idle_ms));
		}
	}
}

static void
qdwin_on_wake_signal(struct wl_listener *listener, void *data)
{
	struct qdwin *qdwin =
		wl_container_of(listener, qdwin, wake_signal_listener);
	struct qdwin_idle_notification *n;
	(void)data;
	wl_list_for_each(n, &qdwin->idle_notifications, link) {
		/* §6.7(a) follow-up: in internal-idle mode, rearm the timer
		 * to the full timeout so the notification fires exactly
		 * timeout_ms after this activity. Outside internal mode,
		 * disarm — idle_signal will rearm as needed. */
		if (n->timer) {
			if (qdwin->idle_internal_mode && n->timeout_ms > 0)
				wl_event_source_timer_update(
					n->timer, (int)n->timeout_ms);
			else
				wl_event_source_timer_update(n->timer, 0);
		}
		if (!n->is_idle)
			continue;
		n->is_idle = 0;
		ext_idle_notification_v1_send_resumed(n->resource);
	}
}

/* ------------------------------------------------------------------
 * §6.7 cursor-shape-v1.
 *
 * §6.7(b) lands libXcursor theme loading + per-shape image cache +
 * validation + informative logging. When a client calls set_shape,
 * the compositor looks up the preloaded XcursorImages, logs the
 * selected image dimensions + hotspot, and validates that a theme
 * image was actually available.
 *
 * What is still deferred: installing the loaded image as the
 * pointer's sprite (weston_pointer::sprite). Investigation notes
 * from §6.7(b) follow-up (2026-04-24):
 *
 *   - weston_buffer_create_solid_rgba() is the only public API that
 *     creates a server-side weston_buffer_reference; it takes a
 *     solid color, not an arbitrary pixel array. Good for a
 *     proof-of-plumbing but not for theme sprites.
 *   - weston_buffer_from_resource(ec, wl_resource) requires a valid
 *     wl_buffer resource, which only exists if a client has uploaded
 *     one. That's the crux: to feed the compositor pixel data, some
 *     wl_client has to hold a wl_shm_pool, create wl_buffer, and
 *     call wl_surface.attach + commit on a surface it owns. That
 *     client doesn't exist today.
 *   - The canonical fix is an internal client: wl_client_create(
 *     display, sv[0]) where sv[] is a socketpair, wait for it to
 *     roundtrip via a worker thread, then have the client side
 *     (running the wayland-client library in a thread) bind wl_shm,
 *     create pool from an anonymous shm fd pre-filled with the
 *     XcursorImage pixels, upload wl_buffer, commit, send a tiny
 *     "cursor ready" sentinel, which the server side picks up and
 *     assigns to pointer->sprite.
 *   - The per-pointer serial tracking in set_shape(serial, shape)
 *     also requires knowing which seat the cursor-shape device is
 *     bound to — today we ignore pointer_resource in get_pointer,
 *     which would need to change to resolve the weston_pointer.
 *
 * This is ~1 week of bounded work (socketpair plumbing, thread
 * lifecycle, shm buffer management, pointer resolution). It is not
 * on the §6.6 critical path — qdshell can set_cursor from its own
 * wl_surfaces directly via standard wl_pointer.set_cursor. Filed as
 * a separate follow-up; for now, set_shape logs the validated image
 * dimensions + hotspot so the data flow is observable.
 *
 * §6.6 follow-up research finding (2026-04-25):
 *
 *   Surveyed libweston-14 public headers for a server-side set-sprite
 *   API. weston_surface_create(compositor) exists, but does NOT
 *   create a weston_buffer — weston_buffer_create_solid_rgba only
 *   produces solid colors, and weston_buffer_from_resource requires
 *   an uploaded wl_resource. Neither supports raw pixel upload from
 *   server-owned XcursorImage data.
 *
 *   Pre-condition for the full impl:
 *     (a) libweston patch adding weston_buffer_create_from_shm(pixels,
 *         stride, format, w, h) OR
 *     (b) internal wl_client (socketpair + wayland-client worker thread
 *         binding wl_shm + wl_compositor, doing the upload, and
 *         forwarding the resulting wl_buffer resource back over a
 *         control socket), OR
 *     (c) weston_pointer::sprite is a private field inside
 *         libweston.c — there is no public setter. Even if we could
 *         create the buffer, we'd need to patch libweston to expose
 *         weston_pointer_set_sprite(pointer, view, hotspot_x, hotspot_y).
 *
 *   Conclusion: full sprite install requires either an upstream
 *   libweston patch or the internal-client plumbing. qdshell's
 *   decoration chrome already paints via internal wl_client threads
 *   (qdshell → qdwin over /run/user/1000/wayland-1), so (b) is
 *   precedented but lives on the client side, not inside qdwin. A
 *   libweston patch ((a)+(c)) is cleaner; scoped as post-Phase-6.
 *
 * Mapping of cursor-shape-v1 enum → Xcursor name follows CSS naming
 * (the protocol's enum names ARE the CSS cursor names); modern
 * themes (Adwaita, DMZ, etc.) expose them directly. XcursorLibrary-
 * LoadImages falls through the theme's inheritance chain so legacy
 * ICCCM-style names (xterm, fleur, etc.) are reachable by alias
 * without us mapping them explicitly.
 * ------------------------------------------------------------------ */

static const char *
qdwin_cursor_shape_name(uint32_t shape)
{
	switch (shape) {
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT:      return "default";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU: return "context-menu";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP:         return "help";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER:      return "pointer";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS:     return "progress";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT:         return "wait";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL:         return "cell";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR:    return "crosshair";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT:         return "text";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT:return "vertical-text";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS:        return "alias";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY:         return "copy";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE:         return "move";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP:      return "no-drop";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED:  return "not-allowed";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB:         return "grab";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING:     return "grabbing";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE:     return "e-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE:     return "n-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE:    return "ne-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE:    return "nw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE:     return "s-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE:    return "se-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE:    return "sw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE:     return "w-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE:    return "ew-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE:    return "ns-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE:  return "nesw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE:  return "nwse-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE:   return "col-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE:   return "row-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL:   return "all-scroll";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN:      return "zoom-in";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT:     return "zoom-out";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK:      return "dnd-ask";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE:   return "all-resize";
	default:                                            return "other";
	}
}

/* Load one image from the theme for every known shape, keyed by enum.
 * Missing theme entries leave the slot NULL; set_shape logs a miss. */
static void
qdwin_cursor_theme_load(struct qdwin *qdwin)
{
	const char *env_theme = getenv("XCURSOR_THEME");
	const char *env_size  = getenv("XCURSOR_SIZE");
	int size = env_size ? atoi(env_size) : 0;
	if (size <= 0)
		size = 24;
	qdwin->cursor_size = size;
	qdwin->cursor_theme_name = env_theme ? strdup(env_theme) : NULL;

	unsigned loaded = 0;
	for (uint32_t s = 1; s <= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE; s++) {
		const char *name = qdwin_cursor_shape_name(s);
		if (!name || !strcmp(name, "other"))
			continue;
		qdwin->cursor_images[s] =
			XcursorLibraryLoadImages(name,
						 qdwin->cursor_theme_name,
						 size);
		if (qdwin->cursor_images[s])
			loaded++;
	}
	weston_log("qdwin: cursor-shape theme=%s size=%d loaded=%u/%d\n",
		   qdwin->cursor_theme_name ? qdwin->cursor_theme_name
					    : "(default)",
		   size, loaded,
		   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE);
}

static void
qdwin_cursor_theme_destroy(struct qdwin *qdwin)
{
	for (size_t i = 0;
	     i < sizeof qdwin->cursor_images / sizeof qdwin->cursor_images[0];
	     i++) {
		if (qdwin->cursor_images[i]) {
			XcursorImagesDestroy(qdwin->cursor_images[i]);
			qdwin->cursor_images[i] = NULL;
		}
	}
	for (size_t i = 0;
	     i < sizeof qdwin->cursor_sprites /
		 sizeof qdwin->cursor_sprites[0];
	     i++) {
		if (qdwin->cursor_sprites[i].surface) {
			wl_list_remove(
				&qdwin->cursor_sprites[i].destroy_listener.link);
			if (qdwin->cursor_sprites[i].commit_listener.link.next)
				wl_list_remove(
					&qdwin->cursor_sprites[i].commit_listener.link);
			qdwin->cursor_sprites[i].surface = NULL;
		}
	}
	free(qdwin->cursor_theme_name);
	qdwin->cursor_theme_name = NULL;
}

/* §6.6 follow-up 2026-04-25: sprite install via solid-color proxy.
 *
 * Gated on QDWIN_CURSOR_SPRITE_SOLID=1 and `qdwin_cursor_solid_enabled`
 * below. Uses public libweston-14 API (weston_buffer_create_solid_rgba
 * + weston_surface_attach_solid + weston_view_create + direct
 * assignment to weston_pointer::sprite, which IS a public field in the
 * version-14 header contrary to the earlier §6.7(b) finding). Per-
 * shape solid colour makes the visible cursor shape-distinguishable
 * without the theme-image upload path, which still requires an
 * internal wl_client worker thread (tracked as the "full theme"
 * follow-up in the parking notes above).
 *
 * Lifecycle: one solid sprite surface per cursor-shape device, torn
 * down on device destroy or re-created on each set_shape. The
 * sprite_destroy_listener in weston_pointer is wired so libweston
 * clears pointer->sprite when our view is destroyed (important: we
 * initialise wl_list on the listener's link slot before sending).
 */
struct qdwin_cursor_shape_device {
	struct qdwin *qdwin;
	struct weston_pointer *pointer; /* resolved at get_pointer */
	struct wl_listener pointer_destroy_listener;
	struct weston_surface *sprite_surface;
	struct weston_view *sprite_view;
	struct weston_buffer_reference *sprite_buffer_ref;
	uint32_t current_shape;
};

/* Forward decls — definitions live further down (theme path is below
 * the cursor_shape_device_impl table because it needs the device struct,
 * but set_shape needs to call into them). */
static void
qdwin_cursor_device_install_theme_sprite(struct qdwin_cursor_shape_device *dev,
					 uint32_t shape);
static void
qdwin_cursor_device_install_solid_sprite(struct qdwin_cursor_shape_device *dev,
					 uint32_t shape);

static bool
qdwin_cursor_solid_enabled(void)
{
	const char *v = getenv("QDWIN_CURSOR_SPRITE_SOLID");
	return v && v[0] && v[0] != '0';
}

static void
qdwin_cursor_shape_colour(uint32_t shape, float *r, float *g, float *b,
			  float *a)
{
	*a = 0.9f;
	/* Default: near-white so a "default" arrow stands out. */
	*r = 0.95f; *g = 0.95f; *b = 0.95f;
	switch (shape) {
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT:
		*r = 0.15f; *g = 0.15f; *b = 0.15f; return;
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER:
		*r = 0.30f; *g = 0.85f; *b = 0.30f; return;
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS:
		*r = 0.95f; *g = 0.85f; *b = 0.20f; return;
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP:
		*r = 0.90f; *g = 0.25f; *b = 0.25f; return;
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE:
		*r = 0.30f; *g = 0.50f; *b = 0.90f; return;
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP:
		*r = 0.70f; *g = 0.50f; *b = 0.95f; return;
	default:
		return;
	}
}

static void
qdwin_cursor_sprite_tear_down(struct qdwin_cursor_shape_device *dev)
{
	if (dev->sprite_view) {
		if (dev->pointer && dev->pointer->sprite == dev->sprite_view)
			dev->pointer->sprite = NULL;
		weston_view_destroy(dev->sprite_view);
		dev->sprite_view = NULL;
	}
	if (dev->sprite_buffer_ref) {
		weston_buffer_destroy_solid(dev->sprite_buffer_ref);
		dev->sprite_buffer_ref = NULL;
	}
	if (dev->sprite_surface) {
		weston_surface_unref(dev->sprite_surface);
		dev->sprite_surface = NULL;
	}
}

static void
qdwin_cursor_device_install_solid_sprite(struct qdwin_cursor_shape_device *dev,
					 uint32_t shape)
{
	if (!dev->pointer) {
		weston_log("qdwin: cursor-shape solid install skipped "
			   "(no pointer resolved)\n");
		return;
	}
	qdwin_cursor_sprite_tear_down(dev);

	float r, g, b, a;
	qdwin_cursor_shape_colour(shape, &r, &g, &b, &a);

	const int size = 24;
	struct weston_surface *surface =
		weston_surface_create(dev->qdwin->compositor);
	if (!surface)
		return;
	struct weston_buffer_reference *ref =
		weston_buffer_create_solid_rgba(dev->qdwin->compositor,
						r, g, b, a);
	if (!ref) {
		weston_surface_unref(surface);
		return;
	}
	weston_surface_attach_solid(surface, ref, size, size);
	struct weston_view *view = weston_view_create(surface);
	if (!view) {
		weston_buffer_destroy_solid(ref);
		weston_surface_unref(surface);
		return;
	}
	dev->sprite_surface = surface;
	dev->sprite_buffer_ref = ref;
	dev->sprite_view = view;
	dev->current_shape = shape;

	/* Hotspot at the top-left for a simple coloured cursor; most
	 * cursor shapes from CSS point into their upper-left quadrant. */
	dev->pointer->hotspot = weston_coord_surface(0, 0, surface);
	dev->pointer->sprite = view;

	weston_compositor_schedule_repaint(dev->qdwin->compositor);
	weston_log("qdwin: cursor-shape sprite installed "
		   "(shape=%s, solid rgba=%.2f,%.2f,%.2f,%.2f, %dx%d)\n",
		   qdwin_cursor_shape_name(shape), r, g, b, a, size, size);
}

static void
qdwin_cursor_device_pointer_destroyed(struct wl_listener *listener,
				      void *data)
{
	struct qdwin_cursor_shape_device *dev =
		wl_container_of(listener, dev, pointer_destroy_listener);
	(void)data;
	qdwin_cursor_sprite_tear_down(dev);
	dev->pointer = NULL;
}

static void
qdwin_cursor_shape_device_destroy(struct wl_client *client,
				  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_cursor_shape_device_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_cursor_shape_device *dev =
		wl_resource_get_user_data(resource);
	if (!dev)
		return;
	qdwin_cursor_sprite_tear_down(dev);
	if (dev->pointer) {
		wl_list_remove(&dev->pointer_destroy_listener.link);
		dev->pointer = NULL;
	}
	free(dev);
}

static void
qdwin_cursor_shape_device_set_shape(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t serial,
				    uint32_t shape)
{
	struct qdwin_cursor_shape_device *dev =
		wl_resource_get_user_data(resource);
	struct qdwin *qdwin = dev ? dev->qdwin : NULL;
	(void)client;
	(void)serial;
	if (shape < 1 ||
	    shape > WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE) {
		wl_resource_post_error(resource,
			WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
			"invalid cursor shape %u", shape);
		return;
	}
	XcursorImages *imgs = qdwin ? qdwin->cursor_images[shape] : NULL;
	if (imgs && imgs->nimage > 0) {
		XcursorImage *img = imgs->images[0];
		weston_log("qdwin: cursor-shape set_shape=%s dims=%ux%u "
			   "hotspot=%u,%u nframes=%d (sprite=%s)\n",
			   qdwin_cursor_shape_name(shape),
			   img->width, img->height,
			   img->xhot, img->yhot, imgs->nimage,
			   qdwin_cursor_solid_enabled()
				? "installing-solid"
				: "deferred");
	} else {
		weston_log("qdwin: cursor-shape set_shape=%s "
			   "(theme miss; sprite=%s)\n",
			   qdwin_cursor_shape_name(shape),
			   qdwin_cursor_solid_enabled()
				? "installing-solid"
				: "deferred");
	}

	if (!dev)
		return;
	/* §6.8 cursor-sprite full theme: prefer a shell-registered sprite
	 * if present; fall back to solid colour (gated by
	 * QDWIN_CURSOR_SPRITE_SOLID); else leave whatever the previous
	 * sprite was. */
	if (qdwin && shape < sizeof qdwin->cursor_sprites /
			      sizeof qdwin->cursor_sprites[0] &&
	    qdwin->cursor_sprites[shape].surface) {
		qdwin_cursor_device_install_theme_sprite(dev, shape);
	} else if (qdwin_cursor_solid_enabled()) {
		qdwin_cursor_device_install_solid_sprite(dev, shape);
	}
}

static const struct wp_cursor_shape_device_v1_interface
qdwin_cursor_shape_device_impl = {
	.destroy   = qdwin_cursor_shape_device_destroy,
	.set_shape = qdwin_cursor_shape_device_set_shape,
};

/* §6.8 cursor-sprite full theme: shell-provided per-shape wl_surface
 * applied on set_shape. The shell (or a helper it spawns) loads
 * libXcursor itself, paints each shape into a wl_shm-backed
 * wl_surface, and registers it via qdwin_shell_v1.set_cursor_sprite.
 * The compositor caches {shape -> weston_surface, hotspot}; on
 * set_shape we install that surface + hotspot on the active pointer.
 * Falls back to the solid-colour path (QDWIN_CURSOR_SPRITE_SOLID) or
 * a no-op if neither is present. */

/* plan3 M2: per-commit hook that re-asserts the cursor input-region
 * invariant. Weston's libweston/input.c:pointer_cursor_surface_committed
 * clears these on every commit; qdwin's install path only clears them
 * once. The sprite helper is allowed to re-commit (theme change, scale,
 * new frame) and any non-empty pending.input it ships would otherwise
 * promote the sprite back to a pickable surface above shell UI. The
 * post-commit log line is the discriminator for
 * tests/host/test_cursor_sprite_input_invariant.py. */
static void
qdwin_cursor_sprite_on_commit(struct wl_listener *listener, void *data)
{
	struct qdwin_cursor_sprite *slot =
		wl_container_of(listener, slot, commit_listener);
	struct weston_surface *s = slot->surface;
	(void)data;
	if (!s)
		return;
	int had_pending = pixman_region32_not_empty(&s->pending.input);
	int had_current = pixman_region32_not_empty(&s->input);
	if (had_pending)
		pixman_region32_clear(&s->pending.input);
	if (had_current)
		pixman_region32_clear(&s->input);
	if (had_pending || had_current) {
		weston_log("qdwin: cursor-sprite commit re-cleared input "
			   "shape=%s pending=%d current=%d\n",
			   qdwin_cursor_shape_name(slot->shape),
			   had_pending, had_current);
	}
}

static void
qdwin_cursor_sprite_clear_slot(struct qdwin *qdwin, uint32_t shape)
{
	if (shape >= sizeof qdwin->cursor_sprites /
		      sizeof qdwin->cursor_sprites[0])
		return;
	struct qdwin_cursor_sprite *slot = &qdwin->cursor_sprites[shape];
	if (slot->surface) {
		wl_list_remove(&slot->destroy_listener.link);
		if (slot->commit_listener.link.next)
			wl_list_remove(&slot->commit_listener.link);
		slot->surface = NULL;
	}
	slot->hotspot_x = 0;
	slot->hotspot_y = 0;
}

static void
qdwin_cursor_sprite_on_surface_destroyed(struct wl_listener *listener,
					 void *data)
{
	struct qdwin_cursor_sprite *slot =
		wl_container_of(listener, slot, destroy_listener);
	(void)data;
	weston_log("qdwin: cursor-sprite cleared shape=%s "
		   "(surface destroyed)\n",
		   qdwin_cursor_shape_name(slot->shape));
	/* Don't call qdwin_cursor_sprite_clear_slot — it does
	 * wl_list_remove on the destroy_listener.link, but libwayland
	 * already removed it before invoking us. Just reset state. */
	if (slot->commit_listener.link.next)
		wl_list_remove(&slot->commit_listener.link);
	slot->surface = NULL;
	slot->hotspot_x = 0;
	slot->hotspot_y = 0;
	wl_list_init(&slot->destroy_listener.link);
	wl_list_init(&slot->commit_listener.link);
}

/* B6 / task(134): install a cached cursor sprite as pointer->sprite,
 * mirroring weston's `pointer_set_cursor` (libweston/input.c). The
 * minimal-but-correct steps are:
 *
 *   1. Surface needs role "wl_pointer-cursor" — without it weston
 *      treats the surface as un-mapped-in-disguise.
 *   2. Surface needs to be mapped — or weston skips it on composition.
 *   3. View needs to live in compositor->cursor_layer.view_list — or
 *      no layer iteration ever picks it up; the renderer / DRM cursor
 *      plane both walk that specific layer.
 *
 * Used by both `qdwin_install_default_cursor_on_pointer` (no-client-
 * owns-cursor fallback) and `qdwin_cursor_device_install_theme_sprite`
 * (wp_cursor_shape_v1.set_shape from clients). Returns NULL on failure,
 * the freshly-installed view on success. The caller owns positioning
 * + scheduling repaint; this helper does both at the end so the cursor
 * is immediately visible. */
static struct weston_view *
qdwin_install_cursor_sprite_view(struct qdwin *qdwin,
				 struct weston_pointer *pointer,
				 struct weston_surface *surface,
				 int32_t hotspot_x, int32_t hotspot_y,
				 const char *log_prefix)
{
	const char *role = weston_surface_get_role(surface);
	if (!role) {
		if (weston_surface_set_role(surface, "wl_pointer-cursor",
					    NULL, 0) < 0) {
			weston_log("qdwin: %s: set_role failed\n", log_prefix);
			return NULL;
		}
	} else if (strcmp(role, "wl_pointer-cursor") != 0) {
		weston_log("qdwin: %s: surface has role=%s, skipping\n",
			   log_prefix, role);
		return NULL;
	}

	struct weston_view *view = weston_view_create(surface);
	if (!view) {
		weston_log("qdwin: %s: view create failed\n", log_prefix);
		return NULL;
	}
	pointer->hotspot = weston_coord_surface(hotspot_x, hotspot_y, surface);
	pointer->sprite = view;

	struct weston_coord_surface hotspot_inv =
		weston_coord_surface_invert(pointer->hotspot);
	weston_view_set_position_with_offset(view, pointer->pos, hotspot_inv);

	if (!weston_surface_is_mapped(surface))
		weston_surface_map(surface);
	pixman_region32_clear(&surface->pending.input);
	pixman_region32_clear(&surface->input);
	weston_view_move_to_layer(view,
				  &qdwin->compositor->cursor_layer.view_list);

	weston_compositor_schedule_repaint(qdwin->compositor);
	weston_log("qdwin: %s: mapped on cursor_layer (hotspot=%d,%d)\n",
		   log_prefix, hotspot_x, hotspot_y);
	return view;
}

/* B6: install the cached "default" sprite as pointer->sprite when no
 * client owns the cursor (over desktop, panel, or any non-Wayland
 * surface; also during the early boot window before any client maps). */
static void
qdwin_install_default_cursor_on_pointer(struct qdwin *qdwin,
					struct weston_pointer *pointer)
{
	if (!qdwin || !pointer)
		return;
	struct qdwin_cursor_sprite *slot =
		&qdwin->cursor_sprites[WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT];
	if (!slot->surface) {
		/* Log once per qdwin lifetime — fires on every pointer
		 * focus change otherwise (1000+ identical lines per
		 * session under apps that move focus a lot). */
		if (!qdwin->cursor_default_warned) {
			qdwin->cursor_default_warned = 1;
			weston_log("qdwin: install_default_cursor: no surface "
				   "yet (helper not started?) — further "
				   "occurrences suppressed\n");
		}
		return;     /* helper hasn't registered the default yet */
	}
	qdwin_install_cursor_sprite_view(qdwin, pointer, slot->surface,
					 slot->hotspot_x, slot->hotspot_y,
					 "install_default_cursor");
}

/* Restore the default cursor when pointer focus leaves a client
 * surface. Without this, the last client's cursor sticks even after
 * the pointer is over the bare desktop. */
static void
qdwin_default_cursor_on_focus_changed(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer = data;
	(void)listener;
	if (pointer->focus)
		return;     /* focus on a client → that client owns cursor */
	if (pointer->sprite)
		return;     /* something else already set a sprite */
	if (qdwin_singleton)
		qdwin_install_default_cursor_on_pointer(qdwin_singleton, pointer);
}

/* B6 fix: timer callback to retry cursor installation when pointer
 * wasn't ready when cursor-sprites helper ran. */
static struct wl_event_source *cursor_retry_timer;

static int
qdwin_cursor_retry_timer_cb(void *data)
{
	(void)data;
	/* Backup: try to install cursor if pointer is now available */
	struct qdwin *qdwin = qdwin_singleton;
	if (qdwin) {
		struct weston_seat *seat;
		wl_list_for_each(seat, &qdwin->compositor->seat_list, link) {
			struct weston_pointer *p = weston_seat_get_pointer(seat);
			if (p && !p->sprite)
				qdwin_install_default_cursor_on_pointer(qdwin, p);
		}
	}
	cursor_retry_timer = NULL;
	return 0;
}

static int
qdwin_cursor_retry_install(struct qdwin *qdwin)
{
	struct wl_event_loop *loop;

	if (cursor_retry_timer)
		return 0;  /* already scheduled */

	loop = wl_display_get_event_loop(qdwin->compositor->wl_display);
	cursor_retry_timer = wl_event_loop_add_timer(loop, qdwin_cursor_retry_timer_cb, qdwin);
	if (!cursor_retry_timer) {
		weston_log("qdwin: cursor_retry: failed to add timer\n");
		return -1;
	}
	weston_log("qdwin: cursor_retry_install: scheduling first retry in 1000ms\n");
	wl_event_source_timer_update(cursor_retry_timer, 1000);
	return 0;
}

static void
qdwin_handle_set_cursor_sprite(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t shape,
			       struct wl_resource *surface_resource,
			       int32_t hotspot_x,
			       int32_t hotspot_y)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;

	if (shape < 1 ||
	    shape > WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE) {
		wl_resource_post_error(resource,
			WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
			"set_cursor_sprite: invalid shape %u", shape);
		return;
	}

	qdwin_cursor_sprite_clear_slot(qdwin, shape);

	if (!surface_resource) {
		weston_log("qdwin: cursor-sprite cleared shape=%s\n",
			   qdwin_cursor_shape_name(shape));
		return;
	}

	struct weston_surface *ws =
		wl_resource_get_user_data(surface_resource);
	if (!ws) {
		weston_log("qdwin: cursor-sprite shape=%s — surface "
			   "resource invalid\n",
			   qdwin_cursor_shape_name(shape));
		return;
	}

	struct qdwin_cursor_sprite *slot = &qdwin->cursor_sprites[shape];
	slot->surface = ws;
	slot->hotspot_x = hotspot_x;
	slot->hotspot_y = hotspot_y;
	slot->qdwin = qdwin;
	slot->shape = shape;
	slot->destroy_listener.notify =
		qdwin_cursor_sprite_on_surface_destroyed;
	wl_signal_add(&ws->destroy_signal, &slot->destroy_listener);

	/* plan3 M2: stay aligned with weston cursor-surface invariant on
	 * every commit, not just at install. The first install also clears
	 * the regions via qdwin_install_cursor_sprite_view; this listener
	 * catches subsequent re-commits from the helper. */
	slot->commit_listener.notify = qdwin_cursor_sprite_on_commit;
	wl_signal_add(&ws->commit_signal, &slot->commit_listener);
	/* Also clear once on register so a sprite whose creating client
	 * already attached a non-empty input region cannot leak before
	 * its first commit. */
	pixman_region32_clear(&ws->pending.input);
	pixman_region32_clear(&ws->input);

	weston_log("qdwin: cursor-sprite registered shape=%s hotspot=%d,%d\n",
		   qdwin_cursor_shape_name(shape), hotspot_x, hotspot_y);

	/* B6 fix: when the "default" sprite registers, attach it to every
	 * existing pointer that has no client-set sprite. Without this,
	 * the cursor stays invisible until a client (foot, etc.) takes
	 * pointer focus and calls wp_cursor_shape_v1.set_shape — over an
	 * empty desktop or non-Wayland-aware area there's nothing to set
	 * the cursor and SPICE has no cursor data to send to the viewer.
	 *
	 * Also installs on pointer focus → NULL transitions via the
	 * focus_signal listener wired up in qdwin_install_default_cursor_*.
	 */
	if (shape == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT) {
		struct weston_seat *seat;
		int any_pointer = 0;
		wl_list_for_each(seat, &qdwin->compositor->seat_list, link) {
			struct weston_pointer *p = weston_seat_get_pointer(seat);
			if (p) {
				any_pointer = 1;
				if (!p->sprite)
					qdwin_install_default_cursor_on_pointer(qdwin, p);
			}
		}
		/* B6: if no pointer existed yet, schedule a retry as backup */
		if (!any_pointer)
			qdwin_cursor_retry_install(qdwin);
	}
}

static void
qdwin_cursor_device_install_theme_sprite(struct qdwin_cursor_shape_device *dev,
					 uint32_t shape)
{
	struct qdwin *qdwin = dev->qdwin;
	if (!dev->pointer)
		return;
	if (shape >= sizeof qdwin->cursor_sprites /
		     sizeof qdwin->cursor_sprites[0])
		return;
	struct qdwin_cursor_sprite *slot = &qdwin->cursor_sprites[shape];
	if (!slot->surface)
		return;

	qdwin_cursor_sprite_tear_down(dev);

	char log_prefix[64];
	snprintf(log_prefix, sizeof log_prefix,
		 "cursor-shape install shape=%s",
		 qdwin_cursor_shape_name(shape));
	struct weston_view *view = qdwin_install_cursor_sprite_view(
		qdwin, dev->pointer, slot->surface,
		slot->hotspot_x, slot->hotspot_y, log_prefix);
	if (!view)
		return;
	dev->sprite_view = view;
	dev->current_shape = shape;
}

static void
qdwin_cursor_shape_manager_destroy(struct wl_client *client,
				   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static struct weston_pointer *
qdwin_resolve_first_pointer(struct qdwin *qdwin)
{
	/* Single-seat single-pointer is the only shape qdwin supports
	 * today (RDP backend creates one seat per client; we pin to the
	 * first seat's pointer for cursor-shape sprite install). */
	struct weston_seat *seat;
	wl_list_for_each(seat, &qdwin->compositor->seat_list, link) {
		struct weston_pointer *p = weston_seat_get_pointer(seat);
		if (p)
			return p;
	}
	return NULL;
}

static void
qdwin_cursor_shape_manager_get_pointer(struct wl_client *client,
				       struct wl_resource *resource,
				       uint32_t id,
				       struct wl_resource *pointer_resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct wl_resource *dev_res;
	(void)pointer_resource;
	dev_res = wl_resource_create(client,
				     &wp_cursor_shape_device_v1_interface,
				     wl_resource_get_version(resource), id);
	if (!dev_res) {
		wl_client_post_no_memory(client);
		return;
	}
	struct qdwin_cursor_shape_device *dev = calloc(1, sizeof *dev);
	if (!dev) {
		wl_resource_destroy(dev_res);
		wl_client_post_no_memory(client);
		return;
	}
	dev->qdwin = qdwin;
	dev->pointer = qdwin_resolve_first_pointer(qdwin);
	if (dev->pointer) {
		dev->pointer_destroy_listener.notify =
			qdwin_cursor_device_pointer_destroyed;
		wl_signal_add(&dev->pointer->destroy_signal,
			      &dev->pointer_destroy_listener);
	} else {
		wl_list_init(&dev->pointer_destroy_listener.link);
	}
	wl_resource_set_implementation(dev_res,
				       &qdwin_cursor_shape_device_impl,
				       dev,
				       qdwin_cursor_shape_device_resource_destroy);
}

static void
qdwin_cursor_shape_manager_get_tablet_tool_v2(struct wl_client *client,
					      struct wl_resource *resource,
					      uint32_t id,
					      struct wl_resource *tool_resource)
{
	qdwin_cursor_shape_manager_get_pointer(client, resource, id,
					       tool_resource);
}

static const struct wp_cursor_shape_manager_v1_interface
qdwin_cursor_shape_manager_impl = {
	.destroy            = qdwin_cursor_shape_manager_destroy,
	.get_pointer        = qdwin_cursor_shape_manager_get_pointer,
	.get_tablet_tool_v2 = qdwin_cursor_shape_manager_get_tablet_tool_v2,
};

static void
bind_qdwin_cursor_shape_manager(struct wl_client *client, void *data,
				uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource = wl_resource_create(
		client, &wp_cursor_shape_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource,
				       &qdwin_cursor_shape_manager_impl,
				       qdwin, NULL);
}

/* ------------------------------------------------------------------
 * §6.7 wp_fractional_scale_v1.
 *
 * Each get_fractional_scale creates a wp_fractional_scale_v1 tied to
 * a specific wl_surface. The compositor sends `preferred_scale(N)`
 * with N/120 giving the recommended buffer scale for that surface's
 * current output.
 *
 * §6.7(c): we track the resource + backing surface in
 * qdwin::fractional_scales and recompute preferred_scale whenever
 * outputs change (output_created / output_resized) by looking at
 * the first available output's integer scale. When weston later
 * gains true fractional-scale output configuration, this function
 * is the single place to upgrade. A per-output inherited scale
 * (following the surface's primary output assignment) is the
 * planned follow-up — requires wiring weston_surface::output into
 * the compute path and a surface-output-changed listener.
 *
 * Protocol encoding: scale unit is 120. 120 = 1.0×, 180 = 1.5×,
 * 240 = 2.0×. If no output is present (headless, very early init)
 * we conservatively report 120.
 * ------------------------------------------------------------------ */

struct qdwin_fractional_scale {
	struct qdwin *qdwin;
	struct wl_resource *resource;      /* wp_fractional_scale_v1 */
	struct weston_surface *surface;    /* may go NULL if destroyed */
	struct wl_listener surface_destroy_listener;
	struct wl_listener surface_commit_listener;
	uint32_t last_sent_scale;          /* 0 = nothing sent yet */
	struct wl_list link;               /* qdwin::fractional_scales */
};

static uint32_t
qdwin_compute_preferred_scale_for_surface(struct qdwin *qdwin,
					  struct weston_surface *surface)
{
	/* §6.7(c) follow-up: per-surface output selection. If the surface
	 * has a primary output (weston_surface::output, set internally by
	 * weston_view_assign_output), prefer its current_scale. Otherwise
	 * fall back to the first output, or 120 when headless.
	 *
	 * Env override QDWIN_FRACTIONAL_SCALE=N forces a specific value
	 * for testing non-integer scales (150 = 1.25×). */
	const char *env = getenv("QDWIN_FRACTIONAL_SCALE");
	if (env && *env) {
		long n = strtol(env, NULL, 10);
		if (n >= 30 && n <= 960)
			return (uint32_t)n;
	}
	if (surface && surface->output && surface->output->current_scale > 0)
		return (uint32_t)surface->output->current_scale * 120u;
	struct weston_output *out;
	wl_list_for_each(out, &qdwin->compositor->output_list, link) {
		if (out->current_scale > 0)
			return (uint32_t)out->current_scale * 120u;
		break;
	}
	return 120;
}

static void
qdwin_fractional_scale_push(struct qdwin_fractional_scale *fs)
{
	uint32_t scale =
		qdwin_compute_preferred_scale_for_surface(fs->qdwin, fs->surface);
	if (fs->last_sent_scale == scale)
		return;
	fs->last_sent_scale = scale;
	wp_fractional_scale_v1_send_preferred_scale(fs->resource, scale);
}

static void
qdwin_fractional_scale_broadcast(struct qdwin *qdwin)
{
	struct qdwin_fractional_scale *fs;
	wl_list_for_each(fs, &qdwin->fractional_scales, link)
		qdwin_fractional_scale_push(fs);
}

static void
qdwin_fractional_scale_surface_commit(struct wl_listener *l, void *data)
{
	struct qdwin_fractional_scale *fs =
		wl_container_of(l, fs, surface_commit_listener);
	(void)data;
	/* weston_view_assign_output runs on commit and may flip
	 * surface->output. Re-evaluate the preferred scale. */
	qdwin_fractional_scale_push(fs);
}

static void
qdwin_fractional_scale_surface_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_fractional_scale *fs =
		wl_container_of(l, fs, surface_destroy_listener);
	(void)data;
	wl_list_remove(&fs->surface_destroy_listener.link);
	wl_list_init(&fs->surface_destroy_listener.link);
	wl_list_remove(&fs->surface_commit_listener.link);
	wl_list_init(&fs->surface_commit_listener.link);
	fs->surface = NULL;
}

static void
qdwin_fractional_scale_destroy(struct wl_client *client,
			       struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_fractional_scale_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_fractional_scale *fs = wl_resource_get_user_data(resource);
	if (!fs)
		return;
	if (fs->surface) {
		wl_list_remove(&fs->surface_destroy_listener.link);
		wl_list_remove(&fs->surface_commit_listener.link);
	}
	wl_list_remove(&fs->link);
	free(fs);
}

static const struct wp_fractional_scale_v1_interface
qdwin_fractional_scale_impl = {
	.destroy = qdwin_fractional_scale_destroy,
};

static void
qdwin_fractional_scale_manager_destroy(struct wl_client *client,
				       struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_fractional_scale_manager_get(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t id,
				   struct wl_resource *surface_resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct weston_surface *surface =
		surface_resource ? wl_resource_get_user_data(surface_resource)
				 : NULL;
	struct qdwin_fractional_scale *fs = calloc(1, sizeof *fs);
	struct wl_resource *fs_res;
	uint32_t scale;
	if (!fs) {
		wl_client_post_no_memory(client);
		return;
	}
	fs_res = wl_resource_create(client,
				    &wp_fractional_scale_v1_interface,
				    wl_resource_get_version(resource), id);
	if (!fs_res) {
		free(fs);
		wl_client_post_no_memory(client);
		return;
	}
	fs->qdwin = qdwin;
	fs->resource = fs_res;
	fs->surface = surface;
	wl_list_init(&fs->surface_destroy_listener.link);
	wl_list_init(&fs->surface_commit_listener.link);
	if (surface) {
		fs->surface_destroy_listener.notify =
			qdwin_fractional_scale_surface_destroyed;
		wl_signal_add(&surface->destroy_signal,
			      &fs->surface_destroy_listener);
		fs->surface_commit_listener.notify =
			qdwin_fractional_scale_surface_commit;
		wl_signal_add(&surface->commit_signal,
			      &fs->surface_commit_listener);
	}
	wl_list_insert(&qdwin->fractional_scales, &fs->link);
	wl_resource_set_implementation(fs_res, &qdwin_fractional_scale_impl,
				       fs,
				       qdwin_fractional_scale_resource_destroy);
	scale = qdwin_compute_preferred_scale_for_surface(qdwin, surface);
	fs->last_sent_scale = scale;
	wp_fractional_scale_v1_send_preferred_scale(fs_res, scale);
	(void)scale;
}

static const struct wp_fractional_scale_manager_v1_interface
qdwin_fractional_scale_manager_impl = {
	.destroy              = qdwin_fractional_scale_manager_destroy,
	.get_fractional_scale = qdwin_fractional_scale_manager_get,
};

static void
bind_qdwin_fractional_scale_manager(struct wl_client *client, void *data,
				    uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource = wl_resource_create(
		client, &wp_fractional_scale_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource,
				       &qdwin_fractional_scale_manager_impl,
				       qdwin, NULL);
}

/* ------------------------------------------------------------------
 * §6.7 primary-selection-unstable-v1.
 *
 * Middle-click clipboard (per-seat). One qdwin_primary_seat per
 * weston_seat tracks the current source + attached devices. A client
 * calls create_source(), then source.offer(mime) for each advertised
 * type, then device.set_selection(source, serial). The compositor
 * fans out a fresh offer resource to every other device on the seat
 * and tells them about the new selection. A reading client calls
 * offer.receive(mime, fd) → compositor forwards source.send(mime, fd)
 * to the source's client, which writes the data and closes the fd.
 *
 * Scope today: broadcast delivery to all devices on the seat. The
 * spec permits focus-gated delivery (only the keyboard-focused
 * client gets selection events) as a privacy hardening; we leave
 * that as a follow-up once §6.6 S0 gives us reliable focus hooks.
 * Primary-selection content is generally low-sensitivity (transient
 * text), and qdwin's peer-uid plugin gate limits clients to the
 * same uid anyway.
 * ------------------------------------------------------------------ */

struct qdwin_primary_seat;
struct qdwin_primary_source;
struct qdwin_primary_device;

struct qdwin_primary_mime {
	char *type;
	struct wl_list link;  /* qdwin_primary_source::mime_types */
};

struct qdwin_primary_source {
	struct qdwin *qdwin;
	struct wl_resource *resource;
	struct qdwin_primary_seat *pseat;  /* NULL until set_selection wins */
	struct wl_list mime_types;          /* qdwin_primary_mime::link */
};

struct qdwin_primary_device {
	struct qdwin *qdwin;
	struct qdwin_primary_seat *pseat;
	struct wl_resource *resource;
	struct wl_list link;  /* qdwin_primary_seat::devices */
};

struct qdwin_primary_offer {
	struct qdwin_primary_source *source;  /* NULL after source destroyed */
	struct wl_resource *resource;
};

struct qdwin_primary_seat {
	struct qdwin *qdwin;
	struct weston_seat *seat;
	struct qdwin_primary_source *current_source;
	struct wl_list devices;  /* qdwin_primary_device::link */
	struct wl_listener seat_destroy_listener;
	struct wl_list link;  /* qdwin::primary_seats::link */
};

static struct qdwin_primary_seat *
qdwin_primary_seat_find(struct qdwin *qdwin, struct weston_seat *seat)
{
	struct qdwin_primary_seat *pseat;
	wl_list_for_each(pseat, &qdwin->primary_seats, link)
		if (pseat->seat == seat)
			return pseat;
	return NULL;
}

static void qdwin_primary_seat_seat_destroyed(struct wl_listener *l,
					      void *data);

static struct qdwin_primary_seat *
qdwin_primary_seat_ensure(struct qdwin *qdwin, struct weston_seat *seat)
{
	struct qdwin_primary_seat *pseat = qdwin_primary_seat_find(qdwin, seat);
	if (pseat)
		return pseat;
	pseat = calloc(1, sizeof *pseat);
	if (!pseat)
		return NULL;
	pseat->qdwin = qdwin;
	pseat->seat = seat;
	wl_list_init(&pseat->devices);
	pseat->seat_destroy_listener.notify = qdwin_primary_seat_seat_destroyed;
	wl_signal_add(&seat->destroy_signal, &pseat->seat_destroy_listener);
	wl_list_insert(&qdwin->primary_seats, &pseat->link);
	return pseat;
}

/* --- primary_selection_offer -------------------------------------- */

static void
qdwin_primary_offer_receive(struct wl_client *client,
			    struct wl_resource *resource,
			    const char *mime_type, int32_t fd)
{
	struct qdwin_primary_offer *offer = wl_resource_get_user_data(resource);
	(void)client;
	if (offer && offer->source && offer->source->resource) {
		zwp_primary_selection_source_v1_send_send(
			offer->source->resource, mime_type, fd);
	}
	close(fd);
}

static void
qdwin_primary_offer_destroy(struct wl_client *client,
			    struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_offer_v1_interface
qdwin_primary_offer_impl = {
	.receive = qdwin_primary_offer_receive,
	.destroy = qdwin_primary_offer_destroy,
};

static void
qdwin_primary_offer_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_primary_offer *offer = wl_resource_get_user_data(resource);
	free(offer);
}

/* Build a new offer for `device`, hand it to the client, and announce
 * each mime type. Does not send selection(offer) itself — caller does. */
static struct wl_resource *
qdwin_primary_build_offer_for_device(struct qdwin_primary_device *device,
				     struct qdwin_primary_source *source)
{
	struct wl_client *client = wl_resource_get_client(device->resource);
	uint32_t version = wl_resource_get_version(device->resource);
	struct qdwin_primary_offer *offer = calloc(1, sizeof *offer);
	struct wl_resource *res;
	struct qdwin_primary_mime *m;
	if (!offer)
		return NULL;
	res = wl_resource_create(client,
				 &zwp_primary_selection_offer_v1_interface,
				 version, 0);
	if (!res) {
		free(offer);
		return NULL;
	}
	offer->source = source;
	offer->resource = res;
	wl_resource_set_implementation(res, &qdwin_primary_offer_impl, offer,
				       qdwin_primary_offer_resource_destroy);
	zwp_primary_selection_device_v1_send_data_offer(device->resource, res);
	wl_list_for_each(m, &source->mime_types, link)
		zwp_primary_selection_offer_v1_send_offer(res, m->type);
	return res;
}

/* --- primary_selection_source ------------------------------------ */

static void
qdwin_primary_source_clear_offers(struct qdwin_primary_source *source)
{
	/* Offers keep a pointer back to source; mark them stale so their
	 * receive() becomes a no-op after source goes away. */
	(void)source;
}

static void
qdwin_primary_source_free_mimes(struct qdwin_primary_source *source)
{
	struct qdwin_primary_mime *m, *tmp;
	wl_list_for_each_safe(m, tmp, &source->mime_types, link) {
		wl_list_remove(&m->link);
		free(m->type);
		free(m);
	}
}

static void
qdwin_primary_source_offer(struct wl_client *client,
			   struct wl_resource *resource,
			   const char *mime_type)
{
	struct qdwin_primary_source *source = wl_resource_get_user_data(resource);
	struct qdwin_primary_mime *m;
	(void)client;
	if (!mime_type)
		return;
	m = calloc(1, sizeof *m);
	if (!m)
		return;
	m->type = strdup(mime_type);
	if (!m->type) {
		free(m);
		return;
	}
	wl_list_insert(source->mime_types.prev, &m->link);
}

static void
qdwin_primary_source_destroy_req(struct wl_client *client,
				 struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_source_v1_interface
qdwin_primary_source_impl = {
	.offer   = qdwin_primary_source_offer,
	.destroy = qdwin_primary_source_destroy_req,
};

static void
qdwin_primary_seat_clear_selection(struct qdwin_primary_seat *pseat,
				   int notify_source);

static void
qdwin_primary_source_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_primary_source *source = wl_resource_get_user_data(resource);
	if (!source)
		return;
	if (source->pseat && source->pseat->current_source == source) {
		/* Source died while owning the selection. Notify all
		 * devices with selection(NULL) but don't fire cancelled
		 * on ourselves — the resource is already being torn down. */
		qdwin_primary_seat_clear_selection(source->pseat, 0);
	}
	qdwin_primary_source_clear_offers(source);
	qdwin_primary_source_free_mimes(source);
	free(source);
}

/* --- primary_selection_device ------------------------------------ */

static void
qdwin_primary_seat_clear_selection(struct qdwin_primary_seat *pseat,
				   int notify_source)
{
	struct qdwin_primary_source *prev = pseat->current_source;
	struct qdwin_primary_device *device;
	pseat->current_source = NULL;
	if (prev) {
		if (notify_source && prev->resource)
			zwp_primary_selection_source_v1_send_cancelled(
				prev->resource);
		prev->pseat = NULL;
	}
	wl_list_for_each(device, &pseat->devices, link)
		zwp_primary_selection_device_v1_send_selection(
			device->resource, NULL);
}

static void
qdwin_primary_device_set_selection(struct wl_client *client,
				   struct wl_resource *resource,
				   struct wl_resource *source_resource,
				   uint32_t serial)
{
	struct qdwin_primary_device *device = wl_resource_get_user_data(resource);
	struct qdwin_primary_source *source =
		source_resource ? wl_resource_get_user_data(source_resource)
				: NULL;
	struct qdwin_primary_seat *pseat = device ? device->pseat : NULL;
	struct qdwin_primary_device *d;
	(void)client;
	(void)serial;
	if (!pseat)
		return;
	if (source && source->pseat && source->pseat != pseat) {
		/* Source already bound elsewhere — reject per spec intent. */
		return;
	}
	/* Replace previous selection. Notify old source via cancelled. */
	if (pseat->current_source && pseat->current_source != source)
		qdwin_primary_seat_clear_selection(pseat, 1);
	else if (!source)
		qdwin_primary_seat_clear_selection(pseat, 1);
	if (!source)
		return;
	source->pseat = pseat;
	pseat->current_source = source;
	/* For every device on the seat, emit a fresh data_offer, then
	 * announce the selection. */
	wl_list_for_each(d, &pseat->devices, link) {
		struct wl_resource *offer_res =
			qdwin_primary_build_offer_for_device(d, source);
		zwp_primary_selection_device_v1_send_selection(
			d->resource, offer_res);
	}
	/* spec/10: surface to the shell so it can broker-gate. mime_types
	 * here are stored as struct qdwin_primary_mime entries on a
	 * wl_list, not a wl_array — pack them into a flat wl_array on
	 * the stack so we can reuse the regular pack helper. */
	if (qdwin_shell_can_receive_v11(pseat->qdwin)) {
		struct wl_array tmp;
		wl_array_init(&tmp);
		struct qdwin_primary_mime *m;
		wl_list_for_each(m, &source->mime_types, link) {
			char **slot = wl_array_add(&tmp, sizeof(char *));
			if (slot)
				*slot = m->type;
		}
		struct wl_client *src_client = source->resource ?
			wl_resource_get_client(source->resource) : NULL;
		qdwin_emit_selection_set(pseat->qdwin, pseat->seat, &tmp, 1,
					 src_client);
		wl_array_release(&tmp);
	}
}

static void
qdwin_primary_device_destroy_req(struct wl_client *client,
				 struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_device_v1_interface
qdwin_primary_device_impl = {
	.set_selection = qdwin_primary_device_set_selection,
	.destroy       = qdwin_primary_device_destroy_req,
};

static void
qdwin_primary_device_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_primary_device *device = wl_resource_get_user_data(resource);
	if (!device)
		return;
	wl_list_remove(&device->link);
	free(device);
}

/* --- primary_selection_device_manager ---------------------------- */

static void
qdwin_primary_manager_create_source(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_primary_source *source = calloc(1, sizeof *source);
	struct wl_resource *res;
	if (!source) {
		wl_client_post_no_memory(client);
		return;
	}
	res = wl_resource_create(client,
				 &zwp_primary_selection_source_v1_interface,
				 wl_resource_get_version(resource), id);
	if (!res) {
		free(source);
		wl_client_post_no_memory(client);
		return;
	}
	source->qdwin = qdwin;
	source->resource = res;
	wl_list_init(&source->mime_types);
	wl_resource_set_implementation(res, &qdwin_primary_source_impl,
				       source,
				       qdwin_primary_source_resource_destroy);
}

static void
qdwin_primary_manager_get_device(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t id,
				 struct wl_resource *seat_resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct weston_seat *seat =
		seat_resource ? wl_resource_get_user_data(seat_resource)
			      : NULL;
	struct qdwin_primary_seat *pseat;
	struct qdwin_primary_device *device;
	struct wl_resource *res;
	if (!seat) {
		wl_client_post_no_memory(client);
		return;
	}
	pseat = qdwin_primary_seat_ensure(qdwin, seat);
	if (!pseat) {
		wl_client_post_no_memory(client);
		return;
	}
	device = calloc(1, sizeof *device);
	if (!device) {
		wl_client_post_no_memory(client);
		return;
	}
	res = wl_resource_create(client,
				 &zwp_primary_selection_device_v1_interface,
				 wl_resource_get_version(resource), id);
	if (!res) {
		free(device);
		wl_client_post_no_memory(client);
		return;
	}
	device->qdwin = qdwin;
	device->pseat = pseat;
	device->resource = res;
	wl_list_insert(&pseat->devices, &device->link);
	wl_resource_set_implementation(res, &qdwin_primary_device_impl,
				       device,
				       qdwin_primary_device_resource_destroy);
	/* If a selection is already active on this seat, announce it to
	 * the brand-new device. */
	if (pseat->current_source) {
		struct wl_resource *offer_res =
			qdwin_primary_build_offer_for_device(
				device, pseat->current_source);
		zwp_primary_selection_device_v1_send_selection(res, offer_res);
	}
}

static void
qdwin_primary_manager_destroy_req(struct wl_client *client,
				  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_device_manager_v1_interface
qdwin_primary_manager_impl = {
	.create_source = qdwin_primary_manager_create_source,
	.get_device    = qdwin_primary_manager_get_device,
	.destroy       = qdwin_primary_manager_destroy_req,
};

static void
bind_qdwin_primary_manager(struct wl_client *client, void *data,
			   uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource = wl_resource_create(
		client, &zwp_primary_selection_device_manager_v1_interface,
		version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &qdwin_primary_manager_impl,
				       qdwin, NULL);
}

static void
qdwin_primary_seat_seat_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_primary_seat *pseat =
		wl_container_of(l, pseat, seat_destroy_listener);
	struct qdwin_primary_device *device, *tmp;
	(void)data;
	wl_list_remove(&pseat->seat_destroy_listener.link);
	wl_list_init(&pseat->seat_destroy_listener.link);
	if (pseat->current_source)
		qdwin_primary_seat_clear_selection(pseat, 1);
	wl_list_for_each_safe(device, tmp, &pseat->devices, link)
		device->pseat = NULL;
	wl_list_remove(&pseat->link);
	free(pseat);
}

/* ------------------------------------------------------------------
 * §6.8 S0 qdwin_nested_v1 — stub.
 *
 * S0 ships bind + advertise_toplevel logging only. No proxy surface
 * creation yet. Rationale + staged plan in
 * . Shape-A (PipeWire per-
 * toplevel stream from nested → outer proxy surface) is locked in;
 * S1 wires the nested-side PipeWire publish, S2 wires the outer-side
 * proxy-surface creation + qdwin_shell_v1.toplevel_added integration,
 * S3 wires input injection via the companion PipeWire sink, S4 seals
 * lifecycle + authorization via admin-broker CheckPermission.
 * ------------------------------------------------------------------ */

/* Defined inside the §6.8 S0 nested block; stub closure of the forward
 * decl above. Lives here so the surface_added/_committed paths above
 * can reference it without needing the full nested_toplevel layout. */

struct qdwin_nested_toplevel {
	struct qdwin *qdwin;
	struct wl_resource *resource;
	char *pw_node;       /* §6.8 S1 v2: string identifier */
	char *input_sink;    /* §6.8 S1 v2: string identifier */
	char *app_id;
	char *title;
	uint32_t origin_uid;
	uint32_t configured_w;
	uint32_t configured_h;
	/* §6.8 S2: the qdwin_toplevel proxy synthesised from this advertise.
	 * Owns the placeholder curtain + view in the outer's normal_layer.
	 * NULL until proxy creation succeeds; cleared on proxy teardown. */
	struct qdwin_toplevel *proxy_tl;
	struct wl_list link;  /* qdwin::nested_toplevels */
};

static void
qdwin_nested_toplevel_destroy_req(struct wl_client *client,
				  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

/* §6.8 S2/S4: send close_requested on the nested-toplevel resource
 * owned by the given proxy. Defined here (after the full
 * qdwin_nested_toplevel struct) but called from request_close at the
 * top of the file via the forward decl. */
static void
qdwin_nested_proxy_send_close(struct qdwin_toplevel *tl)
{
	if (!tl || !tl->is_nested_proxy)
		return;
	if (tl->proxy_nested_owner && tl->proxy_nested_owner->resource)
		qdwin_nested_toplevel_v1_send_close_requested(
			tl->proxy_nested_owner->resource);
}

static void
qdwin_nested_toplevel_set_title(struct wl_client *client,
				struct wl_resource *resource,
				const char *title)
{
	struct qdwin_nested_toplevel *t =
		wl_resource_get_user_data(resource);
	(void)client;
	if (!t)
		return;
	free(t->title);
	t->title = title ? strdup(title) : NULL;
	weston_log("qdwin: nested-toplevel set_title app_id=%s title=%s\n",
		   t->app_id ? t->app_id : "",
		   t->title ? t->title : "");
	if (t->proxy_tl)
		qdwin_nested_proxy_set_title(t->proxy_tl, t->title);
}

static void
qdwin_nested_toplevel_set_app_id(struct wl_client *client,
				 struct wl_resource *resource,
				 const char *app_id)
{
	struct qdwin_nested_toplevel *t =
		wl_resource_get_user_data(resource);
	(void)client;
	if (!t)
		return;
	free(t->app_id);
	t->app_id = app_id ? strdup(app_id) : NULL;
	weston_log("qdwin: nested-toplevel set_app_id=%s\n",
		   t->app_id ? t->app_id : "");
	if (t->proxy_tl)
		qdwin_nested_proxy_set_app_id(t->proxy_tl, t->app_id);
}

static void
qdwin_nested_toplevel_set_geometry(struct wl_client *client,
				   struct wl_resource *resource,
				   int32_t w, int32_t h)
{
	struct qdwin_nested_toplevel *t =
		wl_resource_get_user_data(resource);
	(void)client;
	if (!t)
		return;
	t->configured_w = (w > 0) ? (uint32_t)w : 0;
	t->configured_h = (h > 0) ? (uint32_t)h : 0;
	weston_log("qdwin: nested-toplevel set_geometry=%dx%d\n", w, h);
	if (t->proxy_tl)
		qdwin_nested_proxy_set_geometry(t->proxy_tl, w, h);
}

static const struct qdwin_nested_toplevel_v1_interface
qdwin_nested_toplevel_impl = {
	.destroy      = qdwin_nested_toplevel_destroy_req,
	.set_title    = qdwin_nested_toplevel_set_title,
	.set_app_id   = qdwin_nested_toplevel_set_app_id,
	.set_geometry = qdwin_nested_toplevel_set_geometry,
};

static void
qdwin_nested_toplevel_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_nested_toplevel *t =
		wl_resource_get_user_data(resource);
	if (!t)
		return;
	if (t->proxy_tl) {
		t->proxy_tl->proxy_nested_owner = NULL;
		qdwin_nested_proxy_destroy(t->proxy_tl);
		t->proxy_tl = NULL;
	}
	wl_list_remove(&t->link);
	free(t->app_id);
	free(t->title);
	free(t->pw_node);
	free(t->input_sink);
	free(t);
}

static void
qdwin_nested_manager_destroy_req(struct wl_client *client,
				 struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_nested_manager_advertise_toplevel(struct wl_client *client,
					struct wl_resource *resource,
					uint32_t id,
					const char *pw_node,
					const char *input_sink,
					const char *app_id,
					const char *title,
					uint32_t origin_uid)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct wl_resource *tl_res = wl_resource_create(
		client, &qdwin_nested_toplevel_v1_interface,
		wl_resource_get_version(resource), id);
	if (!tl_res) {
		wl_client_post_no_memory(client);
		return;
	}
	struct qdwin_nested_toplevel *t = calloc(1, sizeof *t);
	if (!t) {
		wl_resource_destroy(tl_res);
		wl_client_post_no_memory(client);
		return;
	}
	t->qdwin       = qdwin;
	t->resource    = tl_res;
	t->pw_node     = pw_node    ? strdup(pw_node)    : NULL;
	t->input_sink  = input_sink ? strdup(input_sink) : NULL;
	t->app_id      = app_id     ? strdup(app_id)     : NULL;
	t->title       = title      ? strdup(title)      : NULL;
	t->origin_uid  = origin_uid;
	wl_list_insert(&qdwin->nested_toplevels, &t->link);

	wl_resource_set_implementation(tl_res,
				       &qdwin_nested_toplevel_impl, t,
				       qdwin_nested_toplevel_resource_destroy);

	weston_log("qdwin: nested-toplevel advertise pw_node='%s' "
		   "input_sink='%s' app_id=%s title=%s origin_uid=%u\n",
		   t->pw_node    ? t->pw_node    : "",
		   t->input_sink ? t->input_sink : "",
		   t->app_id     ? t->app_id     : "",
		   t->title      ? t->title      : "",
		   origin_uid);

	/* §6.8 S2: synthesise the outer-side proxy toplevel + curtain. */
	t->proxy_tl = qdwin_nested_proxy_create(qdwin, t,
					       t->app_id, t->title,
					       origin_uid, 800, 600);
	/* §6.8 S3: connect to the nested-published input sink and send
	 * the wire-format-proving PING. Real motion/button/key encoders
	 * arrive in S3b. The fd persists on the proxy until destroy. */
	if (t->proxy_tl) {
		t->proxy_tl->proxy_input_sink_fd = -1;
		if (input_sink && *input_sink) {
			int fd = qdwin_nested_input_sink_connect(input_sink);
			if (fd >= 0) {
				t->proxy_tl->proxy_input_sink_fd = fd;
				if (qdwin_nested_input_sink_send(fd, 1, NULL, 0) == 0)
					weston_log("qdwin/nested-proxy: "
						   "input-sink PING sent "
						   "handle=%u path=%s\n",
						   t->proxy_tl->handle,
						   input_sink);
				/* §6.8 S3b synthetic injection (env-gated):
				 * sends a motion/button/key/axis/focus burst
				 * for the bats decoder test. Production input
				 * forwarding goes through the pointer/keyboard
				 * grab paths, not this one. */
				if (getenv("QDWIN_NESTED_S3B_TEST")) {
					qdwin_nested_input_sink_send_focus(
						fd, 1);
					qdwin_nested_input_sink_send_motion(
						fd, 0,
						wl_fixed_from_int(120),
						wl_fixed_from_int(80));
					qdwin_nested_input_sink_send_button(
						fd, 0, 0x110 /*BTN_LEFT*/, 1);
					qdwin_nested_input_sink_send_button(
						fd, 0, 0x110, 0);
					qdwin_nested_input_sink_send_key(
						fd, 0, 30 /*KEY_A*/, 1);
					qdwin_nested_input_sink_send_key(
						fd, 0, 30, 0);
					qdwin_nested_input_sink_send_axis(
						fd, 0, 0 /*vert*/,
						wl_fixed_from_int(15));
					qdwin_nested_input_sink_send_focus(
						fd, 0);
					weston_log("qdwin/nested-proxy: "
						   "S3b synthetic burst sent "
						   "handle=%u\n",
						   t->proxy_tl->handle);
				}
				/* §6.8 S3c keyboard-grab synthetic test
				 * (env-gated): forces active_input_proxy to
				 * this proxy so the default keyboard grab
				 * encodes QDNI keys, then drives notify_key
				 * on the first seat with a keyboard. The
				 * grab callback's QDNI emission is the
				 * acceptance criterion — nested decodes a
				 * key whose wire entry was a real keyboard
				 * grab call rather than a direct send_key.
				 *
				 * Uses KEY_S (31) so it's distinct from the
				 * S3b KEY_A (30) burst, even when both env
				 * vars happen to be set in the same run. */
				if (getenv("QDWIN_NESTED_S3C_TEST")) {
					struct qdwin_toplevel *saved =
						qdwin->active_input_proxy;
					qdwin_nested_input_sink_send_focus(
						fd, 1);
					qdwin->active_input_proxy = t->proxy_tl;
					struct weston_seat *seat;
					struct timespec ts = {0, 0};
					int dispatched = 0;
					wl_list_for_each(seat,
						&qdwin->compositor->seat_list,
						link) {
						struct weston_keyboard *kb =
							weston_seat_get_keyboard(seat);
						if (!kb)
							continue;
						notify_key(seat, &ts,
							31 /*KEY_S*/,
							WL_KEYBOARD_KEY_STATE_PRESSED,
							STATE_UPDATE_AUTOMATIC);
						notify_key(seat, &ts,
							31,
							WL_KEYBOARD_KEY_STATE_RELEASED,
							STATE_UPDATE_AUTOMATIC);
						dispatched = 1;
						break;
					}
					qdwin_nested_input_sink_send_focus(
						fd, 0);
					qdwin->active_input_proxy = saved;
					weston_log("qdwin/nested-proxy: "
						   "S3c keyboard-grab burst "
						   "handle=%u dispatched=%d\n",
						   t->proxy_tl->handle,
						   dispatched);
				}
			} else {
				weston_log("qdwin/nested-proxy: input-sink "
					   "connect failed handle=%u path=%s\n",
					   t->proxy_tl->handle, input_sink);
			}
		}
	}
	int cw = t->proxy_tl ? t->proxy_tl->last_width  : 800;
	int ch = t->proxy_tl ? t->proxy_tl->last_height : 600;

	qdwin_nested_toplevel_v1_send_configured(tl_res, cw, ch);
	t->configured_w = cw;
	t->configured_h = ch;
}

static const struct qdwin_nested_manager_v1_interface
qdwin_nested_manager_impl = {
	.destroy            = qdwin_nested_manager_destroy_req,
	.advertise_toplevel = qdwin_nested_manager_advertise_toplevel,
};

/* §P10: callback referenced only when role=host; see bind_qdwin_locker
 * note above. */
__attribute__((unused))
static void
bind_qdwin_nested_manager(struct wl_client *client, void *data,
			  uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;

	/* Peer-uid filter: same rule as qdwin_shell_v1 — only the
	 * allowed_uid may bind. Nested-compositor clients run as the
	 * user hosting the session. */
	if (qdwin->allowed_uid != (uid_t)-1) {
		uid_t uid;
		pid_t pid;
		gid_t gid;
		wl_client_get_credentials(client, &pid, &uid, &gid);
		if (uid != qdwin->allowed_uid) {
			weston_log("qdwin: nested bind refused uid=%u (allowed=%u)\n",
				   (unsigned)uid, (unsigned)qdwin->allowed_uid);
			wl_client_post_implementation_error(
				client,
				"qdwin_nested: peer-uid filter rejected bind");
			return;
		}
	}

	struct wl_resource *resource = wl_resource_create(
		client, &qdwin_nested_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource,
				       &qdwin_nested_manager_impl,
				       qdwin, NULL);
	weston_log("qdwin: nested_manager bound v%u\n", version);
}

/* ------------------------------------------------------------------
 * §6.7 xdg-activation-v1.
 *
 * Focus-stealing-prevention + launcher tokens. Two interfaces:
 *   xdg_activation_v1        — one global; issues tokens, consumes
 *                              them in activate(token, surface).
 *   xdg_activation_token_v1  — per-get_activation_token handle; the
 *                              client accumulates (serial, seat,
 *                              app_id, requesting_surface) via
 *                              set_*, then commits to receive `done`.
 *
 * Policy today: tokens are single-use and anonymous. Any valid
 * token grants activation; the compositor raises the target
 * toplevel to the normal layer's top. This matches weston's own
 * behaviour at this version. A future policy gate can reject
 * tokens by requester-uid, seat staleness, or app_id mismatch
 * without changing the wire protocol.
 * ------------------------------------------------------------------ */

struct qdwin_activation_token {
	struct qdwin *qdwin;
	struct wl_resource *token_resource;  /* xdg_activation_token_v1 */
	char *token;                         /* NULL until commit */
	char *app_id;
	struct weston_surface *requesting_surface;
	struct wl_listener requesting_surface_destroy;
	uint32_t serial;
	struct wl_list link;                 /* qdwin::activation_tokens */
	int committed;
	int used;
};

/* Defined below the qdwin_activation_pending struct — clears any
 * `ap->token == t` back-references so freeing `t` here can't leave
 * a dangling pointer on the pending list. */
static void qdwin_activation_pending_drop_token_refs(
	struct qdwin *qdwin, struct qdwin_activation_token *t);

static void
qdwin_activation_token_free(struct qdwin_activation_token *t)
{
	wl_list_remove(&t->link);
	if (t->requesting_surface)
		wl_list_remove(&t->requesting_surface_destroy.link);
	/* Sever the wl_resource → t back-reference. Many call paths
	 * (perform/use/timeout/cancel) free `t` while the client's
	 * xdg_activation_token_v1 resource is still alive; when the
	 * client later destroys it, qdwin_activation_token_resource_destroy
	 * fires and dereferences the user_data. Without this NULL-out,
	 * that destructor re-enters this function on a freed `t` and
	 * SEGVs in wl_list_remove. */
	if (t->token_resource)
		wl_resource_set_user_data(t->token_resource, NULL);
	/* The token may also be referenced from a pending activation
	 * (`ap->token`) that's still waiting on a shell decision or
	 * its 10s safety timeout. If the client destroys the token
	 * resource OR a perform/cancel path frees `t` while an `ap`
	 * still owns it, the next `qdwin_activation_pending_free`
	 * would call `qdwin_activation_token_free(ap->token)` on the
	 * already-freed `t` and SEGV in `wl_list_remove(&t->link)`.
	 * The pending-struct layout is defined further down; defer
	 * the back-reference scrub to a helper below. */
	qdwin_activation_pending_drop_token_refs(t->qdwin, t);
	free(t->token);
	free(t->app_id);
	free(t);
}

static struct qdwin_activation_token *
qdwin_activation_token_find(struct qdwin *qdwin, const char *token)
{
	struct qdwin_activation_token *t;
	if (!token)
		return NULL;
	wl_list_for_each(t, &qdwin->activation_tokens, link) {
		if (t->token && !t->used && strcmp(t->token, token) == 0)
			return t;
	}
	return NULL;
}

static void
qdwin_generate_token(char out[33])
{
	unsigned char raw[16];
	static const char hex[] = "0123456789abcdef";
	ssize_t n = getrandom(raw, sizeof raw, 0);
	if (n != (ssize_t)sizeof raw) {
		/* Fallback: coarse clock xor. Good enough for a non-secret
		 * correlation id; the token's threat model is replay within
		 * a single session. */
		for (size_t i = 0; i < sizeof raw; i++)
			raw[i] = (unsigned char)(i * 37u + (unsigned)time(NULL));
	}
	for (int i = 0; i < 16; i++) {
		out[i * 2]     = hex[(raw[i] >> 4) & 0xf];
		out[i * 2 + 1] = hex[raw[i]        & 0xf];
	}
	out[32] = '\0';
}

static void
qdwin_activation_requesting_surface_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_activation_token *t =
		wl_container_of(l, t, requesting_surface_destroy);
	(void)data;
	wl_list_remove(&t->requesting_surface_destroy.link);
	t->requesting_surface = NULL;
}

static void
qdwin_activation_token_destroy(struct wl_client *client,
			       struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_activation_token_set_serial(struct wl_client *client,
				  struct wl_resource *resource,
				  uint32_t serial,
				  struct wl_resource *seat_resource)
{
	struct qdwin_activation_token *t = wl_resource_get_user_data(resource);
	(void)client;
	(void)seat_resource;
	if (!t || t->committed)
		return;
	t->serial = serial;
}

static void
qdwin_activation_token_set_app_id(struct wl_client *client,
				  struct wl_resource *resource,
				  const char *app_id)
{
	struct qdwin_activation_token *t = wl_resource_get_user_data(resource);
	(void)client;
	if (!t || t->committed)
		return;
	free(t->app_id);
	t->app_id = app_id ? strdup(app_id) : NULL;
}

static void
qdwin_activation_token_set_surface(struct wl_client *client,
				   struct wl_resource *resource,
				   struct wl_resource *surface_resource)
{
	struct qdwin_activation_token *t = wl_resource_get_user_data(resource);
	struct weston_surface *surface =
		surface_resource ? wl_resource_get_user_data(surface_resource)
				 : NULL;
	(void)client;
	if (!t || t->committed)
		return;
	if (t->requesting_surface) {
		wl_list_remove(&t->requesting_surface_destroy.link);
		t->requesting_surface = NULL;
	}
	if (surface) {
		t->requesting_surface = surface;
		t->requesting_surface_destroy.notify =
			qdwin_activation_requesting_surface_destroyed;
		wl_signal_add(&surface->destroy_signal,
			      &t->requesting_surface_destroy);
	}
}

static void
qdwin_activation_token_commit(struct wl_client *client,
			      struct wl_resource *resource)
{
	struct qdwin_activation_token *t = wl_resource_get_user_data(resource);
	char token_buf[33];
	(void)client;
	if (!t || t->committed)
		return;
	qdwin_generate_token(token_buf);
	free(t->token);
	t->token = strdup(token_buf);
	t->committed = 1;
	t->qdwin->activation_token_counter++;
	xdg_activation_token_v1_send_done(resource,
					  t->token ? t->token : "");
	weston_log("qdwin: xdg-activation token issued #%u app_id=%s\n",
		   (unsigned)t->qdwin->activation_token_counter,
		   t->app_id ? t->app_id : "(none)");
}

static const struct xdg_activation_token_v1_interface
qdwin_activation_token_impl = {
	.destroy    = qdwin_activation_token_destroy,
	.set_serial = qdwin_activation_token_set_serial,
	.set_app_id = qdwin_activation_token_set_app_id,
	.set_surface= qdwin_activation_token_set_surface,
	.commit     = qdwin_activation_token_commit,
};

static void
qdwin_activation_token_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_activation_token *t = wl_resource_get_user_data(resource);
	if (t)
		qdwin_activation_token_free(t);
}

static void
qdwin_activation_destroy(struct wl_client *client,
			 struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_activation_get_activation_token(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t id)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_activation_token *t = calloc(1, sizeof *t);
	struct wl_resource *tok_resource;
	if (!t) {
		wl_client_post_no_memory(client);
		return;
	}
	tok_resource = wl_resource_create(
		client, &xdg_activation_token_v1_interface,
		wl_resource_get_version(resource), id);
	if (!tok_resource) {
		free(t);
		wl_client_post_no_memory(client);
		return;
	}
	t->qdwin = qdwin;
	t->token_resource = tok_resource;
	wl_list_insert(&qdwin->activation_tokens, &t->link);
	wl_resource_set_implementation(tok_resource,
				       &qdwin_activation_token_impl,
				       t,
				       qdwin_activation_token_resource_destroy);
}

static struct qdwin_toplevel *
qdwin_toplevel_from_wl_surface(struct qdwin *qdwin,
			       struct weston_surface *surface)
{
	struct qdwin_toplevel *tl;
	if (!surface)
		return NULL;
	wl_list_for_each(tl, &qdwin->toplevels, link) {
		if (!tl->desktop_surface)
			continue;
		if (weston_desktop_surface_get_surface(tl->desktop_surface)
		    == surface)
			return tl;
	}
	return NULL;
}

/* spec/09: pending xdg-activation, stalled until the shell calls
 * activation_decision. Owned by qdwin::activation_pending; freed on
 * either decision. Carries the original token and target so the
 * decision-handler can resume the activate body without re-walking
 * the protocol arguments. */
struct qdwin_activation_pending {
	struct qdwin *qdwin;
	uint32_t handle;
	struct qdwin_activation_token *token;
	struct qdwin_toplevel *target_tl;
	struct wl_listener target_destroy_listener;
	struct wl_event_source *timeout_source;
	struct wl_list link;  /* qdwin::activation_pending */
};

static void
qdwin_activation_pending_drop_token_refs(struct qdwin *qdwin,
					 struct qdwin_activation_token *t)
{
	struct qdwin_activation_pending *ap;
	wl_list_for_each(ap, &qdwin->activation_pending, link) {
		if (ap->token == t)
			ap->token = NULL;
	}
}

static void
qdwin_activation_pending_free(struct qdwin_activation_pending *ap)
{
	wl_list_remove(&ap->link);
	if (ap->timeout_source)
		wl_event_source_remove(ap->timeout_source);
	wl_list_remove(&ap->target_destroy_listener.link);
	if (ap->token)
		qdwin_activation_token_free(ap->token);
	free(ap);
}

static void
qdwin_activation_pending_free_all(struct qdwin *qdwin)
{
	struct qdwin_activation_pending *ap, *tmp;
	wl_list_for_each_safe(ap, tmp, &qdwin->activation_pending, link)
		qdwin_activation_pending_free(ap);
}

static void
qdwin_activation_pending_target_destroyed(struct wl_listener *l, void *data)
{
	struct qdwin_activation_pending *ap =
		wl_container_of(l, ap, target_destroy_listener);
	(void)data;
	weston_log("qdwin: activation_pending handle=%u target destroyed "
		   "before decision\n", ap->handle);
	qdwin_activation_pending_free(ap);
}

/* Run the original raise+focus body. Caller must NOT use `t` after
 * this — perform consumes it. */
static void
qdwin_activation_perform(struct qdwin *qdwin,
			 struct qdwin_toplevel *tl,
			 struct qdwin_activation_token *t)
{
	struct weston_seat *seat = NULL;
	if (tl->decorated) {
		weston_view_move_to_layer(tl->view,
					  &qdwin->normal_layer.view_list);
	}
	if (!wl_list_empty(&qdwin->compositor->seat_list)) {
		seat = wl_container_of(qdwin->compositor->seat_list.next,
				       seat, link);
	}
	if (seat) {
		weston_view_activate_input(tl->view, seat,
					   WESTON_ACTIVATE_FLAG_CONFIGURE);
	}
	weston_log("qdwin: xdg-activation activated handle=%u app_id=%s\n",
		   tl->handle,
		   t && t->app_id ? t->app_id : "(none)");
	if (t)
		qdwin_activation_token_free(t);
}

static int
qdwin_activation_pending_timeout_cb(void *data)
{
	struct qdwin_activation_pending *ap = data;
	weston_log("qdwin: activation_pending handle=%u timed out → deny\n",
		   ap->handle);
	ap->timeout_source = NULL;  /* libwayland frees on cb return for one-shot */
	qdwin_activation_pending_free(ap);
	return 0;
}

static struct qdwin_activation_pending *
qdwin_activation_pending_find(struct qdwin *qdwin, uint32_t handle)
{
	struct qdwin_activation_pending *ap;
	wl_list_for_each(ap, &qdwin->activation_pending, link)
		if (ap->handle == handle)
			return ap;
	return NULL;
}

static int
qdwin_shell_can_receive_v12(struct qdwin *qdwin)
{
	return qdwin->shell_bound && qdwin->shell_resource &&
	       wl_resource_get_version(qdwin->shell_resource) >= 12;
}

static void
qdwin_activation_activate(struct wl_client *client,
			  struct wl_resource *resource,
			  const char *token_str,
			  struct wl_resource *surface_resource)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct weston_surface *surface =
		surface_resource ? wl_resource_get_user_data(surface_resource)
				 : NULL;
	struct qdwin_activation_token *t;
	struct qdwin_toplevel *tl;

	(void)client;

	t = qdwin_activation_token_find(qdwin, token_str);
	if (!t) {
		weston_log("qdwin: xdg-activation activate with unknown token\n");
		return;
	}
	t->used = 1;

	tl = qdwin_toplevel_from_wl_surface(qdwin, surface);
	if (!tl) {
		weston_log("qdwin: xdg-activation target surface has no toplevel\n");
		qdwin_activation_token_free(t);
		return;
	}

	/* spec/09: gate via shell-broker if the shell speaks v12,
	 * otherwise auto-allow (legacy behaviour). The shell decides
	 * same-silo (trivial allow) vs cross-silo (broker-rule-or-deny). */
	if (!qdwin_shell_can_receive_v12(qdwin)) {
		qdwin_activation_perform(qdwin, tl, t);
		return;
	}

	struct qdwin_activation_pending *ap =
		calloc(1, sizeof *ap);
	if (!ap) {
		/* fail closed on alloc failure: gated activation must not auto-allow */
		weston_log("qdwin: activation_pending calloc failed → deny "
			   "(token consumed) app_id=%s\n",
			   t->app_id ? t->app_id : "(none)");
		qdwin_activation_token_free(t);
		return;
	}
	ap->qdwin = qdwin;
	ap->handle = ++qdwin->activation_pending_next_handle;
	ap->token = t;
	ap->target_tl = tl;
	ap->target_destroy_listener.notify =
		qdwin_activation_pending_target_destroyed;
	wl_signal_add(&surface->destroy_signal,
		      &ap->target_destroy_listener);
	wl_list_insert(&qdwin->activation_pending, &ap->link);

	/* 10s safety timeout — broker should answer in <100ms; if the
	 * shell is wedged we don't want pending state to accumulate. */
	struct wl_event_loop *loop =
		wl_display_get_event_loop(qdwin->compositor->wl_display);
	ap->timeout_source = wl_event_loop_add_timer(
		loop, qdwin_activation_pending_timeout_cb, ap);
	if (ap->timeout_source)
		wl_event_source_timer_update(ap->timeout_source, 10000);

	struct qdwin_toplevel *src_tl =
		qdwin_toplevel_from_wl_surface(qdwin, t->requesting_surface);
	uint32_t src_handle = src_tl ? src_tl->handle : UINT32_MAX;
	uint32_t tgt_handle = tl->handle;
	qdwin_shell_v1_send_activation_pending(qdwin->shell_resource,
					       ap->handle,
					       src_handle, tgt_handle,
					       t->app_id ? t->app_id : "");
	weston_log("qdwin: activation_pending handle=%u src=%u tgt=%u "
		   "app_id=%s\n",
		   ap->handle, src_handle, tgt_handle,
		   t->app_id ? t->app_id : "");
}

/* spec/09: shell answers an activation_pending. */
static void
qdwin_handle_activation_decision(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t handle,
				 uint32_t decision,
				 const char *reason)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	(void)client;
	struct qdwin_activation_pending *ap =
		qdwin_activation_pending_find(qdwin, handle);
	if (!ap) {
		weston_log("qdwin: activation_decision unknown handle=%u\n",
			   handle);
		return;
	}
	struct qdwin_activation_token *t = ap->token;
	struct qdwin_toplevel *tl = ap->target_tl;
	ap->token = NULL;  /* perform consumes it */
	wl_list_remove(&ap->link);
	if (ap->timeout_source) {
		wl_event_source_remove(ap->timeout_source);
		ap->timeout_source = NULL;
	}
	wl_list_remove(&ap->target_destroy_listener.link);
	wl_list_init(&ap->target_destroy_listener.link);
	free(ap);
	if (decision == 0) {
		weston_log("qdwin: activation_decision handle=%u → allow\n",
			   handle);
		qdwin_activation_perform(qdwin, tl, t);
	} else {
		weston_log("qdwin: activation_decision handle=%u → "
			   "%s reason=%s\n", handle,
			   decision == 1 ? "deny" : "defer",
			   reason ? reason : "");
		if (t)
			qdwin_activation_token_free(t);
	}
}

static const struct xdg_activation_v1_interface qdwin_activation_impl = {
	.destroy              = qdwin_activation_destroy,
	.get_activation_token = qdwin_activation_get_activation_token,
	.activate             = qdwin_activation_activate,
};

static void
bind_xdg_activation(struct wl_client *client, void *data,
		    uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *resource = wl_resource_create(
		client, &xdg_activation_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &qdwin_activation_impl,
				       qdwin, NULL);
}

/* ------------------------------------------------------------------
 * zxdg_decoration_manager_v1 — always-server_side stub.
 *
 * Background. qdwin's design has qdshell draw chrome via the private
 * qdwin_shell_v1 protocol, so the compositor never wants toolkit-drawn
 * CSDs (they would stack on top of qdshell's chrome — see
 * tests/gui/AGENTS.md pitfall #2 for the foot case). The natural way
 * to tell toolkits "skip CSD" is to advertise xdg-decoration-v1 and
 * always reply with mode=server_side; Qt-Wayland and GTK4 both honour
 * this. We don't actually draw any decoration in response — qdshell
 * does that via qdwin_shell_v1.attach_decoration.
 *
 * Without this global, PyQt6 (Qt-Wayland built-in bradient/adwaita
 * CSD plugin) aborts in QApplication during first-widget construction:
 *
 *     Fatal Python error: Aborted
 *       File "<string>", line N in <module>
 *     Extension modules: PyQt6.QtCore, PyQt6.QtGui, PyQt6.QtWidgets
 *
 * Set QT_WAYLAND_DISABLE_WINDOWDECORATION=1 and the abort disappears,
 * which is what isolated the crash to the CSD attach path. With this
 * stub global the abort also disappears because Qt opts out of CSD
 * on its own. See todo/qdwin-crash.md.
 * ------------------------------------------------------------------ */

static void
qdwin_toplevel_decoration_destroy(struct wl_client *client,
				  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_toplevel_decoration_set_mode(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t mode)
{
	(void)client; (void)mode;
	/* Whatever the client asks for, we mandate server_side. */
	zxdg_toplevel_decoration_v1_send_configure(
		resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void
qdwin_toplevel_decoration_unset_mode(struct wl_client *client,
				     struct wl_resource *resource)
{
	(void)client;
	zxdg_toplevel_decoration_v1_send_configure(
		resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_toplevel_decoration_v1_interface
qdwin_toplevel_decoration_impl = {
	.destroy    = qdwin_toplevel_decoration_destroy,
	.set_mode   = qdwin_toplevel_decoration_set_mode,
	.unset_mode = qdwin_toplevel_decoration_unset_mode,
};

static void
qdwin_xdg_decoration_manager_destroy(struct wl_client *client,
				     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_xdg_decoration_manager_get_toplevel_decoration(
	struct wl_client *client,
	struct wl_resource *manager_resource,
	uint32_t id,
	struct wl_resource *xdg_toplevel_resource)
{
	(void)manager_resource; (void)xdg_toplevel_resource;
	uint32_t version = wl_resource_get_version(manager_resource);
	struct wl_resource *dec = wl_resource_create(
		client, &zxdg_toplevel_decoration_v1_interface, version, id);
	if (!dec) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(dec, &qdwin_toplevel_decoration_impl,
				       NULL, NULL);
	/* Send the initial configure immediately. The client must ack
	 * via xdg_surface.ack_configure before attaching a buffer; Qt
	 * and GTK both handle a same-frame initial mode correctly. */
	zxdg_toplevel_decoration_v1_send_configure(
		dec, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_decoration_manager_v1_interface
qdwin_xdg_decoration_manager_impl = {
	.destroy                 = qdwin_xdg_decoration_manager_destroy,
	.get_toplevel_decoration =
		qdwin_xdg_decoration_manager_get_toplevel_decoration,
};

static void
bind_qdwin_xdg_decoration_manager(struct wl_client *client, void *data,
				  uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(
		client, &zxdg_decoration_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource,
				       &qdwin_xdg_decoration_manager_impl,
				       data, NULL);
}

/* ------------------------------------------------------------------
 * §6.8 S1 — nested-mode publisher.
 *
 * When QDWIN_NESTED_MODE=1 is set in the environment, qdwin-shell.so
 * is being loaded by a *nested* weston (one whose backend is
 * `wayland-backend.so` talking to the *outer* qdwin). Instead of
 * acting as the admin shell with chrome / panel / launcher, this
 * shell acts as a publisher:
 *
 *   1. Opens a separate wl_display connection to the outer qdwin
 *      (env QDWIN_OUTER_DISPLAY, default "wayland-0"). Reuses the
 *      shell's allowed_uid for peer-uid filtering on the outer.
 *   2. Loads the pipewire-output API; expects backend-pipewire to
 *      be present in the nested compositor (configure via
 *      `[core] backend=wayland-backend.so,pipewire-backend.so`).
 *   3. On each new desktop surface (= inner xdg_toplevel of an
 *      inner wayland client), allocates a dedicated pipewire output
 *      and pins the toplevel onto it. The pipewire output's frames
 *      become a PipeWire stream node visible on the host's
 *      PipeWire daemon — same uid as the outer qdwin, so the
 *      outer's libpipewire client can resolve it.
 *   4. Calls advertise_toplevel(pw_node, input_sink, app_id, title,
 *      origin_uid) on the outer's qdwin_nested_manager_v1 (bound
 *      via the separate connection). pw_node is a string of form
 *      "weston.pipewire:<nested-pid>:<weston-output-name>".
 *      input_sink is empty for S1 (deferred to S3).
 *
 * S1 scope: lifecycle plumbing + pixel publish. The outer side's
 * proxy surface creation is S2; input injection is S3.
 *
 * Failure modes (all log + degrade gracefully):
 *
 *   - QDWIN_OUTER_DISPLAY missing or wl_display_connect fails:
 *     nested mode disabled with a single-line warning. Inner
 *     toplevels render normally inside the nested compositor; the
 *     §6.6 S6 outer-toplevel-fallback shape continues to apply.
 *   - Manager not advertised on outer (peer-uid rejected): same.
 *   - pipewire-output API unavailable: log + run inner clients
 *     without per-toplevel publish.
 *
 * ------------------------------------------------------------------ */

static int
qdwin_nested_outer_fd_handler(int fd, uint32_t mask, void *data)
{
	struct qdwin *qdwin = data;
	(void)fd;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		weston_log("qdwin: nested-mode outer connection closed\n");
		return 0;
	}
	int n = qdwin_nested_client_dispatch_pending(qdwin->nested_client);
	if (n < 0) {
		weston_log("qdwin: nested-mode outer dispatch error\n");
		return 0;
	}
	qdwin_nested_client_flush(qdwin->nested_client);
	return n;
}

static void
qdwin_nested_init(struct qdwin *qdwin)
{
	const char *enabled = getenv("QDWIN_NESTED_MODE");
	if (!enabled || strcmp(enabled, "1") != 0)
		return;

	const char *outer = getenv("QDWIN_OUTER_DISPLAY");
	if (!outer || !*outer)
		outer = "wayland-0";

	weston_log("qdwin: NESTED_MODE on; pid=%d outer=%s\n",
		   (int)getpid(), outer);

	/* Hard isolate the outer connection from the nested compositor's
	 * own server: drop any inherited WAYLAND_SOCKET (used by parent-
	 * launched children to reuse a pre-opened fd) so wl_display_connect
	 * actually opens a fresh socket to `outer`. Restore after. */
	const char *saved_wsock = getenv("WAYLAND_SOCKET");
	if (saved_wsock)
		unsetenv("WAYLAND_SOCKET");

	qdwin->nested_client = qdwin_nested_client_new(outer);

	if (saved_wsock)
		setenv("WAYLAND_SOCKET", saved_wsock, 1);
	if (qdwin_nested_client_failed(qdwin->nested_client)) {
		weston_log("qdwin: nested-mode disabled — outer "
			   "qdwin_nested_manager_v1 unavailable on display=%s\n",
			   outer);
		if (qdwin->nested_client) {
			qdwin_nested_client_destroy(qdwin->nested_client);
			qdwin->nested_client = NULL;
		}
		return;
	}

	int fd = qdwin_nested_client_get_fd(qdwin->nested_client);
	struct wl_event_loop *loop =
		wl_display_get_event_loop(qdwin->compositor->wl_display);
	qdwin->nested_outer_event_source = wl_event_loop_add_fd(
		loop, fd, WL_EVENT_READABLE,
		qdwin_nested_outer_fd_handler, qdwin);
	if (!qdwin->nested_outer_event_source) {
		weston_log("qdwin: nested-mode disabled — event loop "
			   "integration failed\n");
		qdwin_nested_client_destroy(qdwin->nested_client);
		qdwin->nested_client = NULL;
		return;
	}

	/* Count pre-allocated pipewire outputs in the nested compositor
	 * (from [pipewire] num-outputs=N in weston.ini). Each inner
	 * toplevel pins to one; if pool is empty we publish without
	 * pixels. */
	int pw_pool = 0;
	struct weston_output *o;
	wl_list_for_each(o, &qdwin->compositor->output_list, link) {
		if (strncmp(o->name, "pipewire", 8) == 0)
			pw_pool++;
	}

	qdwin->nested_mode = true;
	qdwin->nested_next_pw_id = 0;
	weston_log("qdwin: nested-mode publisher ready (outer manager v%u; "
		   "pipewire-output pool=%d)\n",
		   qdwin_nested_client_manager_version(qdwin->nested_client),
		   pw_pool);
}

/* §6.8 S3b: lazy init for the per-toplevel inner-seat. First non-PING
 * packet arrival triggers this; PING-only S3-mvp probes never pay it. */
static void
qdwin_nested_inner_seat_init(struct qdwin_toplevel *tl)
{
	if (tl->nested_inner_seat_inited)
		return;
	struct qdwin *qdwin = tl->qdwin;
	char seat_name[64];
	snprintf(seat_name, sizeof seat_name,
		 "qdwin-nested-T%u", tl->handle);
	weston_seat_init(&tl->nested_inner_seat, qdwin->compositor, seat_name);
	if (weston_seat_init_pointer(&tl->nested_inner_seat) < 0)
		weston_log("qdwin/nested: inner-seat init_pointer failed "
			   "handle=%u\n", tl->handle);
	struct xkb_keymap *keymap = NULL;
	if (qdwin->compositor->xkb_info)
		keymap = qdwin->compositor->xkb_info->keymap;
	if (weston_seat_init_keyboard(&tl->nested_inner_seat, keymap) < 0)
		weston_log("qdwin/nested: inner-seat init_keyboard failed "
			   "handle=%u\n", tl->handle);
	tl->nested_inner_seat_inited = true;
	weston_log("qdwin/nested: inner-seat '%s' ready\n", seat_name);
}

static void
qdwin_nested_inner_seat_release(struct qdwin_toplevel *tl)
{
	if (!tl->nested_inner_seat_inited)
		return;
	weston_seat_release_keyboard(&tl->nested_inner_seat);
	weston_seat_release_pointer(&tl->nested_inner_seat);
	weston_seat_release(&tl->nested_inner_seat);
	tl->nested_inner_seat_inited = false;
}

/* Pin pointer + keyboard focus to the inner toplevel's view/surface so
 * notify_motion_absolute / notify_button / notify_key route to the
 * inner client. Cheap when focus is already correct. */
static void
qdwin_nested_inner_seat_assert_focus(struct qdwin_toplevel *tl)
{
	if (!tl->nested_inner_seat_inited || !tl->view || !tl->view->surface)
		return;
	struct weston_pointer *ptr =
		weston_seat_get_pointer(&tl->nested_inner_seat);
	if (ptr && ptr->focus != tl->view)
		weston_pointer_set_focus(ptr, tl->view);
	struct weston_keyboard *kbd =
		weston_seat_get_keyboard(&tl->nested_inner_seat);
	if (kbd && kbd->focus != tl->view->surface)
		weston_keyboard_set_focus(kbd, tl->view->surface);
}

static struct timespec
qdwin_nested_ts_from_msec(uint32_t time_msec)
{
	struct timespec ts;
	ts.tv_sec  = time_msec / 1000u;
	ts.tv_nsec = (long)(time_msec % 1000u) * 1000000L;
	return ts;
}

static void
qdwin_nested_dispatch_motion(struct qdwin_toplevel *tl,
			     const struct qdni_motion_payload *p)
{
	qdwin_nested_inner_seat_init(tl);
	qdwin_nested_inner_seat_assert_focus(tl);
	if (!tl->view || !tl->view->surface)
		return;
	weston_view_update_transform(tl->view);
	struct weston_coord_surface cs = {
		.c = { .x = wl_fixed_to_double(p->x_fixed),
		       .y = wl_fixed_to_double(p->y_fixed) },
		.coordinate_space_id = tl->view->surface,
	};
	struct weston_coord_global gpos =
		weston_coord_surface_to_global(tl->view, cs);
	struct timespec ts = qdwin_nested_ts_from_msec(p->time_msec);
	notify_motion_absolute(&tl->nested_inner_seat, &ts, gpos);
}

static void
qdwin_nested_dispatch_button(struct qdwin_toplevel *tl,
			     const struct qdni_button_payload *p)
{
	qdwin_nested_inner_seat_init(tl);
	qdwin_nested_inner_seat_assert_focus(tl);
	struct timespec ts = qdwin_nested_ts_from_msec(p->time_msec);
	notify_button(&tl->nested_inner_seat, &ts, (int32_t)p->button,
		      p->state ? WL_POINTER_BUTTON_STATE_PRESSED
			       : WL_POINTER_BUTTON_STATE_RELEASED);
}

static void
qdwin_nested_dispatch_key(struct qdwin_toplevel *tl,
			  const struct qdni_key_payload *p)
{
	qdwin_nested_inner_seat_init(tl);
	qdwin_nested_inner_seat_assert_focus(tl);
	struct timespec ts = qdwin_nested_ts_from_msec(p->time_msec);
	notify_key(&tl->nested_inner_seat, &ts, p->key,
		   p->state ? WL_KEYBOARD_KEY_STATE_PRESSED
			    : WL_KEYBOARD_KEY_STATE_RELEASED,
		   STATE_UPDATE_AUTOMATIC);
}

static void
qdwin_nested_dispatch_axis(struct qdwin_toplevel *tl,
			   const struct qdni_axis_payload *p)
{
	qdwin_nested_inner_seat_init(tl);
	struct weston_pointer_axis_event ev = {
		.axis = p->axis,
		.value = wl_fixed_to_double(p->value_fixed),
		.has_discrete = false,
		.discrete = 0,
	};
	struct timespec ts = qdwin_nested_ts_from_msec(p->time_msec);
	notify_axis(&tl->nested_inner_seat, &ts, &ev);
}

/* §6.8 S3/S3b: peer-fd readable callback — drain one packet at a time.
 * S3-mvp PING is the wire-format proof; S3b decodes motion / button /
 * key / axis / focus via the per-toplevel inner-seat. */
static int
qdwin_nested_input_sink_peer_cb(int fd, uint32_t mask, void *data)
{
	struct qdwin_toplevel *tl = data;
	(void)fd;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		weston_log("qdwin/nested: input-sink peer closed handle=%u\n",
			   tl->handle);
		if (tl->nested_input_peer_source) {
			wl_event_source_remove(tl->nested_input_peer_source);
			tl->nested_input_peer_source = NULL;
		}
		if (tl->nested_input_sink && tl->nested_input_sink->peer_fd >= 0) {
			close(tl->nested_input_sink->peer_fd);
			tl->nested_input_sink->peer_fd = -1;
		}
		return 0;
	}
	/* §6.8 Round-8 fix: drain the per-peer buffer until the parser
	 * reports QDWIN_NESTED_INPUT_SINK_AGAIN (not enough bytes for the
	 * next full packet). The outer sends the S3b burst as ~9 packets
	 * back-to-back; on a SOCK_STREAM they typically arrive in a single
	 * readable notification, so reading just one packet per callback
	 * used to drop the trailing packets unless the event loop happened
	 * to re-trigger (the ~50% Round-7 flake). */
	for (;;) {
		uint8_t version = 0, payload[256];
		uint16_t plen = 0;
		int et = qdwin_nested_input_sink_read_one(
			tl->nested_input_sink,
			&version, &plen, payload, sizeof payload);
		if (et == QDWIN_NESTED_INPUT_SINK_AGAIN)
			break;
		if (et < 0)
			return 0;
		switch (et) {
		case QDNI_EVENT_PING:
			weston_log("qdwin/nested: input-sink PING handle=%u "
				   "version=%u (S3 wire-format proven)\n",
				   tl->handle, (unsigned)version);
			break;
		case QDNI_EVENT_MOTION:
			if (plen < sizeof(struct qdni_motion_payload)) break;
			qdwin_nested_dispatch_motion(tl,
				(const struct qdni_motion_payload *)payload);
			if (getenv("QDWIN_NESTED_INPUT_DEBUG"))
				weston_log("qdwin/nested: motion handle=%u\n",
					   tl->handle);
			break;
		case QDNI_EVENT_BUTTON: {
			if (plen < sizeof(struct qdni_button_payload)) break;
			const struct qdni_button_payload *p =
				(const struct qdni_button_payload *)payload;
			qdwin_nested_dispatch_button(tl, p);
			weston_log("qdwin/nested: button handle=%u "
				   "btn=0x%x state=%u\n",
				   tl->handle, p->button, p->state);
			break;
		}
		case QDNI_EVENT_KEY: {
			if (plen < sizeof(struct qdni_key_payload)) break;
			const struct qdni_key_payload *p =
				(const struct qdni_key_payload *)payload;
			qdwin_nested_dispatch_key(tl, p);
			weston_log("qdwin/nested: key handle=%u "
				   "key=%u state=%u\n",
				   tl->handle, p->key, p->state);
			break;
		}
		case QDNI_EVENT_AXIS:
			if (plen < sizeof(struct qdni_axis_payload)) break;
			qdwin_nested_dispatch_axis(tl,
				(const struct qdni_axis_payload *)payload);
			break;
		case QDNI_EVENT_FOCUS:
			if (plen < sizeof(struct qdni_focus_payload)) break;
			weston_log("qdwin/nested: focus handle=%u "
				   "focused=%u\n",
				   tl->handle,
				   ((const struct qdni_focus_payload *)
				    payload)->focused);
			break;
		default:
			weston_log("qdwin/nested: input-sink unknown "
				   "event_type=%d plen=%u handle=%u\n",
				   et, (unsigned)plen, tl->handle);
			break;
		}
	}
	return 1;
}

/* §6.8 S3: listen-fd readable callback — accept the outer's connect. */
static int
qdwin_nested_input_sink_listen_cb(int fd, uint32_t mask, void *data)
{
	struct qdwin_toplevel *tl = data;
	struct qdwin *qdwin = tl->qdwin;
	(void)fd;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		return 0;
	if (qdwin_nested_input_sink_accept(tl->nested_input_sink) < 0)
		return 0;
	weston_log("qdwin/nested: input-sink connected handle=%u\n",
		   tl->handle);
	struct wl_event_loop *loop = wl_display_get_event_loop(
		qdwin->compositor->wl_display);
	tl->nested_input_peer_source = wl_event_loop_add_fd(
		loop, tl->nested_input_sink->peer_fd, WL_EVENT_READABLE,
		qdwin_nested_input_sink_peer_cb, tl);
	return 1;
}

static void
qdwin_nested_on_configured(void *userdata, int32_t w, int32_t h)
{
	struct qdwin_toplevel *tl = userdata;
	tl->nested_configured_w = w;
	tl->nested_configured_h = h;
	weston_log("qdwin/nested: outer configured handle=%u %dx%d\n",
		   tl->handle, w, h);
	if (tl->desktop_surface && w > 0 && h > 0)
		weston_desktop_surface_set_size(tl->desktop_surface, w, h);
}

static void
qdwin_nested_on_close_requested(void *userdata)
{
	struct qdwin_toplevel *tl = userdata;
	weston_log("qdwin/nested: outer close_requested handle=%u\n",
		   tl->handle);
	if (tl->desktop_surface)
		weston_desktop_surface_close(tl->desktop_surface);
}

static void
qdwin_nested_on_focus_changed(void *userdata, uint32_t focused)
{
	struct qdwin_toplevel *tl = userdata;
	weston_log("qdwin/nested: outer focus_changed handle=%u focused=%u\n",
		   tl->handle, focused);
	(void)focused;
	/* S1: log only. S3 ties this to inner-seat focus. */
}

/* Pick the next un-pinned pipewire output from the nested compositor's
 * pre-allocated pool ([pipewire] num-outputs=N in weston.ini). This is
 * the §6.5 view-stream shape applied to nested mode: backend-pipewire's
 * weston_pipewire_output_api_v2 doesn't expose dynamic create_output
 * via an external-friendly pointer (the head_create entry takes a
 * weston_backend* that isn't reachable from a shell plugin), so static
 * pre-allocation is the LLM-friendly path for libweston-14.
 *
 * Returns NULL if all pipewire outputs are already pinned to other
 * nested toplevels — caller advertises with pw_node="...:none" in
 * that case so the outer can still surface the toplevel without
 * pixels (admin shell decides whether to grow the pool or refuse).
 */
static struct weston_output *
qdwin_nested_pick_pw_output(struct qdwin *qdwin)
{
	struct weston_output *o;
	wl_list_for_each(o, &qdwin->compositor->output_list, link) {
		if (strncmp(o->name, "pipewire", 8) != 0)
			continue;
		/* Skip outputs already pinned to a nested-pub. */
		struct qdwin_toplevel *tl;
		int taken = 0;
		wl_list_for_each(tl, &qdwin->toplevels, link) {
			if (tl->nested_pw_output == o) { taken = 1; break; }
		}
		if (taken)
			continue;
		return o;
	}
	return NULL;
}

static void
qdwin_nested_publish_toplevel(struct qdwin_toplevel *tl)
{
	struct qdwin *qdwin = tl->qdwin;
	if (!qdwin->nested_mode || !qdwin->nested_client)
		return;
	if (tl->nested_pub) /* already advertised */
		return;
	if (!qdwin_nested_client_manager_ready(qdwin->nested_client)) {
		weston_log("qdwin/nested: skip publish — manager not ready\n");
		return;
	}

	tl->nested_pw_output = qdwin_nested_pick_pw_output(qdwin);

	/* Pin the inner toplevel's view onto the pipewire output if we
	 * got one. Otherwise leave on whatever the desktop layout chose;
	 * the outer side will see the advertise but no pixels. */
	if (tl->nested_pw_output) {
		struct weston_output *pw = tl->nested_pw_output;
		struct weston_coord_global pos = {
			.c = weston_coord(pw->pos.c.x, pw->pos.c.y),
		};
		weston_view_set_position(tl->view, pos);
		weston_view_set_output(tl->view, pw);
		weston_view_update_transform(tl->view);
		if (tl->view->surface)
			weston_surface_damage(tl->view->surface);
		weston_output_schedule_repaint(pw);
	}

	const char *app_id =
		weston_desktop_surface_get_app_id(tl->desktop_surface);
	const char *title =
		weston_desktop_surface_get_title(tl->desktop_surface);
	uid_t uid = qdwin_client_uid(tl->desktop_surface);

	char pw_node[128];
	if (tl->nested_pw_output && tl->nested_pw_output->name)
		snprintf(pw_node, sizeof pw_node,
			 "weston.pipewire:%d:%s",
			 (int)getpid(), tl->nested_pw_output->name);
	else
		snprintf(pw_node, sizeof pw_node,
			 "weston.pipewire:%d:none", (int)getpid());

	/* §6.8 S3: open the per-toplevel input sink + integrate listen
	 * fd into the weston event loop. The accept callback sets up a
	 * second event source for the connected peer fd. */
	tl->nested_input_sink = qdwin_nested_input_sink_open(tl->handle);
	const char *input_sink_path = "";
	if (tl->nested_input_sink) {
		input_sink_path = tl->nested_input_sink->socket_path;
		struct wl_event_loop *loop = wl_display_get_event_loop(
			qdwin->compositor->wl_display);
		tl->nested_input_sink_source = wl_event_loop_add_fd(
			loop, tl->nested_input_sink->listen_fd,
			WL_EVENT_READABLE,
			qdwin_nested_input_sink_listen_cb, tl);
	}

	tl->nested_pub = qdwin_nested_client_advertise(
		qdwin->nested_client,
		pw_node,
		input_sink_path,
		app_id ? app_id : "",
		title ? title : "",
		(uint32_t)uid,
		tl,
		qdwin_nested_on_configured,
		qdwin_nested_on_close_requested,
		qdwin_nested_on_focus_changed);
	if (!tl->nested_pub) {
		weston_log("qdwin/nested: advertise_toplevel failed for "
			   "handle=%u app_id=%s\n",
			   tl->handle, app_id ? app_id : "(null)");
		return;
	}
	weston_log("qdwin/nested: advertised handle=%u pw_node=%s "
		   "app_id=%s title=%s origin_uid=%u\n",
		   tl->handle, pw_node,
		   app_id ? app_id : "(null)", title ? title : "(null)",
		   (unsigned)uid);
}

static void
qdwin_nested_unpublish_toplevel(struct qdwin_toplevel *tl)
{
	if (!tl->nested_pub)
		return;
	weston_log("qdwin/nested: unpublish handle=%u\n", tl->handle);
	qdwin_nested_client_pub_destroy(tl->nested_pub);
	tl->nested_pub = NULL;
	/* The pipewire output stays for the lifetime of the compositor —
	 * weston doesn't currently expose a public output_destroy on
	 * pipewire heads, and reuse is fine since names are unique. */
	tl->nested_pw_output = NULL;
	/* §6.8 S3: tear down the input sink lifecycle. Closing the
	 * listener removes outer-side ability to reconnect; the peer fd
	 * (if connected) gets closed too. */
	if (tl->nested_input_peer_source) {
		wl_event_source_remove(tl->nested_input_peer_source);
		tl->nested_input_peer_source = NULL;
	}
	if (tl->nested_input_sink_source) {
		wl_event_source_remove(tl->nested_input_sink_source);
		tl->nested_input_sink_source = NULL;
	}
	if (tl->nested_input_sink) {
		qdwin_nested_input_sink_close(tl->nested_input_sink);
		tl->nested_input_sink = NULL;
	}
	/* §6.8 S3b: tear the per-toplevel inner-seat down too. The seat
	 * may have been lazily inited only if real (non-PING) events
	 * landed; release is a no-op when never inited. */
	qdwin_nested_inner_seat_release(tl);
}

/* ------------------------------------------------------------------
 * §6.8 S2 — outer proxy surface from advertise.
 *
 * On the *outer* qdwin side: when qdwin_nested_v1.advertise_toplevel
 * lands, synthesise a placeholder weston_curtain + wrap it in a
 * qdwin_toplevel marked is_nested_proxy=true, then fire
 * qdwin_shell_v1.toplevel_added so qdshell decorates the proxy
 * exactly like a real xdg_toplevel. Real PipeWire-fed pixels are
 * S2b — the curtain is a stable placeholder until the consumer side
 * lands. set_title forwards via toplevel_title; app_id changes are
 * lossy (qdwin_shell_v1 has no toplevel_app_id event); set_geometry
 * resizes the curtain.
 *
 * Lifecycle:
 *   advertise_toplevel(...) -> qdwin_nested_proxy_create()
 *      -> curtain at default 800x600, visible on normal_layer
 *      -> toplevel_added event on outer shell
 *   set_title -> proxy_title cache + toplevel_title event
 *   set_geometry -> resize curtain (destroy + recreate)
 *   resource destroy -> qdwin_nested_proxy_destroy()
 *      -> toplevel_removed + curtain teardown + free
 *
 * Chrome attach (qdwin_shell_v1.attach_decoration) finds the proxy
 * by handle in qdwin->toplevels (proxies live in the same list as
 * real desktop_surface-backed toplevels) and the existing chrome
 * pipeline works unchanged because chrome positions are derived from
 * the view's geometry, not desktop_surface state.
 *
 * Future work (S2b/S3/S4):
 *   - Replace placeholder curtain with PipeWire-fed buffers via a
 *     small consumer process (pattern from qdistro-forward §6.5).
 *   - Wire close_requested when shell asks to close the proxy.
 *   - Wire focus_changed onto outer seat focus events for the proxy.
 *   - admin-broker CheckPermission gate at advertise time (S4).
 * ------------------------------------------------------------------ */

static struct qdwin_toplevel *
qdwin_nested_proxy_create(struct qdwin *qdwin,
			  struct qdwin_nested_toplevel *owner,
			  const char *app_id, const char *title,
			  uint32_t origin_uid, int w, int h)
{
	if (w <= 0) w = 800;
	if (h <= 0) h = 600;
	struct qdwin_toplevel *tl = calloc(1, sizeof *tl);
	if (!tl)
		return NULL;
	tl->qdwin              = qdwin;
	tl->desktop_surface    = NULL;
	tl->handle             = ++qdwin->next_handle;
	tl->is_nested_proxy    = true;
	tl->proxy_app_id       = app_id ? strdup(app_id) : NULL;
	tl->proxy_title        = title  ? strdup(title)  : NULL;
	tl->proxy_origin_uid   = origin_uid;
	tl->proxy_nested_owner = owner;
	tl->mapped             = 1;
	tl->decorated          = 0;
	tl->last_width         = w;
	tl->last_height        = h;
	for (int s = 0; s < QDWIN_SIDES; s++) {
		tl->chrome[s].side = s;
		tl->chrome[s].tl   = tl;
		wl_list_init(&tl->chrome[s].surface_destroy.link);
		wl_list_init(&tl->chrome[s].surface_commit.link);
	}

	struct weston_output *out = qdwin_primary_output(qdwin);
	int cx = 0, cy = 0;
	if (out) {
		/* Pixel-coord math; cast pos.c.{x,y} (double) to int so the
		 * integer division isn't flagged as float-context loss. */
		cx = (int)out->pos.c.x + (out->width  - w) / 2;
		cy = (int)out->pos.c.y + (out->height - h) / 2;
		if (cx < 0) cx = 0;
		if (cy < 0) cy = 0;
	}
	struct weston_curtain_params params = {
		.r = 0.20f, .g = 0.22f, .b = 0.28f, .a = 1.0f,
		.pos = { .c = weston_coord(cx, cy) },
		.width = w,
		.height = h,
	};
	tl->proxy_curtain =
		weston_shell_utils_curtain_create(qdwin->compositor, &params);
	if (!tl->proxy_curtain) {
		free(tl->proxy_app_id);
		free(tl->proxy_title);
		free(tl);
		return NULL;
	}
	tl->view = tl->proxy_curtain->view;

	/* §6.8 S4: gate visibility behind admin-broker decision when a
	 * v8+ shell is bound. Pre-v8 shells (or no shell) auto-allow
	 * unless QDWIN_NESTED_BROKER_REQUIRED=1, in which case we hold
	 * indefinitely (fail-closed). The proxy starts on held layer
	 * (invisible) and releases on `allow`, destroys on `deny`. */
	bool gate_required = false;
	const char *req_env = getenv("QDWIN_NESTED_BROKER_REQUIRED");
	if (req_env && strcmp(req_env, "1") == 0)
		gate_required = true;
	bool shell_can_gate = (qdwin->shell_bound && qdwin->shell_resource &&
		wl_resource_get_version(qdwin->shell_resource) >= 8);

	if (shell_can_gate || gate_required) {
		weston_view_move_to_layer(tl->view,
					  &qdwin->held_layer.view_list);
		tl->decorated = 0;
		tl->nested_proxy_pending_decision = true;
	} else {
		weston_view_move_to_layer(tl->view,
					  &qdwin->normal_layer.view_list);
		tl->decorated = 1;
		tl->nested_proxy_pending_decision = false;
	}

	wl_list_insert(&qdwin->toplevels, &tl->link);

	weston_log("qdwin/nested-proxy: created handle=%u uid=%u app_id=%s "
		   "title=%s size=%dx%d pos=(%d,%d) pending=%d\n",
		   tl->handle, origin_uid,
		   app_id ? app_id : "(null)",
		   title ? title : "(null)", w, h, cx, cy,
		   tl->nested_proxy_pending_decision ? 1 : 0);

	qdwin_send_toplevel_added(qdwin, tl);
	if (qdwin->shell_bound && qdwin->shell_resource) {
		qdwin_shell_v1_send_toplevel_geometry(
			qdwin->shell_resource, tl->handle, cx, cy,
			(uint32_t)w, (uint32_t)h);
	}
	if (shell_can_gate) {
		qdwin_shell_v1_send_nested_proxy_pending(
			qdwin->shell_resource, tl->handle,
			app_id ? app_id : "", origin_uid);
	}
	/* §6.8 S2b: a v9 shell can spawn a pixel consumer. Emit pw_node +
	 * input_sink the consumer needs to resolve via libpipewire. */
	if (qdwin->shell_bound && qdwin->shell_resource &&
	    wl_resource_get_version(qdwin->shell_resource) >= 9 &&
	    owner) {
		qdwin_shell_v1_send_nested_proxy_pixel_source(
			qdwin->shell_resource, tl->handle,
			owner->pw_node ? owner->pw_node : "",
			owner->input_sink ? owner->input_sink : "");
	}

	return tl;
}

/* §6.8 S4: shell's verdict on a pending nested proxy.
 *
 * decision values per qdwin-shell-v1.xml:
 *   0=allow (release proxy from held to normal — visible)
 *   1=deny  (post policy_denied error on the nested toplevel resource
 *            and destroy the proxy)
 *   2=defer (keep proxy held; pending stays true; shell may re-decide
 *            later when the broker policy changes)
 *
 * Idempotent: extra/stale decisions on a non-pending toplevel log + no-op.
 */
static void
qdwin_handle_nested_proxy_decision(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t handle,
				   uint32_t decision,
				   const char *reason)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl = qdwin_toplevel_from_handle(qdwin, handle);
	(void)client;

	if (!qdwin_shell_require_bound(qdwin, resource))
		return;
	if (!tl || !tl->is_nested_proxy) {
		weston_log("qdwin: nested_proxy_decision handle=%u: "
			   "no nested-proxy with that handle (stale?)\n",
			   handle);
		return;
	}

	switch (decision) {
	case 0: /* allow */
		if (!tl->nested_proxy_pending_decision) {
			weston_log("qdwin: nested_proxy_decision handle=%u "
				   "allow: not pending (idempotent no-op)\n",
				   handle);
			return;
		}
		tl->nested_proxy_pending_decision = false;
		qdwin_toplevel_release_holding(tl, "nested_proxy_decision/allow");
		weston_log("qdwin: nested_proxy_decision handle=%u ALLOW "
			   "reason='%s'\n", handle, reason ? reason : "");
		break;
	case 1: /* deny */
		weston_log("qdwin: nested_proxy_decision handle=%u DENY "
			   "reason='%s'\n", handle, reason ? reason : "");
		if (tl->proxy_nested_owner) {
			struct qdwin_nested_toplevel *nt =
				tl->proxy_nested_owner;
			if (nt->resource) {
				wl_resource_post_error(
					nt->resource,
					QDWIN_NESTED_MANAGER_V1_ERROR_POLICY_DENIED,
					"qdistro.nested.advertise denied "
					"by admin policy: %s",
					reason ? reason : "(no reason)");
			}
			/* Break the back-edge first so the resource_destroy
			 * cleanup that follows the protocol error doesn't
			 * call qdwin_nested_proxy_destroy on already-freed
			 * memory. */
			nt->proxy_tl = NULL;
			tl->proxy_nested_owner = NULL;
			/* The protocol error tears down the resource client-
			 * side, but we destroy the proxy locally now to free
			 * the curtain immediately. */
		}
		qdwin_nested_proxy_destroy(tl);
		break;
	case 2: /* defer */
		weston_log("qdwin: nested_proxy_decision handle=%u DEFER "
			   "reason='%s' (proxy stays held)\n",
			   handle, reason ? reason : "");
		/* Keep nested_proxy_pending_decision = true. */
		break;
	default:
		weston_log("qdwin: nested_proxy_decision handle=%u "
			   "unknown decision=%u (ignored)\n",
			   handle, decision);
		break;
	}
}

/* §6.8 S2b: when the consumer wl_client (or the bound surface) goes
 * away, restore the placeholder curtain so the proxy stays visible
 * (still in the admin shell) while the shell decides what to do
 * (e.g. respawn the consumer). */
static void
qdwin_proxy_pixel_surface_destroyed(struct wl_listener *listener, void *data)
{
	(void)data;
	struct qdwin_toplevel *tl =
		wl_container_of(listener, tl, proxy_pixel_destroy_listener);
	struct qdwin *qdwin = tl->qdwin;
	weston_log("qdwin/nested-proxy: pixel surface destroyed handle=%u "
		   "(reverting to placeholder curtain)\n", tl->handle);
	if (tl->proxy_pixel_view) {
		weston_view_destroy(tl->proxy_pixel_view);
		tl->proxy_pixel_view = NULL;
	}
	tl->proxy_pixel_surface = NULL;
	wl_list_remove(&tl->proxy_pixel_destroy_listener.link);
	wl_list_init(&tl->proxy_pixel_destroy_listener.link);

	/* Re-create a placeholder curtain at the last known position +
	 * size so the user still sees something. */
	int w = tl->last_width  > 0 ? tl->last_width  : 800;
	int h = tl->last_height > 0 ? tl->last_height : 600;
	struct weston_output *out = qdwin_primary_output(qdwin);
	int cx = 0, cy = 0;
	if (out) {
		/* Pixel-coord math; cast pos.c.{x,y} (double) to int so the
		 * integer division isn't flagged as float-context loss. */
		cx = (int)out->pos.c.x + (out->width  - w) / 2;
		cy = (int)out->pos.c.y + (out->height - h) / 2;
		if (cx < 0) cx = 0;
		if (cy < 0) cy = 0;
	}
	struct weston_curtain_params params = {
		.r = 0.20f, .g = 0.22f, .b = 0.28f, .a = 1.0f,
		.pos = { .c = weston_coord(cx, cy) },
		.width = w,
		.height = h,
	};
	tl->proxy_curtain = weston_shell_utils_curtain_create(
		qdwin->compositor, &params);
	if (tl->proxy_curtain) {
		tl->view = tl->proxy_curtain->view;
		weston_view_move_to_layer(tl->view,
					  &qdwin->normal_layer.view_list);
	}
}

static void
qdwin_handle_bind_proxy_pixels(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t handle,
			       struct wl_resource *surface_res)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	struct qdwin_toplevel *tl = qdwin_toplevel_from_handle(qdwin, handle);
	(void)client;
	/* §6.8 S2b: consumer wl_clients (qdistro-nested-pixelfeed) bind
	 * qdwin_shell_v1 via the standard registry path (already
	 * peer-uid-filtered to allowed_uid in bind_qdwin_shell), so we
	 * intentionally do NOT require qdwin_shell_v1.bind_as_shell here
	 * — only the admin shell calls that, but the consumer is a
	 * separate process at the same uid. The handle lookup below
	 * still rejects bogus handles. */
	if (!tl || !tl->is_nested_proxy) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_INVALID_HANDLE,
				       "bind_proxy_pixels: handle %u is not a "
				       "nested-proxy", handle);
		return;
	}
	struct weston_surface *ws = wl_resource_get_user_data(surface_res);
	if (!ws) {
		wl_resource_post_error(resource,
				       QDWIN_SHELL_V1_ERROR_INVALID_HANDLE,
				       "bind_proxy_pixels: surface invalid");
		return;
	}

	/* If we already had a bound pixel surface, drop it (consumer
	 * respawn case; new wins). */
	if (tl->proxy_pixel_view) {
		wl_list_remove(&tl->proxy_pixel_destroy_listener.link);
		wl_list_init(&tl->proxy_pixel_destroy_listener.link);
		weston_view_destroy(tl->proxy_pixel_view);
		tl->proxy_pixel_view = NULL;
		tl->proxy_pixel_surface = NULL;
	}

	/* Capture position from current view (curtain) so the swap is
	 * imperceptible. */
	struct weston_coord_global pos = weston_view_get_pos_offset_global(
		tl->view);

	/* Tear down the placeholder curtain — pixel feed takes over. */
	if (tl->proxy_curtain) {
		weston_shell_utils_curtain_destroy(tl->proxy_curtain);
		tl->proxy_curtain = NULL;
		tl->view = NULL;
	}

	tl->proxy_pixel_surface = ws;
	tl->proxy_pixel_view = weston_view_create(ws);
	if (!tl->proxy_pixel_view) {
		weston_log("qdwin/nested-proxy: bind_proxy_pixels handle=%u "
			   "weston_view_create failed\n", handle);
		tl->proxy_pixel_surface = NULL;
		return;
	}
	tl->view = tl->proxy_pixel_view;

	tl->proxy_pixel_destroy_listener.notify =
		qdwin_proxy_pixel_surface_destroyed;
	wl_signal_add(&ws->destroy_signal, &tl->proxy_pixel_destroy_listener);

	weston_view_set_position(tl->view, pos);
	weston_view_move_to_layer(tl->view, &qdwin->normal_layer.view_list);
	if (!weston_surface_is_mapped(ws))
		weston_surface_map(ws);
	weston_view_update_transform(tl->view);

	weston_log("qdwin/nested-proxy: bind_proxy_pixels handle=%u "
		   "surface=%p (curtain swapped for live feed)\n",
		   handle, (void *)ws);
}

static void
qdwin_nested_proxy_destroy(struct qdwin_toplevel *tl)
{
	if (!tl || !tl->is_nested_proxy)
		return;
	struct qdwin *qdwin = tl->qdwin;
	if (qdwin->active_input_proxy == tl)
		qdwin->active_input_proxy = NULL;
	weston_log("qdwin/nested-proxy: destroy handle=%u\n", tl->handle);
	qdwin_send_toplevel_removed(qdwin, tl);

	for (int s = 0; s < QDWIN_SIDES; s++)
		qdwin_chrome_detach(&tl->chrome[s]);

	if (tl->proxy_curtain) {
		weston_shell_utils_curtain_destroy(tl->proxy_curtain);
		tl->proxy_curtain = NULL;
		tl->view = NULL;
	}
	/* §6.8 S2b: drop the pixel-feed view + listener (the underlying
	 * weston_surface is owned by the consumer wl_client; we only
	 * release our view + listener). */
	if (tl->proxy_pixel_view) {
		wl_list_remove(&tl->proxy_pixel_destroy_listener.link);
		weston_view_destroy(tl->proxy_pixel_view);
		tl->proxy_pixel_view = NULL;
		tl->proxy_pixel_surface = NULL;
	}
	if (tl->proxy_input_sink_fd >= 0) {
		close(tl->proxy_input_sink_fd);
		tl->proxy_input_sink_fd = -1;
	}
	wl_list_remove(&tl->link);
	free(tl->proxy_app_id);
	free(tl->proxy_title);
	free(tl);
}

static void
qdwin_nested_proxy_set_title(struct qdwin_toplevel *tl, const char *title)
{
	if (!tl || !tl->is_nested_proxy)
		return;
	free(tl->proxy_title);
	tl->proxy_title = title ? strdup(title) : NULL;
	struct qdwin *qdwin = tl->qdwin;
	if (qdwin->shell_bound && qdwin->shell_resource) {
		qdwin_shell_v1_send_toplevel_title(
			qdwin->shell_resource, tl->handle,
			tl->proxy_title ? tl->proxy_title : "");
	}
}

static void
qdwin_nested_proxy_set_app_id(struct qdwin_toplevel *tl, const char *app_id)
{
	if (!tl || !tl->is_nested_proxy)
		return;
	free(tl->proxy_app_id);
	tl->proxy_app_id = app_id ? strdup(app_id) : NULL;
	/* qdwin_shell_v1 has no app_id-changed event today (app_id is
	 * sent once at toplevel_added). Cached value still affects
	 * subsequent re-emissions / debug output. */
	weston_log("qdwin/nested-proxy: handle=%u app_id updated to %s "
		   "(no shell event — qdwin_shell_v1 lacks toplevel_app_id)\n",
		   tl->handle, app_id ? app_id : "");
}

static void
qdwin_nested_proxy_set_geometry(struct qdwin_toplevel *tl, int w, int h)
{
	if (!tl || !tl->is_nested_proxy || !tl->proxy_curtain)
		return;
	if (w <= 0) w = 800;
	if (h <= 0) h = 600;
	if (tl->last_width == w && tl->last_height == h)
		return;
	struct qdwin *qdwin = tl->qdwin;

	/* Recreate curtain at new size. weston_curtain has no public
	 * resize, so destroy+recreate keeping position. */
	struct weston_coord_global pos =
		weston_view_get_pos_offset_global(tl->view);
	weston_shell_utils_curtain_destroy(tl->proxy_curtain);
	tl->proxy_curtain = NULL;
	tl->view = NULL;

	struct weston_curtain_params params = {
		.r = 0.20f, .g = 0.22f, .b = 0.28f, .a = 1.0f,
		.pos = pos,
		.width = w,
		.height = h,
	};
	tl->proxy_curtain =
		weston_shell_utils_curtain_create(qdwin->compositor, &params);
	if (!tl->proxy_curtain) {
		weston_log("qdwin/nested-proxy: handle=%u resize curtain_create "
			   "failed (toplevel pixels gone)\n", tl->handle);
		return;
	}
	tl->view = tl->proxy_curtain->view;
	weston_view_move_to_layer(tl->view, &qdwin->normal_layer.view_list);
	tl->last_width  = w;
	tl->last_height = h;

	if (qdwin->shell_bound && qdwin->shell_resource) {
		qdwin_shell_v1_send_toplevel_geometry(
			qdwin->shell_resource, tl->handle,
			(int)pos.c.x, (int)pos.c.y,
			(uint32_t)w, (uint32_t)h);
	}
	weston_log("qdwin/nested-proxy: handle=%u resized to %dx%d\n",
		   tl->handle, w, h);
}

static uid_t
qdwin_parse_allowed_uid(int argc, char *argv[])
{
	const char *env = getenv("QDWIN_ALLOWED_UID");
	long v;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--qdwin-allowed-uid=", 20) == 0) {
			env = argv[i] + 20;
			break;
		}
	}

	if (!env || !*env)
		return (uid_t)getuid();

	v = strtol(env, NULL, 10);
	if (v < 0)
		return (uid_t)getuid();
	return (uid_t)v;
}

/* ------------------------------------------------------------------
 * §6.10 wp_security_context_v1 — sandboxed-client identity.
 *
 * The protocol lets a sandbox engine (waypipe, flatpak, bubblewrap,
 * ...) register a listening socket and attach (sandbox_engine,
 * app_id, instance_id) metadata to all clients arriving on it. The
 * compositor owns the listening fd from `commit` onwards — accept()
 * on the fd, create a wl_client per accepted connection, tag it with
 * the secctx for later lookup.
 *
 * For qdistro the win is twofold:
 *
 *   1. waypipe 0.11's `--secctx` stops being a hard-fail (it required
 *      the global to be present), unblocking tier-3's signed
 *      identity path.
 *   2. qdwin can apply per-app policy (uid filter bypass, broker
 *      lookup keyed on app_id) for sandboxed clients without
 *      relying on PEERCRED.
 *
 * The protocol forbids nesting: a client that already has a secctx
 * attached isn't allowed to bind the manager. We enforce via a
 * global filter installed in wet_shell_init.
 * ------------------------------------------------------------------ */

struct qdwin_secctx {
	struct qdwin *qdwin;
	struct wl_resource *resource;       /* wp_security_context_v1 */
	int listen_fd;
	int close_fd;
	char *sandbox_engine;
	char *app_id;
	char *instance_id;
	int committed;
	struct wl_event_source *listen_source;
	struct wl_event_source *close_source;
	struct wl_list link;                /* qdwin::secctxs */
};

struct qdwin_secctx_client {
	struct wl_client *client;
	struct qdwin_secctx *secctx;        /* MAY become NULL when the
					     * listener is torn down before
					     * its accepted clients exit; the
					     * per-client metadata duplicated
					     * below survives the dangling
					     * pointer. */
	/* Per-client metadata copy. Filled at accept-time from secctx
	 * fields. Decoupled from `secctx` lifetime because per spec the
	 * listener outlives its creator AND the accepted clients outlive
	 * the listener — there's no symmetric guarantee on the tag struct,
	 * so we replicate to keep tag-introspection (qdwin_send_toplevel_
	 * security_context) safe. */
	char *sandbox_engine;
	char *app_id;
	char *instance_id;
	/* Option-B identity tuple, snapshotted at accept-time so the broker
	 * can re-verify the live process against /proc at decision time.
	 * (peer_pid, peer_starttime) is the anti-PID-reuse key; peer_exe
	 * and peer_selinux_label are empty if unreadable. See
	 * todo/decisions/secctx-identity-contract.md. */
	uint32_t peer_pid;
	uint64_t peer_starttime;
	uint32_t peer_uid;
	char *peer_exe;
	char *peer_selinux_label;
	struct wl_listener client_destroy_listener;
	struct wl_list link;                /* qdwin::secctx_clients */
};

static struct qdwin_secctx_client *
qdwin_secctx_client_find(struct qdwin *qdwin, struct wl_client *client)
{
	struct qdwin_secctx_client *sc;
	wl_list_for_each(sc, &qdwin->secctx_clients, link)
		if (sc->client == client)
			return sc;
	return NULL;
}

/* Public: returns the per-client secctx tag entry for `client`, or NULL
 * when unsandboxed. The entry's `secctx` pointer MAY be NULL if the
 * underlying listener was torn down before the client exited (per
 * spec the listener outlives its creator AND accepted clients outlive
 * the listener). The entry's snapshotted strings (sandbox_engine,
 * app_id, instance_id) survive that teardown. */
static struct qdwin_secctx_client *
qdwin_secctx_client_lookup(struct qdwin *qdwin, struct wl_client *client)
{
	return qdwin_secctx_client_find(qdwin, client);
}

static const char *qdwin_secctx_client_engine(struct qdwin_secctx_client *sc)
{ return sc && sc->sandbox_engine ? sc->sandbox_engine : ""; }
static const char *qdwin_secctx_client_app_id(struct qdwin_secctx_client *sc)
{ return sc && sc->app_id ? sc->app_id : ""; }
static const char *qdwin_secctx_client_instance_id(struct qdwin_secctx_client *sc)
{ return sc && sc->instance_id ? sc->instance_id : ""; }
static uint32_t qdwin_secctx_client_peer_pid(struct qdwin_secctx_client *sc)
{ return sc ? sc->peer_pid : 0; }
static uint64_t qdwin_secctx_client_peer_starttime(struct qdwin_secctx_client *sc)
{ return sc ? sc->peer_starttime : 0; }
static uint32_t qdwin_secctx_client_peer_uid(struct qdwin_secctx_client *sc)
{ return sc ? sc->peer_uid : 0; }
static const char *qdwin_secctx_client_peer_exe(struct qdwin_secctx_client *sc)
{ return sc && sc->peer_exe ? sc->peer_exe : ""; }
static const char *qdwin_secctx_client_peer_selinux_label(struct qdwin_secctx_client *sc)
{ return sc && sc->peer_selinux_label ? sc->peer_selinux_label : ""; }

static void
qdwin_secctx_client_on_destroy(struct wl_listener *l, void *data)
{
	struct qdwin_secctx_client *sc =
		wl_container_of(l, sc, client_destroy_listener);
	(void)data;
	wl_list_remove(&sc->link);
	wl_list_remove(&sc->client_destroy_listener.link);
	free(sc->sandbox_engine);
	free(sc->app_id);
	free(sc->instance_id);
	free(sc->peer_exe);
	free(sc->peer_selinux_label);
	free(sc);
}

/* Option-B identity capture helpers. Read /proc/<pid>/stat field 22
 * (starttime, the kernel's PID-reuse-safe pinning value), /proc/<pid>/exe
 * (resolved), and /proc/<pid>/attr/current (SELinux label). All return
 * a heap-allocated string the caller must free; readlink/read errors
 * map to an empty string so downstream consumers see "unverifiable" not
 * "spoofed". starttime is read into a uint64. */
static uint64_t
qdwin_proc_starttime(pid_t pid)
{
	if (pid <= 0)
		return 0;
	char path[64];
	snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	char buf[2048];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	/* The `comm` field is parenthesised and may contain spaces; skip
	 * past the *last* ')' so subsequent space-tokenisation is safe. */
	char *p = strrchr(buf, ')');
	if (!p)
		return 0;
	p++;
	/* After the ')' there's a leading space then field 3 (state).
	 * Field 22 (starttime) is the 20th whitespace-separated token after
	 * the ')'. */
	int field = 2;  /* the ')' itself is field 2 */
	while (*p && field < 22) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			return 0;
		field++;
		if (field == 22)
			break;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	if (field != 22)
		return 0;
	unsigned long long v = 0;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (unsigned)(*p - '0');
		p++;
	}
	return (uint64_t)v;
}

static char *
qdwin_proc_exe(pid_t pid)
{
	if (pid <= 0)
		return strdup("");
	char path[64];
	snprintf(path, sizeof path, "/proc/%d/exe", (int)pid);
	char buf[PATH_MAX];
	ssize_t n = readlink(path, buf, sizeof(buf) - 1);
	if (n <= 0)
		return strdup("");
	buf[n] = '\0';
	return strdup(buf);
}

static char *
qdwin_proc_selinux_label(pid_t pid)
{
	if (pid <= 0)
		return strdup("");
	char path[64];
	snprintf(path, sizeof path, "/proc/%d/attr/current", (int)pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return strdup("");
	char buf[512];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return strdup("");
	buf[n] = '\0';
	/* Kernel appends a trailing NUL or newline; strip any trailing
	 * whitespace/NUL bytes so the broker's string compare is stable. */
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\0' ||
			 buf[n - 1] == ' ' || buf[n - 1] == '\r')) {
		buf[--n] = '\0';
	}
	return strdup(buf);
}

static int
qdwin_secctx_listen_cb(int fd, uint32_t mask, void *data)
{
	struct qdwin_secctx *sec = data;
	struct qdwin *qdwin = sec->qdwin;
	(void)mask;
	int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (client_fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			weston_log("qdwin: secctx accept(%s/%s) failed: %s\n",
				   sec->sandbox_engine ? sec->sandbox_engine : "?",
				   sec->app_id ? sec->app_id : "?",
				   strerror(errno));
		return 0;
	}
	struct wl_client *new_client =
		wl_client_create(qdwin->compositor->wl_display, client_fd);
	if (!new_client) {
		close(client_fd);
		return 0;
	}
	struct qdwin_secctx_client *sc = calloc(1, sizeof *sc);
	if (!sc) {
		/* Connection accepted but tagging failed — close the
		 * client to keep state consistent (otherwise we'd treat
		 * a sandboxed client as unsandboxed, which is unsafe). */
		wl_client_destroy(new_client);
		return 0;
	}
	sc->client = new_client;
	sc->secctx = sec;
	/* Snapshot tag metadata into the client so introspection (the v13
	 * toplevel_security_context fanout) stays valid even after the
	 * listener (and its qdwin_secctx) is torn down. */
	sc->sandbox_engine = sec->sandbox_engine ? strdup(sec->sandbox_engine) : NULL;
	sc->app_id         = sec->app_id         ? strdup(sec->app_id)         : NULL;
	sc->instance_id    = sec->instance_id    ? strdup(sec->instance_id)    : NULL;
	/* Option-B identity capture: SO_PEERCRED on the accepted socket
	 * gives us the (pid, uid) the kernel pinned at connect-time, which
	 * the broker re-verifies against /proc at decision time. We snapshot
	 * starttime + exe + SELinux label here because the underlying
	 * process may exit before the broker call; the snapshot lets the
	 * broker still detect "the process is gone" vs "it changed identity". */
	pid_t peer_pid = 0;
	uid_t peer_uid = 0;
	gid_t peer_gid_unused = 0;
	wl_client_get_credentials(new_client, &peer_pid, &peer_uid,
				  &peer_gid_unused);
	sc->peer_pid = (uint32_t)peer_pid;
	sc->peer_uid = (uint32_t)peer_uid;
	sc->peer_starttime = qdwin_proc_starttime(peer_pid);
	sc->peer_exe = qdwin_proc_exe(peer_pid);
	sc->peer_selinux_label = qdwin_proc_selinux_label(peer_pid);
	sc->client_destroy_listener.notify = qdwin_secctx_client_on_destroy;
	wl_client_add_destroy_listener(new_client,
				       &sc->client_destroy_listener);
	wl_list_insert(&qdwin->secctx_clients, &sc->link);
	weston_log("qdwin/secctx: client accepted engine=%s app_id=%s "
		   "instance_id=%s (total tagged: %u)\n",
		   sec->sandbox_engine ? sec->sandbox_engine : "?",
		   sec->app_id ? sec->app_id : "?",
		   sec->instance_id ? sec->instance_id : "?",
		   (unsigned)wl_list_length(&qdwin->secctx_clients));
	return 0;
}

static void
qdwin_secctx_destroy(struct qdwin_secctx *sec);

static int
qdwin_secctx_close_cb(int fd, uint32_t mask, void *data)
{
	struct qdwin_secctx *sec = data;
	(void)fd; (void)mask;
	weston_log("qdwin/secctx: close_fd hangup engine=%s app_id=%s "
		   "→ tearing down listener\n",
		   sec->sandbox_engine ? sec->sandbox_engine : "?",
		   sec->app_id ? sec->app_id : "?");
	qdwin_secctx_destroy(sec);
	return 0;
}

static void qdwin_secctx_destroy(struct qdwin_secctx *sec);

static void
qdwin_secctx_destroy_all(struct qdwin *qdwin)
{
	struct qdwin_secctx *sec, *tmp;
	wl_list_for_each_safe(sec, tmp, &qdwin->secctxs, link)
		qdwin_secctx_destroy(sec);
}

static void
qdwin_secctx_destroy(struct qdwin_secctx *sec)
{
	if (sec->listen_source) {
		wl_event_source_remove(sec->listen_source);
		sec->listen_source = NULL;
	}
	if (sec->close_source) {
		wl_event_source_remove(sec->close_source);
		sec->close_source = NULL;
	}
	if (sec->listen_fd >= 0) { close(sec->listen_fd); sec->listen_fd = -1; }
	if (sec->close_fd >= 0) { close(sec->close_fd); sec->close_fd = -1; }
	wl_list_remove(&sec->link);
	/* Detach already-accepted clients so they don't dangle a freed
	 * pointer. Each client snapshots its own copy of the engine /
	 * app_id / instance_id at accept-time, so introspection (the v13
	 * fanout) keeps working post-tear-down. */
	struct qdwin_secctx_client *sc;
	wl_list_for_each(sc, &sec->qdwin->secctx_clients, link) {
		if (sc->secctx == sec)
			sc->secctx = NULL;
	}
	free(sec->sandbox_engine);
	free(sec->app_id);
	free(sec->instance_id);
	free(sec);
}

static void
qdwin_secctx_destroy_req(struct wl_client *client,
			 struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_secctx_set_sandbox_engine(struct wl_client *client,
				struct wl_resource *resource,
				const char *name)
{
	struct qdwin_secctx *sec = wl_resource_get_user_data(resource);
	(void)client;
	if (sec->committed) {
		wl_resource_post_error(resource,
				       WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
				       "set_sandbox_engine after commit");
		return;
	}
	if (sec->sandbox_engine) {
		wl_resource_post_error(resource,
				       WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
				       "sandbox_engine already set");
		return;
	}
	sec->sandbox_engine = name ? strdup(name) : NULL;
}

static void
qdwin_secctx_set_app_id(struct wl_client *client,
			struct wl_resource *resource, const char *app_id)
{
	struct qdwin_secctx *sec = wl_resource_get_user_data(resource);
	(void)client;
	if (sec->committed) {
		wl_resource_post_error(resource,
				       WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
				       "set_app_id after commit");
		return;
	}
	if (sec->app_id) {
		wl_resource_post_error(resource,
				       WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
				       "app_id already set");
		return;
	}
	sec->app_id = app_id ? strdup(app_id) : NULL;
}

static void
qdwin_secctx_set_instance_id(struct wl_client *client,
			     struct wl_resource *resource,
			     const char *instance_id)
{
	struct qdwin_secctx *sec = wl_resource_get_user_data(resource);
	(void)client;
	if (sec->committed) {
		wl_resource_post_error(resource,
				       WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
				       "set_instance_id after commit");
		return;
	}
	if (sec->instance_id) {
		wl_resource_post_error(resource,
				       WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
				       "instance_id already set");
		return;
	}
	sec->instance_id = instance_id ? strdup(instance_id) : NULL;
}

static void
qdwin_secctx_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct qdwin_secctx *sec = wl_resource_get_user_data(resource);
	struct qdwin *qdwin = sec->qdwin;
	(void)client;
	if (sec->committed) {
		wl_resource_post_error(resource,
				       WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
				       "commit twice");
		return;
	}
	sec->committed = 1;
	struct wl_event_loop *loop =
		wl_display_get_event_loop(qdwin->compositor->wl_display);
	sec->listen_source = wl_event_loop_add_fd(
		loop, sec->listen_fd, WL_EVENT_READABLE,
		qdwin_secctx_listen_cb, sec);
	sec->close_source = wl_event_loop_add_fd(
		loop, sec->close_fd, WL_EVENT_HANGUP | WL_EVENT_ERROR,
		qdwin_secctx_close_cb, sec);
	if (!sec->listen_source || !sec->close_source) {
		weston_log("qdwin/secctx: event_loop_add_fd failed for "
			   "engine=%s app_id=%s\n",
			   sec->sandbox_engine ? sec->sandbox_engine : "?",
			   sec->app_id ? sec->app_id : "?");
		qdwin_secctx_destroy(sec);
		return;
	}
	weston_log("qdwin/secctx: committed engine=%s app_id=%s "
		   "instance_id=%s listen_fd=%d close_fd=%d\n",
		   sec->sandbox_engine ? sec->sandbox_engine : "?",
		   sec->app_id ? sec->app_id : "?",
		   sec->instance_id ? sec->instance_id : "?",
		   sec->listen_fd, sec->close_fd);
}

static const struct wp_security_context_v1_interface qdwin_secctx_impl = {
	.destroy = qdwin_secctx_destroy_req,
	.set_sandbox_engine = qdwin_secctx_set_sandbox_engine,
	.set_app_id = qdwin_secctx_set_app_id,
	.set_instance_id = qdwin_secctx_set_instance_id,
	.commit = qdwin_secctx_commit,
};

static void
qdwin_secctx_resource_destroy(struct wl_resource *resource)
{
	struct qdwin_secctx *sec = wl_resource_get_user_data(resource);
	if (!sec)
		return;
	/* If commit fired, the listener stays alive past the client's
	 * resource lifetime (per spec — "must continue to accept
	 * connections ... when the Wayland client which created the
	 * security context disconnects"). Otherwise tear down. */
	if (!sec->committed)
		qdwin_secctx_destroy(sec);
	else
		sec->resource = NULL;  /* listener detaches from client */
}

static void
qdwin_secctx_manager_destroy_req(struct wl_client *client,
				 struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
qdwin_secctx_manager_create_listener(struct wl_client *client,
				     struct wl_resource *resource,
				     uint32_t id,
				     int32_t listen_fd,
				     int32_t close_fd)
{
	struct qdwin *qdwin = wl_resource_get_user_data(resource);
	if (listen_fd < 0) {
		wl_resource_post_error(resource,
		    WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD,
		    "listen_fd is invalid");
		if (close_fd >= 0) close(close_fd);
		return;
	}
	struct qdwin_secctx *sec = calloc(1, sizeof *sec);
	if (!sec) {
		wl_client_post_no_memory(client);
		close(listen_fd);
		if (close_fd >= 0) close(close_fd);
		return;
	}
	sec->qdwin = qdwin;
	sec->listen_fd = listen_fd;
	sec->close_fd = close_fd;
	sec->resource = wl_resource_create(client,
		&wp_security_context_v1_interface,
		wl_resource_get_version(resource), id);
	if (!sec->resource) {
		close(listen_fd);
		if (close_fd >= 0) close(close_fd);
		free(sec);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(sec->resource, &qdwin_secctx_impl,
				       sec, qdwin_secctx_resource_destroy);
	wl_list_insert(&qdwin->secctxs, &sec->link);
}

static const struct wp_security_context_manager_v1_interface
qdwin_secctx_manager_impl = {
	.destroy = qdwin_secctx_manager_destroy_req,
	.create_listener = qdwin_secctx_manager_create_listener,
};

/* Block sandboxed clients from binding the manager (per protocol's
 * nesting prohibition). Installed as a wl_global_filter; returns true
 * to allow this client to see/bind the global. */
static bool
qdwin_secctx_global_filter(const struct wl_client *client,
			   const struct wl_global *global, void *data)
{
	struct qdwin *qdwin = data;
	if (global != qdwin->security_context_manager_global)
		return true;  /* only filter the secctx manager */
	struct wl_client *cw = (struct wl_client *)client;
	return qdwin_secctx_client_find(qdwin, cw) == NULL;
}

static void
bind_qdwin_secctx_manager(struct wl_client *client, void *data,
			  uint32_t version, uint32_t id)
{
	struct qdwin *qdwin = data;
	struct wl_resource *r = wl_resource_create(client,
		&wp_security_context_manager_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &qdwin_secctx_manager_impl,
				       qdwin, NULL);
	pid_t pid; uid_t uid; gid_t gid;
	wl_client_get_credentials(client, &pid, &uid, &gid);
	weston_log("qdwin/secctx: manager bound by uid=%u pid=%d\n",
		   (unsigned)uid, (int)pid);
}

WL_EXPORT int
wet_shell_init(struct weston_compositor *ec, int *argc, char *argv[])
{
	struct qdwin *qdwin;

	qdwin = calloc(1, sizeof *qdwin);
	if (!qdwin)
		return -1;

	qdwin->compositor = ec;
	/* §6.8 S3b: install our default-pointer-grab so nested-proxy
	 * input forwarding lives even when no other grab is active.
	 * Singleton hookup so the grab callbacks (which receive only a
	 * weston_pointer_grab*) can find this qdwin instance. */
	qdwin_singleton = qdwin;
	weston_compositor_set_default_pointer_grab(
		ec, &qdwin_proxy_default_pointer_grab_iface);
	qdwin->allowed_uid = qdwin_parse_allowed_uid(*argc, argv);
	/* Default the locker uid to the shell uid (single-admin) until
	 * an explicit `--qdwin-allowed-locker-uid=N` is wired. Using
	 * (uid_t)-1 as the sentinel rather than 0 so a root-owned
	 * locker is a valid configuration. */
	qdwin->allowed_locker_uid = (uid_t)-1;
	wl_list_init(&qdwin->hotkeys);
	wl_list_init(&qdwin->toplevels);
	wl_list_init(&qdwin->view_streams);
	wl_list_init(&qdwin->seat_trackers);
	wl_list_init(&qdwin->activation_tokens);
	wl_list_init(&qdwin->activation_pending);
	wl_list_init(&qdwin->data_source_wraps);
	wl_list_init(&qdwin->data_offer_pending);
	wl_list_init(&qdwin->idle_notifications);
	wl_list_init(&qdwin->idle_inhibitors);
	wl_list_init(&qdwin->fractional_scales);
	wl_list_init(&qdwin->primary_seats);
	wl_list_init(&qdwin->nested_toplevels);
	qdwin->next_stream_port = 3401;  /* pool start for per-stream ports */

	/* Layers: background (BACKGROUND) < held (HIDDEN) < normal (NORMAL).
	 * minimized_layer is intentionally *not* passed through
	 * weston_layer_set_position — that leaves it detached from the
	 * compositor's layer_list so layer_is_visible() returns false,
	 * and weston_view_move_to_layer() unmaps views moving into it
	 * (matching desktop-shell's minimised pattern). Setting position
	 * HIDDEN on the held layer is harmless because views placed
	 * there carry no buffer yet; they become visible only after the
	 * move to normal_layer in release_holding. */
	weston_layer_init(&qdwin->background_layer, ec);
	weston_layer_init(&qdwin->held_layer, ec);
	weston_layer_init(&qdwin->normal_layer, ec);
	weston_layer_init(&qdwin->minimized_layer, ec);
	weston_layer_init(&qdwin->panel_layer, ec);
	weston_layer_init(&qdwin->notification_layer, ec);
	weston_layer_init(&qdwin->launcher_layer, ec);
	weston_layer_init(&qdwin->lock_layer, ec);
	weston_layer_init(&qdwin->popup_layer, ec);
	for (int i = 0; i < 4; i++)
		weston_layer_init(&qdwin->layer_shell_layer[i], ec);
	weston_layer_set_position(&qdwin->background_layer,
				  WESTON_LAYER_POSITION_BACKGROUND);
	weston_layer_set_position(&qdwin->held_layer,
				  WESTON_LAYER_POSITION_HIDDEN);
	weston_layer_set_position(&qdwin->normal_layer,
				  WESTON_LAYER_POSITION_NORMAL);
	/* Panel sits between NORMAL (0x50000000) and UI/popup (0x80000000).
	 * 0x70000000 keeps popups on top so right-click chrome menus still
	 * overlay the panel. */
	weston_layer_set_position(&qdwin->panel_layer, 0x70000000u);
	/* Notifications live above panel but below popups (popup_layer is
	 * WESTON_LAYER_POSITION_UI = 0x80000000). 0x78000000 fits between.
	 * Launcher between notifications and popup (0x7C000000). */
	weston_layer_set_position(&qdwin->notification_layer, 0x78000000u);
	weston_layer_set_position(&qdwin->launcher_layer,    0x7C000000u);
	weston_layer_set_position(&qdwin->popup_layer,
				  WESTON_LAYER_POSITION_UI);
	/* Lock layer lives at WESTON_LAYER_POSITION_LOCK (above
	 * everything except cursor + fade). Set after popup so the
	 * position order is clear in the source. */
	weston_layer_set_position(&qdwin->lock_layer,
				  WESTON_LAYER_POSITION_LOCK);
	/* zwlr_layer_shell_v1 layer ladder. Position values picked to
	 * cohabit with qdwin's native panel layers:
	 *   BACKGROUND (0): just above qdwin->background_layer (0x02).
	 *   BOTTOM     (1): WESTON_LAYER_POSITION_BOTTOM_UI (0x30000000).
	 *   TOP        (2): 0x90000000 — above qdwin->popup_layer (UI =
	 *                   0x80000000), below FULLSCREEN (0xb0000000)
	 *                   so a fullscreen toplevel still covers TOP-
	 *                   layer panels per spec.
	 *   OVERLAY    (3): WESTON_LAYER_POSITION_TOP_UI (0xe0000000) —
	 *                   above FULLSCREEN per spec ("OVERLAY layer is
	 *                   above fullscreen surfaces").
	 */
	weston_layer_set_position(&qdwin->layer_shell_layer[0], 0x00000003u);
	weston_layer_set_position(&qdwin->layer_shell_layer[1],
				  WESTON_LAYER_POSITION_BOTTOM_UI);
	weston_layer_set_position(&qdwin->layer_shell_layer[2], 0x90000000u);
	weston_layer_set_position(&qdwin->layer_shell_layer[3],
				  WESTON_LAYER_POSITION_TOP_UI);
	wl_list_init(&qdwin->panels);
	wl_list_init(&qdwin->notifications);
	wl_list_init(&qdwin->launchers);

	qdwin->desktop = weston_desktop_create(ec, &qdwin_desktop_api, qdwin);
	if (!qdwin->desktop) {
		weston_log("qdwin: weston_desktop_create failed\n");
		goto fail;
	}

	qdwin->shell_global = wl_global_create(ec->wl_display,
					       &qdwin_shell_v1_interface,
					       23, qdwin, bind_qdwin_shell);
	if (!qdwin->shell_global) {
		weston_log("qdwin: wl_global_create failed\n");
		goto fail;
	}

	/* qdwin_locker_v1 — peer locker global. Defaults to the same uid
	 * as the shell unless overridden (single-admin assumption per
	 * sessions.md:4-14). The sentinel is (uid_t)-1 not 0 so a
	 * root-owned locker is a legitimate explicit configuration. See
	 * doc/locker.md.
	 *
	 * §P10: compiled out in role=guest builds. The in-VM compositor
	 * is locked at the host level (the outer host qdwin owns the
	 * locker; the guest is the locked thing, not a locker target).
	 * Spec: plan2/research/spice-retirement/00-overview.md §"What it
	 * drops vs the host qdwin". */
#ifndef QDWIN_ROLE_GUEST
	if (qdwin->allowed_locker_uid == (uid_t)-1)
		qdwin->allowed_locker_uid = qdwin->allowed_uid;
	qdwin->locker_global = wl_global_create(ec->wl_display,
						&qdwin_locker_v1_interface,
						1, qdwin, bind_qdwin_locker);
	if (!qdwin->locker_global) {
		weston_log("qdwin: locker wl_global_create failed\n");
		goto fail;
	}
#else
	/* role=guest: explicitly leave qdwin->locker_global NULL so any
	 * code that later wants to broadcast through it short-circuits. */
	qdwin->locker_global = NULL;
#endif

	/* §6.5 S5: input-injection global. Visible to any client; the
	 * access_token in claim() is the gate. qdistro-forward gets the
	 * token via spawn argv from qdwin. */
	qdwin->stream_input_global = wl_global_create(
		ec->wl_display, &qdwin_stream_input_v1_interface,
		2, qdwin, bind_qdwin_stream_input);
	if (!qdwin->stream_input_global) {
		weston_log("qdwin: stream_input wl_global_create failed\n");
		goto fail;
	}

	/* §6.7: xdg-activation-v1 public global. Any client may bind. */
	qdwin->xdg_activation_global = wl_global_create(
		ec->wl_display, &xdg_activation_v1_interface,
		1, qdwin, bind_xdg_activation);
	if (!qdwin->xdg_activation_global) {
		weston_log("qdwin: xdg-activation wl_global_create failed\n");
		goto fail;
	}

	/* §6.7: ext-idle-notify-v1 + idle-inhibit-unstable-v1. Both
	 * public. idle_signal/wake_signal drive notification fan-out. */
	qdwin->idle_notifier_global = wl_global_create(
		ec->wl_display, &ext_idle_notifier_v1_interface,
		2, qdwin, bind_qdwin_idle_notifier);
	if (!qdwin->idle_notifier_global) {
		weston_log("qdwin: ext-idle-notify wl_global_create failed\n");
		goto fail;
	}
	qdwin->idle_inhibit_manager_global = wl_global_create(
		ec->wl_display, &zwp_idle_inhibit_manager_v1_interface,
		1, qdwin, bind_qdwin_idle_inhibit_manager);
	if (!qdwin->idle_inhibit_manager_global) {
		weston_log("qdwin: idle-inhibit wl_global_create failed\n");
		goto fail;
	}
	/* §6.6 S3/S4 keybindings: Ctrl+Space → launcher_requested;
	 * Alt+Tab → switcher_next(+1); Alt+Shift+Tab → switcher_next(-1).
	 * The compositor owns the key grab — events fan out to the
	 * shell via qdwin_shell_v1, keeping app clients unaffected.
	 * Alt release → switcher_commit via a modifier binding. */
	weston_compositor_add_key_binding(ec, KEY_SPACE, MODIFIER_CTRL,
					  qdwin_on_launcher_key, qdwin);
	weston_compositor_add_key_binding(ec, KEY_TAB, MODIFIER_ALT,
					  qdwin_on_switcher_key, qdwin);
	weston_compositor_add_key_binding(
		ec, KEY_TAB, (enum weston_keyboard_modifier)
		(MODIFIER_ALT | MODIFIER_SHIFT),
		qdwin_on_switcher_back_key, qdwin);
	weston_compositor_add_modifier_binding(ec, MODIFIER_ALT,
					       qdwin_on_alt_released, qdwin);
	/* §6.6 S5 full: Ctrl+Alt+L → lock_requested event.
	 * Shell decides whether to enter lock; compositor only relays. */
	weston_compositor_add_key_binding(
		ec, KEY_L, (enum weston_keyboard_modifier)
		(MODIFIER_CTRL | MODIFIER_ALT),
		qdwin_on_lock_key, qdwin);

	qdwin->idle_signal_listener.notify = qdwin_on_idle_signal;
	wl_signal_add(&ec->idle_signal, &qdwin->idle_signal_listener);
	qdwin->wake_signal_listener.notify = qdwin_on_wake_signal;
	wl_signal_add(&ec->wake_signal, &qdwin->wake_signal_listener);
	/* §6.7(a) follow-up: enable internal idle tracker if weston's own
	 * idle timer is disabled. Read once at init — ec->idle_time doesn't
	 * change post-init in weston-14. */
	qdwin->idle_internal_mode = (ec->idle_time == 0);
	weston_log("qdwin: ext-idle-notify idle_time=%d internal_mode=%d\n",
		   ec->idle_time, qdwin->idle_internal_mode);

	qdwin->cursor_shape_manager_global = wl_global_create(
		ec->wl_display, &wp_cursor_shape_manager_v1_interface,
		2, qdwin, bind_qdwin_cursor_shape_manager);
	if (!qdwin->cursor_shape_manager_global) {
		weston_log("qdwin: cursor-shape wl_global_create failed\n");
		goto fail;
	}
	qdwin_cursor_theme_load(qdwin);
	qdwin->fractional_scale_manager_global = wl_global_create(
		ec->wl_display, &wp_fractional_scale_manager_v1_interface,
		1, qdwin, bind_qdwin_fractional_scale_manager);
	if (!qdwin->fractional_scale_manager_global) {
		weston_log("qdwin: fractional-scale wl_global_create failed\n");
		goto fail;
	}
	qdwin->primary_selection_manager_global = wl_global_create(
		ec->wl_display,
		&zwp_primary_selection_device_manager_v1_interface,
		1, qdwin, bind_qdwin_primary_manager);
	if (!qdwin->primary_selection_manager_global) {
		weston_log("qdwin: primary-selection wl_global_create failed\n");
		goto fail;
	}

	/* §6.10: wp_security_context_v1. Hides itself from sandboxed
	 * clients via the global filter so nesting is impossible. */
	wl_list_init(&qdwin->secctxs);
	wl_list_init(&qdwin->secctx_clients);
	qdwin->security_context_manager_global = wl_global_create(
		ec->wl_display,
		&wp_security_context_manager_v1_interface,
		1, qdwin, bind_qdwin_secctx_manager);
	if (!qdwin->security_context_manager_global) {
		weston_log("qdwin: security-context wl_global_create failed\n");
		goto fail;
	}
	wl_display_set_global_filter(ec->wl_display,
				     qdwin_secctx_global_filter, qdwin);

	/* §6.8 S1: qdwin_nested_v1 manager global at v2 (string node IDs).
	 * Peer-uid-filtered.
	 *
	 * §P10: compiled out in role=guest builds. The in-VM compositor
	 * is the *inner* end of the nested-compositor topology — it never
	 * advertises a further nesting target to its own clients. Outer
	 * (host) qdwin keeps the global so it can publish proxy toplevels
	 * for guest-side apps via qdwin-bystander --forward-all-toplevels.
	 * Spec: plan2/research/spice-retirement/00-overview.md §"What it
	 * drops vs the host qdwin". */
#ifndef QDWIN_ROLE_GUEST
	qdwin->nested_manager_global = wl_global_create(
		ec->wl_display, &qdwin_nested_manager_v1_interface,
		2, qdwin, bind_qdwin_nested_manager);
	if (!qdwin->nested_manager_global) {
		weston_log("qdwin: nested-manager wl_global_create failed\n");
		goto fail;
	}
#else
	qdwin->nested_manager_global = NULL;
#endif

	/* zwlr_layer_shell_v1 v5: external panels/notifications/lockscreens
	 * (waybar, Quickshell/noctalia, eww, fuzzel, mako, swaylock).
	 * Stub stage — accepts the protocol and completes configure/ack
	 * but does not lay out or render yet. See impl block above. */
	/* Test aperture: unconditional public bind. Production posture documented in qdwin/doc/protocol.md §"Production posture: layer-shell is a test aperture". */
	wl_list_init(&qdwin->layer_surfaces);
	qdwin->layer_shell_global = wl_global_create(
		ec->wl_display, &zwlr_layer_shell_v1_interface,
		5, qdwin, bind_qdwin_layer_shell);
	if (!qdwin->layer_shell_global) {
		weston_log("qdwin: layer-shell wl_global_create failed\n");
		goto fail;
	}

	/* plan3 H1: register a handler for xdg_popup.grab on layer-
	 * parented popups. Without this hook the running libweston (if
	 * patched) posts XDG_POPUP_ERROR_INVALID_GRAB; with it, qdwin
	 * installs its own pointer grab that dismisses on outside click.
	 * dlsym-soft so an unpatched libweston still links; the layer-
	 * popup grab path then falls back to libweston's error.
	 *
	 * plan3 L2: also probe the other two layer-popup helpers and log
	 * once at startup so the operator knows what degraded behaviour to
	 * expect on an unpatched libweston. Their failure modes are silent
	 * otherwise: get_geometry missing makes popup positioning a no-op
	 * (popup floats at 0,0); dismiss missing makes outside-click
	 * detection log but never tear the popup down. */
	{
		qdwin_xdg_popup_set_layer_grab_handler_fn set_handler =
			qdwin_xdg_popup_set_layer_grab_handler_sym();
		if (set_handler) {
			set_handler(qdwin_layer_popup_layer_grab_handler,
				    qdwin);
			weston_log("qdwin: layer-popup grab handler "
				   "registered\n");
		} else {
			weston_log("qdwin: layer-popup grab handler NOT "
				   "registered (libweston symbol "
				   "weston_desktop_xdg_popup_set_layer_grab_handler "
				   "missing) — xdg_popup.grab on layer-parented "
				   "popups will return INVALID_GRAB\n");
		}
		if (!qdwin_xdg_popup_get_geometry_sym()) {
			weston_log("qdwin: layer-popup positioning DEGRADED "
				   "(libweston symbol "
				   "weston_desktop_xdg_popup_get_geometry "
				   "missing) — layer-parented popups will "
				   "render at parent origin\n");
		}
		if (!qdwin_xdg_popup_dismiss_layer_grab_sym()) {
			weston_log("qdwin: layer-popup outside-click dismissal "
				   "DEGRADED (libweston symbol "
				   "weston_desktop_xdg_popup_dismiss_layer_grab "
				   "missing) — the grab will end on outside "
				   "click but xdg_popup.popup_done will not be "
				   "sent\n");
		}
	}

	/* zxdg_decoration_manager_v1 v1: always-server_side stub. Keeps
	 * toolkit CSDs (Qt-Wayland bradient, GTK4 fallback) off so the
	 * private qdwin_shell_v1 chrome is the only decoration. */
	qdwin->xdg_decoration_manager_global = wl_global_create(
		ec->wl_display, &zxdg_decoration_manager_v1_interface,
		1, qdwin, bind_qdwin_xdg_decoration_manager);
	if (!qdwin->xdg_decoration_manager_global) {
		weston_log("qdwin: xdg-decoration wl_global_create failed\n");
		goto fail;
	}

	qdwin->destroy_listener.notify = qdwin_destroy;
	wl_signal_add(&ec->destroy_signal, &qdwin->destroy_listener);

	qdwin->output_created_listener.notify = qdwin_on_output_changed;
	wl_signal_add(&ec->output_created_signal,
		      &qdwin->output_created_listener);
	qdwin->output_resized_listener.notify = qdwin_on_output_resized;
	wl_signal_add(&ec->output_resized_signal,
		      &qdwin->output_resized_listener);
	qdwin->output_destroyed_listener.notify = qdwin_on_output_destroyed;
	wl_signal_add(&ec->output_destroyed_signal,
		      &qdwin->output_destroyed_listener);

	qdwin->seat_created_listener.notify = qdwin_on_seat_created;
	wl_signal_add(&ec->seat_created_signal,
		      &qdwin->seat_created_listener);
	/* Pick up seats that may already exist at shell-plugin load time
	 * (e.g. the default libinput seat on some backends). */
	{
		struct weston_seat *seat;
		wl_list_for_each(seat, &ec->seat_list, link)
			qdwin_track_seat(qdwin, seat);
	}

	qdwin_refresh_background(qdwin);

	/* Enables the weston-screenshooter CLI for capture during
	 * development; safe to call even if the symbol is missing
	 * from some hypothetical alternative weston build (the library
	 * loader fails fast at module load, not here). Gated to dev-only:
	 * the screenshooter exposes whole-output capture outside qdwin's
	 * per-view stream authorization model and must not ship enabled. */
	{
		const char *ss_env = getenv("QDWIN_ENABLE_SCREENSHOOTER");
		int ss_enabled = ss_env && (strcmp(ss_env, "1") == 0 ||
					    strcasecmp(ss_env, "true") == 0 ||
					    strcasecmp(ss_env, "yes") == 0);
		if (ss_enabled) {
			weston_log("qdwin: WARNING screenshooter enabled via "
				   "QDWIN_ENABLE_SCREENSHOOTER — dev/test only, "
				   "do not use in production\n");
			screenshooter_create(ec);
		}
	}

	/* §6.8 S1: nested-mode publisher init. No-op unless
	 * QDWIN_NESTED_MODE=1 in env. Must run after compositor + outputs
	 * are ready and after the pipewire-output API can be retrieved
	 * (backend-pipewire registers its API at backend init). */
	qdwin_nested_init(qdwin);

	weston_log("qdwin: shell loaded (allowed_uid=%u%s); "
		   "desktop + qdwin_shell_v1 ready\n",
		   (unsigned)qdwin->allowed_uid,
		   qdwin->nested_mode ? "; nested-mode publisher" : "");
	return 0;

fail:
	if (qdwin->desktop)
		weston_desktop_destroy(qdwin->desktop);
	weston_layer_fini(&qdwin->background_layer);
	weston_layer_fini(&qdwin->held_layer);
	weston_layer_fini(&qdwin->normal_layer);
	weston_layer_fini(&qdwin->minimized_layer);
	weston_layer_fini(&qdwin->panel_layer);
	weston_layer_fini(&qdwin->notification_layer);
	weston_layer_fini(&qdwin->launcher_layer);
	weston_layer_fini(&qdwin->lock_layer);
	weston_layer_fini(&qdwin->popup_layer);
	for (int i = 0; i < 4; i++)
		weston_layer_fini(&qdwin->layer_shell_layer[i]);
	free(qdwin);
	return -1;
}
