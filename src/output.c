#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include "ext-workspace-v1-protocol.h"
#include "swizzle.h"

static struct wl_listener new_output;
static struct wl_listener layout_change;
static struct wl_listener mgr_apply, mgr_test, power_set_mode, workspace_commit;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_ext_workspace_manager_v1 *workspaces;
static struct wl_event_source *state_idle, *lock_retry;
static bool finishing, focus_cleared, any_fullscreen;

static bool layout_is(struct monitor *m, const char *name);
static int tag_slot(uint32_t tags);
static int switch_slot(struct monitor *m, uint32_t old);
static void list_swap(struct wl_list *a, struct wl_list *b);
static bool tiled(struct client *c, struct monitor *m);
static int strip_columns(struct monitor *m);

static bool monitor_off(struct monitor *m)
{
	return (!m->output->enabled && !m->asleep) || m->oc.enabled == 0;
}

static struct wlr_output_mode *pick_mode(struct wlr_output *o, const struct output_conf *oc)
{
	struct wlr_output_mode *mode, *best = NULL;
	struct wlr_output_mode *pref = wlr_output_preferred_mode(o);
	int w = oc->width ? oc->width : pref ? pref->width : 0;
	int h = oc->width ? oc->height : pref ? pref->height : 0;
	int want = (int)(oc->refresh * 1000.0f);
	wl_list_for_each(mode, &o->modes, link) {
		if (mode->width != w || mode->height != h)
			continue;
		if (!best || (want ? abs(mode->refresh - want) < abs(best->refresh - want) : mode->refresh > best->refresh))
			best = mode;
	}
	return best ? best : pref;
}

static void build_state(struct monitor *m, struct wlr_output_state *st, bool with_vrr)
{
	const struct output_conf *oc = &m->oc;
	struct wlr_output *o = m->output;
	int refresh = (int)(oc->refresh * 1000.0f);

	if (!o->enabled && !m->asleep)
		wlr_output_state_set_enabled(st, true);
	if (!wl_list_empty(&o->modes)) {
		struct wlr_output_mode *mode = pick_mode(o, oc);
		if (mode && (!oc->width || (mode->width == oc->width && mode->height == oc->height))) {
			if (mode != o->current_mode)
				wlr_output_state_set_mode(st, mode);
		} else if (oc->width) {
			wlr_output_state_set_custom_mode(st, oc->width, oc->height, refresh);
		}
	} else if (oc->width && (o->width != oc->width || o->height != oc->height || (refresh && o->refresh != refresh))) {
		wlr_output_state_set_custom_mode(st, oc->width, oc->height, refresh);
	}
	float scale = oc->scale > 0 ? oc->scale : 1.0f;
	if (scale != o->scale)
		wlr_output_state_set_scale(st, scale);
	enum wl_output_transform transform = oc->transform >= 0 ? (enum wl_output_transform)oc->transform :
		WL_OUTPUT_TRANSFORM_NORMAL;
	if (transform != o->transform)
		wlr_output_state_set_transform(st, transform);
	bool vrr = oc->vrr == VRR_ON;
	if (with_vrr && o->adaptive_sync_supported && vrr != (o->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED))
		wlr_output_state_set_adaptive_sync_enabled(st, vrr);
}

static void layout_add(struct monitor *m, bool pos, int x, int y)
{
	struct wlr_output_layout_output *lo = pos ? wlr_output_layout_add(srv.output_layout, m->output, x, y) :
		wlr_output_layout_add_auto(srv.output_layout, m->output);
	if (lo && m->scene_output)
		wlr_scene_output_layout_add_output(srv.scene_layout, lo, m->scene_output);
}

void output_apply_config(struct monitor *m)
{
	const struct output_conf *oc = &m->oc;
	struct wlr_output_state st;
	bool ok, had_vrr;

	struct output_conf next;
	config_output(m->output->name, &next);
	if (m->configured)
		m->tag_layout[tag_slot(m->tags)] = m->layout;
	for (int t = 0; t <= TAGCOUNT; t++) {
		int want = t < TAGCOUNT && next.tag_layout[t] >= 0 ? next.tag_layout[t] : next.layout;
		int had = t < TAGCOUNT && m->cfg.tag_layout[t] >= 0 ? m->cfg.tag_layout[t] : m->cfg.layout;
		if (!m->configured || want != had)
			m->tag_layout[t] = want;
	}
	m->configured = true;
	m->layout = m->tag_layout[tag_slot(m->tags)];
	struct output_conf eff = next;
	if (next.enabled == m->cfg.enabled)
		eff.enabled = m->oc.enabled;
	if (next.width == m->cfg.width && next.height == m->cfg.height && next.refresh == m->cfg.refresh)
		eff.width = m->oc.width, eff.height = m->oc.height, eff.refresh = m->oc.refresh;
	if (next.scale == m->cfg.scale)
		eff.scale = m->oc.scale;
	if (next.transform == m->cfg.transform)
		eff.transform = m->oc.transform;
	if (next.has_pos == m->cfg.has_pos && next.x == m->cfg.x && next.y == m->cfg.y)
		eff.has_pos = m->oc.has_pos, eff.x = m->oc.x, eff.y = m->oc.y;
	if (next.vrr == m->cfg.vrr)
		eff.vrr = m->oc.vrr;
	m->cfg = next;
	m->oc = eff;
	m->vrr_failed = false;
	bool sw = oc->hardware_cursor == 0 || (oc->hardware_cursor == 2 && srv.is_virtual);
	if (sw != m->swcursor_locked) {
		wlr_output_lock_software_cursors(m->output, sw);
		m->swcursor_locked = sw;
		wlr_log(WLR_INFO, "%s: %s cursor", m->output->name, sw ? "software" : "hardware");
	}
	bool lock = oc->direct_scanout == 0;
	if (lock != m->scanout_locked) {
		wlr_output_lock_attach_render(m->output, lock);
		m->scanout_locked = lock;
		wlr_log(WLR_INFO, "%s: direct scanout %s", m->output->name, lock ? "disabled" : "enabled");
	}
	if (m->oc.enabled == 0) {
		m->asleep = false;
		wlr_output_layout_remove(srv.output_layout, m->output);
		if (m->output->enabled) {
			wlr_output_state_init(&st);
			wlr_output_state_set_enabled(&st, false);
			wlr_output_commit_state(m->output, &st);
			wlr_output_state_finish(&st);
		}
		return;
	}
	if (m->asleep)
		goto layout;
	wlr_output_state_init(&st);
	build_state(m, &st, true);
	had_vrr = st.committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
	ok = wlr_output_commit_state(m->output, &st);
	wlr_output_state_finish(&st);
	if (!ok && had_vrr) {
		wlr_log(WLR_ERROR, "%s: config rejected, retrying without vrr", m->output->name);
		wlr_output_state_init(&st);
		build_state(m, &st, false);
		ok = wlr_output_commit_state(m->output, &st);
		wlr_output_state_finish(&st);
		m->vrr_failed = ok && oc->vrr == VRR_ON;
	}
	if (!ok) {
		wlr_log(WLR_ERROR, "%s: config rejected, using preferred mode", m->output->name);
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, true);
		struct wlr_output_mode *pref = wlr_output_preferred_mode(m->output);
		if (pref)
			wlr_output_state_set_mode(&st, pref);
		wlr_output_commit_state(m->output, &st);
		wlr_output_state_finish(&st);
	}
layout:
	layout_add(m, oc->has_pos, oc->x, oc->y);
}

