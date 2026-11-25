#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <libinput.h>
#ifdef __linux__
#include <linux/input-event-codes.h>
#else
#include <dev/evdev/input-event-codes.h>
#endif
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/region.h>
#include <wlr/xcursor.h>
#include "swizzle.h"

struct device {
	struct wl_list link;
	struct wlr_input_device *dev;
	struct wl_listener destroy;
};

struct vkeyboard {
	struct wlr_keyboard *kb;
	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener destroy;
};

struct inhibitor {
	struct wlr_idle_inhibitor_v1 *in;
	struct wl_listener destroy;
};

struct constraint {
	struct wlr_pointer_constraint_v1 *c;
	struct wl_listener destroy;
};

static struct wl_list keyboards, vpointers;
static struct wl_listener new_input, kb_key, kb_modifiers;
static struct wl_listener cursor_motion, cursor_motion_abs, cursor_button, cursor_axis, cursor_frame;
static struct wl_listener swipe_begin, swipe_update, swipe_end;
static struct wl_listener pinch_begin, pinch_update, pinch_end, hold_begin, hold_end;
static struct wl_listener request_cursor, request_shape, request_selection, request_primary;
static struct wl_listener request_drag, start_drag, drag_destroy, new_constraint;
static struct wl_listener new_inhibitor, new_vkeyboard, new_vpointer, new_kb_inhibitor;
static struct wl_listener touch_down, touch_up, touch_motion, touch_cancel, touch_frame;
static struct wl_list touches;
static struct wlr_keyboard_shortcuts_inhibit_manager_v1 *kb_inhibit;
static struct {
	bool used;
	int32_t id;
	double dx, dy;
	int ox, oy;
} fingers[10];
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
static struct wl_event_source *repeat_timer;
static uint32_t repeat_mods;
static xkb_keysym_t repeat_sym;
static uint32_t swallowed[8];
static uint32_t consumed[WLR_KEYBOARD_KEYS_CAP];
static size_t nconsumed;
static uint32_t grab_button;

static bool key_consumed(uint32_t keycode)
{
	for (size_t i = 0; i < nconsumed; i++)
		if (consumed[i] == keycode)
			return true;
	return false;
}

static bool unconsume_key(uint32_t keycode)
{
	for (size_t i = 0; i < nconsumed; i++)
		if (consumed[i] == keycode) {
			consumed[i] = consumed[--nconsumed];
			return true;
		}
	return false;
}
static double press_dx, press_dy;
static int press_nx, press_ny;
static struct wlr_surface *press_surface;

void input_surface_origin(struct wlr_surface *s, int *x, int *y)
{
	struct client *c = client_from_surface(s);
	struct layer_surface *ls = c ? NULL : layer_from_surface(s);
	struct wlr_scene_node *n = c ? &c->scene->node : ls ? &ls->scene->node : NULL;
	*x = *y = 0;
	if (n)
		wlr_scene_node_coords(n, x, y);
}

static struct xkb_keymap *build_keymap(void)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = {
		.model = srv.conf.kb_model,
		.layout = srv.conf.kb_layout,
		.variant = srv.conf.kb_variant,
		.options = srv.conf.kb_options,
	};
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km) {
		wlr_log(WLR_ERROR, "invalid keyboard layout, using defaults");
		km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	xkb_context_unref(ctx);
	return km;
}

static int pick(int class, int base)
{
	return class >= 0 ? class : base;
}

