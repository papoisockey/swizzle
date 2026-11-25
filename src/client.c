#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/edges.h>
#ifdef XWAYLAND
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xcursor.h>
#include <xcb/xcb_icccm.h>
#endif
#include "swizzle.h"

struct popup {
	struct wlr_xdg_popup *popup;
	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener destroy;
};

static struct wl_listener new_toplevel;
static struct wl_listener new_popup;
static struct wl_listener new_decoration;
static struct wlr_foreign_toplevel_manager_v1 *ftl_mgr;
static struct wlr_server_decoration_manager *kde_deco;
#ifdef XWAYLAND
static struct wl_listener xwayland_ready;
static struct wl_listener new_xwayland_surface;
#endif

struct wlr_surface *client_surface(struct client *c)
{
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG)
		return c->s.xwl->surface;
#endif
	return c->s.xdg->base->surface;
}

const char *client_app_id(struct client *c)
{
	const char *s;
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG)
		s = c->s.xwl->class;
	else
#endif
		s = c->s.xdg->app_id;
	return s ? s : "";
}

const char *client_title(struct client *c)
{
	const char *s;
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG)
		s = c->s.xwl->title;
	else
#endif
		s = c->s.xdg->title;
	return s ? s : "";
}

bool client_visible(struct client *c)
{
	if (!c->mapped || !c->mon || c->minimized)
		return false;
	if (c->mon->overview && !c->scratch)
		return true;
	return c->scratch ? c->mon->scratch_shown : c->sticky || (c->tags & c->mon->tags);
}

bool client_wants_tearing(struct client *c)
{
	struct wlr_surface *s = client_surface(c);
	return s && srv.tearing &&
		wlr_tearing_control_manager_v1_surface_hint_from_surface(srv.tearing, s) == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}

struct client *client_from_surface(struct wlr_surface *surface)
{
	while (surface) {
		struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(surface);
		if (sub) {
			surface = sub->parent;
			continue;
		}
		struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
		if (xdg) {
			if (xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
				return xdg->data;
			if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup) {
				surface = xdg->popup->parent;
				continue;
			}
			return NULL;
		}
#ifdef XWAYLAND
		struct wlr_xwayland_surface *xwl = wlr_xwayland_surface_try_from_wlr_surface(surface);
		if (xwl)
			return xwl->data;
#endif
		return NULL;
	}
	return NULL;
}

struct client *client_focused(void)
{
	struct wlr_surface *s = srv.seat->keyboard_state.focused_surface;
	if (srv.seat->drag && srv.seat->keyboard_state.grab == &srv.seat->drag->keyboard_grab && !layer_from_surface(s)) {
		struct client *h = client_last_focused();
		return h && h->active ? h : NULL;
	}
	struct client *c = client_from_surface(s);
	return c && c->type != CLIENT_X11_UNMANAGED && c->mapped ? c : NULL;
}

struct client *client_last_focused(void)
{
	if (wl_list_empty(&srv.fstack))
		return NULL;
	struct client *c = wl_container_of(srv.fstack.next, c, flink);
	return c->mapped ? c : NULL;
}

struct client *client_at(double x, double y, struct wlr_surface **surf, double *sx, double *sy)
{
	double nx, ny;
	struct wlr_scene_node *node = NULL;
	for (int l = LYR_COUNT - 1; l >= 0 && !node; l--)
		if (l != LYR_DRAG)
			node = wlr_scene_node_at(&srv.layers[l]->node, x, y, &nx, &ny);
	if (surf)
		*surf = NULL;
	if (!node)
		return NULL;
	if (node->type == WLR_SCENE_NODE_BUFFER && surf) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		if (ss) {
			*surf = ss->surface;
			*sx = nx;
			*sy = ny;
		}
	}
	for (struct wlr_scene_tree *t = node->parent; t; t = t->node.parent) {
		enum client_type *type = t->node.data;
		if (!type)
			continue;
		return *type == CLIENT_LAYER ? NULL : (struct client *)type;
	}
	return NULL;
}

static void client_set_border_color(struct client *c, const float color[4])
{
	for (size_t i = 0; i < 4; i++)
		if (c->border[i])
			wlr_scene_rect_set_color(c->border[i], color);
}

const float *client_border_color(struct client *c)
{
	const struct config *k = &srv.conf;
	if (!c->active)
		return c->urgent ? k->border_urgent : k->border_normal;
	const float *state = c->maximized ? k->border_maximized : c->scratch ? k->border_scratchpad :
		c->overlay ? k->border_overlay : c->sticky ? k->border_sticky : NULL;
	return state && state[3] >= 0 ? state : k->border_focus;
}

void client_set_active(struct client *c, bool on)
{
	c->active = on;
	client_set_border_color(c, client_border_color(c));
	effects_focus(c);
	if (c->ftl)
		wlr_foreign_toplevel_handle_v1_set_activated(c->ftl, on);
#ifdef XWAYLAND
	if (c->type == CLIENT_X11) {
		if (on && c->s.xwl->minimized)
			wlr_xwayland_surface_set_minimized(c->s.xwl, false);
		wlr_xwayland_surface_activate(c->s.xwl, on);
		return;
	}
#endif
	wlr_xdg_toplevel_set_activated(c->s.xdg, on);
}

void client_update_border(struct client *c)
{
	client_set_border_color(c, client_border_color(c));
	effects_resize(c);
}

static void client_set_urgent(struct client *c, bool urgent)
{
	c->urgent = urgent;
	client_update_border(c);
	state_changed();
}

int border_of(struct client *c)
{
	if (c->fullscreen || c->type == CLIENT_X11_UNMANAGED)
		return 0;
	if ((c->single && srv.conf.no_border_when_single) || (srv.conf.monocle_no_border && c->mon && !c->floating &&
			!c->scratch && !c->maximized && !strcmp(layout_name(c->mon->layout), "monocle")))
		return 0;
	return srv.conf.border_width + srv.conf.border_outer_width;
}