static bool commit_frame(struct monitor *m)
{
	const struct output_conf *oc = &m->oc;
	struct client *fs = m->fullscreen;
	struct wlr_output_state st;
	bool ok = false;

	wlr_output_state_init(&st);
	if (!wlr_scene_output_build_state(m->scene_output, &st, NULL))
		goto out;

	struct client *f = oc->vrr == VRR_CONTENT ? client_last_focused() : NULL;
	bool want_vrr = oc->vrr == VRR_ON || ((oc->vrr == VRR_FULLSCREEN || oc->vrr == VRR_CONTENT) && fs) ||
		(f && f->mon == m && client_visible(f) && client_wants_vrr(f));
	bool cur_vrr = m->output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
	if (!m->vrr_failed && m->output->adaptive_sync_supported && want_vrr != cur_vrr)
		wlr_output_state_set_adaptive_sync_enabled(&st, want_vrr);

	st.tearing_page_flip = oc->tearing == 1 && fs && client_wants_tearing(fs);

	if (st.committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED)
		wlr_log(WLR_INFO, "%s: vrr %s", m->output->name, want_vrr ? "on" : "off");

	ok = wlr_output_commit_state(m->output, &st);
	if (!ok && st.tearing_page_flip) {
		wlr_log(WLR_DEBUG, "%s: tearing page flip rejected", m->output->name);
		st.tearing_page_flip = false;
		ok = wlr_output_commit_state(m->output, &st);
	}
	if (st.tearing_page_flip != m->tearing_active) {
		m->tearing_active = st.tearing_page_flip;
		wlr_log(WLR_INFO, "%s: tearing %s", m->output->name, m->tearing_active ? "on" : "off");
	}
	if (ok)
		goto out;
	wlr_log(WLR_ERROR, "%s: frame commit failed", m->output->name);
	if (st.committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED) {
		wlr_log(WLR_INFO, "%s: vrr %s rejected", m->output->name, want_vrr ? "on" : "off");
		st.committed &= ~WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
		ok = wlr_output_commit_state(m->output, &st);
		m->vrr_failed = ok && want_vrr;
	}
out:
	wlr_output_state_finish(&st);
	return ok;
}

static int handle_lock_retry(void *data)
{
	struct monitor *m;
	wl_list_for_each(m, &srv.monitors, link)
		if (m->output->enabled && (!srv.lock_pending || m->lock_frames < 2))
			wlr_output_schedule_frame(m->output);
	return 0;
}