static void configure_pointer(struct wlr_input_device *dev)
{
	if (!wlr_input_device_is_libinput(dev))
		return;
	struct libinput_device *d = wlr_libinput_get_device_handle(dev);
	bool touchpad = libinput_device_config_tap_get_finger_count(d) > 0;
	const struct pointer_conf *b = &srv.conf.pointer, *p = touchpad ? &srv.conf.touchpad : &srv.conf.mouse;
	int v;
	if (touchpad) {
		v = pick(p->tap, b->tap);
		libinput_device_config_tap_set_enabled(d, v < 0 ? libinput_device_config_tap_get_default_enabled(d) :
			v ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
		v = pick(p->tap_drag, b->tap_drag);
		libinput_device_config_tap_set_drag_enabled(d, v < 0 ? libinput_device_config_tap_get_default_drag_enabled(d) :
			v ? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
		v = pick(p->dwt, b->dwt);
		if (libinput_device_config_dwt_is_available(d))
			libinput_device_config_dwt_set_enabled(d, v < 0 ? libinput_device_config_dwt_get_default_enabled(d) :
				v ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
		v = pick(p->drag_lock, b->drag_lock);
		libinput_device_config_tap_set_drag_lock_enabled(d, v < 0 ?
			libinput_device_config_tap_get_default_drag_lock_enabled(d) :
			v ? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED : LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
	}
	v = pick(p->enabled, b->enabled);
	libinput_device_config_send_events_set_mode(d, v < 0 ? libinput_device_config_send_events_get_default_mode(d) :
		v ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED);
	v = pick(p->natural_scroll, b->natural_scroll);
	if (libinput_device_config_scroll_has_natural_scroll(d))
		libinput_device_config_scroll_set_natural_scroll_enabled(d, v < 0 ?
			libinput_device_config_scroll_get_default_natural_scroll_enabled(d) : v);
	v = pick(p->left_handed, b->left_handed);
	if (libinput_device_config_left_handed_is_available(d))
		libinput_device_config_left_handed_set(d, v < 0 ? libinput_device_config_left_handed_get_default(d) : v);
	v = pick(p->middle_emulation, b->middle_emulation);
	if (libinput_device_config_middle_emulation_is_available(d))
		libinput_device_config_middle_emulation_set_enabled(d, v < 0 ?
			libinput_device_config_middle_emulation_get_default_enabled(d) :
			v ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
	if (libinput_device_config_accel_is_available(d))
		libinput_device_config_accel_set_speed(d, p->has_accel ? p->accel : b->has_accel ? b->accel :
			libinput_device_config_accel_get_default_speed(d));
	v = pick(p->accel_profile, b->accel_profile);
	if (v < 0 || !(libinput_device_config_accel_get_profiles(d) & (uint32_t)v))
		v = libinput_device_config_accel_get_default_profile(d);
	if (v != LIBINPUT_CONFIG_ACCEL_PROFILE_NONE)
		libinput_device_config_accel_set_profile(d, v);
	v = pick(p->scroll_method, b->scroll_method);
	if (v < 0 || (v != LIBINPUT_CONFIG_SCROLL_NO_SCROLL && !(libinput_device_config_scroll_get_methods(d) & (uint32_t)v)))
		v = libinput_device_config_scroll_get_default_method(d);
	libinput_device_config_scroll_set_method(d, v);
	v = pick(p->click_method, b->click_method);
	if (v < 0 || (v != LIBINPUT_CONFIG_CLICK_METHOD_NONE && !(libinput_device_config_click_get_methods(d) & (uint32_t)v)))
		v = libinput_device_config_click_get_default_method(d);
	libinput_device_config_click_set_method(d, v);
}

void input_map_touch(void)
{
	struct device *d;
	wl_list_for_each(d, &touches, link) {
		struct wlr_touch *t = wlr_touch_from_input_device(d->dev);
		struct monitor *m = t->output_name ? output_by_name(t->output_name) : NULL;
		if (m)
			wlr_cursor_map_input_to_output(srv.cursor, d->dev, m->output);
	}
}

static void update_caps(void)
{
	wlr_seat_set_capabilities(srv.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD |
		(wl_list_empty(&touches) ? 0 : WL_SEAT_CAPABILITY_TOUCH));
}

static void handle_device_destroy(struct wl_listener *l, void *data)
{
	struct device *d = wl_container_of(l, d, destroy);
	wl_list_remove(&d->link);
	wl_list_remove(&d->destroy.link);
	free(d);
	update_caps();
}

static void track_device(struct wl_list *list, struct wlr_input_device *dev)
{
	struct device *d = calloc(1, sizeof(*d));
	if (!d)
		return;
	d->dev = dev;
	wl_list_insert(list, &d->link);
	LISTEN(&dev->events.destroy, &d->destroy, handle_device_destroy);
}

static char *gtk_cursor_theme(void)
{
	char path[4096], line[256], val[256];
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (xdg && *xdg)
		snprintf(path, sizeof(path), "%s/gtk-3.0/settings.ini", xdg);
	else if (home)
		snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini", home);
	else
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *theme = NULL;
	while (!theme && fgets(line, sizeof(line), f))
		if (sscanf(line, " gtk-cursor-theme-name = %255[^\r\n]", val) == 1)
			theme = strdup(val);
	fclose(f);
	return theme;
}

static void numlock_on(struct wlr_keyboard *kb)
{
	xkb_mod_index_t num = kb->keymap ? xkb_keymap_mod_get_index(kb->keymap, XKB_MOD_NAME_NUM) : XKB_MOD_INVALID;
	if (srv.conf.numlock && num != XKB_MOD_INVALID)
		wlr_keyboard_notify_modifiers(kb, kb->modifiers.depressed, kb->modifiers.latched,
			kb->modifiers.locked | (1u << num), kb->modifiers.group);
}

void input_apply_config(void)
{
	struct device *d;
	struct xkb_keymap *km = build_keymap();
	if (km && srv.kb_group->keyboard.keymap && wlr_keyboard_keymaps_match(km, srv.kb_group->keyboard.keymap)) {
		xkb_keymap_unref(km);
		km = NULL;
	}
	wl_event_source_timer_update(repeat_timer, 0);
	static bool numlock_applied;
	bool numlock = km || (srv.conf.numlock && !numlock_applied);
	numlock_applied = srv.conf.numlock;
	if (km) {
		wlr_keyboard_set_keymap(&srv.kb_group->keyboard, km);
		wl_list_for_each(d, &keyboards, link)
			wlr_keyboard_set_keymap(wlr_keyboard_from_input_device(d->dev), km);
		xkb_keymap_unref(km);
	}
	if (numlock) {
		numlock_on(&srv.kb_group->keyboard);
		wl_list_for_each(d, &keyboards, link)
			numlock_on(wlr_keyboard_from_input_device(d->dev));
	}
	wlr_keyboard_set_repeat_info(&srv.kb_group->keyboard, srv.conf.repeat_rate, srv.conf.repeat_delay);
	wl_list_for_each(d, &keyboards, link)
		wlr_keyboard_set_repeat_info(wlr_keyboard_from_input_device(d->dev), srv.conf.repeat_rate, srv.conf.repeat_delay);
	wl_list_for_each(d, &srv.pointers, link)
		configure_pointer(d->dev);

	static char *env_theme;
	static bool env_read;
	if (!env_read) {
		const char *env = config_env_original("XCURSOR_THEME");
		env_theme = env && *env ? strdup(env) : NULL;
		env_read = true;
	}
	const char *theme_env = env_theme;
	for (size_t i = 0; i < srv.conf.nenv; i++)
		if (!strcmp(srv.conf.env[i].key, "XCURSOR_THEME"))
			theme_env = *srv.conf.env[i].value ? srv.conf.env[i].value : NULL;
	if (!srv.conf.cursor_theme)
		srv.conf.cursor_theme = theme_env ? strdup(theme_env) : gtk_cursor_theme();
	char size[16];
	snprintf(size, sizeof(size), "%d", srv.conf.cursor_size);
	if (srv.conf.cursor_theme) {
		setenv("XCURSOR_THEME", srv.conf.cursor_theme, 1);
		wlr_log(WLR_INFO, "cursor theme: %s %d", srv.conf.cursor_theme, srv.conf.cursor_size);
	} else {
		unsetenv("XCURSOR_THEME");
	}
	setenv("XCURSOR_SIZE", size, 1);

	static char *loaded_theme;
	static int loaded_size;
	const char *theme = srv.conf.cursor_theme ? srv.conf.cursor_theme : "";
	if (srv.xcursor_mgr && loaded_size == srv.conf.cursor_size && loaded_theme && !strcmp(loaded_theme, theme))
		return;
	bool reload = srv.xcursor_mgr != NULL;
	free(loaded_theme);
	loaded_theme = strdup(theme);
	loaded_size = srv.conf.cursor_size;
	wlr_cursor_unset_image(srv.cursor);
	if (srv.xcursor_mgr)
		wlr_xcursor_manager_destroy(srv.xcursor_mgr);
	srv.xcursor_mgr = wlr_xcursor_manager_create(srv.conf.cursor_theme, srv.conf.cursor_size);
	wlr_cursor_set_xcursor(srv.cursor, srv.xcursor_mgr, srv.cursor_mode == CURSOR_NORMAL ? "default" :
		srv.grab_edges ? wlr_xcursor_get_resize_name(srv.grab_edges) : "grabbing");
	client_update_xcursor();
	if (reload && srv.cursor_mode == CURSOR_NORMAL) {
		wlr_seat_pointer_notify_clear_focus(srv.seat);
		input_motion(0, true);
	}
}

static void handle_new_input(struct wl_listener *l, void *data)
{
	struct wlr_input_device *dev = data;

	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: {
		struct wlr_keyboard *kb = wlr_keyboard_from_input_device(dev);
		wlr_keyboard_set_keymap(kb, srv.kb_group->keyboard.keymap);
		wlr_keyboard_set_repeat_info(kb, srv.conf.repeat_rate, srv.conf.repeat_delay);
		wlr_keyboard_group_add_keyboard(srv.kb_group, kb);
		numlock_on(kb);
		track_device(&keyboards, dev);
		break;
	}
	case WLR_INPUT_DEVICE_POINTER:
		configure_pointer(dev);
		wlr_cursor_attach_input_device(srv.cursor, dev);
		track_device(&srv.pointers, dev);
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		wlr_cursor_attach_input_device(srv.cursor, dev);
		track_device(&touches, dev);
		input_map_touch();
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		lid_add(dev);
		break;
	case WLR_INPUT_DEVICE_TABLET:
		tablet_add(dev);
		break;
	default:
		break;
	}
	update_caps();
}

static uint32_t binding_mods(struct wlr_keyboard *kb)
{
	return kb ? wlr_keyboard_get_modifiers(kb) & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2) : 0;
}

static uint32_t pointer_mods(void)
{
	return binding_mods(&srv.kb_group->keyboard) | binding_mods(wlr_seat_get_keyboard(srv.seat));
}

static bool repeatable(const char *cmd)
{
	static const char *ok[] = {"focus", "swap", "mfact", "nmaster", "viewnext", "viewprev"};
	size_t n = strcspn(cmd, " \t");
	for (size_t i = 0; i < LENGTH(ok); i++)
		if (strlen(ok[i]) == n && !strncmp(cmd, ok[i], n))
			return true;
	return false;
}

static bool run_binding(uint32_t mods, xkb_keysym_t sym, bool *repeat)
{
	for (size_t i = 0; i < srv.conf.nbinds; i++) {
		const struct bind *b = &srv.conf.binds[i];
		if (b->mods != mods || b->sym != sym || (srv.locked && !b->locked))
			continue;
		*repeat = repeatable(b->cmd);
		action_exec(b->cmd);
		return true;
	}
	return false;
}

static bool has_binding(uint32_t mods, xkb_keysym_t sym)
{
	for (size_t i = 0; i < srv.conf.nbinds; i++)
		if (srv.conf.binds[i].mods == mods && srv.conf.binds[i].sym == sym && (!srv.locked || srv.conf.binds[i].locked))
			return true;
	return false;
}

static bool shortcuts_inhibited(void)
{
	struct wlr_keyboard_shortcuts_inhibitor_v1 *in;
	struct wlr_surface *focused = srv.seat->keyboard_state.focused_surface;
	wl_list_for_each(in, &kb_inhibit->inhibitors, link)
		if (in->active && in->surface == focused)
			return true;
	return false;
}

static void handle_new_kb_inhibitor(struct wl_listener *l, void *data)
{
	wlr_keyboard_shortcuts_inhibitor_v1_activate(data);
}

static bool bindings_allowed(void)
{
	return srv.locked || (!srv.exclusive_focus && !shortcuts_inhibited());
}

static int handle_repeat(void *data)
{
	bool repeat;
	if (!bindings_allowed() || srv.conf.repeat_rate <= 0)
		return 0;
	if (run_binding(repeat_mods, repeat_sym, &repeat) && repeat)
		wl_event_source_timer_update(repeat_timer, MAX(1, 1000 / srv.conf.repeat_rate));
	return 0;
}

static void keyboard_key(struct wlr_keyboard *kb, struct wlr_keyboard_key_event *ev)
{
	uint32_t keycode = ev->keycode + 8;
	bool handled = false, repeat = false, bindings = bindings_allowed();

	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
	wl_event_source_timer_update(repeat_timer, 0);

	bool was_consumed = unconsume_key(ev->keycode);
	if (ev->state == WL_KEYBOARD_KEY_STATE_RELEASED && was_consumed)
		return;
	if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		const xkb_keysym_t *syms;
		bool physical = !wlr_input_device_get_virtual_keyboard(&kb->base);
		if (nconsumed < LENGTH(consumed))
			consumed[nconsumed++] = ev->keycode;
		int n = xkb_state_key_get_syms(kb->xkb_state, keycode, &syms);
		for (int i = 0; i < n; i++)
			if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
				if (srv.session && physical)
					wlr_session_change_vt(srv.session, syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
				handled = true;
			}
		uint32_t mods = binding_mods(kb);
		xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);
		xkb_level_index_t levels = xkb_keymap_num_levels_for_key(kb->keymap, keycode, layout);
		for (xkb_level_index_t lv = 0; lv < levels && !handled && bindings; lv++) {
			n = xkb_keymap_key_get_syms_by_level(kb->keymap, keycode, layout, lv, &syms);
			for (int i = 0; i < n && !handled; i++) {
				handled = run_binding(mods, xkb_keysym_to_lower(syms[i]), &repeat);
				if (handled && repeat && srv.conf.repeat_rate > 0) {
					repeat_mods = mods;
					repeat_sym = xkb_keysym_to_lower(syms[i]);
					wl_event_source_timer_update(repeat_timer, MAX(1, srv.conf.repeat_delay));
				}
			}
		}
		if (!handled)
			unconsume_key(ev->keycode);
	}
	if (handled || ime_key(kb, ev))
		return;
	if (srv.locked && srv.seat->keyboard_state.grab != srv.seat->keyboard_state.default_grab) {
		wlr_seat_keyboard_end_grab(srv.seat);
		lock_focus(NULL);
	}
	wlr_seat_set_keyboard(srv.seat, kb);
	wlr_seat_keyboard_notify_key(srv.seat, ev->time_msec, ev->keycode, ev->state);
}

static void handle_key(struct wl_listener *l, void *data)
{
	keyboard_key(&srv.kb_group->keyboard, data);
}

static void keyboard_modifiers(struct wlr_keyboard *kb)
{
	if (ime_modifiers(kb))
		return;
	wlr_seat_set_keyboard(srv.seat, kb);
	wlr_seat_keyboard_notify_modifiers(srv.seat, &kb->modifiers);
}

static void handle_modifiers(struct wl_listener *l, void *data)
{
	keyboard_modifiers(&srv.kb_group->keyboard);
}

static void handle_vkeyboard_key(struct wl_listener *l, void *data)
{
	struct vkeyboard *v = wl_container_of(l, v, key);
	if (srv.locked)
		return;
	keyboard_key(v->kb, data);
}

static void handle_vkeyboard_modifiers(struct wl_listener *l, void *data)
{
	struct vkeyboard *v = wl_container_of(l, v, modifiers);
	if (srv.locked)
		return;
	keyboard_modifiers(v->kb);
}

static void handle_vkeyboard_destroy(struct wl_listener *l, void *data)
{
	struct vkeyboard *v = wl_container_of(l, v, destroy);
	wl_list_remove(&v->key.link);
	wl_list_remove(&v->modifiers.link);
	wl_list_remove(&v->destroy.link);
	free(v);
}

static void handle_new_vkeyboard(struct wl_listener *l, void *data)
{
	struct wlr_virtual_keyboard_v1 *vk = data;
	struct vkeyboard *v = calloc(1, sizeof(*v));
	if (!v)
		return;
	v->kb = &vk->keyboard;
	LISTEN(&vk->keyboard.events.key, &v->key, handle_vkeyboard_key);
	LISTEN(&vk->keyboard.events.modifiers, &v->modifiers, handle_vkeyboard_modifiers);
	LISTEN(&vk->keyboard.base.events.destroy, &v->destroy, handle_vkeyboard_destroy);
}

static void handle_new_vpointer(struct wl_listener *l, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *ev = data;
	struct wlr_input_device *dev = &ev->new_pointer->pointer.base;
	wlr_cursor_attach_input_device(srv.cursor, dev);
	track_device(&vpointers, dev);
	if (ev->suggested_output)
		wlr_cursor_map_input_to_output(srv.cursor, dev, ev->suggested_output);
}

static bool locked_virtual(struct wlr_pointer *p)
{
	struct device *d;
	if (!srv.locked)
		return false;
	wl_list_for_each(d, &vpointers, link)
		if (d->dev == &p->base)
			return true;
	return false;
}

void input_update_idle(struct wlr_idle_inhibitor_v1 *gone)
{
	struct wlr_idle_inhibitor_v1 *in;
	bool inhibit = false;
	wl_list_for_each(in, &idle_inhibit->inhibitors, link) {
		if (in == gone)
			continue;
		struct client *c = client_from_surface(in->surface);
		if (srv.conf.idle_inhibit_hidden || (c ? client_visible(c) : in->surface->mapped)) {
			inhibit = true;
			break;
		}
	}
	struct monitor *m;
	if (srv.conf.idle_inhibit_fullscreen)
		wl_list_for_each(m, &srv.monitors, link)
			if (m->output->enabled && m->fullscreen)
				inhibit = true;
	wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibit && !srv.locked);
}