static void set_ring(struct wlr_scene_rect **r, int off, int w, int gw, int gh)
{
	struct wlr_box b[4] = {
		{off, off, gw - 2 * off, w},
		{off, gh - off - w, gw - 2 * off, w},
		{off, off + w, w, gh - 2 * off - 2 * w},
		{gw - off - w, off + w, w, gh - 2 * off - 2 * w},
	};
	for (size_t i = 0; i < 4; i++) {
		wlr_scene_node_set_enabled(&r[i]->node, w > 0);
		wlr_scene_rect_set_size(r[i], MAX(b[i].width, 0), MAX(b[i].height, 0));
		wlr_scene_node_set_position(&r[i]->node, b[i].x, b[i].y);
	}
}

void client_resize(struct client *c, struct wlr_box geo)
{
	int bw = border_of(c), ow = bw ? srv.conf.border_outer_width : 0;
	geo.width = MAX(geo.width, 2 * bw + 1);
	geo.height = MAX(geo.height, 2 * bw + 1);
	c->geom = geo;

	effects_move(c, geo.x, geo.y);
	if (c->scene_surface)
		wlr_scene_node_set_position(&c->scene_surface->node, bw, bw);
	if (c->popups)
		wlr_scene_node_set_position(&c->popups->node, bw, bw);
	if (c->border[0]) {
		set_ring(c->border + 4, 0, ow, geo.width, geo.height);
		set_ring(c->border, ow, bw - ow, geo.width, geo.height);
	}
	effects_resize(c);

	int w = geo.width - 2 * bw, h = geo.height - 2 * bw;
	struct wlr_box clip = {0, 0, w, h};
	if (c->type == CLIENT_XDG)
		clip.x = c->s.xdg->base->geometry.x, clip.y = c->s.xdg->base->geometry.y;
	if (c->scene_surface && !wlr_box_equal(&clip, &c->clip)) {
		wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
		c->clip = clip;
	}
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG) {
		if (c->s.xwl->x != geo.x + bw || c->s.xwl->y != geo.y + bw || c->s.xwl->width != w || c->s.xwl->height != h)
			wlr_xwayland_surface_configure(c->s.xwl, geo.x + bw, geo.y + bw, w, h);
		return;
	}
#endif
	if (!c->s.xdg->base->initialized)
		return;
	if (c->s.xdg->scheduled.width == w && c->s.xdg->scheduled.height == h)
		return;
	wlr_xdg_toplevel_set_size(c->s.xdg, w, h);
}

struct wlr_box client_fit(struct wlr_box b, struct monitor *m)
{
	if (!m)
		return b;
	if (!wlr_box_contains_point(&m->m, b.x + b.width / 2, b.y + b.height / 2)) {
		b.x = m->w.x + (m->w.width - b.width) / 2;
		b.y = m->w.y + (m->w.height - b.height) / 2;
	}
	b.x = MAX(m->w.x, MIN(b.x, m->w.x + m->w.width - b.width));
	b.y = MAX(m->w.y, MIN(b.y, m->w.y + m->w.height - b.height));
	return b;
}

void client_restack(struct client *c)
{
	enum scene_layer l = c->type == CLIENT_X11_UNMANAGED ? LYR_OVERLAY : c->overlay ? LYR_ABOVE :
		c->fullscreen || c->scratch ? LYR_FS : c->floating || c->maximized ? LYR_FLOAT : LYR_TILE;
	wlr_scene_node_reparent(&c->scene->node, srv.layers[l]);
}

static bool client_is_child_of(struct client *c, struct client *parent);

static struct wlr_box real_geom(struct client *c)
{
	return c->in_overview ? c->overview_geom : c->geom;
}

void client_set_fullscreen(struct client *c, bool fs)
{
	if (c->fullscreen == fs && c->mapped)
		return;
	if (fs && c->mapped)
		client_leave_column(c);
	c->fullscreen = fs;
	if (fs)
		c->fake_fullscreen = false;
	else
		client_join_strip(c);
	if (c->ftl)
		wlr_foreign_toplevel_handle_v1_set_fullscreen(c->ftl, fs);
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG)
		wlr_xwayland_surface_set_fullscreen(c->s.xwl, fs);
	else
#endif
		wlr_xdg_toplevel_set_fullscreen(c->s.xdg, fs);
	if (!c->mapped)
		return;
	client_restack(c);
	struct client *d;
	wl_list_for_each(d, &srv.clients, link)
		if (client_is_child_of(d, c) && fs)
			wlr_scene_node_reparent(&d->scene->node, srv.layers[LYR_FS]);
		else if (client_is_child_of(d, c))
			client_restack(d);
	if (!c->mon)
		return;
	if (fs) {
		if (c->floating && !c->maximized)
			c->prev = real_geom(c);
		client_resize(c, c->mon->m);
	} else if (c->floating) {
		client_resize(c, client_fit(c->prev, c->mon));
	}
	arrange(c->mon);
	struct client *f = client_focused();
	if (f && f->mon == c->mon && !client_reachable(f))
		focus_top(c->mon);
	ipc_event("fullscreen %d", fs);
}

void client_set_floating(struct client *c, bool fl)
{
	if (c->fullscreen)
		return;
	if (!fl && c->floating && !c->maximized)
		c->prev = real_geom(c);
	if (fl && !c->floating)
		client_leave_column(c);
	c->floating = fl;
	if (!fl)
		client_join_strip(c);
	client_restack(c);
	if (fl && c->prev.width)
		client_resize(c, client_fit(c->prev, c->mon));
	arrange(c->mon);
}

void client_close(struct client *c)
{
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG) {
		wlr_xwayland_surface_close(c->s.xwl);
		return;
	}
#endif
	wlr_xdg_toplevel_send_close(c->s.xdg);
}

static void deco_apply(struct client *c)
{
	enum wlr_xdg_toplevel_decoration_v1_mode mode = c->deco->requested_mode;
	if (srv.conf.prefer_no_csd)
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	else if (mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE)
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	wlr_xdg_toplevel_decoration_v1_set_mode(c->deco, mode);
}

