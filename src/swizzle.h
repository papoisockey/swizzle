#ifndef SWIZZLE_H
#define SWIZZLE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#ifdef WITH_SCENEFX
#include <scenefx/types/wlr_scene.h>
#else
#include <wlr/types/wlr_scene.h>
#endif
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#endif

#define TAGCOUNT 9
#define TAGMASK ((1u << TAGCOUNT) - 1)
#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define LISTEN(sig, l, fn) ((l)->notify = (fn), wl_signal_add((sig), (l)))

enum renderer_pref { RENDERER_AUTO, RENDERER_VULKAN, RENDERER_GLES, RENDERER_PIXMAN };
enum vrr_mode { VRR_OFF, VRR_ON, VRR_FULLSCREEN, VRR_CONTENT };
enum attach { ATTACH_TOP, ATTACH_ASIDE, ATTACH_BOTTOM };
enum anim_kind { ANIM_NONE, ANIM_SLIDE, ANIM_ZOOM };
enum tag_anim { TAG_ANIM_FADE, TAG_ANIM_SLIDE, TAG_ANIM_SLIDE_VERTICAL, TAG_ANIM_NONE };
enum scroll_center { SCROLL_EDGE, SCROLL_ALWAYS, SCROLL_OUTSIDE };
enum hot_corner { CORNER_TOP_LEFT, CORNER_TOP_RIGHT, CORNER_BOTTOM_LEFT, CORNER_BOTTOM_RIGHT };
struct curve {
	float x1, y1, x2, y2;
};
enum curve_kind { CURVE_OPEN, CURVE_CLOSE, CURVE_MOVE, CURVE_TAG, CURVE_FADE_IN, CURVE_FADE_OUT, CURVE_FOCUS, CURVE_COUNT };
enum duration_kind { DUR_OPEN, DUR_CLOSE, DUR_MOVE, DUR_TAG, DUR_FOCUS, DUR_COUNT };
#define SYM_WHEEL 0x21000000u
#define SYM_BUTTON 0x22000000u
#define SYM_SWIPE 0x23000000u
#define SYM_SWIPE_OF(n, d) (SYM_SWIPE + (uint32_t)(n) * 4 + (uint32_t)(d))
enum scene_layer { LYR_BG, LYR_BOTTOM, LYR_TILE, LYR_FLOAT, LYR_FS, LYR_ABOVE, LYR_TOP, LYR_OVERLAY, LYR_DRAG, LYR_LOCK,
	LYR_COUNT };
enum client_type { CLIENT_XDG, CLIENT_X11, CLIENT_X11_UNMANAGED, CLIENT_LAYER };
enum cursor_mode { CURSOR_NORMAL, CURSOR_MOVE, CURSOR_RESIZE };

struct bind {
	uint32_t mods;
	xkb_keysym_t sym;
	char *cmd;
	bool locked;
};

struct rule {
	char *app_id;
	char *title;
	int floating;
	int fullscreen;
	int scratchpad;
	int sticky;
	bool focus;
	uint32_t tags;
	char *output;
};

struct layer_rule {
	char *namespace;
	int animations, blur, shadow;
	int anim_open, anim_close;
};

struct output_conf {
	char *name;
	int width, height;
	float refresh;
	float scale;
	int x, y;
	bool has_pos;
	int transform;
	int vrr;
	int tearing;
	int enabled;
	int direct_scanout;
	int hardware_cursor;
	int layout;
	int primary;
	int tag_layout[TAGCOUNT];
};

struct pointer_conf {
	int tap, tap_drag, dwt, natural_scroll, left_handed, middle_emulation;
	int accel_profile, scroll_method, click_method;
	int drag_lock, enabled;
	double accel;
	bool has_accel;
};

struct env_var {
	char *key;
	char *value;
};