static void handle_inhibitor_destroy(struct wl_listener *l, void *data)
{
	struct inhibitor *i = wl_container_of(l, i, destroy);
	struct wlr_idle_inhibitor_v1 *in = i->in;
	wl_list_remove(&i->destroy.link);
	free(i);
	input_update_idle(in);
}

static void handle_new_inhibitor(struct wl_listener *l, void *data)
{
	struct inhibitor *i = calloc(1, sizeof(*i));
	if (i) {
		i->in = data;
		LISTEN(&i->in->events.destroy, &i->destroy, handle_inhibitor_destroy);
	}
	input_update_idle(NULL);
}

void input_activity(void)
{
	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
}

void input_warp_to(struct client *c)
{
	if (!c || !c->mapped || srv.cursor_mode != CURSOR_NORMAL ||
			wlr_box_contains_point(&c->geom, srv.cursor->x, srv.cursor->y))
		return;
	wlr_cursor_warp_closest(srv.cursor, NULL, c->geom.x + c->geom.width / 2.0, c->geom.y + c->geom.height / 2.0);
	input_motion(0, true);
}

static void warp_to_hint(struct wlr_pointer_constraint_v1 *pc)
{
	struct wlr_seat_pointer_state *p = &srv.seat->pointer_state;
	if (!pc->current.cursor_hint.enabled || p->focused_surface != pc->surface)
		return;
	double sx = pc->current.cursor_hint.x, sy = pc->current.cursor_hint.y;
	wlr_cursor_warp(srv.cursor, NULL, srv.cursor->x - p->sx + sx, srv.cursor->y - p->sy + sy);
	wlr_seat_pointer_warp(srv.seat, sx, sy);
}

