#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include "swizzle.h"

static const enum scene_layer layermap[] = {LYR_BG, LYR_BOTTOM, LYR_TOP, LYR_OVERLAY};
static struct wl_listener new_layer_surface;

struct layer_surface *layer_from_surface(struct wlr_surface *surface)
{
	while (surface) {
		struct wlr_layer_surface_v1 *l = wlr_layer_surface_v1_try_from_wlr_surface(surface);
		if (l)
			return l->data;
		struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
		if (!xdg || xdg->role != WLR_XDG_SURFACE_ROLE_POPUP || !xdg->popup)
			return NULL;
		surface = xdg->popup->parent;
	}
	return NULL;
}

static void update_exclusive_focus(void)
{
	struct monitor *m;
	struct layer_surface *ls;
	static const int order[] = {ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP};

	wl_list_for_each(m, &srv.monitors, link)
		for (size_t i = 0; i < LENGTH(order) && (m->output->enabled || m->asleep); i++)
			wl_list_for_each(ls, &m->layers[order[i]], link) {
				if (!ls->mapped || ls->ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
					continue;
				if (srv.exclusive_focus != ls) {
					srv.exclusive_focus = ls;
					input_set_focus_surface(ls->ls->surface);
				}
				return;
			}
	if (srv.exclusive_focus) {
		srv.exclusive_focus = NULL;
		focus_top(srv.selmon);
	}
}

void layer_arrange(struct monitor *m)
{
	struct wlr_box usable = m->m;
	struct layer_surface *ls;

	if (!m->output->enabled && !m->asleep)
		return;
	for (int pass = 0; pass < 2; pass++)
		for (int i = LENGTH(m->layers) - 1; i >= 0; i--)
			wl_list_for_each(ls, &m->layers[i], link) {
				if (!ls->ls->initialized || ls->unmapping || (ls->ls->current.exclusive_zone > 0) != (pass == 0))
					continue;
				wlr_scene_layer_surface_v1_configure(ls->scene_ls, &m->m, &usable);
				wlr_scene_node_set_position(&ls->popups->node, ls->scene->node.x, ls->scene->node.y);
			}
	m->w = usable;
	arrange(m);
	update_exclusive_focus();
}

static void handle_layer_commit(struct wl_listener *l, void *data)
{
	struct layer_surface *ls = wl_container_of(l, ls, commit);
	struct wlr_layer_surface_v1 *s = ls->ls;

	ls->unmapping = false;
	if (!ls->mon)
		return;
	if (s->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		wl_list_remove(&ls->link);
		wl_list_insert(&ls->mon->layers[s->current.layer], &ls->link);
		wlr_scene_node_reparent(&ls->scene->node, srv.layers[layermap[s->current.layer]]);
		wlr_scene_node_set_enabled(&ls->scene->node, s->surface->mapped);
		wlr_scene_node_set_enabled(&ls->popups->node, true);
	}
	if (s->initial_commit) {
		struct wlr_layer_surface_v1_state old = s->current;
		s->current = s->pending;
		layer_arrange(ls->mon);
		s->current = old;
		return;
	}
	if (s->current.committed || ls->mapped != s->surface->mapped)
		layer_arrange(ls->mon);
	effects_layer_commit(ls);
	if (s->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
		effects_background_changed(ls->mon);
	if (s->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
			srv.seat->keyboard_state.focused_surface == s->surface) {
		input_set_focus_surface(NULL);
		focus_top(srv.selmon);
	}
}

static void handle_layer_map(struct wl_listener *l, void *data)
{
	struct layer_surface *ls = wl_container_of(l, ls, map);
	ls->mapped = true;
	wlr_log(WLR_DEBUG, "layer surface mapped: %s", ls->ls->namespace ? ls->ls->namespace : "");
	if (ls->mon)
		layer_arrange(ls->mon);
	effects_layer_map(ls);
	if (!srv.exclusive_focus && ls->ls->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
			ls->ls->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)
		input_set_focus_surface(ls->ls->surface);
	input_motion(0, true);
}

static void handle_layer_unmap(struct wl_listener *l, void *data)
{
	struct layer_surface *ls = wl_container_of(l, ls, unmap);
	effects_layer_unmap(ls);
	ls->mapped = false;
	ls->unmapping = true;
	if (srv.exclusive_focus == ls)
		srv.exclusive_focus = NULL;
	if (srv.seat->keyboard_state.focused_surface == ls->ls->surface) {
		input_set_focus_surface(NULL);
		focus_top(srv.selmon);
	}
	if (ls->mon)
		layer_arrange(ls->mon);
	input_motion(0, true);
}

static void handle_layer_destroy(struct wl_listener *l, void *data)
{
	struct layer_surface *ls = wl_container_of(l, ls, destroy);
	struct monitor *m = ls->mon;
	if (srv.exclusive_focus == ls)
		srv.exclusive_focus = NULL;
	effects_layer_destroy(ls);
	wl_list_remove(&ls->link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->destroy.link);
	wlr_scene_node_destroy(&ls->popups->node);
	free(ls);
	if (m && !m->destroying)
		layer_arrange(m);
}

static void handle_new_layer_surface(struct wl_listener *l, void *data)
{
	struct wlr_layer_surface_v1 *s = data;
	struct monitor *m = s->output ? s->output->data : NULL;

	if (!m || (!m->output->enabled && !m->asleep))
		m = srv.selmon;
	if (!m) {
		wlr_layer_surface_v1_destroy(s);
		return;
	}
	s->output = m->output;
	struct layer_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) {
		wlr_layer_surface_v1_destroy(s);
		return;
	}
	ls->type = CLIENT_LAYER;
	ls->ls = s;
	ls->mon = m;
	s->data = ls;
	ls->scene_ls = wlr_scene_layer_surface_v1_create(srv.layers[layermap[s->pending.layer]], s);
	ls->scene = ls->scene_ls->tree;
	ls->scene->node.data = ls;
	s->surface->data = ls->scene;
	ls->popups = wlr_scene_tree_create(srv.layers[LYR_OVERLAY]);
	ls->popups->node.data = ls;
	effects_layer_create(ls);

	LISTEN(&s->surface->events.commit, &ls->commit, handle_layer_commit);
	LISTEN(&s->surface->events.map, &ls->map, handle_layer_map);
	LISTEN(&s->surface->events.unmap, &ls->unmap, handle_layer_unmap);
	LISTEN(&s->events.destroy, &ls->destroy, handle_layer_destroy);
	wl_list_insert(&ls->mon->layers[s->pending.layer], &ls->link);
}

struct lock_surface {
	struct wlr_surface *surface;
	struct wl_listener destroy;
};

static struct wlr_session_lock_v1 *cur;
static struct wlr_scene_rect *backdrop;
static struct wl_listener new_lock, new_surface, unlock, lock_destroy;

static void place(struct wlr_session_lock_surface_v1 *ls)
{
	struct wlr_box box;
	struct wlr_scene_tree *tree = ls->surface->data;
	wlr_output_layout_get_box(srv.output_layout, ls->output, &box);
	if (!tree || wlr_box_empty(&box))
		return;
	wlr_scene_node_set_position(&tree->node, box.x, box.y);
	wlr_session_lock_surface_v1_configure(ls, box.width, box.height);
}

static void lock_check(void)
{
	struct monitor *m;
	if (!srv.lock_pending)
		return;
	wl_list_for_each(m, &srv.monitors, link)
		if (m->output->enabled && m->lock_frames < 2)
			return;
	srv.lock_pending = false;
	if (cur)
		wlr_session_lock_v1_send_locked(cur);
}

void lock_arrange(void)
{
	struct wlr_box box;
	struct wlr_session_lock_surface_v1 *ls;

	wlr_output_layout_get_box(srv.output_layout, NULL, &box);
	wlr_scene_node_set_position(&backdrop->node, box.x, box.y);
	wlr_scene_rect_set_size(backdrop, box.width, box.height);
	if (cur)
		wl_list_for_each(ls, &cur->surfaces, link)
			place(ls);
	lock_check();
}

void lock_focus(struct wlr_surface *skip)
{
	struct wlr_session_lock_surface_v1 *ls, *pick = NULL;
	if (cur)
		wl_list_for_each(ls, &cur->surfaces, link)
			if (ls->surface != skip && (!pick || (srv.selmon && ls->output == srv.selmon->output)))
				pick = ls;
	input_set_focus_surface(pick ? pick->surface : NULL);
}

static void handle_surface_destroy(struct wl_listener *l, void *data)
{
	struct lock_surface *s = wl_container_of(l, s, destroy);
	struct wlr_surface *focused = srv.seat->keyboard_state.focused_surface;
	if (srv.locked && (!focused || focused == s->surface))
		lock_focus(s->surface);
	wl_list_remove(&s->destroy.link);
	free(s);
}

static void handle_new_surface(struct wl_listener *l, void *data)
{
	struct wlr_session_lock_surface_v1 *ls = data;
	struct lock_surface *s = calloc(1, sizeof(*s));
	if (!s)
		return;
	s->surface = ls->surface;
	LISTEN(&ls->events.destroy, &s->destroy, handle_surface_destroy);
	ls->surface->data = wlr_scene_subsurface_tree_create(srv.layers[LYR_LOCK], ls->surface);
	place(ls);
	if (!srv.seat->keyboard_state.focused_surface || (srv.selmon && ls->output == srv.selmon->output))
		input_set_focus_surface(ls->surface);
}

void lock_frame(struct monitor *m)
{
	if (++m->lock_frames < 2)
		wlr_output_schedule_frame(m->output);
	else
		lock_check();
}

static void handle_unlock(struct wl_listener *l, void *data)
{
	srv.locked = false;
	wlr_scene_node_set_enabled(&srv.layers[LYR_LOCK]->node, false);
	if (srv.exclusive_focus)
		input_set_focus_surface(srv.exclusive_focus->ls->surface);
	else
		focus_top(srv.selmon);
	input_motion(0, true);
	state_changed();
}

static void handle_lock_destroy(struct wl_listener *l, void *data)
{
	wl_list_remove(&new_surface.link);
	wl_list_remove(&unlock.link);
	wl_list_remove(&lock_destroy.link);
	cur = NULL;
	srv.lock_pending = false;
}

static void handle_new_lock(struct wl_listener *l, void *data)
{
	struct wlr_session_lock_v1 *lock = data;
	struct monitor *m;
	if (cur) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	cur = lock;
	srv.locked = true;
	input_cancel_grab();
	LISTEN(&lock->events.new_surface, &new_surface, handle_new_surface);
	LISTEN(&lock->events.unlock, &unlock, handle_unlock);
	LISTEN(&lock->events.destroy, &lock_destroy, handle_lock_destroy);
	wlr_scene_node_set_enabled(&srv.layers[LYR_LOCK]->node, true);
	lock_arrange();
	wlr_seat_keyboard_end_grab(srv.seat);
	wlr_seat_pointer_end_grab(srv.seat);
	wlr_seat_touch_end_grab(srv.seat);
	input_set_focus_surface(NULL);
	input_motion(0, true);
	wl_list_for_each(m, &srv.monitors, link) {
		m->lock_frames = 0;
		if (m->output->enabled) {
			srv.lock_pending = true;
			wlr_output_schedule_frame(m->output);
		}
	}
	if (!srv.lock_pending)
		wlr_session_lock_v1_send_locked(lock);
	state_changed();
}

struct text_input {
	struct wlr_text_input_v3 *ti;
	struct wl_list link;
	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener disable;
	struct wl_listener destroy;
};

struct ime_popup {
	struct wlr_input_popup_surface_v2 *popup;
	struct wlr_scene_tree *tree;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static struct wlr_input_method_v2 *im;
static struct wl_list text_inputs;
static struct wl_listener new_text_input, new_input_method;
static struct wl_listener im_commit, im_grab, im_new_popup, im_destroy, grab_destroy;

static struct wlr_text_input_v3 *focused_ti(void)
{
	struct text_input *t;
	wl_list_for_each(t, &text_inputs, link)
		if (t->ti->focused_surface && t->ti->current_enabled)
			return t->ti;
	return NULL;
}

static void place_popup(struct ime_popup *p)
{
	struct wlr_text_input_v3 *ti = focused_ti();
	struct wlr_scene_tree *parent = ti ? ti->focused_surface->data : NULL;
	int x, y;
	wlr_scene_node_set_enabled(&p->tree->node, parent != NULL);
	if (!parent)
		return;
	struct wlr_box r = ti->current.cursor_rectangle;
	wlr_scene_node_coords(&parent->node, &x, &y);
	struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(ti->focused_surface);
	if (xdg)
		x -= xdg->geometry.x, y -= xdg->geometry.y;
	wlr_scene_node_set_position(&p->tree->node, x + r.x, y + r.y + r.height);
	wlr_input_popup_surface_v2_send_text_input_rectangle(p->popup, &(struct wlr_box){0, -r.height, r.width, r.height});
}

static void place_popups(void)
{
	struct wlr_input_popup_surface_v2 *popup;
	if (im)
		wl_list_for_each(popup, &im->popup_surfaces, link)
			if (popup->data)
				place_popup(popup->data);
}

static void send_state(struct wlr_text_input_v3 *ti)
{
	if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
		wlr_input_method_v2_send_surrounding_text(im, ti->current.surrounding.text,
			ti->current.surrounding.cursor, ti->current.surrounding.anchor);
	wlr_input_method_v2_send_text_change_cause(im, ti->current.text_change_cause);
	if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
		wlr_input_method_v2_send_content_type(im, ti->current.content_type.hint, ti->current.content_type.purpose);
	wlr_input_method_v2_send_done(im);
	place_popups();
}

static void deactivate(void)
{
	if (!im)
		return;
	wlr_input_method_v2_send_deactivate(im);
	wlr_input_method_v2_send_done(im);
	place_popups();
}

static void handle_ti_enable(struct wl_listener *l, void *data)
{
	struct text_input *t = wl_container_of(l, t, enable);
	if (!im || !t->ti->focused_surface)
		return;
	wlr_input_method_v2_send_activate(im);
	send_state(t->ti);
}

static void handle_ti_commit(struct wl_listener *l, void *data)
{
	struct text_input *t = wl_container_of(l, t, commit);
	if (im && t->ti->focused_surface && t->ti->current_enabled)
		send_state(t->ti);
}

static void handle_ti_disable(struct wl_listener *l, void *data)
{
	struct text_input *t = wl_container_of(l, t, disable);
	if (t->ti->focused_surface)
		deactivate();
}

static void handle_ti_destroy(struct wl_listener *l, void *data)
{
	struct text_input *t = wl_container_of(l, t, destroy);
	if (t->ti->focused_surface && t->ti->current_enabled)
		deactivate();
	wl_list_remove(&t->link);
	wl_list_remove(&t->enable.link);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->disable.link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

static void handle_new_text_input(struct wl_listener *l, void *data)
{
	struct wlr_text_input_v3 *ti = data;
	struct wlr_surface *focused = srv.seat->keyboard_state.focused_surface;
	struct text_input *t = calloc(1, sizeof(*t));
	if (!t)
		return;
	t->ti = ti;
	wl_list_insert(&text_inputs, &t->link);
	LISTEN(&ti->events.enable, &t->enable, handle_ti_enable);
	LISTEN(&ti->events.commit, &t->commit, handle_ti_commit);
	LISTEN(&ti->events.disable, &t->disable, handle_ti_disable);
	LISTEN(&ti->events.destroy, &t->destroy, handle_ti_destroy);
	if (focused && !srv.locked && wl_resource_get_client(ti->resource) == wl_resource_get_client(focused->resource))
		wlr_text_input_v3_send_enter(ti, focused);
}

void ime_focus(struct wlr_surface *surface)
{
	struct text_input *t;
	wl_list_for_each(t, &text_inputs, link) {
		struct wlr_text_input_v3 *ti = t->ti;
		if (ti->focused_surface == surface)
			continue;
		if (ti->focused_surface) {
			if (ti->current_enabled)
				deactivate();
			wlr_text_input_v3_send_leave(ti);
		}
		if (surface && !srv.locked && wl_resource_get_client(ti->resource) == wl_resource_get_client(surface->resource))
			wlr_text_input_v3_send_enter(ti, surface);
	}
}

static bool from_im(struct wlr_keyboard *kb)
{
	struct wlr_virtual_keyboard_v1 *vk = wlr_input_device_get_virtual_keyboard(&kb->base);
	return vk && wl_resource_get_client(vk->resource) == wl_resource_get_client(im->keyboard_grab->resource);
}

static struct wlr_input_method_keyboard_grab_v2 *ime_grab(struct wlr_keyboard *kb)
{
	if (srv.locked || !im || !im->keyboard_grab || from_im(kb))
		return NULL;
	wlr_input_method_keyboard_grab_v2_set_keyboard(im->keyboard_grab, kb);
	return im->keyboard_grab;
}

bool ime_key(struct wlr_keyboard *kb, struct wlr_keyboard_key_event *ev)
{
	struct wlr_input_method_keyboard_grab_v2 *g = ime_grab(kb);
	if (g)
		wlr_input_method_keyboard_grab_v2_send_key(g, ev->time_msec, ev->keycode, ev->state);
	return g != NULL;
}

bool ime_modifiers(struct wlr_keyboard *kb)
{
	struct wlr_input_method_keyboard_grab_v2 *g = ime_grab(kb);
	if (g)
		wlr_input_method_keyboard_grab_v2_send_modifiers(g, &kb->modifiers);
	return g != NULL;
}

static void handle_im_commit(struct wl_listener *l, void *data)
{
	struct wlr_text_input_v3 *ti = focused_ti();
	if (!ti || srv.locked)
		return;
	if (im->current.preedit.text)
		wlr_text_input_v3_send_preedit_string(ti, im->current.preedit.text,
			im->current.preedit.cursor_begin, im->current.preedit.cursor_end);
	if (im->current.commit_text)
		wlr_text_input_v3_send_commit_string(ti, im->current.commit_text);
	if (im->current.delete.before_length || im->current.delete.after_length)
		wlr_text_input_v3_send_delete_surrounding_text(ti, im->current.delete.before_length,
			im->current.delete.after_length);
	wlr_text_input_v3_send_done(ti);
}

static void handle_grab_destroy(struct wl_listener *l, void *data)
{
	wl_list_remove(&grab_destroy.link);
}

static void handle_im_grab(struct wl_listener *l, void *data)
{
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	wlr_input_method_keyboard_grab_v2_set_keyboard(grab, wlr_seat_get_keyboard(srv.seat));
	LISTEN(&grab->events.destroy, &grab_destroy, handle_grab_destroy);
}

static void handle_ime_popup_commit(struct wl_listener *l, void *data)
{
	struct ime_popup *p = wl_container_of(l, p, commit);
	place_popup(p);
}

static void handle_ime_popup_destroy(struct wl_listener *l, void *data)
{
	struct ime_popup *p = wl_container_of(l, p, destroy);
	wlr_scene_node_destroy(&p->tree->node);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	p->popup->data = NULL;
	free(p);
}

static void handle_im_new_popup(struct wl_listener *l, void *data)
{
	struct wlr_input_popup_surface_v2 *popup = data;
	struct ime_popup *p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->popup = popup;
	p->tree = wlr_scene_tree_create(srv.layers[LYR_OVERLAY]);
	wlr_scene_subsurface_tree_create(p->tree, popup->surface);
	popup->data = p;
	LISTEN(&popup->surface->events.commit, &p->commit, handle_ime_popup_commit);
	LISTEN(&popup->events.destroy, &p->destroy, handle_ime_popup_destroy);
	place_popup(p);
}

static void handle_im_destroy(struct wl_listener *l, void *data)
{
	wl_list_remove(&im_commit.link);
	wl_list_remove(&im_grab.link);
	wl_list_remove(&im_new_popup.link);
	wl_list_remove(&im_destroy.link);
	im = NULL;
}

static void handle_new_input_method(struct wl_listener *l, void *data)
{
	struct wlr_input_method_v2 *method = data;
	if (im) {
		wlr_input_method_v2_send_unavailable(method);
		return;
	}
	im = method;
	LISTEN(&im->events.commit, &im_commit, handle_im_commit);
	LISTEN(&im->events.grab_keyboard, &im_grab, handle_im_grab);
	LISTEN(&im->events.new_popup_surface, &im_new_popup, handle_im_new_popup);
	LISTEN(&im->events.destroy, &im_destroy, handle_im_destroy);
	struct wlr_text_input_v3 *ti = focused_ti();
	if (ti) {
		wlr_input_method_v2_send_activate(im);
		send_state(ti);
	}
}

void shell_finish(void)
{
	wl_list_remove(&new_layer_surface.link);
	wl_list_remove(&new_lock.link);
	wl_list_remove(&new_text_input.link);
	wl_list_remove(&new_input_method.link);
}

void shell_init(void)
{
	static const float black[4] = {0, 0, 0, 1};
	srv.layer_shell = wlr_layer_shell_v1_create(srv.display, 4);
	privileged_add(srv.layer_shell->global);
	LISTEN(&srv.layer_shell->events.new_surface, &new_layer_surface, handle_new_layer_surface);

	struct wlr_session_lock_manager_v1 *lock_mgr = wlr_session_lock_manager_v1_create(srv.display);
	privileged_add(lock_mgr->global);
	LISTEN(&lock_mgr->events.new_lock, &new_lock, handle_new_lock);
	backdrop = wlr_scene_rect_create(srv.layers[LYR_LOCK], 0, 0, black);
	wlr_scene_node_set_enabled(&srv.layers[LYR_LOCK]->node, false);

	wl_list_init(&text_inputs);
	struct wlr_text_input_manager_v3 *ti_mgr = wlr_text_input_manager_v3_create(srv.display);
	struct wlr_input_method_manager_v2 *im_mgr = wlr_input_method_manager_v2_create(srv.display);
	privileged_add(im_mgr->global);
	LISTEN(&ti_mgr->events.new_text_input, &new_text_input, handle_new_text_input);
	LISTEN(&im_mgr->events.new_input_method, &new_input_method, handle_new_input_method);
}