void client_update_borders(void)
{
	struct client *c;
	wl_list_for_each(c, &srv.clients, link) {
		client_set_border_color(c, client_border_color(c));
		for (size_t i = 4; i < LENGTH(c->border) && c->border[i]; i++)
			wlr_scene_rect_set_color(c->border[i], srv.conf.border_outer);
		if (c->deco && c->s.xdg->base->initialized)
			deco_apply(c);
		if (c->floating && !c->fullscreen)
			client_resize(c, c->geom);
		effects_focus(c);
	}
	wlr_server_decoration_manager_set_default_mode(kde_deco, srv.conf.prefer_no_csd ?
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER : WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
}

void client_send_to_monitor(struct client *c, struct monitor *m, uint32_t tags)
{
	struct monitor *old = c->mon;
	uint32_t newtags = tags & tag_mask() ? tags & tag_mask() : m ? m->tags : 1;
	bool moved = !old || old != m || newtags != c->tags;
	if (old && moved)
		client_leave_column(c);
	c->mon = m;
	c->tags = newtags;
	if (moved)
		client_join_strip(c);
	if (c->ftl && old != m) {
		if (old)
			wlr_foreign_toplevel_handle_v1_output_leave(c->ftl, old->output);
		if (m)
			wlr_foreign_toplevel_handle_v1_output_enter(c->ftl, m->output);
	}
	if (!m || old == m)
		return;
	if (c->scratch && old && old->scratch_shown)
		scratch_show(m, true);
	bool restored = false;
	if (c->in_overview && !m->overview) {
		c->in_overview = false;
		restored = c->floating && !c->fullscreen && !c->maximized;
		if (restored)
			c->geom = c->overview_geom;
	}
	int cx = c->geom.x + c->geom.width / 2, cy = c->geom.y + c->geom.height / 2;
	if (c->floating && !wlr_box_contains_point(&m->m, cx, cy)) {
		c->geom.x = old ? m->w.x + (c->geom.x - old->w.x) : m->w.x + (m->w.width - c->geom.width) / 2;
		c->geom.y = old ? m->w.y + (c->geom.y - old->w.y) : m->w.y + (m->w.height - c->geom.height) / 2;
		client_resize(c, client_fit(c->geom, m));
	} else if (restored) {
		client_resize(c, client_fit(c->geom, m));
	}
	if (c->fullscreen)
		client_resize(c, m->m);
	if (old)
		arrange(old);
	arrange(m);
}

static bool client_is_child_of(struct client *c, struct client *parent)
{
	if (!parent || c->type != parent->type)
		return false;
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG) {
		for (struct wlr_xwayland_surface *x = c->s.xwl->parent; x; x = x->parent)
			if (x == parent->s.xwl)
				return true;
		return false;
	}
#endif
	for (struct wlr_xdg_toplevel *t = c->s.xdg->parent; t; t = t->parent)
		if (t == parent->s.xdg)
			return true;
	return false;
}

void client_request_activate(struct client *c, bool user)
{
	if (!c || !c->mapped || c->type == CLIENT_X11_UNMANAGED || c == client_focused())
		return;
	if (user && srv.conf.focus_on_activate && !srv.locked && client_visible(c) &&
			(!c->mon->fullscreen || c->mon->fullscreen == c)) {
		focus_client(c, true);
		return;
	}
	client_set_urgent(c, true);
}

void client_reveal(struct client *c)
{
	if (!c || !c->mapped || !c->mon)
		return;
	if (c->minimized)
		client_set_minimized(c, false);
	if (!client_visible(c)) {
		srv.selmon = c->mon;
		if (c->scratch)
			scratch_toggle();
		else
			view(c->tags);
	}
	focus_client(c, true);
}

static void handle_ftl_activate(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, ftl_activate);
	if (!srv.locked)
		client_reveal(c);
}

static void handle_ftl_close(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, ftl_close);
	if (!srv.locked)
		client_close(c);
}

static void handle_ftl_fullscreen(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, ftl_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
	if (!srv.locked)
		client_set_fullscreen(c, ev->fullscreen);
}

static void handle_ftl_minimize(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, ftl_minimize);
	struct wlr_foreign_toplevel_handle_v1_minimized_event *ev = data;
	if (!srv.locked)
		client_set_minimized(c, ev->minimized);
}

void client_set_minimized(struct client *c, bool on)
{
	if (!c || !c->mapped || c->minimized == on || c->type == CLIENT_X11_UNMANAGED)
		return;
	bool was_focused = c == client_focused();
	if (on && srv.grab_c == c)
		input_cancel_grab();
	if (on)
		client_leave_column(c);
	c->minimized = on;
	if (!on)
		client_join_strip(c);
	if (c->ftl)
		wlr_foreign_toplevel_handle_v1_set_minimized(c->ftl, on);
#ifdef XWAYLAND
	if (c->type == CLIENT_X11)
		wlr_xwayland_surface_set_minimized(c->s.xwl, on);
#endif
	if (on && was_focused)
		focus_top(c->mon);
	arrange(c->mon);
	ipc_client_event(on ? "minimize" : "restore", c);
}

bool client_restore(void)
{
	struct client *c;
	struct monitor *m = srv.selmon;
	wl_list_for_each(c, &srv.fstack, flink) {
		if (!c->minimized)
			continue;
		if (m && (c->mon != m || !(c->sticky || c->scratch || (c->tags & m->tags))))
			client_send_to_monitor(c, m, c->scratch ? c->tags : m->tags);
		client_reveal(c);
		return true;
	}
	return false;
}

void client_set_maximized(struct client *c, bool on)
{
	if (!c || !c->mapped || c->type == CLIENT_X11_UNMANAGED || c->maximized == on || (on && c->scratch))
		return;
	if (on)
		client_leave_column(c);
	c->maximized = on;
	if (!on)
		client_join_strip(c);
	if (on && c->floating && !c->fullscreen)
		c->prev = real_geom(c);
#ifdef XWAYLAND
	if (c->type == CLIENT_X11)
		wlr_xwayland_surface_set_maximized(c->s.xwl, on, on);
	else
#endif
		wlr_xdg_toplevel_set_maximized(c->s.xdg, on);
	client_restack(c);
	if (!on && c->floating && !c->fullscreen)
		client_resize(c, client_fit(c->prev, c->mon));
	if (c->ftl)
		wlr_foreign_toplevel_handle_v1_set_maximized(c->ftl, on);
	client_update_border(c);
	arrange(c->mon);
}