static void input_check_constraint(void)
{
	struct wlr_surface *s = srv.seat->keyboard_state.focused_surface;
	struct wlr_pointer_constraint_v1 *c = s ? wlr_pointer_constraints_v1_constraint_for_surface(srv.constraints, s, srv.seat) : NULL;
	if (c == srv.active_constraint)
		return;
	if (srv.active_constraint) {
		warp_to_hint(srv.active_constraint);
		wlr_pointer_constraint_v1_send_deactivated(srv.active_constraint);
	}
	srv.active_constraint = c;
	if (c)
		wlr_pointer_constraint_v1_send_activated(c);
}

void input_set_focus_surface(struct wlr_surface *surface)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
	if (srv.locked && surface && !wlr_session_lock_surface_v1_try_from_wlr_surface(surface))
		return;
	if (srv.seat->drag && srv.seat->keyboard_state.grab == &srv.seat->drag->keyboard_grab)
		return;
	if (surface != srv.seat->keyboard_state.focused_surface &&
			srv.seat->keyboard_state.grab != srv.seat->keyboard_state.default_grab)
		wlr_seat_keyboard_end_grab(srv.seat);
	if (!surface) {
		wlr_seat_keyboard_notify_clear_focus(srv.seat);
	} else if (kb) {
		uint32_t keys[WLR_KEYBOARD_KEYS_CAP];
		size_t n = 0;
		for (size_t i = 0; i < kb->num_keycodes && n < LENGTH(keys); i++)
			if (!key_consumed(kb->keycodes[i]))
				keys[n++] = kb->keycodes[i];
		wlr_seat_keyboard_notify_enter(srv.seat, surface, keys, n, &kb->modifiers);
	} else
		wlr_seat_keyboard_notify_enter(srv.seat, surface, NULL, 0, NULL);
	input_check_constraint();
	ime_focus(surface);
}

static void handle_constraint_destroy(struct wl_listener *l, void *data)
{
	struct constraint *c = wl_container_of(l, c, destroy);
	if (srv.active_constraint == c->c) {
		warp_to_hint(c->c);
		srv.active_constraint = NULL;
	}
	wl_list_remove(&c->destroy.link);
	free(c);
}

static void handle_new_constraint(struct wl_listener *l, void *data)
{
	struct wlr_pointer_constraint_v1 *pc = data;
	struct constraint *c = calloc(1, sizeof(*c));
	if (!c)
		return;
	c->c = pc;
	LISTEN(&pc->events.destroy, &c->destroy, handle_constraint_destroy);
	input_check_constraint();
}

void input_apply_grab(void)
{
	if (srv.grab_pending && srv.grab_c)
		client_resize(srv.grab_c, srv.grab_box);
	srv.grab_pending = false;
}

static void grab_to(struct client *c, struct wlr_box box)
{
	srv.grab_box = box;
	srv.grab_pending = true;
	if (c->mon)
		wlr_output_schedule_frame(c->mon->output);
}

void input_start_grab(struct client *c, uint32_t edges)
{
	if (!c || !c->mapped || c->fullscreen || c->type == CLIENT_X11_UNMANAGED || srv.locked ||
			srv.cursor_mode != CURSOR_NORMAL)
		return;
	bool to_tile = !c->floating && !edges && srv.conf.drag_tile_to_tile && !c->maximized && !c->scratch &&
		!layout_floats(c->mon);
	if (!to_tile && (c->maximized || !c->floating)) {
		if (c->maximized)
			client_set_maximized(c, false);
		if (!c->floating) {
			c->prev = c->geom;
			client_set_floating(c, true);
		}
	}
	struct wlr_seat_pointer_state *p = &srv.seat->pointer_state;
	grab_button = p->button_count ? p->buttons[p->button_count - 1].button : 0;
	srv.grab_c = c;
	srv.grab_geo = c->geom;
	srv.grab_x = srv.cursor->x;
	srv.grab_y = srv.cursor->y;
	if (to_tile) {
		srv.grab_tiled = true;
		if (srv.conf.drag_tile_small) {
			srv.grab_geo.width = MAX(50, c->geom.width / 2);
			srv.grab_geo.height = MAX(50, c->geom.height / 2);
			srv.grab_geo.x = (int)srv.cursor->x - srv.grab_geo.width / 2;
			srv.grab_geo.y = (int)srv.cursor->y - srv.grab_geo.height / 2;
			client_resize(c, srv.grab_geo);
		}
		wlr_scene_node_reparent(&c->scene->node, srv.layers[LYR_FLOAT]);
		arrange(c->mon);
		wlr_scene_node_set_enabled(&c->scene->node, true);
	}
	srv.grab_edges = edges;
	srv.cursor_mode = edges ? CURSOR_RESIZE : CURSOR_MOVE;
	wlr_cursor_set_xcursor(srv.cursor, srv.xcursor_mgr, edges ? wlr_xcursor_get_resize_name(edges) : "grabbing");
}

void input_cancel_grab(void)
{
	srv.grab_pending = false;
	if (srv.cursor_mode == CURSOR_NORMAL)
		return;
	struct client *c = srv.grab_c;
	srv.cursor_mode = CURSOR_NORMAL;
	srv.grab_c = NULL;
	srv.drop_target = NULL;
	if (srv.drop_rect)
		wlr_scene_node_set_enabled(&srv.drop_rect->node, false);
	if (srv.grab_tiled) {
		srv.grab_tiled = false;
		if (c && c->mapped) {
			client_restack(c);
			arrange(c->mon);
		}
	}
	wlr_cursor_set_xcursor(srv.cursor, srv.xcursor_mgr, "default");
}

static void update_drop_target(void)
{
	struct client *o, *target = NULL;
	wl_list_for_each(o, &srv.clients, link)
		if (o != srv.grab_c && client_tiled(o) && wlr_box_contains_point(&o->geom, srv.cursor->x, srv.cursor->y)) {
			target = o;
			break;
		}
	srv.drop_target = target;
	if (!srv.drop_rect && !(srv.drop_rect = wlr_scene_rect_create(srv.layers[LYR_FLOAT], 0, 0, srv.conf.drop_color)))
		return;
	wlr_scene_node_set_enabled(&srv.drop_rect->node, target != NULL);
	if (!target)
		return;
	wlr_scene_rect_set_color(srv.drop_rect, srv.conf.drop_color);
	wlr_scene_rect_set_size(srv.drop_rect, target->geom.width, target->geom.height);
	wlr_scene_node_set_position(&srv.drop_rect->node, target->geom.x, target->geom.y);
	wlr_scene_node_place_below(&srv.drop_rect->node, &srv.grab_c->scene->node);
}