struct config {
	char *path;
	int errors;
	enum renderer_pref renderer;
	enum attach attach;
	bool tag_back_and_forth;
	char *tag_names[TAGCOUNT];
	char *lid_close, *lid_open;
	bool lid_close_locked, lid_open_locked;
	int gap_ih, gap_iv, gap_oh, gap_ov;
	bool smart_gaps;
	int border_width;
	int border_outer_width;
	float border_outer[4];
	float border_focus[4];
	float border_normal[4];
	float border_urgent[4];
	float border_maximized[4], border_scratchpad[4], border_sticky[4], border_overlay[4];
	float root_color[4];
	bool no_border_when_single, no_radius_when_single, monocle_no_border, monocle_no_gap;
	float mfact;
	int nmaster;
	int tag_count;
	bool focus_follows_mouse;
	bool focus_on_activate;
	bool warp_cursor;
	bool focus_cross_monitor, focus_cross_tag, focus_overlap_only;
	bool prefer_no_csd;
	uint32_t mouse_mod;
	int snap_distance;
	bool drag_tile_to_tile, drag_tile_small;
	float drop_color[4];
	char *cursor_theme;
	int cursor_size;
	float scratchpad_width, scratchpad_height;
	float scroll_width, scroll_width_single;
	float scroll_presets[8];
	int nscroll_presets;
	enum scroll_center scroll_center;
	int scroll_struts;
	int overview_gap_inner, overview_gap_outer;
	int hotarea_size;
	enum hot_corner hotarea_corner;
	bool hotarea_fullscreen;
	bool idle_inhibit_hidden, idle_inhibit_fullscreen;
	int axis_timeout;
	int animation_duration;
	int durations[DUR_COUNT];
	struct curve curves[CURVE_COUNT];
	enum anim_kind anim_open, anim_close;
	bool fade_in, fade_out;
	float fade_in_opacity, fade_out_opacity;
	float zoom_start, zoom_end;
	enum tag_anim tag_animation;
	bool layer_animations;
	float opacity_focused, opacity_unfocused;
	float dim_focused[4], dim_unfocused[4];
	int shadow_size, shadow_blur;
	int shadow_x, shadow_y;
	float shadow_color[4];
	bool shadow_floating_only, shadow_layers;
	int corner_radius;
	int blur_passes, blur_radius;
	float blur_noise, blur_brightness, blur_contrast, blur_saturation;
	bool blur_layers, blur_optimized;

	char *kb_layout, *kb_variant, *kb_model, *kb_options;
	int repeat_rate, repeat_delay;
	bool numlock;
	int gesture_fingers;
	int swipe_threshold;
	struct pointer_conf pointer, touchpad, mouse;

	struct output_conf *outputs;
	size_t noutputs;
	struct rule *rules;
	size_t nrules;
	struct layer_rule *layer_rules;
	size_t nlayer_rules;
	struct bind *binds;
	size_t nbinds;
	char **autostart;
	size_t nautostart;
	struct env_var *env;
	size_t nenv;
};