static void handle_frame(struct wl_listener *l, void *data)
{
	struct monitor *m = wl_container_of(l, m, frame);
	struct timespec now;

	if (srv.grab_c && srv.grab_c->mon == m)
		input_apply_grab();
	effects_frame();
	bool shown = !wlr_scene_output_needs_frame(m->scene_output) || commit_frame(m);
	if (srv.lock_pending && shown) {
		lock_frame(m);
	} else if (!shown) {
		if (srv.lock_pending)
			m->lock_frames = 0;
		wl_event_source_timer_update(lock_retry, 100);
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

static void handle_request_state(struct wl_listener *l, void *data)
{
	struct monitor *m = wl_container_of(l, m, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(m->output, ev->state);
	output_update_all();
}

static void handle_destroy(struct wl_listener *l, void *data)
{
	struct monitor *m = wl_container_of(l, m, destroy);
	struct layer_surface *ls, *tmp;

	m->destroying = true;
	for (size_t i = 0; i < LENGTH(m->layers); i++)
		wl_list_for_each_safe(ls, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(ls->ls);

	for (size_t i = 0; i < TAGCOUNT; i++)
		if (m->ws[i])
			wlr_ext_workspace_handle_v1_destroy(m->ws[i]);
	if (m->ws_group)
		wlr_ext_workspace_group_handle_v1_destroy(m->ws_group);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->request_state.link);
	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->link);
	m->output->data = NULL;
	if (m->root)
		wlr_scene_node_destroy(&m->root->node);
	if (m->optimized_blur)
		wlr_scene_node_destroy(m->optimized_blur);

	if (srv.selmon == m)
		srv.selmon = wl_list_empty(&srv.monitors) ? NULL : wl_container_of(srv.monitors.next, srv.selmon, link);

	struct client *c;
	wl_list_for_each(c, &srv.clients, link)
		if (c->mon == m) {
			c->mon = NULL;
			c->scroll_stacked = false;
		}
	free(m);
	output_update_all();
}

static void update_workspace_handles(struct monitor *m)
{
	for (int i = 0; i < TAGCOUNT; i++) {
		char id[64], name[12];
		if (i >= srv.conf.tag_count) {
			if (m->ws[i])
				wlr_ext_workspace_handle_v1_destroy(m->ws[i]);
			m->ws[i] = NULL;
			continue;
		}
		if (!m->ws[i]) {
			snprintf(id, sizeof(id), "%s/%d", m->output->name, i + 1);
			m->ws[i] = wlr_ext_workspace_handle_v1_create(workspaces, id,
				EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
			if (!m->ws[i])
				continue;
			m->ws[i]->data = m;
			wlr_ext_workspace_handle_v1_set_group(m->ws[i], m->ws_group);
		}
		snprintf(name, sizeof(name), "%d", i + 1);
		wlr_ext_workspace_handle_v1_set_name(m->ws[i], srv.conf.tag_names[i] ? srv.conf.tag_names[i] : name);
	}
}

static void handle_new_output(struct wl_listener *l, void *data)
{
	struct wlr_output *o = data;

	if (o->non_desktop) {
		lease_offer(o);
		return;
	}

	if (!wlr_output_init_render(o, srv.allocator, srv.renderer)) {
		wlr_log(WLR_ERROR, "%s: cannot init rendering", o->name);
		return;
	}
	struct monitor *m = calloc(1, sizeof(*m));
	if (!m)
		return;
	m->output = o;
	o->data = m;
	m->tags = m->prevtags = 1;
	m->mfact = srv.conf.mfact;
	m->nmaster = srv.conf.nmaster;
	for (int t = 0; t <= TAGCOUNT; t++)
		m->tag_mfact[t] = m->mfact, m->tag_nmaster[t] = m->nmaster;
	for (size_t i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);

	LISTEN(&o->events.frame, &m->frame, handle_frame);
	LISTEN(&o->events.request_state, &m->request_state, handle_request_state);
	LISTEN(&o->events.destroy, &m->destroy, handle_destroy);

	wl_list_insert(srv.monitors.prev, &m->link);
	m->ws_group = wlr_ext_workspace_group_handle_v1_create(workspaces, 0);
	if (m->ws_group)
		wlr_ext_workspace_group_handle_v1_output_enter(m->ws_group, o);
	update_workspace_handles(m);
	m->scene_output = wlr_scene_output_create(srv.scene, o);
	if (wlr_output_is_drm(o) && wlr_drm_backend_get_parent(o->backend))
		wlr_log(WLR_INFO, "%s: on a secondary GPU, frames are copied across GPUs", o->name);
	output_apply_config(m);
	input_map_touch();
	if (!srv.selmon || (m->oc.primary == 1 && wl_list_empty(&srv.clients)))
		srv.selmon = m;

	wlr_log(WLR_INFO, "%s: %dx%d@%.3f vrr=%s", o->name, o->width, o->height, o->refresh / 1000.0,
		o->adaptive_sync_supported ? "supported" : "unsupported");
	output_update_all();
	ipc_event("layout %s %s", o->name, layout_name(m->layout));
	ipc_event("overview %s %d", o->name, m->overview);
}

static void handle_layout_change(struct wl_listener *l, void *data)
{
	output_update_all();
}

static void update_output_manager(void)
{
	struct monitor *m;
	struct wlr_output_configuration_v1 *cfg = wlr_output_configuration_v1_create();
	if (!cfg)
		return;
	wl_list_for_each(m, &srv.monitors, link) {
		struct wlr_output_configuration_head_v1 *h = wlr_output_configuration_head_v1_create(cfg, m->output);
		if (!h)
			continue;
		h->state.enabled = !monitor_off(m);
		h->state.x = m->m.x;
		h->state.y = m->m.y;
	}
	wlr_output_manager_v1_set_configuration(output_mgr, cfg);
}

void output_update_all(void)
{
	struct monitor *m;
	struct client *c, *tmp;

	wl_list_for_each(m, &srv.monitors, link) {
		bool off = monitor_off(m);
		struct layer_surface *ls;
		for (size_t i = 0; i < LENGTH(m->layers); i++)
			wl_list_for_each(ls, &m->layers[i], link) {
				wlr_scene_node_set_enabled(&ls->scene->node, !off && ls->mapped);
				wlr_scene_node_set_enabled(&ls->popups->node, !off);
			}
		if (off)
			continue;
		wlr_output_layout_get_box(srv.output_layout, m->output, &m->m);
		layer_arrange(m);
	}
	if (!srv.selmon || monitor_off(srv.selmon)) {
		srv.selmon = NULL;
		wl_list_for_each(m, &srv.monitors, link)
			if (!monitor_off(m)) {
				srv.selmon = m;
				break;
			}
	}
	if (srv.selmon) {
		wl_list_for_each_safe(c, tmp, &srv.clients, link)
			if ((!c->mon || monitor_off(c->mon)) && c->mapped)
				client_send_to_monitor(c, srv.selmon, c->tags);
	}
	wl_list_for_each(c, &srv.clients, link)
		if (c->mapped && c->mon && !monitor_off(c->mon) && c->floating && !c->fullscreen && !c->maximized &&
				c != srv.grab_c && !wlr_box_contains_point(&c->mon->m, c->geom.x + c->geom.width / 2,
				c->geom.y + c->geom.height / 2))
			client_resize(c, client_fit(c->geom, c->mon));
	lock_arrange();
	update_output_manager();
	output_update_root();
	arrange_all();
	struct client *f = client_focused();
	if (f ? !client_reachable(f) : !srv.seat->keyboard_state.focused_surface)
		focus_top(f && f->mon && !monitor_off(f->mon) ? f->mon : srv.selmon);
	input_motion(0, true);
}

struct monitor *output_at(double x, double y)
{
	struct wlr_output *o = wlr_output_layout_output_at(srv.output_layout, x, y);
	return o ? o->data : NULL;
}

struct monitor *output_by_name(const char *name)
{
	struct monitor *m;
	wl_list_for_each(m, &srv.monitors, link)
		if (!strcmp(m->output->name, name))
			return m;
	return NULL;
}

struct monitor *output_dir(struct monitor *from, int dir)
{
	if (!from || wl_list_length(&srv.monitors) < 2)
		return from;
	struct wl_list *n = &from->link;
	do {
		n = dir > 0 ? n->next : n->prev;
		if (n == &srv.monitors)
			continue;
		struct monitor *m = wl_container_of(n, m, link);
		if (!monitor_off(m))
			return m;
	} while (n != &from->link);
	return from;
}

void output_update_tag_names(void)
{
	struct monitor *m;
	struct client *c, *tmp;
	uint32_t mask = tag_mask(), last = 1u << (srv.conf.tag_count - 1);
	wl_list_for_each(c, &srv.clients, link)
		if (!(c->tags & mask) || (c->tags & ~mask))
			client_leave_column(c);
	wl_list_for_each(m, &srv.monitors, link) {
		uint32_t old = m->tags;
		update_workspace_handles(m);
		if (!(m->tags & mask))
			m->tags = last;
		m->tags &= mask;
		m->prevtags = m->prevtags & mask ? m->prevtags & mask : m->tags;
		int layout = m->layout;
		if (m->tags != old)
			switch_slot(m, old);
		if (m->layout != layout)
			ipc_event("layout %s %s", m->output->name, layout_name(m->layout));
	}
	wl_list_for_each_safe(c, tmp, &srv.clients, link)
		if (!(c->tags & mask) || (c->tags & ~mask)) {
			c->tags = c->tags & mask ? c->tags & mask : last;
			client_join_strip(c);
		}
}

void output_update_root(void)
{
	struct monitor *m;
	wl_list_for_each(m, &srv.monitors, link) {
		if (!m->root) {
			m->root = wlr_scene_rect_create(srv.layers[LYR_BG], 0, 0, srv.conf.root_color);
			if (!m->root)
				continue;
			wlr_scene_node_lower_to_bottom(&m->root->node);
		}
		wlr_scene_rect_set_color(m->root, srv.conf.root_color);
		wlr_scene_rect_set_size(m->root, m->m.width, m->m.height);
		wlr_scene_node_set_position(&m->root->node, m->m.x, m->m.y);
		wlr_scene_node_set_enabled(&m->root->node, !monitor_off(m));
		effects_monitor_update(m);
	}
}

static void output_update_workspaces(void)
{
	struct monitor *m;
	struct client *c;
	wl_list_for_each(m, &srv.monitors, link) {
		uint32_t urgent = 0;
		wl_list_for_each(c, &srv.clients, link)
			if (c->mon == m && c->urgent && !c->scratch)
				urgent |= c->tags;
		for (int i = 0; i < TAGCOUNT; i++)
			if (m->ws[i]) {
				wlr_ext_workspace_handle_v1_set_active(m->ws[i], m->tags & 1u << i);
				wlr_ext_workspace_handle_v1_set_urgent(m->ws[i], urgent & 1u << i);
			}
	}
}

static void handle_workspace_commit(struct wl_listener *l, void *data)
{
	struct wlr_ext_workspace_v1_commit_event *ev = data;
	struct wlr_ext_workspace_v1_request *req;
	if (srv.locked)
		return;
	wl_list_for_each(req, ev->requests, link) {
		if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE || !req->activate.workspace)
			continue;
		struct monitor *m = req->activate.workspace->data;
		if (monitor_off(m))
			continue;
		for (int i = 0; i < TAGCOUNT; i++)
			if (m->ws[i] == req->activate.workspace) {
				srv.selmon = m;
				view(1u << i);
			}
	}
}

static void apply_output_config(struct wlr_output_configuration_v1 *cfg, bool test)
{
	size_t n;
	struct wlr_output_configuration_head_v1 *h;
	wl_list_for_each(h, &cfg->heads, link) {
		struct monitor *m = h->state.output->data;
		if (m)
			m->vrr_toggled = h->state.adaptive_sync_enabled !=
				(m->output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	}
	struct wlr_backend_output_state *states = wlr_output_configuration_v1_build_state(cfg, &n);
	size_t kept = 0;
	for (size_t i = 0; states && i < n; i++) {
		struct monitor *m = states[i].output->data;
		if (m && m->asleep && states[i].base.enabled)
			wlr_output_state_finish(&states[i].base);
		else
			states[kept++] = states[i];
	}
	n = kept;
	bool ok = states && (test ? wlr_backend_test(srv.backend, states, n) : wlr_backend_commit(srv.backend, states, n));
	for (size_t i = 0; states && i < n; i++)
		wlr_output_state_finish(&states[i].base);
	free(states);

	wl_list_for_each(h, &cfg->heads, link) {
		struct monitor *m = h->state.output->data;
		if (!ok || test || !m)
			continue;
		m->asleep = m->asleep && h->state.enabled;
		if (m->vrr_toggled) {
			m->oc.vrr = h->state.adaptive_sync_enabled ? VRR_ON : VRR_OFF;
			m->vrr_failed = false;
		}
		m->oc.enabled = h->state.enabled;
		if (!h->state.enabled) {
			wlr_output_layout_remove(srv.output_layout, m->output);
			continue;
		}
		if (h->state.mode)
			m->oc.width = h->state.mode->width, m->oc.height = h->state.mode->height,
			m->oc.refresh = h->state.mode->refresh / 1000.0f;
		else if (h->state.custom_mode.width)
			m->oc.width = h->state.custom_mode.width, m->oc.height = h->state.custom_mode.height,
			m->oc.refresh = h->state.custom_mode.refresh / 1000.0f;
		m->oc.scale = h->state.scale;
		m->oc.transform = h->state.transform;
		m->oc.has_pos = true, m->oc.x = h->state.x, m->oc.y = h->state.y;
		layout_add(m, true, h->state.x, h->state.y);
	}
	if (ok)
		wlr_output_configuration_v1_send_succeeded(cfg);
	else
		wlr_output_configuration_v1_send_failed(cfg);
	wlr_output_configuration_v1_destroy(cfg);
	if (ok && !test)
		output_update_all();
}

static void handle_mgr_apply(struct wl_listener *l, void *data)
{
	apply_output_config(data, false);
}

static void handle_mgr_test(struct wl_listener *l, void *data)
{
	apply_output_config(data, true);
}

static void handle_power_set_mode(struct wl_listener *l, void *data)
{
	struct wlr_output_power_v1_set_mode_event *ev = data;
	struct monitor *m = ev->output->data;
	bool on = ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON;
	struct wlr_output_state st;
	if (!m || monitor_off(m))
		return;
	wlr_output_state_init(&st);
	if (on)
		build_state(m, &st, !m->vrr_failed);
	wlr_output_state_set_enabled(&st, on);
	bool ok = wlr_output_commit_state(m->output, &st);
	wlr_output_state_finish(&st);
	if (!ok && on) {
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, true);
		ok = wlr_output_commit_state(m->output, &st);
		wlr_output_state_finish(&st);
	}
	if (ok)
		m->asleep = !on;
	if (on)
		layer_arrange(m);
	lock_arrange();
}

void output_finish(void)
{
	wl_list_remove(&new_output.link);
	wl_list_remove(&layout_change.link);
	wl_list_remove(&mgr_apply.link);
	wl_list_remove(&mgr_test.link);
	wl_list_remove(&power_set_mode.link);
	wl_list_remove(&workspace_commit.link);
	lease_finish();
	finishing = true;
	if (state_idle)
		wl_event_source_remove(state_idle);
	state_idle = NULL;
	wl_event_source_remove(lock_retry);
	lock_retry = NULL;
}

void output_init(void)
{
	wl_list_init(&srv.monitors);
	srv.output_layout = wlr_output_layout_create(srv.display);
	srv.scene_layout = wlr_scene_attach_output_layout(srv.scene, srv.output_layout);
	LISTEN(&srv.output_layout->events.change, &layout_change, handle_layout_change);
	LISTEN(&srv.backend->events.new_output, &new_output, handle_new_output);

	output_mgr = wlr_output_manager_v1_create(srv.display);
	struct wlr_output_power_manager_v1 *power = wlr_output_power_manager_v1_create(srv.display);
	struct wlr_gamma_control_manager_v1 *gamma = wlr_gamma_control_manager_v1_create(srv.display);
	workspaces = wlr_ext_workspace_manager_v1_create(srv.display, 1);
	wlr_scene_set_gamma_control_manager_v1(srv.scene, gamma);
	privileged_add(output_mgr->global);
	privileged_add(power->global);
	privileged_add(gamma->global);
	privileged_add(workspaces->global);
	LISTEN(&output_mgr->events.apply, &mgr_apply, handle_mgr_apply);
	LISTEN(&output_mgr->events.test, &mgr_test, handle_mgr_test);
	LISTEN(&power->events.set_mode, &power_set_mode, handle_power_set_mode);
	LISTEN(&workspaces->events.commit, &workspace_commit, handle_workspace_commit);
	lock_retry = wl_event_loop_add_timer(srv.loop, handle_lock_retry, NULL);
	lease_init();
}

struct client *monitor_fullscreen(struct monitor *m)
{
	struct client *c;
	if (!m || m->overview)
		return NULL;
	wl_list_for_each(c, &srv.fstack, flink)
		if (c->mon == m && c->fullscreen && client_visible(c))
			return c;
	return NULL;
}

static bool reachable(struct client *c, struct monitor *m, struct client *fs)
{
	return c->mon == m && client_visible(c) && (!fs || c->scene->node.parent == srv.layers[LYR_FS] ||
		c->scene->node.parent == srv.layers[LYR_ABOVE]);
}

bool client_reachable(struct client *c)
{
	return c->mon && reachable(c, c->mon, monitor_fullscreen(c->mon));
}

static bool tiled(struct client *c, struct monitor *m)
{
	return c->mon == m && client_visible(c) && !c->floating && !c->fullscreen && !c->scratch && !c->maximized &&
		!(c == srv.grab_c && srv.grab_tiled);
}

bool client_tiled(struct client *c)
{
	return c->mon && tiled(c, c->mon);
}

static int count_tiled(struct monitor *m)
{
	struct client *c;
	int n = 0;
	wl_list_for_each(c, &srv.clients, link)
		if (tiled(c, m))
			n++;
	return n;
}

struct gaps {
	int h, v, oh, ov;
};

static struct gaps gaps_for(struct monitor *m, int n)
{
	int d = srv.gaps_delta;
	if (srv.gaps_off || (srv.conf.smart_gaps && n == 1) || (srv.conf.monocle_no_gap && layout_is(m, "monocle")))
		return (struct gaps){0};
	return (struct gaps){MAX(0, srv.conf.gap_ih + d), MAX(0, srv.conf.gap_iv + d), MAX(0, srv.conf.gap_oh + d),
		MAX(0, srv.conf.gap_ov + d)};
}

static struct wlr_box usable(struct monitor *m, struct gaps g)
{
	return (struct wlr_box){m->w.x + g.oh, m->w.y + g.ov, m->w.width - 2 * g.oh, m->w.height - 2 * g.ov};
}

static void split_len(int start, int len, int gap, int i, int n, int *pos, int *size)
{
	int each = (len - gap * (n - 1)) / n;
	*pos = start + i * (each + gap);
	*size = i == n - 1 ? start + len - *pos : each;
}

static void master_stack(struct monitor *m, bool vertical, bool right, bool deck)
{
	struct client *c;
	int n = count_tiled(m), i = 0;
	if (!n)
		return;
	struct gaps g = gaps_for(m, n);
	struct wlr_box a = usable(m, g);
	int nmaster = MIN(m->nmaster, n), nstack = n - nmaster;
	int start = vertical ? a.y : a.x, len = vertical ? a.height : a.width, gap = vertical ? g.v : g.h;
	int ml = !nmaster ? 0 : !nstack ? len : (int)((len + gap) * m->mfact) - gap;
	int sl = !nstack ? 0 : !nmaster ? len : len - ml - gap;
	int mstart = right ? start + len - ml : start, sstart = right || !nmaster ? start : start + ml + gap;
	wl_list_for_each(c, &srv.clients, link) {
		if (!tiled(c, m))
			continue;
		bool master = i < nmaster;
		int k = master ? i : deck ? 0 : i - nmaster, cnt = master ? nmaster : deck ? 1 : nstack, pos, size;
		split_len(vertical ? a.x : a.y, vertical ? a.width : a.height, vertical ? g.h : g.v, k, cnt, &pos, &size);
		int s = master ? mstart : sstart, l = master ? ml : sl;
		client_resize(c, vertical ? (struct wlr_box){pos, s, size, l} : (struct wlr_box){s, pos, l, size});
		i++;
	}
}

static void tile(struct monitor *m)
{
	master_stack(m, false, false, false);
}

#ifdef WITH_RIGHT_TILE
static void right_tile(struct monitor *m)
{
	master_stack(m, false, true, false);
}
#endif

#ifdef WITH_VERTICAL_TILE
static void vertical_tile(struct monitor *m)
{
	master_stack(m, true, false, false);
}
#endif

#ifdef WITH_DECK
static void deck(struct monitor *m)
{
	master_stack(m, false, false, true);
}
#endif

#ifdef WITH_VERTICAL_DECK
static void vertical_deck(struct monitor *m)
{
	master_stack(m, true, false, true);
}
#endif

#ifdef WITH_CENTER_TILE
static void center_tile(struct monitor *m)
{
	struct client *c;
	int n = count_tiled(m), i = 0;
	int nmaster = MIN(m->nmaster, n), nstack = n - nmaster;
	if (!nmaster || nstack < 2) {
		master_stack(m, false, false, false);
		return;
	}
	struct gaps g = gaps_for(m, n);
	struct wlr_box a = usable(m, g);
	int mw = MAX(1, MIN((int)((a.width + g.h) * m->mfact) - g.h, a.width - 2 * g.h - 2));
	int side = MAX(1, (a.width - mw - 2 * g.h) / 2);
	int mx = a.x + side + g.h, rx = mx + mw + g.h, nright = (nstack + 1) / 2, nleft = nstack / 2;
	wl_list_for_each(c, &srv.clients, link) {
		if (!tiled(c, m))
			continue;
		int y, h, k = i - nmaster;
		if (i < nmaster) {
			split_len(a.y, a.height, g.v, i, nmaster, &y, &h);
			client_resize(c, (struct wlr_box){mx, y, mw, h});
		} else if (k % 2 == 0) {
			split_len(a.y, a.height, g.v, k / 2, nright, &y, &h);
			client_resize(c, (struct wlr_box){rx, y, a.x + a.width - rx, h});
		} else {
			split_len(a.y, a.height, g.v, k / 2, nleft, &y, &h);
			client_resize(c, (struct wlr_box){a.x, y, side, h});
		}
		i++;
	}
}
#endif

#ifdef WITH_DWINDLE
static void dwindle(struct monitor *m)
{
	struct client *c;
	int n = count_tiled(m), i = 0;
	if (!n)
		return;
	struct gaps g = gaps_for(m, n);
	struct wlr_box area = usable(m, g);

	wl_list_for_each(c, &srv.clients, link) {
		if (!tiled(c, m))
			continue;
		struct wlr_box b = area;
		c->split_side = c->split == 1 || (c->split == 0 && area.width >= area.height);
		if (i < n - 1) {
			float f = i == 0 ? m->mfact : 0.5f;
			if (c->split_side) {
				b.width = (int)((area.width - g.h) * f);
				area.x += b.width + g.h;
				area.width -= b.width + g.h;
			} else {
				b.height = (int)((area.height - g.v) * f);
				area.y += b.height + g.v;
				area.height -= b.height + g.v;
			}
		}
		client_resize(c, b);
		i++;
	}
}
#endif

void dwindle_toggle_split(void)
{
	struct client *c = client_focused();
	if (!c || !tiled(c, c->mon))
		return;
	bool side = layout_is(c->mon, "dwindle") ? c->split_side : c->split == 1 || (c->split == 0 && c->geom.width >= c->geom.height);
	c->split = side ? 2 : 1;
	arrange(c->mon);
}

#ifdef WITH_MONOCLE
static void monocle(struct monitor *m)
{
	struct client *c;
	struct wlr_box area = usable(m, gaps_for(m, 1));
	wl_list_for_each(c, &srv.clients, link)
		if (tiled(c, m))
			client_resize(c, area);
}
#endif

#if defined(WITH_GRID) || defined(WITH_VERTICAL_GRID)
static void grid_of(struct monitor *m, bool vertical)
{
	struct client *c;
	int n = count_tiled(m), i = 0, lines = 1;
	if (!n)
		return;
	while (lines * lines < n)
		lines++;
	int per = (n + lines - 1) / lines;
	struct gaps g = gaps_for(m, n);
	struct wlr_box a = usable(m, g);
	wl_list_for_each(c, &srv.clients, link) {
		if (!tiled(c, m))
			continue;
		int row = i / lines, in_row = row == per - 1 ? n - row * lines : lines, x, w, y, h;
		if (vertical) {
			split_len(a.x, a.width, g.h, row, per, &x, &w);
			split_len(a.y, a.height, g.v, i % lines, in_row, &y, &h);
		} else {
			split_len(a.y, a.height, g.v, row, per, &y, &h);
			split_len(a.x, a.width, g.h, i % lines, in_row, &x, &w);
		}
		client_resize(c, (struct wlr_box){x, y, w, h});
		i++;
	}
}
#endif

#ifdef WITH_GRID
static void grid(struct monitor *m)
{
	grid_of(m, false);
}
#endif

#ifdef WITH_VERTICAL_GRID
static void vertical_grid(struct monitor *m)
{
	grid_of(m, true);
}
#endif

static struct client *tiled_neighbor(struct client *c, int dir)
{
	struct wl_list *it = &c->link;
	while ((it = dir > 0 ? it->next : it->prev) != &srv.clients) {
		struct client *o = wl_container_of(it, o, link);
		if (tiled(o, c->mon))
			return o;
	}
	return NULL;
}

static struct client *column_last(struct client *head)
{
	struct client *last = head;
	for (struct client *o = tiled_neighbor(head, 1); o && o->scroll_stacked; o = tiled_neighbor(o, 1))
		last = o;
	return last;
}

static bool strip_peer(struct client *o, struct client *c, bool shown)
{
	if (shown)
		return tiled(o, c->mon);
	return o->mon == c->mon && !o->minimized && !o->floating && !o->fullscreen && !o->scratch && !o->maximized &&
		(o->tags & c->tags);
}

static struct client *strip_next(struct client *from, struct client *c, bool shown)
{
	for (struct wl_list *it = from->link.next; it != &srv.clients; it = it->next) {
		struct client *o = wl_container_of(it, o, link);
		if (strip_peer(o, c, shown))
			return o;
	}
	return NULL;
}

void client_join_strip(struct client *c)
{
	if (!c->mon || c->scroll_stacked)
		return;
	bool shown = client_visible(c) && !c->mon->overview;
	if (!strip_peer(c, c, shown))
		return;
	struct client *next = strip_next(c, c, shown), *last = next;
	if (!next || !next->scroll_stacked)
		return;
	for (struct client *o = strip_next(next, c, shown); o && o->scroll_stacked; o = strip_next(o, c, shown))
		last = o;
	wl_list_remove(&c->link);
	wl_list_insert(&last->link, &c->link);
}

#if defined(WITH_SCROLL) || defined(WITH_VERTICAL_SCROLL)
static int column_size(struct client *head)
{
	int n = 1;
	for (struct client *o = tiled_neighbor(head, 1); o && o->scroll_stacked; o = tiled_neighbor(o, 1))
		n++;
	return n;
}

static int strip_size(struct client *c, int len, int gap, int n)
{
	float share = c->scroll_width > 0 ? c->scroll_width :
		n == 1 && srv.conf.scroll_width_single > 0 ? srv.conf.scroll_width_single : srv.conf.scroll_width;
	int s = (int)((len + gap) * share) - gap;
	return MAX(1, MIN(s, len - 2 * srv.conf.scroll_struts));
}

static void scroll_strip(struct monitor *m, bool vertical)
{
	struct client *c, *f = client_last_focused(), *head = NULL;
	int n = count_tiled(m), pos = 0, fpos = -1, fsize = 0, size = 0, k = 0, cnt = 1;
	if (!n)
		return;
	struct gaps g = gaps_for(m, n);
	struct wlr_box a = usable(m, g);
	int len = vertical ? a.height : a.width, gap = vertical ? g.v : g.h, st = srv.conf.scroll_struts, ncols = 0;
	wl_list_for_each(c, &srv.clients, link)
		if (tiled(c, m) && (vertical || !c->scroll_stacked || !ncols))
			ncols++;
	wl_list_for_each(c, &srv.clients, link) {
		if (!tiled(c, m))
			continue;
		if (vertical || !c->scroll_stacked || !head) {
			if (head)
				pos += size + gap;
			head = c;
			size = strip_size(c, len, gap, ncols);
		}
		if (c == f)
			fpos = pos, fsize = size;
	}
	int total = pos + size;
	if (fpos >= 0) {
		bool off = fpos < m->scroll + st || fpos + fsize > m->scroll + len - st;
		if (srv.conf.scroll_center == SCROLL_ALWAYS || (srv.conf.scroll_center == SCROLL_OUTSIDE && off))
			m->scroll = fpos + fsize / 2 - len / 2;
		else if (srv.conf.scroll_center == SCROLL_EDGE)
			m->scroll = MIN(fpos - st, MAX(m->scroll, fpos + fsize - len + st));
	}
	if (srv.conf.scroll_center == SCROLL_EDGE)
		m->scroll = MAX(-st, MIN(m->scroll, total - len + st));
	pos = -m->scroll;
	head = NULL;
	wl_list_for_each(c, &srv.clients, link) {
		if (!tiled(c, m))
			continue;
		if (vertical || !c->scroll_stacked || !head) {
			if (head)
				pos += size + gap;
			head = c;
			size = strip_size(c, len, gap, ncols);
			k = 0;
			cnt = vertical ? 1 : column_size(c);
		}
		int across, alen;
		split_len(vertical ? a.x : a.y, vertical ? a.width : a.height, vertical ? g.h : g.v, k++, cnt, &across, &alen);
		struct wlr_box box = vertical ? (struct wlr_box){across, a.y + pos, alen, size} :
			(struct wlr_box){a.x + pos, across, size, alen}, shown;
		bool visible = wlr_box_intersection(&shown, &box, &a);
		c->crop = visible && !wlr_box_equal(&shown, &box) ?
			(struct wlr_box){shown.x - box.x, shown.y - box.y, shown.width, shown.height} : (struct wlr_box){0};
		client_resize(c, box);
		wlr_scene_node_set_enabled(&c->scene->node, visible);
	}
}
#endif

#ifdef WITH_SCROLL
static void scroll(struct monitor *m)
{
	scroll_strip(m, false);
}
#endif

#ifdef WITH_VERTICAL_SCROLL
static void vertical_scroll(struct monitor *m)
{
	scroll_strip(m, true);
}
#endif

static void swap_flags(struct client *a, struct client *b)
{
	bool s = a->scroll_stacked;
	if (s != b->scroll_stacked && !strip_vertical(a->mon)) {
		float w = a->scroll_width;
		a->scroll_width = b->scroll_width;
		b->scroll_width = w;
	}
	a->scroll_stacked = b->scroll_stacked;
	b->scroll_stacked = s;
}

static void swap_slots(struct client *a, struct client *b)
{
	swap_flags(a, b);
	list_swap(&a->link, &b->link);
}

void scroll_width(struct client *c, const char *arg)
{
	if (c->mon && tiled(c, c->mon) && !strip_vertical(c->mon))
		while (c->scroll_stacked && tiled_neighbor(c, -1))
			c = tiled_neighbor(c, -1);
	const float *presets = srv.conf.scroll_presets;
	int np = srv.conf.nscroll_presets, i = 0;
	float cur = c->scroll_width > 0 ? c->scroll_width :
		srv.conf.scroll_width_single > 0 && strip_columns(c->mon) == 1 ? srv.conf.scroll_width_single : srv.conf.scroll_width;
	if (arg && strcmp(arg, "next") && strcmp(arg, "prev")) {
		double w = strtod(arg, NULL);
		c->scroll_width = (float)(w >= 0.1 ? (w <= 1.0 ? w : 1.0) : 0.1);
		return;
	}
	while (i < np && fabsf(presets[i] - cur) > 0.01f)
		i++;
	if (i == np)
		i = arg && !strcmp(arg, "prev") ? 0 : np - 1;
	c->scroll_width = presets[(arg && !strcmp(arg, "prev") ? i + np - 1 : i + 1) % np];
}

void scroll_stack(int dx, int dy)
{
	struct client *sel = client_focused(), *prev, *next;
	if (!sel || !tiled(sel, sel->mon))
		return;
	prev = tiled_neighbor(sel, -1);
	next = tiled_neighbor(sel, 1);
	bool in_column = (sel->scroll_stacked && prev) || (next && next->scroll_stacked);
	if (dy) {
		struct client *o = dy < 0 ? (sel->scroll_stacked ? prev : NULL) : (next && next->scroll_stacked ? next : NULL);
		if (!o)
			return;
		swap_flags(sel, o);
		wl_list_remove(&sel->link);
		wl_list_insert(dy < 0 ? o->link.prev : &o->link, &sel->link);
	} else if (in_column) {
		struct client *head = sel;
		while (head->scroll_stacked && tiled_neighbor(head, -1))
			head = tiled_neighbor(head, -1);
		struct client *last = column_last(head);
		if (head == sel && next) {
			next->scroll_stacked = false;
			if (!strip_vertical(sel->mon))
				next->scroll_width = sel->scroll_width;
		}
		sel->scroll_stacked = false;
		wl_list_remove(&sel->link);
		if (dx < 0)
			wl_list_insert(head == sel ? next->link.prev : head->link.prev, &sel->link);
		else
			wl_list_insert(last == sel ? &prev->link : &last->link, &sel->link);
	} else if (dx < 0 && prev) {
		wl_list_remove(&sel->link);
		wl_list_insert(&prev->link, &sel->link);
		sel->scroll_stacked = true;
	} else if (dx > 0 && next) {
		struct client *last = column_last(next);
		wl_list_remove(&sel->link);
		wl_list_insert(&last->link, &sel->link);
		sel->scroll_stacked = true;
	}
	arrange(sel->mon);
}

#ifdef WITH_FLOAT
static void floating(struct monitor *m)
{
	struct client *c;
	wl_list_for_each(c, &srv.clients, link)
		if (tiled(c, m))
			client_resize(c, c->geom);
}
#endif

static const struct {
	const char *name;
	void (*arrange)(struct monitor *m);
} layouts[] = {
	{"tile", tile},
#ifdef WITH_DWINDLE
	{"dwindle", dwindle},
#endif
#ifdef WITH_MONOCLE
	{"monocle", monocle},
#endif
#ifdef WITH_GRID
	{"grid", grid},
#endif
#ifdef WITH_SCROLL
	{"scroll", scroll},
#endif
#ifdef WITH_FLOAT
	{"float", floating},
#endif
#ifdef WITH_DECK
	{"deck", deck},
#endif
#ifdef WITH_CENTER_TILE
	{"center_tile", center_tile},
#endif
#ifdef WITH_RIGHT_TILE
	{"right_tile", right_tile},
#endif
#ifdef WITH_VERTICAL_TILE
	{"vertical_tile", vertical_tile},
#endif
#ifdef WITH_VERTICAL_DECK
	{"vertical_deck", vertical_deck},
#endif
#ifdef WITH_VERTICAL_GRID
	{"vertical_grid", vertical_grid},
#endif
#ifdef WITH_VERTICAL_SCROLL
	{"vertical_scroll", vertical_scroll},
#endif
};

bool strip_vertical(struct monitor *m)
{
#ifdef WITH_VERTICAL_SCROLL
	return m && layouts[m->layout].arrange == vertical_scroll;
#else
	return false;
#endif
}

static int strip_columns(struct monitor *m)
{
	struct client *c;
	bool vertical = strip_vertical(m);
	int n = 0;
	wl_list_for_each(c, &srv.clients, link)
		if (tiled(c, m) && (vertical || !c->scroll_stacked || !n))
			n++;
	return n;
}

static bool layout_is(struct monitor *m, const char *name)
{
	return m && !strcmp(layouts[m->layout].name, name);
}

bool layout_floats(struct monitor *m)
{
#ifdef WITH_FLOAT
	return m && layouts[m->layout].arrange == floating;
#else
	return false;
#endif
}

int layout_count(void)
{
	return LENGTH(layouts);
}

const char *layout_name(int l)
{
	return layouts[l].name;
}

int layout_from_name(const char *name)
{
	for (size_t i = 0; i < LENGTH(layouts); i++)
		if (!strcmp(name, layouts[i].name))
			return (int)i;
	return -1;
}

void arrange(struct monitor *m)
{
	struct client *c;
	if (!m)
		return;
	struct client *fs = monitor_fullscreen(m);
	if (fs != m->fullscreen)
		wlr_log(WLR_INFO, "%s: fullscreen %s %s", m->output->name, fs ? "on" : "off", fs ? client_app_id(fs) : "");
	m->fullscreen = fs;
	bool any = false;
	struct monitor *o;
	wl_list_for_each(o, &srv.monitors, link)
		any = any || o->fullscreen;
	if (any != any_fullscreen) {
		any_fullscreen = any;
		ipc_event("fullscreen %d", any);
	}
	bool on = m->output->enabled;
	int ntiled = count_tiled(m);
	wl_list_for_each(c, &srv.clients, link) {
		if (c->mon != m)
			continue;
		client_set_bounds(c, m);
		bool vis = client_visible(c), single = ntiled == 1 && tiled(c, m);
		wlr_scene_node_set_enabled(&c->scene->node, vis);
		if (single != c->single) {
			c->single = single;
			if (!tiled(c, m) && c->mapped)
				client_resize(c, c->geom);
		}
		if (!wlr_box_empty(&c->crop)) {
			c->crop = (struct wlr_box){0};
			client_resize(c, c->geom);
		}
		if (!on)
			continue;
#ifdef XWAYLAND
		if (vis && c->type == CLIENT_X11 && c->s.xwl->minimized)
			wlr_xwayland_surface_set_minimized(c->s.xwl, false);
#endif
		if (m->overview && !c->scratch)
			continue;
		if (vis && c->fullscreen)
			client_resize(c, m->m);
		else if (vis && c->maximized)
			client_resize(c, usable(m, gaps_for(m, 2)));
	}
	if (!on)
		return;

	struct layer_surface *ls;
	wl_list_for_each(ls, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], link) {
		bool shown = !fs || ls->ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
		wlr_scene_node_set_enabled(&ls->scene->node, ls->mapped && shown);
		wlr_scene_node_set_enabled(&ls->popups->node, shown);
	}

	if (m->overview)
		overview_arrange(m);
	else
		layouts[m->layout].arrange(m);
	input_motion(0, true);
	state_changed();
}

static void state_flush(void *data)
{
	state_idle = NULL;
	output_update_workspaces();
	input_update_idle(NULL);
	ipc_notify_state();
}

void state_changed(void)
{
	if (!state_idle && !finishing)
		state_idle = wl_event_loop_add_idle(srv.loop, state_flush, NULL);
}

void arrange_all(void)
{
	struct monitor *m;
	wl_list_for_each(m, &srv.monitors, link)
		arrange(m);
}

void focus_top(struct monitor *m)
{
	struct client *c, *fs = monitor_fullscreen(m);
	wl_list_for_each(c, &srv.fstack, flink)
		if (reachable(c, m, fs)) {
			focus_client(c, true);
			return;
		}
	focus_client(NULL, true);
}

void focus_client(struct client *c, bool lift)
{
	struct client *old = client_last_focused();

	if (srv.exclusive_focus || srv.locked)
		return;
	if (c && c->type == CLIENT_X11_UNMANAGED)
		return;
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);
	if (c && c == old && client_surface(c) == srv.seat->keyboard_state.focused_surface)
		return;

	if (c) {
		wl_list_remove(&c->flink);
		wl_list_insert(&srv.fstack, &c->flink);
		srv.selmon = c->mon ? c->mon : srv.selmon;
		c->urgent = false;
	}
	if (old && old != c)
		client_set_active(old, false);

	if (!c) {
		if (!focus_cleared)
			ipc_event("focus \t");
		focus_cleared = true;
		if (!layer_from_surface(srv.seat->keyboard_state.focused_surface))
			input_set_focus_surface(NULL);
		state_changed();
		return;
	}
	client_set_active(c, true);
	input_set_focus_surface(client_surface(c));
	focus_cleared = false;
	ipc_client_event("focus", c);
#ifdef WITH_SCROLL
	if (lift && c->mon && layouts[c->mon->layout].arrange == scroll)
		arrange(c->mon);
#endif
#ifdef WITH_VERTICAL_SCROLL
	if (lift && c->mon && layouts[c->mon->layout].arrange == vertical_scroll)
		arrange(c->mon);
#endif
	state_changed();
}