static void snap(struct wlr_box *b, struct client *c)
{
	struct monitor *m = c->mon;
	struct client *o;
	int d = srv.conf.snap_distance, best[2] = {d + 1, d + 1};
	if (!m || d <= 0)
		return;
	for (int axis = 0; axis < 2; axis++) {
		int lo = axis ? b->y : b->x, hi = lo + (axis ? b->height : b->width);
		int lines[2] = {axis ? m->w.y : m->w.x, axis ? m->w.y + m->w.height : m->w.x + m->w.width};
		for (int i = 0; i < 2; i++)
			for (int e = 0; e < 2; e++) {
				int diff = lines[i] - (e ? hi : lo);
				if (abs(diff) < abs(best[axis]))
					best[axis] = diff;
			}
		wl_list_for_each(o, &srv.clients, link) {
			if (o == c || o->mon != m || !client_visible(o))
				continue;
			int blo = axis ? b->x : b->y, bhi = blo + (axis ? b->width : b->height);
			int plo = axis ? o->geom.x : o->geom.y, phi = plo + (axis ? o->geom.width : o->geom.height);
			if (phi < blo - d || plo > bhi + d)
				continue;
			int olo = axis ? o->geom.y : o->geom.x, ohi = olo + (axis ? o->geom.height : o->geom.width);
			int a = olo - hi, bb = ohi - lo;
			if (abs(a) < abs(best[axis]))
				best[axis] = a;
			if (abs(bb) < abs(best[axis]))
				best[axis] = bb;
		}
	}
	if (abs(best[0]) <= d)
		b->x += best[0];
	if (abs(best[1]) <= d)
		b->y += best[1];
}

static struct client *hover(uint32_t time, struct wlr_surface **surf, double *sx, double *sy)
{
	if (time) {
		struct monitor *m = output_at(srv.cursor->x, srv.cursor->y);
		if (m)
			srv.selmon = m;
		overview_hotarea(srv.cursor->x, srv.cursor->y);
	}
	struct client *c = client_at(srv.cursor->x, srv.cursor->y, surf, sx, sy);
	struct wlr_surface *kf = srv.seat->keyboard_state.focused_surface;
	struct client *kc = client_from_surface(kf);
	if (time && srv.conf.focus_follows_mouse && c && c->type != CLIENT_X11_UNMANAGED && c != client_focused() &&
			!srv.exclusive_focus && !layer_from_surface(kf) && !(kc && kc->type == CLIENT_X11_UNMANAGED) &&
			srv.seat->pointer_state.grab == srv.seat->pointer_state.default_grab)
		focus_client(c, false);
	return c;
}

void input_hover(uint32_t time)
{
	struct wlr_surface *surf;
	double sx, sy;
	hover(time, &surf, &sx, &sy);
}

void input_motion(uint32_t time, bool warp_only)
{
	struct wlr_surface *surf = NULL;
	double sx = 0, sy = 0;
	struct client *c;

	wlr_scene_node_set_position(&srv.layers[LYR_DRAG]->node, (int)srv.cursor->x, (int)srv.cursor->y);

	if (srv.grab_c && (srv.grab_c->fullscreen || !client_visible(srv.grab_c) ||
			(srv.grab_tiled && srv.grab_c->scene->node.parent != srv.layers[LYR_FLOAT])))
		input_cancel_grab();
	if (srv.cursor_mode != CURSOR_NORMAL && srv.grab_c) {
		struct wlr_box b = srv.grab_geo;
		int dx = (int)(srv.cursor->x - srv.grab_x), dy = (int)(srv.cursor->y - srv.grab_y);
		uint32_t e = srv.grab_edges;
		if (srv.cursor_mode == CURSOR_MOVE)
			b.x += dx, b.y += dy;
		if (e & WLR_EDGE_LEFT)
			b.x += dx, b.width -= dx;
		if (e & WLR_EDGE_RIGHT)
			b.width += dx;
		if (e & WLR_EDGE_TOP)
			b.y += dy, b.height -= dy;
		if (e & WLR_EDGE_BOTTOM)
			b.height += dy;
		const int min = 50;
		if (b.width < min)
			b.x -= e & WLR_EDGE_LEFT ? min - b.width : 0, b.width = min;
		if (b.height < min)
			b.y -= e & WLR_EDGE_TOP ? min - b.height : 0, b.height = min;
		if (srv.cursor_mode == CURSOR_MOVE && !srv.grab_tiled)
			snap(&b, srv.grab_c);
		grab_to(srv.grab_c, b);
		if (srv.grab_tiled)
			update_drop_target();
		return;
	}

	struct wlr_seat_pointer_state *ps = &srv.seat->pointer_state;
	if (!ps->focused_surface)
		press_surface = NULL;
	if (ps->button_count && ps->focused_surface && ps->focused_surface == press_surface && !srv.seat->drag &&
			ps->grab == ps->default_grab &&
			(!srv.locked || wlr_session_lock_surface_v1_try_from_wlr_surface(press_surface))) {
		int nx, ny;
		input_surface_origin(press_surface, &nx, &ny);
		if (time)
			wlr_seat_pointer_notify_motion(srv.seat, time, srv.cursor->x - press_dx - (nx - press_nx),
				srv.cursor->y - press_dy - (ny - press_ny));
		return;
	}

	c = hover(time, &surf, &sx, &sy);
	if (!surf && !srv.seat->drag)
		wlr_cursor_set_xcursor(srv.cursor, srv.xcursor_mgr, "default");

	if (!surf) {
		wlr_seat_pointer_notify_clear_focus(srv.seat);
		return;
	}
	wlr_seat_pointer_notify_enter(srv.seat, surf, sx, sy);
	if (time)
		wlr_seat_pointer_notify_motion(srv.seat, time, sx, sy);
}