struct monitor {
	struct wl_list link;
	struct wlr_output *output;
	struct wlr_scene_output *scene_output;
	struct wlr_box m;
	struct wlr_box w;
	struct wl_list layers[4];
	uint32_t tags;
	uint32_t prevtags;
	float mfact;
	int nmaster;
	int layout;
	float tag_mfact[TAGCOUNT + 1];
	int tag_nmaster[TAGCOUNT + 1];
	int tag_layout[TAGCOUNT + 1];
	int scroll;
	bool overview;
	bool in_hotarea;
	bool configured;
	struct wlr_scene_rect *root;
	struct wlr_scene_node *optimized_blur;
	bool vrr_failed;
	bool vrr_toggled;
	bool tearing_active;
	bool asleep;
	bool scratch_shown;
	int lock_frames;
	bool scanout_locked;
	bool swcursor_locked;
	struct client *fullscreen;
	struct output_conf oc;
	struct output_conf cfg;
	struct wlr_ext_workspace_group_handle_v1 *ws_group;
	struct wlr_ext_workspace_handle_v1 *ws[TAGCOUNT];
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct client {
	enum client_type type;
	union {
		struct wlr_xdg_toplevel *xdg;
#ifdef XWAYLAND
		struct wlr_xwayland_surface *xwl;
#endif
	} s;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *scene_surface;
	struct wlr_scene_rect *border[8];
	struct wl_list link;
	struct wl_list flink;
	struct monitor *mon;
	struct wlr_box geom;
	struct wlr_box prev;
	struct wlr_box overview_geom;
	bool in_overview;
	struct wlr_box clip;
	uint32_t tags;
	bool floating, fullscreen, urgent, mapped, active;
	bool scratch;
	bool scratch_floated;
	bool sticky;
	bool minimized;
	bool maximized;
	bool fake_fullscreen;
	bool overlay;
	bool single;
	float scroll_width;
	bool scroll_stacked;
	int split;
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wlr_foreign_toplevel_handle_v1 *ftl;
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_ftl;
	struct wlr_ext_image_capture_source_v1 *capture;
	unsigned capture_gen;
	struct effects *fx;

	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener request_fullscreen;
	struct wl_listener request_maximize;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener deco_mode;
	struct wl_listener deco_destroy;
	struct wl_listener ftl_activate;
	struct wl_listener ftl_close;
	struct wl_listener ftl_fullscreen;
	struct wl_listener ftl_minimize;
#ifdef XWAYLAND
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener configure;
	struct wl_listener activate;
	struct wl_listener set_hints;
	struct wl_listener minimize;
	struct wl_listener set_geometry;
#endif
};

struct layer_surface {
	enum client_type type;
	struct wl_list link;
	struct wlr_layer_surface_v1 *ls;
	struct wlr_scene_layer_surface_v1 *scene_ls;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct monitor *mon;
	bool mapped;
	bool unmapping;
	struct effects *fx;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
};

struct server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_tree *layers[LYR_COUNT];
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_mgr;
	struct wlr_keyboard_group *kb_group;
	struct wlr_tearing_control_manager_v1 *tearing;
	struct wlr_relative_pointer_manager_v1 *relative_pointer;
	struct wlr_pointer_gestures_v1 *gestures;
	struct wlr_pointer_constraints_v1 *constraints;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wlr_linux_dmabuf_v1 *dmabuf;
	struct wlr_linux_drm_syncobj_manager_v1 *syncobj;
#ifdef XWAYLAND
	struct wlr_xwayland *xwayland;
#endif

	bool is_nvidia;
	bool is_virtual;
	bool soft_gpu;
	int drm_fd;

	struct wl_list monitors;
	struct wl_list clients;
	struct wl_list fstack;
	struct wl_list pointers;
	struct monitor *selmon;
	struct client *grab_c;
	enum cursor_mode cursor_mode;
	double grab_x, grab_y;
	struct wlr_box grab_geo;
	uint32_t grab_edges;
	struct wlr_box grab_box;
	bool grab_pending;
	double swipe_dx, swipe_dy;
	uint32_t swipe_fingers;
	struct layer_surface *exclusive_focus;
	bool locked;
	bool lock_pending;
	bool gaps_off;
	int gaps_delta;
	struct client *drop_target;
	struct wlr_scene_rect *drop_rect;
	bool grab_tiled;
	uint32_t last_axis_ms;

	struct config conf;
	const char *conf_arg;
	int ipc_fd;
};

extern struct server srv;

struct wlr_idle_inhibitor_v1;
struct effects;

void privileged_add(const struct wl_global *g);
pid_t spawn(const char *cmd);
void quit(void);

void output_init(void);
void output_finish(void);
void output_apply_config(struct monitor *m);
void output_update_all(void);
struct monitor *output_at(double x, double y);
struct monitor *output_by_name(const char *name);
struct monitor *output_dir(struct monitor *from, int dir);
void arrange(struct monitor *m);
void arrange_all(void);
void focus_client(struct client *c, bool lift);
void focus_top(struct monitor *m);
void focus_stack(int dir);
void focus_dir(int dx, int dy);
void swap_dir(int dx, int dy);
void view(uint32_t tags);
void toggle_view(uint32_t tags);
void tag(struct client *c, uint32_t tags);
void zoom(void);
const char *layout_name(int l);
int layout_from_name(const char *name);
int layout_count(void);
bool layout_floats(struct monitor *m);
bool client_reachable(struct client *c);
void scroll_width(struct client *c, const char *arg);
void scroll_stack(int dx, int dy);
void dwindle_toggle_split(void);
void tag_step(struct client *c, int dir);
uint32_t tags_shift(int dir, bool occupied);
uint32_t tag_mask(void);
void output_update_tag_names(void);
void output_update_root(void);
void state_changed(void);
struct client *monitor_fullscreen(struct monitor *m);
bool client_tiled(struct client *c);
int border_of(struct client *c);

