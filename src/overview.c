#include "swizzle.h"

void overview_set(struct monitor *m, bool on)
{
	if (!m || m->overview == on)
		return;
	struct client *f = client_focused();
	m->overview = on;
	if (!on && f && f->mon == m && !f->sticky && !f->scratch && !(f->tags & m->tags)) {
		struct monitor *sel = srv.selmon;
		srv.selmon = m;
		view(f->tags);
		srv.selmon = sel;
	}
	struct client *c;
	if (!on)
		wl_list_for_each(c, &srv.clients, link) {
			if (!c->in_overview || (c->mon && c->mon->overview))
				continue;
			c->in_overview = false;
			if (c->floating && !c->fullscreen && !c->maximized)
				client_resize(c, client_fit(c->overview_geom, c->mon));
		}
	arrange(m);
	if (f && !client_reachable(f))
		focus_top(m);
	ipc_event("overview %s %d", m->output->name, on);
}

void overview_arrange(struct monitor *m)
{
	struct client *c;
	int n = 0, i = 0, cols = 1;
	wl_list_for_each(c, &srv.clients, link)
		if (c->mon == m && client_visible(c) && !c->scratch)
			n++;
	if (!n)
		return;
	while (cols * cols < n)
		cols++;
	int rows = (n + cols - 1) / cols, gi = srv.conf.overview_gap_inner, go = srv.conf.overview_gap_outer;
	struct wlr_box a = {m->w.x + go, m->w.y + go, m->w.width - 2 * go, m->w.height - 2 * go};
	int h = (a.height - gi * (rows - 1)) / rows;
	wl_list_for_each(c, &srv.clients, link) {
		if (c->mon != m || !client_visible(c) || c->scratch)
			continue;
		int row = i / cols, in_row = row == rows - 1 ? n - row * cols : cols;
		int w = (a.width - gi * (in_row - 1)) / in_row;
		if (!c->in_overview)
			c->overview_geom = c->geom, c->in_overview = true;
		client_resize(c, (struct wlr_box){a.x + (i % cols) * (w + gi), a.y + row * (h + gi), w, h});
		i++;
	}
}

void overview_hotarea(double x, double y)
{
	struct monitor *m = output_at(x, y);
	int s = srv.conf.hotarea_size;
	if (!m || s <= 0 || srv.locked)
		return;
	bool right = srv.conf.hotarea_corner == CORNER_TOP_RIGHT || srv.conf.hotarea_corner == CORNER_BOTTOM_RIGHT;
	bool bottom = srv.conf.hotarea_corner == CORNER_BOTTOM_LEFT || srv.conf.hotarea_corner == CORNER_BOTTOM_RIGHT;
	double cx = right ? m->m.x + m->m.width - x : x - m->m.x, cy = bottom ? m->m.y + m->m.height - y : y - m->m.y;
	bool in = cx >= 0 && cx < s && cy >= 0 && cy < s;
	if (in && !m->in_hotarea && (srv.conf.hotarea_fullscreen || !m->fullscreen))
		overview_set(m, !m->overview);
	m->in_hotarea = in;
}