static bool constrain(double *dx, double *dy)
{
	struct wlr_pointer_constraint_v1 *pc = srv.active_constraint;
	struct wlr_seat_pointer_state *p = &srv.seat->pointer_state;
	double x, y;
	if (!pc)
		return true;
	if (pc->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
		return p->focused_surface != pc->surface;
	if (p->focused_surface != pc->surface || !pixman_region32_contains_point(&pc->region, (int)p->sx, (int)p->sy, NULL))
		return true;
	if (!wlr_region_confine(&pc->region, p->sx, p->sy, p->sx + *dx, p->sy + *dy, &x, &y))
		return false;
	*dx = x - p->sx;
	*dy = y - p->sy;
	return true;
}

static void locked_end_grabs(void)
{
	if (!srv.locked)
		return;
	if (srv.seat->pointer_state.grab != srv.seat->pointer_state.default_grab)
		wlr_seat_pointer_end_grab(srv.seat);
	if (srv.seat->touch_state.grab != srv.seat->touch_state.default_grab)
		wlr_seat_touch_end_grab(srv.seat);
}

static void handle_motion(struct wl_listener *l, void *data)
{
	struct wlr_pointer_motion_event *ev = data;
	if (locked_virtual(ev->pointer))
		return;
	locked_end_grabs();
	wlr_relative_pointer_manager_v1_send_relative_motion(srv.relative_pointer, srv.seat,
		(uint64_t)ev->time_msec * 1000, ev->delta_x, ev->delta_y, ev->unaccel_dx, ev->unaccel_dy);
	double dx = ev->delta_x, dy = ev->delta_y;
	if (!constrain(&dx, &dy))
		return;
	wlr_cursor_move(srv.cursor, &ev->pointer->base, dx, dy);
	input_motion(ev->time_msec, false);
}

static void handle_motion_abs(struct wl_listener *l, void *data)
{
	struct wlr_pointer_motion_absolute_event *ev = data;
	double lx, ly;
	if (locked_virtual(ev->pointer))
		return;
	locked_end_grabs();
	wlr_cursor_absolute_to_layout_coords(srv.cursor, &ev->pointer->base, ev->x, ev->y, &lx, &ly);
	double dx = lx - srv.cursor->x, dy = ly - srv.cursor->y;
	wlr_relative_pointer_manager_v1_send_relative_motion(srv.relative_pointer, srv.seat,
		(uint64_t)ev->time_msec * 1000, dx, dy, dx, dy);
	if (!constrain(&dx, &dy))
		return;
	wlr_cursor_move(srv.cursor, &ev->pointer->base, dx, dy);
	input_motion(ev->time_msec, false);
}

static void end_grab(void)
{
	struct client *c = srv.grab_c, *target = srv.drop_target;
	bool to_tile = srv.grab_tiled;
	input_apply_grab();
	if (to_tile) {
		srv.grab_tiled = false;
		input_cancel_grab();
		if (c) {
			client_restack(c);
			struct monitor *m = output_at(srv.cursor->x, srv.cursor->y);
			if (!target && m && m != c->mon)
				client_send_to_monitor(c, m, m->tags);
			else
				client_drop_on(c, target);
			if (c->mon)
				srv.selmon = c->mon;
		}
		return;
	}
	input_cancel_grab();
	if (!c)
		return;
	c->prev = c->geom;
	struct monitor *m = output_at(c->geom.x + c->geom.width / 2.0, c->geom.y + c->geom.height / 2.0);
	if (m && m != c->mon)
		client_send_to_monitor(c, m, m->tags);
	if (c->mon)
		srv.selmon = c->mon;
}

void input_click_focus(struct client *c, struct wlr_surface *surf)
{
	struct layer_surface *ls = !c && surf ? layer_from_surface(surf) : NULL;
	if (srv.locked) {
		if (surf)
			input_set_focus_surface(surf);
	} else if (ls && ls->ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
			(!srv.exclusive_focus || srv.exclusive_focus == ls)) {
		input_set_focus_surface(ls->ls->surface);
	} else if (c && c->type != CLIENT_X11_UNMANAGED) {
		focus_client(c, true);
	}
}

bool input_overview_pick(struct client *c)
{
#ifdef WITH_OVERVIEW
	if (!c || !c->mon || !c->mon->overview || srv.locked)
		return false;
	struct monitor *m = c->mon;
	overview_set(m, false);
	srv.selmon = m;
	if (!client_visible(c))
		view(c->tags);
	focus_client(c, true);
	return true;
#else
	return false;
#endif
}

static void swallow(uint32_t button)
{
	for (size_t i = 0; i < LENGTH(swallowed); i++)
		if (!swallowed[i]) {
			swallowed[i] = button;
			return;
		}
}

void input_button(uint32_t time, uint32_t button, enum wl_pointer_button_state state)
{
	locked_end_grabs();
	if (srv.cursor_mode != CURSOR_NORMAL) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			swallow(button);
			return;
		}
		if (grab_button && button != grab_button) {
			for (size_t i = 0; i < srv.seat->pointer_state.button_count; i++)
				if (srv.seat->pointer_state.buttons[i].button == button) {
					wlr_seat_pointer_notify_button(srv.seat, time, button, state);
					break;
				}
			return;
		}
		end_grab();
		for (size_t i = 0; i < srv.seat->pointer_state.button_count; i++)
			if (srv.seat->pointer_state.buttons[i].button == button) {
				wlr_seat_pointer_notify_button(srv.seat, time, button, state);
				break;
			}
		wlr_seat_pointer_notify_clear_focus(srv.seat);
		input_motion(0, true);
		return;
	}
	for (size_t i = 0; i < LENGTH(swallowed); i++)
		if (state == WL_POINTER_BUTTON_STATE_RELEASED && swallowed[i] == button) {
			swallowed[i] = 0;
			return;
		}
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		for (size_t i = 0; i < LENGTH(swallowed); i++)
			if (swallowed[i] == button)
				swallowed[i] = 0;
		struct wlr_surface *surf;
		double sx, sy;
		struct client *c = client_at(srv.cursor->x, srv.cursor->y, &surf, &sx, &sy);
		uint32_t mods = pointer_mods();
		bool repeat, held = press_surface && srv.seat->pointer_state.button_count &&
			srv.seat->pointer_state.focused_surface == press_surface;

		if (!held)
			input_click_focus(c, surf);
		if (bindings_allowed() && has_binding(mods, SYM_BUTTON + button)) {
			swallow(button);
			run_binding(mods, SYM_BUTTON + button, &repeat);
			return;
		}
		if (!held && input_overview_pick(c)) {
			swallow(button);
			return;
		}

		if (mods == srv.conf.mouse_mod && (button == BTN_LEFT || button == BTN_RIGHT)) {
			input_start_grab(c, button == BTN_LEFT ? 0 : WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
			if (srv.cursor_mode != CURSOR_NORMAL) {
				grab_button = button;
				return;
			}
		}
	}
	struct wlr_seat_pointer_state *ps = &srv.seat->pointer_state;
	bool first = state == WL_POINTER_BUTTON_STATE_PRESSED && !ps->button_count;
	wlr_seat_pointer_notify_button(srv.seat, time, button, state);
	if (first) {
		press_surface = ps->focused_surface;
		press_dx = srv.cursor->x - ps->sx;
		press_dy = srv.cursor->y - ps->sy;
		input_surface_origin(press_surface, &press_nx, &press_ny);
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED && !ps->button_count) {
		press_surface = NULL;
		input_motion(0, true);
	}
}

static void handle_button(struct wl_listener *l, void *data)
{
	struct wlr_pointer_button_event *ev = data;
	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && locked_virtual(ev->pointer))
		return;
	input_button(ev->time_msec, ev->button, ev->state);
}

static void handle_axis(struct wl_listener *l, void *data)
{
	struct wlr_pointer_axis_event *ev = data;
	bool repeat;
	if (locked_virtual(ev->pointer))
		return;
	locked_end_grabs();
	uint32_t mods = pointer_mods();
	if (ev->delta != 0) {
		bool vert = ev->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL;
		xkb_keysym_t sym = SYM_WHEEL + (vert ? (ev->delta < 0 ? 0 : 1) : (ev->delta < 0 ? 2 : 3));
		if (bindings_allowed() && has_binding(mods, sym)) {
			if ((uint32_t)(ev->time_msec - srv.last_axis_ms) >= (uint32_t)srv.conf.axis_timeout) {
				srv.last_axis_ms = ev->time_msec;
				run_binding(mods, sym, &repeat);
			}
			return;
		}
	}
	wlr_seat_pointer_notify_axis(srv.seat, ev->time_msec, ev->orientation, ev->delta,
		ev->delta_discrete, ev->source, ev->relative_direction);
}

static void handle_frame(struct wl_listener *l, void *data)
{
	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
	wlr_seat_pointer_notify_frame(srv.seat);
}

static int finger(int32_t id)
{
	for (size_t i = 0; i < LENGTH(fingers); i++)
		if (fingers[i].used && fingers[i].id == id)
			return (int)i;
	return -1;
}

static void handle_touch_down(struct wl_listener *l, void *data)
{
	struct wlr_touch_down_event *ev = data;
	struct wlr_surface *surf;
	double lx, ly, sx, sy;
	locked_end_grabs();
	wlr_cursor_absolute_to_layout_coords(srv.cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly);
	struct client *c = client_at(lx, ly, &surf, &sx, &sy);
	int ox, oy, nx, ny;
	input_surface_origin(surf, &ox, &oy);
	input_click_focus(c, surf);
	if (input_overview_pick(c) || !surf)
		return;
	input_surface_origin(surf, &nx, &ny);
	int old = finger(ev->touch_id);
	if (old >= 0)
		fingers[old].used = false;
	for (size_t i = 0; i < LENGTH(fingers); i++)
		if (!fingers[i].used) {
			fingers[i].used = true;
			fingers[i].id = ev->touch_id;
			fingers[i].dx = lx - sx;
			fingers[i].dy = ly - sy;
			fingers[i].ox = ox;
			fingers[i].oy = oy;
			wlr_seat_touch_notify_down(srv.seat, surf, ev->time_msec, ev->touch_id, sx - (nx - ox), sy - (ny - oy));
			return;
		}
}