void input_init(void);
void input_finish(void);
void input_apply_config(void);
void input_cancel_grab(void);
void input_apply_grab(void);
void input_start_grab(struct client *c, uint32_t edges);
void input_motion(uint32_t time, bool warp_only);
void input_set_focus_surface(struct wlr_surface *surface);
void input_click_focus(struct client *c, struct wlr_surface *surf);
void input_activity(void);
void input_map_touch(void);
void input_update_idle(struct wlr_idle_inhibitor_v1 *gone);
void input_warp_to(struct client *c);

void client_init(void);
void client_finish(void);
void client_resize(struct client *c, struct wlr_box geo);
void client_set_fullscreen(struct client *c, bool fs);
void client_set_floating(struct client *c, bool fl);
void client_restack(struct client *c);
void client_close(struct client *c);
void client_set_active(struct client *c, bool on);
void client_update_borders(void);
void client_update_xcursor(void);
const char *client_app_id(struct client *c);
const char *client_title(struct client *c);
struct wlr_surface *client_surface(struct client *c);
bool client_visible(struct client *c);
bool client_wants_tearing(struct client *c);
struct client *client_at(double x, double y, struct wlr_surface **surf, double *sx, double *sy);
struct client *client_focused(void);
struct client *client_last_focused(void);
struct client *client_from_surface(struct wlr_surface *surface);
void client_send_to_monitor(struct client *c, struct monitor *m, uint32_t tags);
void client_request_activate(struct client *c, bool user);
void client_reveal(struct client *c);
void client_set_minimized(struct client *c, bool on);
bool client_restore(void);
void client_set_maximized(struct client *c, bool on);
void client_set_fake_fullscreen(struct client *c, bool on);
void client_set_overlay(struct client *c, bool on);
void client_nudge(struct client *c, int dx, int dy, int dw, int dh);
void client_drop_on(struct client *c, struct client *target);
struct wlr_box client_fit(struct wlr_box b, struct monitor *m);
const float *client_border_color(struct client *c);

void shell_init(void);
void shell_finish(void);
void layer_arrange(struct monitor *m);
struct layer_surface *layer_from_surface(struct wlr_surface *surface);
void lock_arrange(void);
void lock_frame(struct monitor *m);
void lock_focus(struct wlr_surface *skip);
void ime_focus(struct wlr_surface *surface);
bool ime_key(struct wlr_keyboard *kb, struct wlr_keyboard_key_event *ev);
bool ime_modifiers(struct wlr_keyboard *kb);

bool config_load(struct config *conf, const char *path);
void config_free(struct config *conf);
char *config_default_path(void);
bool config_user_path(char *buf, size_t len);
void config_output(const char *name, struct output_conf *out);
void config_layer(const char *namespace, struct layer_rule *out);
float curve_ease(const struct curve *c, float t);
void config_apply_env(const struct config *old, const struct config *conf);

bool action_run(const char *cmd, char *err, size_t errlen);
bool action_known(const char *cmd);
void action_exec(const char *cmd);
bool ipc_init(void);
void ipc_finish(void);
void ipc_event(const char *fmt, ...);
void ipc_notify_state(void);
void ipc_client_event(const char *ev, struct client *c);
void ipc_title_event(struct client *c);