static struct client *client_in_dir(struct client *sel, int dx, int dy, bool tiled_only)
{
	struct client *c, *best = NULL, *fs = monitor_fullscreen(sel->mon);
	long bestd = 0;
	int sx = sel->geom.x + sel->geom.width / 2, sy = sel->geom.y + sel->geom.height / 2;
	wl_list_for_each(c, &srv.clients, link) {
		if (c == sel || !reachable(c, sel->mon, fs) || (tiled_only && !tiled(c, sel->mon)))
			continue;
		long x = c->geom.x + c->geom.width / 2 - sx, y = c->geom.y + c->geom.height / 2 - sy;
		long along = dx ? x * dx : y * dy, across = labs(dx ? y : x);
		if (along <= 0)
			continue;
		if (srv.conf.focus_overlap_only && (dx ? c->geom.y >= sel->geom.y + sel->geom.height ||
				c->geom.y + c->geom.height <= sel->geom.y : c->geom.x >= sel->geom.x + sel->geom.width ||
				c->geom.x + c->geom.width <= sel->geom.x))
			continue;
		long d = along + 2 * across;
		if (!best || d < bestd)
			best = c, bestd = d;
	}
	return best;
}

void focus_dir(int dx, int dy)
{
	struct client *sel = client_focused(), *best;
	if (!sel || sel->fullscreen)
		return;
	if ((best = client_in_dir(sel, dx, dy, false))) {
		focus_client(best, true);
		return;
	}
	if (srv.conf.focus_cross_monitor && sel->mon) {
		enum wlr_direction d = dx < 0 ? WLR_DIRECTION_LEFT : dx > 0 ? WLR_DIRECTION_RIGHT :
			dy < 0 ? WLR_DIRECTION_UP : WLR_DIRECTION_DOWN;
		struct wlr_output *o = wlr_output_layout_adjacent_output(srv.output_layout, d, sel->mon->output,
			sel->geom.x + sel->geom.width / 2.0, sel->geom.y + sel->geom.height / 2.0);
		if (o && o->data) {
			srv.selmon = o->data;
			focus_top(o->data);
			return;
		}
	}
	if (srv.conf.focus_cross_tag && dx && sel->mon) {
		srv.selmon = sel->mon;
		view(tags_shift(dx, true));
	}
}