static void handle_touch_motion(struct wl_listener *l, void *data)
{
	struct wlr_touch_motion_event *ev = data;
	int i = finger(ev->touch_id);
	double lx, ly;
	if (i < 0)
		return;
	struct wlr_touch_point *p = wlr_seat_touch_get_point(srv.seat, ev->touch_id);
	if (srv.locked && p && p->surface && !wlr_session_lock_surface_v1_try_from_wlr_surface(p->surface))
		return;
	wlr_cursor_absolute_to_layout_coords(srv.cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly);
	if (srv.seat->drag) {
		struct wlr_surface *surf;
		double sx, sy;
		wlr_scene_node_set_position(&srv.layers[LYR_DRAG]->node, (int)lx, (int)ly);
		client_at(lx, ly, &surf, &sx, &sy);
		if (!surf) {
			wlr_seat_touch_point_clear_focus(srv.seat, ev->time_msec, ev->touch_id);
			return;
		}
		wlr_seat_touch_point_focus(srv.seat, surf, ev->time_msec, ev->touch_id, sx, sy);
		wlr_seat_touch_notify_motion(srv.seat, ev->time_msec, ev->touch_id, sx, sy);
		return;
	}
	int nx = fingers[i].ox, ny = fingers[i].oy;
	if (p && p->surface)
		input_surface_origin(p->surface, &nx, &ny);
	wlr_seat_touch_notify_motion(srv.seat, ev->time_msec, ev->touch_id, lx - fingers[i].dx - (nx - fingers[i].ox),
		ly - fingers[i].dy - (ny - fingers[i].oy));
}

static void handle_touch_up(struct wl_listener *l, void *data)
{
	struct wlr_touch_up_event *ev = data;
	int i = finger(ev->touch_id);
	if (i < 0)
		return;
	fingers[i].used = false;
	wlr_seat_touch_notify_up(srv.seat, ev->time_msec, ev->touch_id);
}

static void handle_touch_cancel(struct wl_listener *l, void *data)
{
	struct wlr_touch_cancel_event *ev = data;
	struct wlr_touch_point *p = wlr_seat_touch_get_point(srv.seat, ev->touch_id);
	int i = finger(ev->touch_id);
	if (i >= 0)
		fingers[i].used = false;
	if (p)
		wlr_seat_touch_notify_cancel(srv.seat, p->client);
}

static void handle_touch_frame(struct wl_listener *l, void *data)
{
	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
	wlr_seat_touch_notify_frame(srv.seat);
}

static bool swipe_ours;

static bool swipe_is_ours(void)
{
	if (!bindings_allowed())
		return false;
	if (!srv.locked && srv.conf.gesture_fingers > 0 && srv.swipe_fingers == (uint32_t)srv.conf.gesture_fingers)
		return true;
	for (size_t i = 0; i < srv.conf.nbinds; i++)
		if (srv.conf.binds[i].sym >= SYM_SWIPE_OF(srv.swipe_fingers, 0) &&
				srv.conf.binds[i].sym <= SYM_SWIPE_OF(srv.swipe_fingers, 3) && (!srv.locked || srv.conf.binds[i].locked))
			return true;
	return false;
}

static void handle_swipe_begin(struct wl_listener *l, void *data)
{
	struct wlr_pointer_swipe_begin_event *ev = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
	srv.swipe_fingers = ev->fingers;
	srv.swipe_dx = srv.swipe_dy = 0;
	swipe_ours = swipe_is_ours();
	if (!swipe_ours)
		wlr_pointer_gestures_v1_send_swipe_begin(srv.gestures, srv.seat, ev->time_msec, ev->fingers);
}

static void handle_swipe_update(struct wl_listener *l, void *data)
{
	struct wlr_pointer_swipe_update_event *ev = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
	if (!swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_update(srv.gestures, srv.seat, ev->time_msec, ev->dx, ev->dy);
		return;
	}
	srv.swipe_dx += ev->dx;
	srv.swipe_dy += ev->dy;
}

static void handle_swipe_end(struct wl_listener *l, void *data)
{
	struct wlr_pointer_swipe_end_event *ev = data;
	if (!swipe_ours) {
		wlr_pointer_gestures_v1_send_swipe_end(srv.gestures, srv.seat, ev->time_msec, ev->cancelled);
		return;
	}
	uint32_t fingers = srv.swipe_fingers;
	srv.swipe_fingers = 0;
	swipe_ours = false;
	double dx = srv.swipe_dx, dy = srv.swipe_dy, t = srv.conf.swipe_threshold;
	if (ev->cancelled || !srv.selmon || (fabs(dx) < t && fabs(dy) < t))
		return;
	if (!bindings_allowed())
		return;
	bool horiz = fabs(dx) >= fabs(dy), repeat;
	int dir = horiz ? (dx < 0 ? 0 : 1) : (dy < 0 ? 2 : 3);
	uint32_t mods = pointer_mods();
	if (has_binding(mods, SYM_SWIPE_OF(fingers, dir))) {
		run_binding(mods, SYM_SWIPE_OF(fingers, dir), &repeat);
		return;
	}
	char err[64];
	if (!srv.locked && horiz && fingers == (uint32_t)srv.conf.gesture_fingers)
		action_run(dx < 0 ? "viewnext" : "viewprev", err, sizeof(err));
}

static void handle_pinch_begin(struct wl_listener *l, void *data)
{
	struct wlr_pointer_pinch_begin_event *ev = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
	wlr_pointer_gestures_v1_send_pinch_begin(srv.gestures, srv.seat, ev->time_msec, ev->fingers);
}

static void handle_pinch_update(struct wl_listener *l, void *data)
{
	struct wlr_pointer_pinch_update_event *ev = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
	wlr_pointer_gestures_v1_send_pinch_update(srv.gestures, srv.seat, ev->time_msec, ev->dx, ev->dy, ev->scale, ev->rotation);
}

static void handle_pinch_end(struct wl_listener *l, void *data)
{
	struct wlr_pointer_pinch_end_event *ev = data;
	wlr_pointer_gestures_v1_send_pinch_end(srv.gestures, srv.seat, ev->time_msec, ev->cancelled);
}

static void handle_hold_begin(struct wl_listener *l, void *data)
{
	struct wlr_pointer_hold_begin_event *ev = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, srv.seat);
	wlr_pointer_gestures_v1_send_hold_begin(srv.gestures, srv.seat, ev->time_msec, ev->fingers);
}

static void handle_hold_end(struct wl_listener *l, void *data)
{
	struct wlr_pointer_hold_end_event *ev = data;
	wlr_pointer_gestures_v1_send_hold_end(srv.gestures, srv.seat, ev->time_msec, ev->cancelled);
}