struct wlr_input_device;
#ifdef WITH_TABLET
void tablet_init(void);
void tablet_finish(void);
void tablet_add(struct wlr_input_device *dev);
#else
static inline void tablet_init(void) {}
static inline void tablet_finish(void) {}
static inline void tablet_add(struct wlr_input_device *dev) {}
#endif
#ifdef WITH_LID
void lid_add(struct wlr_input_device *dev);
#else
static inline void lid_add(struct wlr_input_device *dev) {}
#endif
#ifdef WITH_WINDOW_SHARE
void share_init(void);
void share_finish(void);
void share_map(struct client *c);
void share_unmap(struct client *c);
void share_update(struct client *c);
void share_gpu_reset(void);
#else
static inline void share_init(void) {}
static inline void share_finish(void) {}
static inline void share_map(struct client *c) {}
static inline void share_unmap(struct client *c) {}
static inline void share_update(struct client *c) {}
static inline void share_gpu_reset(void) {}
#endif
#ifdef WITH_VR
void lease_init(void);
void lease_finish(void);
void lease_offer(struct wlr_output *o);
#else
static inline void lease_init(void) {}
static inline void lease_finish(void) {}
static inline void lease_offer(struct wlr_output *o) {}
#endif
#ifdef WITH_PORTAL
void portal_init(void);
void portal_export(void);
bool portal_modal(struct wlr_xdg_toplevel *t);
#else
static inline void portal_init(void) {}
static inline void portal_export(void) {}
static inline bool portal_modal(struct wlr_xdg_toplevel *t) { return false; }
#endif
void scratch_show(struct monitor *m, bool show);
void scratch_fit(struct client *c);
#ifdef WITH_SCRATCHPAD
void scratch_toggle(void);
void scratch_set(struct client *c, bool on);
#else
static inline void scratch_toggle(void) {}
static inline void scratch_set(struct client *c, bool on) {}
#endif
#ifdef WITH_EFFECTS
void effects_init(void);
void effects_finish(void);
void effects_apply_config(void);
void effects_client_create(struct client *c);
void effects_client_destroy(struct client *c);
void effects_map(struct client *c);
void effects_unmap(struct client *c);
void effects_move(struct client *c, int x, int y);
void effects_settle(struct client *c);
void effects_resize(struct client *c);
void effects_show(struct monitor *m, uint32_t oldtags);
void effects_tag_leave(struct monitor *m, uint32_t oldtags, uint32_t newtags);
void effects_focus(struct client *c);
void effects_frame(void);
void effects_layer_create(struct layer_surface *ls);
void effects_layer_destroy(struct layer_surface *ls);
void effects_layer_map(struct layer_surface *ls);
void effects_layer_unmap(struct layer_surface *ls);
void effects_layer_commit(struct layer_surface *ls);
void effects_monitor_update(struct monitor *m);
void effects_background_changed(struct monitor *m);
#else
static inline void effects_init(void) {}
static inline void effects_finish(void) {}
static inline void effects_apply_config(void) {}
static inline void effects_client_create(struct client *c) {}
static inline void effects_client_destroy(struct client *c) {}
static inline void effects_map(struct client *c) {}
static inline void effects_unmap(struct client *c) {}
static inline void effects_move(struct client *c, int x, int y) { wlr_scene_node_set_position(&c->scene->node, x, y); }
static inline void effects_settle(struct client *c) {}
static inline void effects_resize(struct client *c) {}
static inline void effects_show(struct monitor *m, uint32_t oldtags) {}
static inline void effects_tag_leave(struct monitor *m, uint32_t oldtags, uint32_t newtags) {}
static inline void effects_focus(struct client *c) {}
static inline void effects_frame(void) {}
static inline void effects_layer_create(struct layer_surface *ls) {}
static inline void effects_layer_destroy(struct layer_surface *ls) {}
static inline void effects_layer_map(struct layer_surface *ls) {}
static inline void effects_layer_unmap(struct layer_surface *ls) {}
static inline void effects_layer_commit(struct layer_surface *ls) {}
static inline void effects_monitor_update(struct monitor *m) {}
static inline void effects_background_changed(struct monitor *m) {}
#endif
#ifdef WITH_OVERVIEW
void overview_set(struct monitor *m, bool on);
void overview_arrange(struct monitor *m);
void overview_hotarea(double x, double y);
#else
static inline void overview_set(struct monitor *m, bool on) {}
static inline void overview_arrange(struct monitor *m) {}
static inline void overview_hotarea(double x, double y) {}
#endif
#ifdef WITH_PRESENTATION
void presentation_init(void);
bool client_wants_vrr(struct client *c);
#else
static inline void presentation_init(void) {}
static inline bool client_wants_vrr(struct client *c) { return false; }
#endif

#endif