static void list_swap(struct wl_list *a, struct wl_list *b)
{
	struct wl_list *ap = a->prev, *bp = b->prev;
	if (bp == a) {
		wl_list_remove(b);
		wl_list_insert(ap, b);
	} else if (ap == b) {
		wl_list_remove(a);
		wl_list_insert(bp, a);
	} else {
		wl_list_remove(a);
		wl_list_insert(bp, a);
		wl_list_remove(b);
		wl_list_insert(ap, b);
	}
}

void swap_dir(int dx, int dy)
{
	struct client *sel = client_focused(), *other;
	if (!sel || !tiled(sel, sel->mon) || !(other = client_in_dir(sel, dx, dy, true)))
		return;
	swap_slots(sel, other);
	arrange(sel->mon);
}

void client_drop_on(struct client *c, struct client *target)
{
	struct monitor *old = c->mon;
	if (!target || target == c || !target->mon) {
		arrange(old);
		return;
	}
	if (target->mon != c->mon)
		client_send_to_monitor(c, target->mon, target->mon->tags);
	swap_slots(c, target);
	arrange(target->mon);
	if (old != target->mon)
		arrange(old);
}

void focus_stack(int dir)
{
	struct client *sel = client_focused(), *c;
	if (!sel || sel->fullscreen)
		return;
	struct client *fs = monitor_fullscreen(sel->mon);
	struct wl_list *start = &sel->link, *it = start;
	do {
		it = dir > 0 ? it->next : it->prev;
		if (it == &srv.clients)
			continue;
		c = wl_container_of(it, c, link);
		if (reachable(c, sel->mon, fs)) {
			focus_client(c, true);
			return;
		}
	} while (it != start);
}