void client_set_fake_fullscreen(struct client *c, bool on)
{
	if (!c || !c->mapped || c->type == CLIENT_X11_UNMANAGED)
		return;
	if (on && c->fullscreen)
		client_set_fullscreen(c, false);
	c->fake_fullscreen = on;
#ifdef XWAYLAND
	if (c->type == CLIENT_X11)
		wlr_xwayland_surface_set_fullscreen(c->s.xwl, on);
	else
#endif
		wlr_xdg_toplevel_set_fullscreen(c->s.xdg, on);
}

void client_set_overlay(struct client *c, bool on)
{
	if (!c || !c->mapped || c->type == CLIENT_X11_UNMANAGED)
		return;
	c->overlay = on;
	client_restack(c);
	client_update_border(c);
	arrange(c->mon);
}

void client_nudge(struct client *c, int dx, int dy, int dw, int dh)
{
	if (!c || !c->mapped || c->fullscreen || c->type == CLIENT_X11_UNMANAGED)
		return;
	if (c->maximized)
		client_set_maximized(c, false);
	struct wlr_box g = real_geom(c);
	if (!c->floating) {
		c->prev = g;
		client_set_floating(c, true);
	}
	struct wlr_box b = {g.x + dx, g.y + dy, MAX(50, g.width + dw), MAX(50, g.height + dh)};
	if (c->in_overview)
		c->overview_geom = b;
	else
		client_resize(c, b);
	c->prev = b;
	struct monitor *m = output_at(b.x + b.width / 2.0, b.y + b.height / 2.0);
	if (m && m != c->mon)
		client_send_to_monitor(c, m, m->tags);
}

static void ftl_update(struct client *c)
{
	if (c->ftl) {
		wlr_foreign_toplevel_handle_v1_set_title(c->ftl, client_title(c));
		wlr_foreign_toplevel_handle_v1_set_app_id(c->ftl, client_app_id(c));
	}
	share_update(c);
}

static void ftl_create(struct client *c)
{
	share_map(c);
	c->ftl = wlr_foreign_toplevel_handle_v1_create(ftl_mgr);
	if (!c->ftl)
		return;
	ftl_update(c);
	LISTEN(&c->ftl->events.request_activate, &c->ftl_activate, handle_ftl_activate);
	LISTEN(&c->ftl->events.request_close, &c->ftl_close, handle_ftl_close);
	LISTEN(&c->ftl->events.request_fullscreen, &c->ftl_fullscreen, handle_ftl_fullscreen);
	LISTEN(&c->ftl->events.request_minimize, &c->ftl_minimize, handle_ftl_minimize);
	wlr_foreign_toplevel_handle_v1_set_minimized(c->ftl, c->minimized);
	wlr_foreign_toplevel_handle_v1_set_maximized(c->ftl, c->maximized);
}

static void ftl_destroy(struct client *c)
{
	share_unmap(c);
	if (!c->ftl)
		return;
	wl_list_remove(&c->ftl_activate.link);
	wl_list_remove(&c->ftl_close.link);
	wl_list_remove(&c->ftl_fullscreen.link);
	wl_list_remove(&c->ftl_minimize.link);
	wlr_foreign_toplevel_handle_v1_destroy(c->ftl);
	c->ftl = NULL;
}

static void apply_rules(struct client *c, struct monitor **mon, uint32_t *tags, int *focus)
{
	const char *app_id = client_app_id(c), *title = client_title(c);
	*mon = srv.selmon;
	*tags = 0;
	*focus = -1;
	for (size_t i = 0; i < srv.conf.nrules; i++) {
		const struct rule *r = &srv.conf.rules[i];
		if (r->app_id && !strstr(app_id, r->app_id))
			continue;
		if (r->title && !strstr(title, r->title))
			continue;
		if (r->floating >= 0)
			c->floating = r->floating;
		if (r->fullscreen >= 0)
			c->fullscreen = r->fullscreen;
		if (r->scratchpad >= 0)
			c->scratch = r->scratchpad;
		if (r->sticky >= 0)
			c->sticky = r->sticky;
		if (r->focus >= 0)
			*focus = r->focus;
		if (r->tags)
			*tags = r->tags;
		if (r->output) {
			struct monitor *m = output_by_name(r->output);
			if (m && (m->output->enabled || m->asleep))
				*mon = m;
		}
	}
}

static struct client *client_parent(struct client *c)
{
	struct client *p;
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG)
		p = c->s.xwl->parent ? c->s.xwl->parent->data : NULL;
	else
#endif
		p = c->s.xdg->parent ? c->s.xdg->parent->base->data : NULL;
	return p && p->mapped && p->mon && p->type != CLIENT_X11_UNMANAGED ? p : NULL;
}

static bool client_wants_maximized(struct client *c)
{
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG)
		return c->s.xwl->maximized_vert && c->s.xwl->maximized_horz;
#endif
	return c->s.xdg->requested.maximized;
}

static void create_borders(struct client *c)
{
	for (size_t i = 0; i < LENGTH(c->border); i++)
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0, i < 4 ? srv.conf.border_normal : srv.conf.border_outer);
}

static void attach(struct client *c, struct monitor *m)
{
	struct wl_list *pos = &srv.clients;
	struct client *o;
	int n = 0;
	if (srv.conf.attach == ATTACH_BOTTOM) {
		pos = srv.clients.prev;
	} else if (srv.conf.attach == ATTACH_ASIDE && m) {
		wl_list_for_each(o, &srv.clients, link) {
			if (n >= m->nmaster)
				break;
			if (o->mon == m && client_tiled(o))
				pos = &o->link, n++;
		}
	}
	wl_list_insert(pos, &c->link);
}