static void handle_request_cursor(struct wl_listener *l, void *data)
{
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	if (srv.cursor_mode == CURSOR_NORMAL && ev->seat_client == srv.seat->pointer_state.focused_client)
		wlr_cursor_set_surface(srv.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void handle_request_shape(struct wl_listener *l, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	if (srv.cursor_mode == CURSOR_NORMAL && ev->seat_client == srv.seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(srv.cursor, srv.xcursor_mgr, wlr_cursor_shape_v1_name(ev->shape));
}

static void handle_request_selection(struct wl_listener *l, void *data)
{
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(srv.seat, ev->source, ev->serial);
}

static void handle_request_primary(struct wl_listener *l, void *data)
{
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(srv.seat, ev->source, ev->serial);
}

static void handle_request_drag(struct wl_listener *l, void *data)
{
	struct wlr_seat_request_start_drag_event *ev = data;
	struct wlr_touch_point *point;
	if (wlr_seat_validate_pointer_grab_serial(srv.seat, ev->origin, ev->serial))
		wlr_seat_start_pointer_drag(srv.seat, ev->drag, ev->serial);
	else if (wlr_seat_validate_touch_grab_serial(srv.seat, ev->origin, ev->serial, &point))
		wlr_seat_start_touch_drag(srv.seat, ev->drag, ev->serial, point);
	else
		wlr_data_source_destroy(ev->drag->source);
}

static void handle_drag_destroy(struct wl_listener *l, void *data)
{
	struct client *h;
	wl_list_remove(&drag_destroy.link);
	wl_list_init(&drag_destroy.link);
	if (srv.locked)
		lock_focus(NULL);
	else if (srv.exclusive_focus)
		input_set_focus_surface(srv.exclusive_focus->ls->surface);
	else if ((h = client_last_focused()) && h->active && client_reachable(h))
		focus_client(h, false);
	else
		focus_top(srv.selmon);
	input_motion(0, true);
}

static void handle_start_drag(struct wl_listener *l, void *data)
{
	struct wlr_drag *drag = data;
	if (drag->icon)
		wlr_scene_drag_icon_create(srv.layers[LYR_DRAG], drag->icon);
	wl_list_remove(&drag_destroy.link);
	LISTEN(&drag->events.destroy, &drag_destroy, handle_drag_destroy);
	input_motion(0, true);
}

void input_finish(void)
{
	struct wl_listener *listeners[] = {
		&kb_key, &kb_modifiers, &new_input, &cursor_motion, &cursor_motion_abs, &cursor_button,
		&cursor_axis, &cursor_frame, &swipe_begin, &swipe_update, &swipe_end, &pinch_begin,
		&pinch_update, &pinch_end, &hold_begin, &hold_end, &request_cursor, &request_shape,
		&request_selection, &request_primary, &request_drag, &start_drag, &drag_destroy, &new_constraint,
		&new_inhibitor, &new_vkeyboard, &new_vpointer, &new_kb_inhibitor, &touch_down, &touch_up,
		&touch_motion, &touch_cancel, &touch_frame
	};
	for (size_t i = 0; i < LENGTH(listeners); i++)
		wl_list_remove(&listeners[i]->link);
	wl_event_source_remove(repeat_timer);
	tablet_finish();
	wlr_keyboard_group_destroy(srv.kb_group);
	srv.kb_group = NULL;
}

void input_init(void)
{
	wl_list_init(&keyboards);
	wl_list_init(&vpointers);
	wl_list_init(&touches);
	wl_list_init(&srv.pointers);
	wl_list_init(&drag_destroy.link);

	srv.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(srv.cursor, srv.output_layout);
	srv.seat = wlr_seat_create(srv.display, "seat0");
	srv.kb_group = wlr_keyboard_group_create();
	wlr_seat_set_keyboard(srv.seat, &srv.kb_group->keyboard);

	srv.relative_pointer = wlr_relative_pointer_manager_v1_create(srv.display);
	srv.gestures = wlr_pointer_gestures_v1_create(srv.display);
	srv.constraints = wlr_pointer_constraints_v1_create(srv.display);
	struct wlr_cursor_shape_manager_v1 *shape = wlr_cursor_shape_manager_v1_create(srv.display, 1);
	update_caps();
	repeat_timer = wl_event_loop_add_timer(srv.loop, handle_repeat, NULL);
	kb_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(srv.display);
	idle_notifier = wlr_idle_notifier_v1_create(srv.display);
	idle_inhibit = wlr_idle_inhibit_v1_create(srv.display);
	struct wlr_virtual_keyboard_manager_v1 *vkeyboard = wlr_virtual_keyboard_manager_v1_create(srv.display);
	struct wlr_virtual_pointer_manager_v1 *vpointer = wlr_virtual_pointer_manager_v1_create(srv.display);
	privileged_add(vkeyboard->global);
	privileged_add(vpointer->global);
	privileged_add(kb_inhibit->global);

	input_apply_config();

	LISTEN(&srv.kb_group->keyboard.events.key, &kb_key, handle_key);
	LISTEN(&srv.kb_group->keyboard.events.modifiers, &kb_modifiers, handle_modifiers);
	LISTEN(&srv.backend->events.new_input, &new_input, handle_new_input);
	LISTEN(&srv.cursor->events.motion, &cursor_motion, handle_motion);
	LISTEN(&srv.cursor->events.motion_absolute, &cursor_motion_abs, handle_motion_abs);
	LISTEN(&srv.cursor->events.button, &cursor_button, handle_button);
	LISTEN(&srv.cursor->events.axis, &cursor_axis, handle_axis);
	LISTEN(&srv.cursor->events.frame, &cursor_frame, handle_frame);
	LISTEN(&srv.cursor->events.swipe_begin, &swipe_begin, handle_swipe_begin);
	LISTEN(&srv.cursor->events.swipe_update, &swipe_update, handle_swipe_update);
	LISTEN(&srv.cursor->events.swipe_end, &swipe_end, handle_swipe_end);
	LISTEN(&srv.cursor->events.pinch_begin, &pinch_begin, handle_pinch_begin);
	LISTEN(&srv.cursor->events.pinch_update, &pinch_update, handle_pinch_update);
	LISTEN(&srv.cursor->events.pinch_end, &pinch_end, handle_pinch_end);
	LISTEN(&srv.cursor->events.hold_begin, &hold_begin, handle_hold_begin);
	LISTEN(&srv.cursor->events.hold_end, &hold_end, handle_hold_end);
	LISTEN(&srv.seat->events.request_set_cursor, &request_cursor, handle_request_cursor);
	LISTEN(&shape->events.request_set_shape, &request_shape, handle_request_shape);
	LISTEN(&srv.seat->events.request_set_selection, &request_selection, handle_request_selection);
	LISTEN(&srv.seat->events.request_set_primary_selection, &request_primary, handle_request_primary);
	LISTEN(&srv.seat->events.request_start_drag, &request_drag, handle_request_drag);
	LISTEN(&srv.seat->events.start_drag, &start_drag, handle_start_drag);
	LISTEN(&srv.constraints->events.new_constraint, &new_constraint, handle_new_constraint);
	LISTEN(&idle_inhibit->events.new_inhibitor, &new_inhibitor, handle_new_inhibitor);
	LISTEN(&vkeyboard->events.new_virtual_keyboard, &new_vkeyboard, handle_new_vkeyboard);
	LISTEN(&vpointer->events.new_virtual_pointer, &new_vpointer, handle_new_vpointer);
	LISTEN(&kb_inhibit->events.new_inhibitor, &new_kb_inhibitor, handle_new_kb_inhibitor);
	LISTEN(&srv.cursor->events.touch_down, &touch_down, handle_touch_down);
	LISTEN(&srv.cursor->events.touch_up, &touch_up, handle_touch_up);
	LISTEN(&srv.cursor->events.touch_motion, &touch_motion, handle_touch_motion);
	LISTEN(&srv.cursor->events.touch_cancel, &touch_cancel, handle_touch_cancel);
	LISTEN(&srv.cursor->events.touch_frame, &touch_frame, handle_touch_frame);
	tablet_init();
}