uint32_t tag_mask(void)
{
	return (1u << srv.conf.tag_count) - 1;
}

static int tag_slot(uint32_t tags)
{
	return tags && !(tags & (tags - 1)) ? __builtin_ctz(tags) : TAGCOUNT;
}

static int switch_slot(struct monitor *m, uint32_t old)
{
	int o = tag_slot(old), n = tag_slot(m->tags);
	m->tag_layout[o] = m->layout, m->tag_mfact[o] = m->mfact, m->tag_nmaster[o] = m->nmaster;
	m->layout = m->tag_layout[n], m->mfact = m->tag_mfact[n], m->nmaster = m->tag_nmaster[n];
	if (m->layout >= (int)LENGTH(layouts))
		m->layout = 0;
	return o;
}

static void tags_switched(struct monitor *m, uint32_t old)
{
	int o = switch_slot(m, old);
	struct client *c;
	wl_list_for_each(c, &srv.clients, link)
		if (c->mon == m && c->fullscreen)
			client_stack_children(c);
	effects_tag_leave(m, old, m->tags);
	focus_top(m);
	arrange(m);
	effects_show(m, old);
	if (m->tag_layout[o] != m->layout)
		ipc_event("layout %s %s", m->output->name, layouts[m->layout].name);
}

void view(uint32_t tags)
{
	struct monitor *m = srv.selmon;
	if (!m)
		return;
	uint32_t old = m->tags;
	if (tags && !(tags & tag_mask()))
		return;
	tags &= tag_mask();
	if (!tags) {
		if (!m->prevtags || m->prevtags == m->tags)
			return;
		uint32_t t = m->tags;
		m->tags = m->prevtags;
		m->prevtags = t;
	} else {
		if (tags == m->tags)
			return;
		m->prevtags = m->tags;
		m->tags = tags;
	}
	tags_switched(m, old);
}