static void client_mapped(struct client *c, struct wlr_box natural)
{
	struct monitor *m;
	uint32_t tags;
	int rule_focus;
	int bw = srv.conf.border_width + srv.conf.border_outer_width;

	c->minimized = c->maximized = c->overlay = c->fake_fullscreen = c->scroll_stacked = c->urgent = false;
	c->scratch = c->scratch_floated = c->sticky = false;
	c->scroll_width = 0;
	c->split = 0;
	c->in_overview = false;
	apply_rules(c, &m, &tags, &rule_focus);
	struct client *parent = client_parent(c);
	if (parent && !tags) {
		m = parent->mon;
		if (!parent->scratch && !parent->sticky)
			tags = parent->tags;
	}
	if (c->scratch) {
		c->scratch_floated = !c->floating;
		c->floating = true;
		c->fullscreen = false;
		if (m)
			scratch_show(m, true);
	}
	bool fs = c->fullscreen;
	c->fullscreen = false;
	if (m) {
		natural.width += 2 * bw;
		natural.height += 2 * bw;
		if (natural.width <= 2 * bw || natural.height <= 2 * bw)
			natural.width = m->w.width / 2, natural.height = m->w.height / 2;
		natural.x = m->w.x + (m->w.width - natural.width) / 2;
		natural.y = m->w.y + (m->w.height - natural.height) / 2;
	}
	c->prev = natural;
	c->geom = natural;
	c->mapped = true;
	attach(c, m);
	wl_list_insert(srv.fstack.prev, &c->flink);
	wlr_scene_node_set_enabled(&c->scene->node, true);
	client_restack(c);
	if (c->floating)
		client_resize(c, natural);
	ftl_create(c);
	client_send_to_monitor(c, m, tags);
	if (!m)
		wlr_scene_node_set_enabled(&c->scene->node, false);

	if (fs)
		client_set_fullscreen(c, true);
	else if (client_wants_maximized(c) && !c->scratch)
		client_set_maximized(c, true);
	scratch_fit(c);

	if (rule_focus == 1 && m && !client_visible(c) && !srv.locked) {
		srv.selmon = m;
		view(c->tags);
	}
	bool child = m && client_is_child_of(c, m->fullscreen);
	if (child)
		wlr_scene_node_reparent(&c->scene->node, srv.layers[LYR_FS]);
	bool may_focus = rule_focus != 0 && m && client_visible(c) && !srv.locked &&
		(!m->fullscreen || c->fullscreen || c->scratch || child);
	if (may_focus && srv.exclusive_focus) {
		struct client *old = client_last_focused();
		if (old && old != c)
			client_set_active(old, false);
		wl_list_remove(&c->flink);
		wl_list_insert(&srv.fstack, &c->flink);
		wlr_scene_node_raise_to_top(&c->scene->node);
	} else if (may_focus)
		focus_client(c, true);
	else if (m && rule_focus != 0)
		client_set_urgent(c, true);
	arrange(m);
	effects_map(c);
	ipc_client_event("map", c);
}

static bool column_member(struct client *c)
{
	return !c->floating && !c->fullscreen && !c->scratch && !c->maximized;
}

void client_leave_column(struct client *c)
{
	bool shown = client_visible(c) && !c->mon->overview;
	if (!c->scroll_stacked && !c->minimized && column_member(c) && !wl_list_empty(&c->link))
		for (struct wl_list *it = c->link.next; it != &srv.clients; it = it->next) {
			struct client *o = wl_container_of(it, o, link);
			if (o->mon != c->mon || o->minimized || !column_member(o) ||
					!(shown ? client_visible(o) : (o->tags & c->tags) != 0))
				continue;
			if (o->scroll_stacked && !strip_vertical(c->mon))
				o->scroll_width = c->scroll_width;
			o->scroll_stacked = false;
			break;
		}
	c->scroll_stacked = false;
}

static void client_unmapped(struct client *c)
{
	bool was_focused = c == client_focused();
	if (srv.grab_c == c)
		input_cancel_grab();
	if (srv.drop_target == c)
		srv.drop_target = NULL;
	effects_unmap(c);
	client_leave_column(c);
	c->mapped = false;
	c->active = false;
	ftl_destroy(c);
	wl_list_remove(&c->link);
	wl_list_remove(&c->flink);
	wl_list_init(&c->link);
	wl_list_init(&c->flink);
	wlr_scene_node_set_enabled(&c->scene->node, false);
	if (was_focused)
		focus_top(srv.selmon);
	arrange(c->mon);
	c->mon = NULL;
	input_motion(0, true);
	ipc_client_event("unmap", c);
}

static void handle_deco_destroy(struct wl_listener *l, void *data);

static void popup_unconstrain(struct popup *p)
{
	struct wlr_box box = {0};
	struct client *c = client_from_surface(p->popup->parent);
	struct layer_surface *ls = c ? NULL : layer_from_surface(p->popup->parent);
	struct monitor *m = c ? c->mon : ls ? ls->mon : srv.selmon;
	if (!m) {
		wlr_xdg_surface_schedule_configure(p->popup->base);
		return;
	}
	box = c ? m->w : m->m;
	if (c) {
		int bw = border_of(c);
		box.x -= c->geom.x + bw;
		box.y -= c->geom.y + bw;
		if (c->type == CLIENT_XDG) {
			box.x += c->s.xdg->base->geometry.x;
			box.y += c->s.xdg->base->geometry.y;
		}
	} else if (ls) {
		box.x -= ls->scene->node.x;
		box.y -= ls->scene->node.y;
	}
	wlr_xdg_popup_unconstrain_from_box(p->popup, &box);
}

static void handle_popup_commit(struct wl_listener *l, void *data)
{
	struct popup *p = wl_container_of(l, p, commit);
	if (!p->popup->base->initial_commit) {
		struct client *c = client_from_surface(p->popup->parent);
		if (c && c->mapped)
			effects_resize(c);
		return;
	}
	struct wlr_scene_tree *parent = p->popup->parent ? p->popup->parent->data : NULL;
	struct wlr_layer_surface_v1 *lsurf = p->popup->parent ? wlr_layer_surface_v1_try_from_wlr_surface(p->popup->parent) : NULL;
	if (lsurf && lsurf->data)
		parent = ((struct layer_surface *)lsurf->data)->popups;
	if (!parent) {
		wlr_xdg_surface_schedule_configure(p->popup->base);
		return;
	}
	if (!p->popup->base->surface->data) {
		p->popup->base->surface->data = wlr_scene_xdg_surface_create(parent, p->popup->base);
		wl_list_remove(&p->commit.link);
		wl_signal_add(&p->popup->base->surface->events.commit, &p->commit);
	}
	popup_unconstrain(p);
}