void toggle_view(uint32_t tags)
{
	struct monitor *m = srv.selmon;
	tags &= tag_mask();
	if (!m || !tags || !(m->tags ^ tags))
		return;
	uint32_t old = m->tags;
	m->tags ^= tags;
	tags_switched(m, old);
}

uint32_t tags_shift(int dir, bool occupied)
{
	uint32_t t = srv.selmon ? srv.selmon->tags : 1, occ = 0;
	int i = __builtin_ctz(t ? t : 1), count = srv.conf.tag_count;
	struct client *c;
	if (occupied && srv.selmon)
		wl_list_for_each(c, &srv.clients, link)
			if (c->mon == srv.selmon && c->mapped && !c->scratch)
				occ |= c->tags;
	for (int step = 0; step < count; step++) {
		i = (i + dir + count) % count;
		if (!occupied || (occ & 1u << i))
			return 1u << i;
	}
	return t;
}

void tag_step(struct client *c, int dir, bool follow)
{
	if (!c || !c->mon)
		return;
	int count = srv.conf.tag_count, i = __builtin_ctz(c->tags & tag_mask() ? c->tags & tag_mask() : 1);
	uint32_t t = 1u << ((i + dir + count) % count);
	tag(c, t);
	if (!follow)
		return;
	srv.selmon = c->mon;
	view(t);
	if (client_visible(c))
		focus_client(c, true);
}

static void scratch_leave(struct client *c)
{
	c->scratch = false;
	if (c->scratch_floated && !c->fullscreen) {
		client_set_floating(c, false);
	} else {
		if (c->scratch_floated)
			c->floating = false;
		client_restack(c);
	}
	c->scratch_floated = false;
	client_update_border(c);
}

void tag(struct client *c, uint32_t tags)
{
	tags &= tag_mask();
	if (!c || !tags)
		return;
	bool was_focused = c == client_focused();
	if (c->scratch)
		scratch_leave(c);
	bool moved = c->tags != tags;
	if (moved)
		client_leave_column(c);
	c->tags = tags;
	if (moved)
		client_join_strip(c);
	if (moved && c->fullscreen)
		client_stack_children(c);
	if (was_focused)
		focus_top(c->mon);
	arrange(c->mon);
}

void zoom(void)
{
	struct client *sel = client_focused(), *c;
	if (!sel || !tiled(sel, sel->mon))
		return;
	struct client *first = NULL;
	wl_list_for_each(c, &srv.clients, link)
		if (tiled(c, sel->mon)) {
			first = c;
			break;
		}
	if (first == sel) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &srv.clients)
				break;
			if (tiled(c, sel->mon)) {
				sel = c;
				break;
			}
		}
		if (sel == first)
			return;
	}
	struct client *below = tiled_neighbor(sel, 1);
	if (below && below->scroll_stacked && !sel->scroll_stacked) {
		below->scroll_stacked = false;
		if (!strip_vertical(sel->mon))
			below->scroll_width = sel->scroll_width;
	}
	sel->scroll_stacked = false;
	wl_list_remove(&sel->link);
	wl_list_insert(&srv.clients, &sel->link);
	focus_client(sel, true);
	arrange(sel->mon);
}

void scratch_fit(struct client *c)
{
	struct monitor *m = c->mon;
	if (!m || !c->scratch || c->fullscreen || (srv.conf.scratchpad_width <= 0 && srv.conf.scratchpad_height <= 0))
		return;
	struct wlr_box b = c->geom;
	if (srv.conf.scratchpad_width > 0)
		b.width = (int)(m->w.width * srv.conf.scratchpad_width);
	if (srv.conf.scratchpad_height > 0)
		b.height = (int)(m->w.height * srv.conf.scratchpad_height);
	b.x = m->w.x + (m->w.width - b.width) / 2;
	b.y = m->w.y + (m->w.height - b.height) / 2;
	client_resize(c, b);
}

void scratch_show(struct monitor *m, bool show)
{
	struct monitor *o;
	m->scratch_shown = show;
	wl_list_for_each(o, &srv.monitors, link)
		if (o != m && o->scratch_shown) {
			o->scratch_shown = false;
			arrange(o);
		}
}

#ifdef WITH_SCRATCHPAD
static bool scratch_any(struct monitor *m, struct client *skip)
{
	struct client *c;
	wl_list_for_each(c, &srv.clients, link)
		if (c != skip && c->scratch && !c->minimized && (!m || c->mon == m))
			return true;
	return false;
}

void scratch_set(struct client *c, bool on)
{
	if (!c || c->scratch == on)
		return;
	bool was_focused = c == client_focused();
	if (on) {
		if (c->mon && c->mon->scratch_shown && !scratch_any(c->mon, c))
			c->mon->scratch_shown = false;
		client_leave_column(c);
		c->scratch = true;
		if (c->fullscreen)
			client_set_fullscreen(c, false);
		c->scratch_floated = !c->floating;
		if (c->maximized)
			client_set_maximized(c, false);
		if (!c->floating)
			client_set_floating(c, true);
		client_restack(c);
		scratch_fit(c);
		client_update_border(c);
	} else {
		if (c->mon && !(c->tags & c->mon->tags))
			c->tags = c->mon->tags;
		scratch_leave(c);
	}
	if (was_focused && !client_reachable(c))
		focus_top(c->mon);
	arrange(c->mon);
}

void scratch_toggle(void)
{
	struct monitor *m = srv.selmon;
	struct client *c, *top = NULL;
	if (!m)
		return;
	bool show = !m->scratch_shown;
	if (show && !scratch_any(NULL, NULL))
		return;
	scratch_show(m, show);
	wl_list_for_each(c, &srv.fstack, flink)
		if (c->scratch && c->mapped && !c->minimized && show) {
			if (c->mon != m)
				client_send_to_monitor(c, m, c->tags);
			scratch_fit(c);
			if (!top)
				top = c;
		}
	arrange_all();
	if (top)
		focus_client(top, true);
	else
		focus_top(m);
}
#endif