static void handle_popup_reposition(struct wl_listener *l, void *data)
{
	struct popup *p = wl_container_of(l, p, reposition);
	popup_unconstrain(p);
}

static void handle_popup_destroy(struct wl_listener *l, void *data)
{
	struct popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->reposition.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void handle_new_popup(struct wl_listener *l, void *data)
{
	struct wlr_xdg_popup *xp = data;
	struct popup *p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->popup = xp;
	LISTEN(&xp->base->surface->events.commit, &p->commit, handle_popup_commit);
	LISTEN(&xp->events.reposition, &p->reposition, handle_popup_reposition);
	LISTEN(&xp->events.destroy, &p->destroy, handle_popup_destroy);
}

static void handle_xdg_commit(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, commit);
	struct wlr_xdg_toplevel *t = c->s.xdg;

	if (t->base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(t, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE);
		wlr_xdg_toplevel_set_tiled(t, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		if (c->deco)
			deco_apply(c);
		wlr_xdg_toplevel_set_size(t, 0, 0);
		return;
	}
	if (c->mapped)
		client_resize(c, c->geom);
}

static void handle_xdg_map(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, map);
	struct wlr_xdg_toplevel *t = c->s.xdg;
	struct wlr_box geo = t->base->geometry;

	c->floating = t->parent || portal_modal(t) || (t->current.min_width && t->current.min_width == t->current.max_width &&
		t->current.min_height && t->current.min_height == t->current.max_height);
	c->fullscreen = t->requested.fullscreen;
	client_mapped(c, geo);
}

static void handle_xdg_unmap(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, unmap);
	client_unmapped(c);
}

static void handle_xdg_destroy(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, destroy);
	if (c->deco)
		handle_deco_destroy(&c->deco_destroy, NULL);
	wl_list_remove(&c->commit.link);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->request_fullscreen.link);
	wl_list_remove(&c->request_maximize.link);
	wl_list_remove(&c->request_move.link);
	wl_list_remove(&c->request_resize.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->set_app_id.link);
	effects_client_destroy(c);
	wlr_scene_node_destroy(&c->scene->node);
	free(c);
}

static void handle_request_fullscreen(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, request_fullscreen);
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG) {
		if (c->fake_fullscreen)
			client_set_fake_fullscreen(c, c->s.xwl->fullscreen);
		else
			client_set_fullscreen(c, c->s.xwl->fullscreen);
		return;
	}
#endif
	if (c->fake_fullscreen)
		client_set_fake_fullscreen(c, c->s.xdg->requested.fullscreen);
	else if (c->mapped)
		client_set_fullscreen(c, c->s.xdg->requested.fullscreen);
	if (c->s.xdg->base->initialized)
		wlr_xdg_surface_schedule_configure(c->s.xdg->base);
}

static bool may_grab(struct client *c, const uint32_t *serial)
{
	if (!(c->floating || layout_floats(c->mon)) || !srv.seat->pointer_state.button_count)
		return false;
	if (client_from_surface(srv.seat->pointer_state.focused_surface) != c)
		return false;
	return !serial || wlr_seat_validate_pointer_grab_serial(srv.seat, NULL, *serial);
}

static void handle_request_move(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, request_move);
	const uint32_t *serial = NULL;
#ifdef XWAYLAND
	if (c->type == CLIENT_XDG)
#endif
		serial = &((struct wlr_xdg_toplevel_move_event *)data)->serial;
	if (may_grab(c, serial))
		input_start_grab(c, 0);
}

static void handle_request_resize(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, request_resize);
	uint32_t edges;
	const uint32_t *serial = NULL;
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG)
		edges = ((struct wlr_xwayland_resize_event *)data)->edges;
	else
#endif
	{
		struct wlr_xdg_toplevel_resize_event *ev = data;
		edges = ev->edges;
		serial = &ev->serial;
	}
	if (edges && may_grab(c, serial))
		input_start_grab(c, edges);
}

static void handle_request_maximize(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, request_maximize);
#ifdef XWAYLAND
	if (c->type != CLIENT_XDG) {
		client_set_maximized(c, client_wants_maximized(c));
		return;
	}
#endif
	if (c->mapped)
		client_set_maximized(c, client_wants_maximized(c));
	if (c->s.xdg->base->initialized)
		wlr_xdg_surface_schedule_configure(c->s.xdg->base);
}

static void handle_set_title(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, set_title);
	ftl_update(c);
	if (c == client_focused())
		ipc_title_event(c);
}

static void handle_set_app_id(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, set_app_id);
	ftl_update(c);
	if (c == client_focused())
		ipc_client_event("focus", c);
}

static struct client *client_create(enum client_type type)
{
	struct client *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->type = type;
	wl_list_init(&c->link);
	wl_list_init(&c->flink);
	c->scene = wlr_scene_tree_create(srv.layers[type == CLIENT_X11_UNMANAGED ? LYR_OVERLAY : LYR_TILE]);
	c->scene->node.data = c;
	wlr_scene_node_set_enabled(&c->scene->node, false);
	if (type != CLIENT_X11_UNMANAGED) {
		create_borders(c);
		effects_client_create(c);
	}
	return c;
}

static void handle_new_toplevel(struct wl_listener *l, void *data)
{
	struct wlr_xdg_toplevel *t = data;
	struct client *c = client_create(CLIENT_XDG);
	if (!c)
		return;
	c->s.xdg = t;
	t->base->data = c;
	c->scene_surface = wlr_scene_xdg_surface_create(c->scene, t->base);
	c->popups = wlr_scene_tree_create(c->scene);
	t->base->surface->data = c->popups;

	LISTEN(&t->base->surface->events.commit, &c->commit, handle_xdg_commit);
	LISTEN(&t->base->surface->events.map, &c->map, handle_xdg_map);
	LISTEN(&t->base->surface->events.unmap, &c->unmap, handle_xdg_unmap);
	LISTEN(&t->events.destroy, &c->destroy, handle_xdg_destroy);
	LISTEN(&t->events.request_fullscreen, &c->request_fullscreen, handle_request_fullscreen);
	LISTEN(&t->events.request_maximize, &c->request_maximize, handle_request_maximize);
	LISTEN(&t->events.request_move, &c->request_move, handle_request_move);
	LISTEN(&t->events.request_resize, &c->request_resize, handle_request_resize);
	LISTEN(&t->events.set_title, &c->set_title, handle_set_title);
	LISTEN(&t->events.set_app_id, &c->set_app_id, handle_set_app_id);
}

static void handle_deco_mode(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, deco_mode);
	if (c->s.xdg->base->initialized)
		deco_apply(c);
}

static void handle_deco_destroy(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, deco_destroy);
	wl_list_remove(&c->deco_mode.link);
	wl_list_remove(&c->deco_destroy.link);
	c->deco = NULL;
}

static void handle_new_decoration(struct wl_listener *l, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	struct client *c = deco->toplevel->base->data;
	if (!c)
		return;
	c->deco = deco;
	LISTEN(&deco->events.request_mode, &c->deco_mode, handle_deco_mode);
	LISTEN(&deco->events.destroy, &c->deco_destroy, handle_deco_destroy);
	handle_deco_mode(&c->deco_mode, deco);
}

#ifdef XWAYLAND
static bool x11_wants_float(struct wlr_xwayland_surface *x)
{
	xcb_size_hints_t *h = x->size_hints;
	static const enum wlr_xwayland_net_wm_window_type types[] = {WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY};
	for (size_t i = 0; i < LENGTH(types); i++)
		if (wlr_xwayland_surface_has_window_type(x, types[i]))
			return true;
	if (x->parent || x->modal)
		return true;
	return h && h->min_width > 0 && h->min_height > 0 &&
		h->min_width == h->max_width && h->min_height == h->max_height;
}

static void handle_x11_commit(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, commit);
	if (c->mapped)
		effects_resize(c);
}

static void handle_x11_map(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, map);
	struct wlr_xwayland_surface *x = c->s.xwl;

	c->type = x->override_redirect ? CLIENT_X11_UNMANAGED : CLIENT_X11;
	if (c->type == CLIENT_X11 && !c->border[0]) {
		create_borders(c);
		effects_client_create(c);
	}
	c->scene_surface = wlr_scene_subsurface_tree_create(c->scene, x->surface);
	x->surface->data = c->scene_surface;
	c->clip = (struct wlr_box){0};

	if (c->type == CLIENT_X11_UNMANAGED) {
		for (size_t i = 0; i < LENGTH(c->border) && c->border[i]; i++)
			wlr_scene_node_set_enabled(&c->border[i]->node, false);
		effects_resize(c);
		wlr_scene_node_reparent(&c->scene->node, srv.layers[LYR_OVERLAY]);
		c->mapped = true;
		wlr_scene_node_set_position(&c->scene->node, x->x, x->y);
		wlr_scene_node_set_enabled(&c->scene->node, true);
		if (!srv.exclusive_focus && wlr_xwayland_surface_override_redirect_wants_focus(x))
			input_set_focus_surface(x->surface);
		return;
	}
	LISTEN(&x->surface->events.commit, &c->commit, handle_x11_commit);
	c->floating = x11_wants_float(x);
	c->fullscreen = x->fullscreen;
	client_mapped(c, (struct wlr_box){x->x, x->y, x->width, x->height});
}

static void handle_x11_unmap(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, unmap);
	if (c->type == CLIENT_X11_UNMANAGED) {
		c->mapped = false;
		if (srv.seat->keyboard_state.focused_surface == c->s.xwl->surface)
			focus_top(srv.selmon);
		wlr_scene_node_set_enabled(&c->scene->node, false);
		input_motion(0, true);
	} else {
		wl_list_remove(&c->commit.link);
		client_unmapped(c);
	}
	if (c->scene_surface)
		wlr_scene_node_destroy(&c->scene_surface->node);
	c->scene_surface = NULL;
	c->s.xwl->surface->data = NULL;
}

static void handle_x11_associate(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, associate);
	LISTEN(&c->s.xwl->surface->events.map, &c->map, handle_x11_map);
	LISTEN(&c->s.xwl->surface->events.unmap, &c->unmap, handle_x11_unmap);
}

static void handle_x11_dissociate(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, dissociate);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
}

static void handle_x11_configure(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	if (!c->mapped || c->type == CLIENT_X11_UNMANAGED) {
		wlr_xwayland_surface_configure(c->s.xwl, ev->x, ev->y, ev->width, ev->height);
		if (c->mapped)
			wlr_scene_node_set_position(&c->scene->node, ev->x, ev->y);
		return;
	}
	int bw = border_of(c);
	if (c->floating && !c->fullscreen && !c->maximized && c != srv.grab_c) {
		struct wlr_box b = {ev->x - bw, ev->y - bw, ev->width + 2 * bw, ev->height + 2 * bw};
		if (!c->in_overview) {
			client_resize(c, b);
			effects_settle(c);
			return;
		}
		c->overview_geom = b;
	}
	wlr_xwayland_surface_configure(c->s.xwl, c->geom.x + bw, c->geom.y + bw,
		c->geom.width - 2 * bw, c->geom.height - 2 * bw);
}

static void handle_x11_geometry(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, set_geometry);
	if (c->type == CLIENT_X11_UNMANAGED && c->mapped)
		wlr_scene_node_set_position(&c->scene->node, c->s.xwl->x, c->s.xwl->y);
}

static void handle_x11_minimize(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, minimize);
	struct wlr_xwayland_minimize_event *ev = data;
	if (c->mapped && c->type == CLIENT_X11 && (client_visible(c) || c->minimized))
		client_set_minimized(c, ev->minimize);
	else
		wlr_xwayland_surface_set_minimized(c->s.xwl, ev->minimize && !client_visible(c));
}

static void handle_x11_activate(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, activate);
	struct client *f = client_from_surface(srv.seat->keyboard_state.focused_surface);
	client_request_activate(c, f && f->type != CLIENT_XDG);
}

static void handle_x11_hints(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, set_hints);
	if (!c->mapped || c == client_focused() || !c->s.xwl->hints)
		return;
	bool urgent = xcb_icccm_wm_hints_get_urgency(c->s.xwl->hints);
	if (urgent != c->urgent)
		client_set_urgent(c, urgent);
}

static void handle_x11_destroy(struct wl_listener *l, void *data)
{
	struct client *c = wl_container_of(l, c, destroy);
	wl_list_remove(&c->associate.link);
	wl_list_remove(&c->dissociate.link);
	wl_list_remove(&c->configure.link);
	wl_list_remove(&c->activate.link);
	wl_list_remove(&c->set_hints.link);
	wl_list_remove(&c->minimize.link);
	wl_list_remove(&c->set_geometry.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->set_app_id.link);
	wl_list_remove(&c->request_fullscreen.link);
	wl_list_remove(&c->request_maximize.link);
	wl_list_remove(&c->request_move.link);
	wl_list_remove(&c->request_resize.link);
	wl_list_remove(&c->destroy.link);
	effects_client_destroy(c);
	wlr_scene_node_destroy(&c->scene->node);
	free(c);
}

static void handle_new_xwayland_surface(struct wl_listener *l, void *data)
{
	struct wlr_xwayland_surface *x = data;
	struct client *c = client_create(x->override_redirect ? CLIENT_X11_UNMANAGED : CLIENT_X11);
	if (!c)
		return;
	c->s.xwl = x;
	x->data = c;
	LISTEN(&x->events.associate, &c->associate, handle_x11_associate);
	LISTEN(&x->events.dissociate, &c->dissociate, handle_x11_dissociate);
	LISTEN(&x->events.request_configure, &c->configure, handle_x11_configure);
	LISTEN(&x->events.request_activate, &c->activate, handle_x11_activate);
	LISTEN(&x->events.set_hints, &c->set_hints, handle_x11_hints);
	LISTEN(&x->events.request_minimize, &c->minimize, handle_x11_minimize);
	LISTEN(&x->events.set_geometry, &c->set_geometry, handle_x11_geometry);
	LISTEN(&x->events.set_title, &c->set_title, handle_set_title);
	LISTEN(&x->events.set_class, &c->set_app_id, handle_set_app_id);
	LISTEN(&x->events.request_fullscreen, &c->request_fullscreen, handle_request_fullscreen);
	LISTEN(&x->events.request_maximize, &c->request_maximize, handle_request_maximize);
	LISTEN(&x->events.request_move, &c->request_move, handle_request_move);
	LISTEN(&x->events.request_resize, &c->request_resize, handle_request_resize);
	LISTEN(&x->events.destroy, &c->destroy, handle_x11_destroy);
}

static void handle_xwayland_ready(struct wl_listener *l, void *data)
{
	wlr_xwayland_set_seat(srv.xwayland, srv.seat);
	client_update_xcursor();
}
#endif

void client_update_xcursor(void)
{
#ifdef XWAYLAND
	if (!srv.xwayland || !srv.xcursor_mgr)
		return;
	wlr_xcursor_manager_load(srv.xcursor_mgr, 1);
	struct wlr_xcursor *xc = wlr_xcursor_manager_get_xcursor(srv.xcursor_mgr, "default", 1);
	if (xc && xc->image_count) {
		struct wlr_xcursor_image *img = xc->images[0];
		wlr_xwayland_set_cursor(srv.xwayland, wlr_xcursor_image_get_buffer(img), img->hotspot_x, img->hotspot_y);
	}
#endif
}

void client_finish(void)
{
	share_finish();
	wl_list_remove(&new_toplevel.link);
	wl_list_remove(&new_popup.link);
	wl_list_remove(&new_decoration.link);
#ifdef XWAYLAND
	if (srv.xwayland) {
		wl_list_remove(&xwayland_ready.link);
		wl_list_remove(&new_xwayland_surface.link);
		wlr_xwayland_destroy(srv.xwayland);
		srv.xwayland = NULL;
	}
#endif
}

void client_init(void)
{
	wl_list_init(&srv.clients);
	wl_list_init(&srv.fstack);
	srv.xdg_shell = wlr_xdg_shell_create(srv.display, 6);
	LISTEN(&srv.xdg_shell->events.new_toplevel, &new_toplevel, handle_new_toplevel);
	LISTEN(&srv.xdg_shell->events.new_popup, &new_popup, handle_new_popup);

	struct wlr_xdg_decoration_manager_v1 *deco = wlr_xdg_decoration_manager_v1_create(srv.display);
	LISTEN(&deco->events.new_toplevel_decoration, &new_decoration, handle_new_decoration);
	kde_deco = wlr_server_decoration_manager_create(srv.display);
	wlr_server_decoration_manager_set_default_mode(kde_deco, srv.conf.prefer_no_csd ?
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER : WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
	ftl_mgr = wlr_foreign_toplevel_manager_v1_create(srv.display);
	privileged_add(ftl_mgr->global);
	share_init();

#ifdef XWAYLAND
	srv.xwayland = wlr_xwayland_create(srv.display, srv.compositor, true);
	if (!srv.xwayland) {
		wlr_log(WLR_ERROR, "xwayland unavailable");
		return;
	}
	LISTEN(&srv.xwayland->events.ready, &xwayland_ready, handle_xwayland_ready);
	LISTEN(&srv.xwayland->events.new_surface, &new_xwayland_surface, handle_new_xwayland_surface);
	setenv("DISPLAY", srv.xwayland->display_name, 1);
	wlr_log(WLR_INFO, "xwayland on DISPLAY=%s", srv.xwayland->display_name);
#endif
}
