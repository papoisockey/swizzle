#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include "swizzle.h"

enum { TL, TR, BL, BR, TOP, BOTTOM, LEFT, RIGHT, NPARTS };

struct snapshot;

struct effects {
	struct client *c;
	struct layer_surface *ls;
	struct wl_list link;
	bool placed;
	struct wlr_scene_tree *shadow;
	struct wlr_scene_buffer *part[NPARTS];
	struct wlr_scene_buffer *fill;
	struct wlr_scene_buffer *dim;
	struct wlr_scene_buffer *surface;
#ifdef WITH_SCENEFX
	struct wlr_scene_shadow *fxshadow;
	struct wlr_scene_blur *blur;
	bool rounded;
#endif
	bool moving, fading, focusing, zoom_pending;
	int from_x, from_y, to_x, to_y, move_dur, fade_dur;
	enum curve_kind move_curve, fade_curve;
	uint32_t move_start, fade_start, focus_start, zoom_wait;
	float fade_from, fade;
	float alpha, alpha_from, alpha_to;
	struct snapshot *intro;
};

struct snode {
	struct wlr_scene_node *node;
	int x, y, w, h;
	float color[4];
};

struct snapshot {
	struct wl_list link;
	struct wlr_scene_tree *tree;
	uint32_t start;
	int dur;
	enum curve_kind curve, alpha_curve;
	float alpha_from, alpha_to, scale_from, scale_to;
	int dx, dy, cx, cy;
	struct effects *owner;
	struct snode *nodes;
	int nnodes, cap;
};

struct pixels {
	struct wlr_buffer base;
	uint32_t *data;
};

static struct wl_list active, snapshots;
static struct wlr_buffer *parts[NPARTS], *fill_buf, *dim_buf[2];
static int built_size = -1, built_blur = -1;
static bool built_full;
static float built_color[4];

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int dur(enum duration_kind k)
{
	int d = srv.conf.durations[k];
	return d >= 0 ? d : k == DUR_FOCUS ? 0 : srv.conf.animation_duration;
}

static float progress(uint32_t now, uint32_t start, int d)
{
	float t = d > 0 ? (float)(uint32_t)(now - start) / (float)d : 1.0f;
	return t < 1.0f ? t : 1.0f;
}

static float ease(enum curve_kind k, float t)
{
	return curve_ease(&srv.conf.curves[k], t);
}

static struct wlr_scene_tree *tree_of(struct effects *fx)
{
	return fx->c ? fx->c->scene : fx->ls->scene;
}

static void pixels_destroy(struct wlr_buffer *b)
{
	struct pixels *p = wl_container_of(b, p, base);
	free(p->data);
	free(p);
}

static bool pixels_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
	struct pixels *p = wl_container_of(b, p, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = p->data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)b->width * 4;
	return true;
}

static void pixels_end(struct wlr_buffer *b)
{
}

static const struct wlr_buffer_impl pixels_impl = {
	.destroy = pixels_destroy,
	.begin_data_ptr_access = pixels_begin,
	.end_data_ptr_access = pixels_end,
};

static struct pixels *pixels_new(int w, int h)
{
	struct pixels *p = calloc(1, sizeof(*p));
	if (!p || !(p->data = calloc((size_t)w * (size_t)h, 4))) {
		free(p);
		return NULL;
	}
	wlr_buffer_init(&p->base, &pixels_impl, w, h);
	return p;
}

static uint32_t pack(const float col[4], double a)
{
	uint32_t px = 0;
	for (int i = 0; i < 4; i++)
		px |= (uint32_t)lround(col[i] * a * 255.0) << (i == 3 ? 24 : 16 - 8 * i);
	return px;
}

static struct wlr_buffer *solid(const float col[4])
{
	struct pixels *p = col[3] > 0 ? pixels_new(1, 1) : NULL;
	if (!p)
		return NULL;
	p->data[0] = pack(col, 1);
	return &p->base;
}

static double falloff(double d, double sigma)
{
	return 0.5 * erfc(d / (sigma * 1.41421356));
}

static struct wlr_buffer *make_part(int part, int r, double sigma, bool full)
{
	int thick = full ? 2 * r : r;
	int w = part < TOP ? 2 * r : part < LEFT ? 1 : thick, h = part < TOP ? 2 * r : part < LEFT ? thick : 1;
	struct pixels *p = pixels_new(w, h);
	if (!p)
		return NULL;
	int sub = full ? r : 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double dx = part == TR || part == BR ? x + 0.5 - r : part == RIGHT ? x + 0.5 - sub : r - (x + 0.5);
			double dy = part == BL || part == BR ? y + 0.5 - r : part == BOTTOM ? y + 0.5 - sub : r - (y + 0.5);
			double a;
			if (part < TOP)
				a = !full && dx < 0 && dy < 0 ? 0 : falloff(dx, sigma) * falloff(dy, sigma);
			else
				a = falloff(part < LEFT ? dy : dx, sigma);
			p->data[y * w + x] = pack(srv.conf.shadow_color, a);
		}
	return &p->base;
}

static bool no_input(struct wlr_scene_buffer *b, double *sx, double *sy)
{
	return false;
}

static struct wlr_scene_buffer *drawn_buffer(struct wlr_scene_tree *parent, struct wlr_buffer *b)
{
	struct wlr_scene_buffer *sb = wlr_scene_buffer_create(parent, b);
	if (sb)
		sb->point_accepts_input = no_input;
	return sb;
}

static void find_surface(struct wlr_scene_buffer *b, int sx, int sy, void *data)
{
	struct effects *fx = data;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(b);
	struct wlr_surface *main = fx->c ? client_surface(fx->c) : fx->ls->ls->surface;
	if (ss && ss->surface == main)
		fx->surface = b;
}

static bool shadow_wanted(struct effects *fx)
{
	if (srv.conf.shadow_size <= 0)
		return false;
	if (fx->ls) {
		struct layer_rule r;
		config_layer(fx->ls->ls->namespace, &r);
		return r.shadow;
	}
	struct client *c = fx->c;
	return !c->fullscreen && c->type != CLIENT_X11_UNMANAGED && (!srv.conf.shadow_floating_only || c->floating ||
		c->scratch || c->maximized);
}

#ifdef WITH_SCENEFX
static int radius(struct client *c)
{
	int r = c->fullscreen || c->type == CLIENT_X11_UNMANAGED || (c->single && srv.conf.no_radius_when_single) ? 0 :
		srv.conf.corner_radius;
	return MIN(r, MIN(c->geom.width, c->geom.height) / 2);
}

struct corner_box {
	int x0, y0, x1, y1, r;
};

static void round_buffer(struct wlr_scene_buffer *b, int sx, int sy, void *data)
{
	const struct corner_box *k = data;
	int x1 = sx + b->dst_width, y1 = sy + b->dst_height;
	bool l = abs(sx - k->x0) <= 1, t = abs(sy - k->y0) <= 1, r = abs(x1 - k->x1) <= 1, bo = abs(y1 - k->y1) <= 1;
	wlr_scene_buffer_set_corner_radii(b, corner_radii_new(t && l ? k->r : 0, t && r ? k->r : 0, bo && r ? k->r : 0,
		bo && l ? k->r : 0));
}

static void round_ring(struct wlr_scene_rect **ring, int off, int width, int rad, int w, int h)
{
	if (width <= 0)
		return;
	wlr_scene_rect_set_size(ring[0], w - 2 * off, h - 2 * off);
	wlr_scene_node_set_position(&ring[0]->node, off, off);
	wlr_scene_rect_set_corner_radius(ring[0], MAX(0, rad - off));
	wlr_scene_rect_set_clipped_region(ring[0], (struct clipped_region){
		.area = {width, width, w - 2 * off - 2 * width, h - 2 * off - 2 * width},
		.corners = corner_radii_all(MAX(0, rad - off - width)),
	});
	for (int i = 1; i < 4; i++)
		wlr_scene_node_set_enabled(&ring[i]->node, false);
}

static bool blur_wanted(struct effects *fx)
{
	if (srv.conf.blur_passes <= 0)
		return false;
	if (fx->ls) {
		struct layer_rule r;
		config_layer(fx->ls->ls->namespace, &r);
		return r.blur;
	}
	return !fx->c->fullscreen;
}

static void layout_fx(struct effects *fx, int w, int h, int rad, int bw)
{
	int r = srv.conf.shadow_size, ox = srv.conf.shadow_x, oy = srv.conf.shadow_y;
	bool shadow = shadow_wanted(fx);
	wlr_scene_node_set_enabled(&fx->shadow->node, shadow);
	if (shadow) {
		wlr_scene_node_set_position(&fx->fxshadow->node, -r + ox, -r + oy);
		wlr_scene_shadow_set_size(fx->fxshadow, w + 2 * r, h + 2 * r);
		wlr_scene_shadow_set_corner_radius(fx->fxshadow, rad + r);
		wlr_scene_shadow_set_blur_sigma(fx->fxshadow, srv.conf.shadow_blur > 0 ? srv.conf.shadow_blur : r / 2.0f);
		wlr_scene_shadow_set_color(fx->fxshadow, srv.conf.shadow_color);
		wlr_scene_shadow_set_clipped_region(fx->fxshadow, (struct clipped_region){
			.area = {r - ox, r - oy, w, h}, .corners = corner_radii_all(rad)});
	}
	struct wlr_scene_buffer *s = fx->surface;
	bool blur = blur_wanted(fx) && s && s->dst_width > 0 && s->dst_height > 0;
	if (blur) {
		pixman_box32_t all = {0, 0, s->dst_width, s->dst_height};
		blur = pixman_region32_contains_rectangle(&s->opaque_region, &all) != PIXMAN_REGION_IN;
	}
	wlr_scene_node_set_enabled(&fx->blur->node, blur);
	if (blur) {
		int sx, sy, cx, cy;
		wlr_scene_node_coords(&s->node, &sx, &sy);
		wlr_scene_node_coords(&tree_of(fx)->node, &cx, &cy);
		wlr_scene_node_set_position(&fx->blur->node, sx - cx, sy - cy);
		wlr_scene_blur_set_size(fx->blur, s->dst_width, s->dst_height);
		wlr_scene_blur_set_corner_radius(fx->blur, MAX(0, rad - bw));
		wlr_scene_blur_set_should_only_blur_bottom_layer(fx->blur, srv.conf.blur_optimized);
	}
}

static void layout_client_fx(struct effects *fx)
{
	struct client *c = fx->c;
	int rad = radius(c), w = c->geom.width, h = c->geom.height, bw = border_of(c);
	int ow = bw ? MIN(bw, srv.conf.border_outer_width) : 0;
	bool round = rad > 0 || fx->rounded;
	fx->rounded = rad > 0;
	if (round && c->border[0]) {
		for (int i = 0; i < 8; i += 4) {
			wlr_scene_rect_set_corner_radius(c->border[i], 0);
			wlr_scene_rect_set_clipped_region(c->border[i], clipped_region_get_default());
		}
		if (rad > 0 && bw > 0) {
			round_ring(c->border + 4, 0, ow, rad, w, h);
			round_ring(c->border, ow, bw - ow, rad, w, h);
		}
	}
	if (round && c->scene_surface) {
		struct corner_box k = {bw, bw, w - bw, h - bw, MAX(0, rad - bw)};
		wlr_scene_node_for_each_buffer(&c->scene_surface->node, round_buffer, &k);
	}
	if (fx->dim)
		wlr_scene_buffer_set_corner_radii(fx->dim, corner_radii_all(MAX(0, rad - bw)));
	layout_fx(fx, w, h, rad, bw);
}
#endif

#ifndef WITH_SCENEFX
static void layout_parts(struct effects *fx, int w, int h)
{
	int r = srv.conf.shadow_size, ox = srv.conf.shadow_x, oy = srv.conf.shadow_y;
	bool on = shadow_wanted(fx) && parts[TL];
	wlr_scene_node_set_enabled(&fx->shadow->node, on);
	if (!on)
		return;
	if (fx->part[TL]->buffer != parts[TL]) {
		for (int i = 0; i < NPARTS; i++)
			wlr_scene_buffer_set_buffer(fx->part[i], parts[i]);
		wlr_scene_buffer_set_buffer(fx->fill, built_full ? fill_buf : NULL);
	}
	const struct wlr_box thin[NPARTS] = {
		{-r, -r, 2 * r, 2 * r}, {w - r, -r, 2 * r, 2 * r}, {-r, h - r, 2 * r, 2 * r}, {w - r, h - r, 2 * r, 2 * r},
		{r, -r, w - 2 * r, r}, {r, h, w - 2 * r, r}, {-r, r, r, h - 2 * r}, {w, r, r, h - 2 * r},
	};
	const struct wlr_box full[NPARTS] = {
		{-r, -r, 2 * r, 2 * r}, {w - r, -r, 2 * r, 2 * r}, {-r, h - r, 2 * r, 2 * r}, {w - r, h - r, 2 * r, 2 * r},
		{r, -r, w - 2 * r, 2 * r}, {r, h - r, w - 2 * r, 2 * r}, {-r, r, 2 * r, h - 2 * r}, {w - r, r, 2 * r, h - 2 * r},
	};
	const struct wlr_box *b = built_full ? full : thin;
	for (int i = 0; i < NPARTS; i++) {
		struct wlr_scene_buffer *sb = fx->part[i];
		bool vis = b[i].width > 0 && b[i].height > 0;
		wlr_scene_node_set_enabled(&sb->node, vis);
		if (!vis)
			continue;
		wlr_scene_node_set_position(&sb->node, b[i].x + ox, b[i].y + oy);
		wlr_scene_buffer_set_dest_size(sb, b[i].width, b[i].height);
	}
	bool fill = built_full && w > 2 * r && h > 2 * r;
	wlr_scene_node_set_enabled(&fx->fill->node, fill);
	if (fill) {
		wlr_scene_node_set_position(&fx->fill->node, r + ox, r + oy);
		wlr_scene_buffer_set_dest_size(fx->fill, w - 2 * r, h - 2 * r);
	}
}
#endif

static void layout_dim(struct effects *fx)
{
	struct client *c = fx->c;
	struct wlr_buffer *b = dim_buf[c->active ? 0 : 1];
	int bw = border_of(c), w = c->geom.width - 2 * bw, h = c->geom.height - 2 * bw;
	bool on = b && w > 0 && h > 0 && c->type != CLIENT_X11_UNMANAGED;
	wlr_scene_node_set_enabled(&fx->dim->node, on);
	if (!on)
		return;
	if (fx->dim->buffer != b)
		wlr_scene_buffer_set_buffer(fx->dim, b);
	wlr_scene_buffer_set_dest_size(fx->dim, w, h);
	wlr_scene_node_set_position(&fx->dim->node, bw, bw);
	if (c->popups)
		wlr_scene_node_place_below(&fx->dim->node, &c->popups->node);
	else
		wlr_scene_node_raise_to_top(&fx->dim->node);
}

static void layout(struct effects *fx)
{
	if (fx->ls) {
		struct wlr_surface *s = fx->ls->ls->surface;
#ifdef WITH_SCENEFX
		layout_fx(fx, s->current.width, s->current.height, 0, 0);
#else
		layout_parts(fx, s->current.width, s->current.height);
#endif
		return;
	}
#ifdef WITH_SCENEFX
	layout_client_fx(fx);
#else
	layout_parts(fx, fx->c->geom.width, fx->c->geom.height);
#endif
	layout_dim(fx);
}

static void schedule_frames(void)
{
	struct monitor *m;
	wl_list_for_each(m, &srv.monitors, link)
		if (m->output->enabled)
			wlr_output_schedule_frame(m->output);
}

static void start(struct effects *fx)
{
	if (wl_list_empty(&fx->link))
		wl_list_insert(&active, &fx->link);
	schedule_frames();
}

static void stop(struct effects *fx)
{
	wl_list_remove(&fx->link);
	wl_list_init(&fx->link);
	fx->moving = fx->fading = fx->focusing = false;
	fx->fade = 1;
}

static void buffer_opacity(struct wlr_scene_buffer *b, int sx, int sy, void *data)
{
	float a = *(float *)data;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(b);
	const struct wlr_alpha_modifier_surface_v1_state *st = ss ? wlr_alpha_modifier_v1_get_surface_state(ss->surface) : NULL;
	wlr_scene_buffer_set_opacity(b, a * (st ? (float)st->multiplier : 1.0f));
}

static void scale_color(float out[4], const float in[4], float a)
{
	for (int i = 0; i < 4; i++)
		out[i] = in[i] * a;
}

static void set_opacity(struct effects *fx, float a)
{
	float col[4];
	wlr_scene_node_for_each_buffer(&tree_of(fx)->node, buffer_opacity, &a);
	struct client *c = fx->c;
	for (size_t i = 0; c && i < LENGTH(c->border) && c->border[i]; i++) {
		scale_color(col, i < 4 ? client_border_color(c) : srv.conf.border_outer, a);
		wlr_scene_rect_set_color(c->border[i], col);
	}
#ifdef WITH_SCENEFX
	scale_color(col, srv.conf.shadow_color, a);
	wlr_scene_shadow_set_color(fx->fxshadow, col);
	wlr_scene_blur_set_alpha(fx->blur, a);
#endif
}

static float opacity_of(struct effects *fx)
{
	return fx->intro || fx->zoom_pending ? 0 : fx->fade * fx->alpha;
}

static void snapshot_destroy(struct snapshot *s)
{
	if (s->owner)
		s->owner->intro = NULL;
	wl_list_remove(&s->link);
	wlr_scene_node_destroy(&s->tree->node);
	free(s->nodes);
	free(s);
}

static void add_node(struct snapshot *s, struct wlr_scene_node *n, int x, int y, int w, int h, const float *color)
{
	if (s->nnodes == s->cap) {
		int cap = s->cap ? s->cap * 2 : 16;
		struct snode *p = realloc(s->nodes, (size_t)cap * sizeof(*p));
		if (!p)
			return;
		s->nodes = p, s->cap = cap;
	}
	struct snode *e = &s->nodes[s->nnodes++];
	*e = (struct snode){n, x, y, w, h, {0}};
	if (color)
		memcpy(e->color, color, sizeof(e->color));
}

static void copy_node(struct snapshot *s, struct wlr_scene_node *n, int x, int y)
{
	if (!n->enabled)
		return;
	x += n->x, y += n->y;
	if (n->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_node *child;
		wl_list_for_each(child, &wlr_scene_tree_from_node(n)->children, link)
			copy_node(s, child, x, y);
	} else if (n->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *r = wlr_scene_rect_from_node(n);
#ifdef WITH_SCENEFX
		struct wlr_scene_rect *nr = wlr_scene_rect_create(s->tree, r->width, r->height, r->color);
		if (!nr)
			return;
		wlr_scene_node_set_position(&nr->node, x, y);
		nr->accepts_input = false;
		wlr_scene_rect_set_corner_radii(nr, r->corners);
		wlr_scene_rect_set_clipped_region(nr, r->clipped_region);
		add_node(s, &nr->node, x, y, r->width, r->height, r->color);
#else
		if (r->width <= 0 || r->height <= 0)
			return;
		struct wlr_buffer *buf = solid(r->color);
		struct wlr_scene_buffer *nb = buf ? drawn_buffer(s->tree, buf) : NULL;
		if (buf)
			wlr_buffer_drop(buf);
		if (!nb)
			return;
		wlr_scene_node_set_position(&nb->node, x, y);
		wlr_scene_buffer_set_dest_size(nb, r->width, r->height);
		add_node(s, &nb->node, x, y, r->width, r->height, NULL);
#endif
#ifdef WITH_SCENEFX
	} else if (n->type == WLR_SCENE_NODE_SHADOW) {
		struct wlr_scene_shadow *sh = wlr_scene_shadow_from_node(n);
		struct wlr_scene_shadow *ns = wlr_scene_shadow_create(s->tree, sh->width, sh->height, sh->corner_radius,
			sh->blur_sigma, sh->color);
		if (!ns)
			return;
		wlr_scene_node_set_position(&ns->node, x, y);
		wlr_scene_shadow_set_clipped_region(ns, sh->clipped_region);
		add_node(s, &ns->node, x, y, sh->width, sh->height, sh->color);
#endif
	} else if (n->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *b = wlr_scene_buffer_from_node(n);
		struct wlr_scene_buffer *nb = b->buffer ? drawn_buffer(s->tree, b->buffer) : NULL;
		if (!nb)
			return;
		wlr_scene_node_set_position(&nb->node, x, y);
		wlr_scene_buffer_set_source_box(nb, &b->src_box);
		wlr_scene_buffer_set_dest_size(nb, b->dst_width, b->dst_height);
		wlr_scene_buffer_set_transform(nb, b->transform);
		wlr_scene_buffer_set_filter_mode(nb, b->filter_mode);
#ifdef WITH_SCENEFX
		wlr_scene_buffer_set_corner_radii(nb, b->corners);
#endif
		add_node(s, &nb->node, x, y, b->dst_width, b->dst_height, NULL);
	}
}

static struct snapshot *snapshot_take(struct effects *fx, int d, enum curve_kind curve)
{
	struct wlr_scene_tree *t = tree_of(fx);
	struct snapshot *s = d > 0 ? calloc(1, sizeof(*s)) : NULL;
	if (!s || !(s->tree = wlr_scene_tree_create(t->node.parent))) {
		free(s);
		return NULL;
	}
	set_opacity(fx, 1);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &t->children, link)
		copy_node(s, child, 0, 0);
	set_opacity(fx, opacity_of(fx));
	wlr_scene_node_set_position(&s->tree->node, t->node.x, t->node.y);
	wlr_scene_node_place_above(&s->tree->node, &t->node);
	s->start = now_ms();
	s->dur = d;
	s->curve = s->alpha_curve = curve;
	s->alpha_from = s->alpha_to = s->scale_from = s->scale_to = 1;
	int w = fx->c ? fx->c->geom.width : fx->ls->ls->surface->current.width;
	int h = fx->c ? fx->c->geom.height : fx->ls->ls->surface->current.height;
	s->cx = w / 2, s->cy = h / 2;
	wl_list_insert(&snapshots, &s->link);
	schedule_frames();
	return s;
}

static void snapshot_apply(struct snapshot *s, float e, float ea)
{
	float sc = s->scale_from + (s->scale_to - s->scale_from) * e, a = s->alpha_from + (s->alpha_to - s->alpha_from) * ea;
	float col[4];
	if (s->owner) {
		struct wlr_scene_node *t = &tree_of(s->owner)->node;
		wlr_scene_node_set_position(&s->tree->node, t->x, t->y);
		wlr_scene_node_set_enabled(&s->tree->node, t->enabled);
	}
	for (int i = 0; i < s->nnodes; i++) {
		struct snode *n = &s->nodes[i];
		int w = MAX(1, (int)lroundf(n->w * sc)), h = MAX(1, (int)lroundf(n->h * sc));
		wlr_scene_node_set_position(n->node, s->cx + (int)lroundf((n->x - s->cx) * sc + s->dx * e),
			s->cy + (int)lroundf((n->y - s->cy) * sc + s->dy * e));
		scale_color(col, n->color, a);
		if (n->node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *b = wlr_scene_buffer_from_node(n->node);
			wlr_scene_buffer_set_dest_size(b, w, h);
			wlr_scene_buffer_set_opacity(b, a);
		} else if (n->node->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *r = wlr_scene_rect_from_node(n->node);
			wlr_scene_rect_set_size(r, w, h);
			wlr_scene_rect_set_color(r, col);
#ifdef WITH_SCENEFX
		} else if (n->node->type == WLR_SCENE_NODE_SHADOW) {
			struct wlr_scene_shadow *sh = wlr_scene_shadow_from_node(n->node);
			wlr_scene_shadow_set_size(sh, w, h);
			wlr_scene_shadow_set_color(sh, col);
#endif
		}
	}
}

static void out_of_output(struct wlr_box box, int *dx, int *dy)
{
	struct monitor *m = output_at(box.x + box.width / 2.0, box.y + box.height / 2.0);
	struct wlr_box o = m ? m->m : box;
	int cx = box.x + box.width / 2, cy = box.y + box.height / 2;
	int left = cx - o.x, right = o.x + o.width - cx, top = cy - o.y, bottom = o.y + o.height - cy;
	int best = MIN(MIN(left, right), MIN(top, bottom));
	*dx = *dy = 0;
	if (best == left)
		*dx = -(box.x + box.width - o.x);
	else if (best == right)
		*dx = o.x + o.width - box.x;
	else if (best == top)
		*dy = -(box.y + box.height - o.y);
	else
		*dy = o.y + o.height - box.y;
}

static void layer_out(struct layer_surface *ls, int *dx, int *dy)
{
	uint32_t a = ls->ls->current.anchor;
	struct wlr_box o = ls->mon ? ls->mon->m : (struct wlr_box){0};
	int x = ls->scene->node.x, y = ls->scene->node.y, w = ls->ls->surface->current.width, h = ls->ls->surface->current.height;
	*dx = *dy = 0;
	bool top = a & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, bottom = a & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	bool left = a & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT, right = a & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (top && !bottom)
		*dy = -(y + h - o.y);
	else if (bottom && !top)
		*dy = o.y + o.height - y;
	else if (left && !right)
		*dx = -(x + w - o.x);
	else if (right && !left)
		*dx = o.x + o.width - x;
	else
		out_of_output((struct wlr_box){x, y, w, h}, dx, dy);
}

void effects_resize(struct client *c)
{
	struct effects *fx = c->fx;
	if (!fx)
		return;
	layout(fx);
	float a = opacity_of(fx);
	if (a < 1.0f)
		set_opacity(fx, a);
}

static void start_move(struct effects *fx, int fx0, int fy0, int x, int y, int d, enum curve_kind curve)
{
	fx->from_x = fx0, fx->from_y = fy0;
	fx->to_x = x, fx->to_y = y;
	fx->move_start = now_ms();
	fx->move_dur = d;
	fx->move_curve = curve;
	fx->moving = true;
	wlr_scene_node_set_position(&tree_of(fx)->node, fx0, fy0);
	start(fx);
}

void effects_move(struct client *c, int x, int y)
{
	struct effects *fx = c->fx;
	struct wlr_scene_node *n = &c->scene->node;
	if (!fx || !fx->placed || dur(DUR_MOVE) <= 0 || !n->enabled || c == srv.grab_c || c->fullscreen) {
		if (fx && fx->moving) {
			fx->moving = false;
			if (!fx->fading && !fx->focusing && !fx->zoom_pending)
				stop(fx);
		}
		wlr_scene_node_set_position(n, x, y);
		return;
	}
	if (fx->moving ? fx->to_x == x && fx->to_y == y : n->x == x && n->y == y)
		return;
	start_move(fx, n->x, n->y, x, y, dur(DUR_MOVE), CURVE_MOVE);
}

void effects_settle(struct client *c)
{
	struct effects *fx = c->fx;
	if (!fx || !fx->moving)
		return;
	fx->moving = false;
	wlr_scene_node_set_position(&c->scene->node, fx->to_x, fx->to_y);
	if (!fx->fading && !fx->focusing && !fx->zoom_pending)
		stop(fx);
}

static void fade_in(struct effects *fx, int d, enum curve_kind curve, float from)
{
	if (d <= 0)
		return;
	fx->fade_start = now_ms();
	fx->fade_dur = d;
	fx->fade_curve = curve;
	fx->fade_from = fx->fade = from;
	fx->fading = true;
	set_opacity(fx, opacity_of(fx));
	start(fx);
}

static struct effects *fx_new(void)
{
	struct effects *fx = calloc(1, sizeof(*fx));
	if (!fx)
		return NULL;
	wl_list_init(&fx->link);
	fx->fade = fx->alpha = fx->alpha_from = fx->alpha_to = 1;
	return fx;
}

static void fx_create_nodes(struct effects *fx, struct wlr_scene_tree *t)
{
	fx->shadow = wlr_scene_tree_create(t);
	wlr_scene_node_lower_to_bottom(&fx->shadow->node);
#ifdef WITH_SCENEFX
	fx->fxshadow = wlr_scene_shadow_create(fx->shadow, 0, 0, 0, 0, srv.conf.shadow_color);
	fx->blur = wlr_scene_blur_create(t, 0, 0);
	wlr_scene_node_place_above(&fx->blur->node, &fx->shadow->node);
	wlr_scene_node_set_enabled(&fx->blur->node, false);
#else
	for (int i = 0; i < NPARTS; i++)
		fx->part[i] = drawn_buffer(fx->shadow, parts[i]);
	fx->fill = drawn_buffer(fx->shadow, built_full ? fill_buf : NULL);
#endif
	wlr_scene_node_set_enabled(&fx->shadow->node, false);
}

void effects_client_create(struct client *c)
{
	if (c->fx)
		return;
	struct effects *fx = fx_new();
	if (!fx)
		return;
	fx->c = c;
	fx_create_nodes(fx, c->scene);
	fx->dim = drawn_buffer(c->scene, NULL);
	wlr_scene_node_set_enabled(&fx->dim->node, false);
	c->fx = fx;
}

static void fx_destroy(struct effects *fx)
{
	if (fx->intro)
		snapshot_destroy(fx->intro);
	wl_list_remove(&fx->link);
	free(fx);
}

void effects_client_destroy(struct client *c)
{
	if (!c->fx)
		return;
	fx_destroy(c->fx);
	c->fx = NULL;
}

static void open_window(struct effects *fx, enum anim_kind kind, bool fade, int d)
{
	if (d <= 0)
		return;
	struct wlr_scene_node *n = &tree_of(fx)->node;
	if (kind == ANIM_ZOOM && fx->c) {
		fx->zoom_pending = true;
		fx->zoom_wait = now_ms();
	} else if (kind != ANIM_NONE) {
		struct snapshot *s = fx->ls || kind == ANIM_ZOOM ? snapshot_take(fx, d, CURVE_OPEN) : NULL;
		if (s) {
			int dx = 0, dy = 0;
			if (kind == ANIM_SLIDE)
				layer_out(fx->ls, &dx, &dy);
			for (int i = 0; i < s->nnodes; i++)
				s->nodes[i].x += dx, s->nodes[i].y += dy;
			s->dx = -dx, s->dy = -dy;
			if (kind == ANIM_ZOOM)
				s->scale_from = srv.conf.zoom_start;
			s->alpha_from = fade ? srv.conf.fade_in_opacity : 1;
			s->alpha_curve = CURVE_FADE_IN;
			s->owner = fx;
			fx->intro = s;
			snapshot_apply(s, 0, 0);
		} else if (kind == ANIM_SLIDE && fx->c) {
			int dx, dy;
			out_of_output(fx->c->geom, &dx, &dy);
			start_move(fx, n->x + dx, n->y + dy, n->x, n->y, d, CURVE_OPEN);
		}
	}
	if (fade && !fx->intro && !fx->zoom_pending)
		fade_in(fx, d, CURVE_FADE_IN, srv.conf.fade_in_opacity);
	set_opacity(fx, opacity_of(fx));
	start(fx);
}

static void zoom_ready(struct effects *fx, uint32_t now)
{
	struct client *c = fx->c;
	int bw = border_of(c);
	bool sized = fx->surface && fx->surface->dst_width == c->geom.width - 2 * bw &&
		fx->surface->dst_height == c->geom.height - 2 * bw;
	if (!sized && (uint32_t)(now - fx->zoom_wait) < 200 && c->scene->node.enabled)
		return;
	fx->zoom_pending = false;
	struct snapshot *s = c->scene->node.enabled ? snapshot_take(fx, dur(DUR_OPEN), CURVE_OPEN) : NULL;
	if (!s) {
		set_opacity(fx, opacity_of(fx));
		return;
	}
	s->scale_from = srv.conf.zoom_start;
	s->alpha_from = srv.conf.fade_in ? srv.conf.fade_in_opacity : 1;
	s->alpha_curve = CURVE_FADE_IN;
	s->owner = fx;
	fx->intro = s;
	snapshot_apply(s, 0, 0);
}

void effects_map(struct client *c)
{
	struct effects *fx = c->fx;
	if (!fx)
		return;
	fx->placed = true;
	fx->surface = NULL;
	wlr_scene_node_for_each_buffer(&c->scene->node, find_surface, fx);
#ifdef WITH_SCENEFX
	if (fx->surface)
		wlr_scene_blur_set_transparency_mask_source(fx->blur, fx->surface);
#endif
	fx->alpha = fx->alpha_from = fx->alpha_to = c->active ? srv.conf.opacity_focused : srv.conf.opacity_unfocused;
	layout(fx);
	if (client_visible(c))
		open_window(fx, srv.conf.anim_open, srv.conf.fade_in, dur(DUR_OPEN));
	set_opacity(fx, opacity_of(fx));
}

void effects_focus(struct client *c)
{
	struct effects *fx = c->fx;
	if (!fx)
		return;
	layout_dim(fx);
	float to = c->active ? srv.conf.opacity_focused : srv.conf.opacity_unfocused;
	if (to == fx->alpha_to && !fx->focusing) {
		fx->alpha = to;
	} else if (dur(DUR_FOCUS) > 0 && fx->placed) {
		fx->alpha_from = fx->alpha;
		fx->alpha_to = to;
		fx->focus_start = now_ms();
		fx->focusing = true;
		start(fx);
		return;
	} else {
		fx->alpha = fx->alpha_to = to;
	}
	set_opacity(fx, opacity_of(fx));
}

static int tag_dir(uint32_t oldtags, uint32_t newtags)
{
	return __builtin_ctz(newtags ? newtags : 1) > __builtin_ctz(oldtags ? oldtags : 1) ? 1 : -1;
}

void effects_tag_leave(struct monitor *m, uint32_t oldtags, uint32_t newtags)
{
	struct client *c;
	int d = dur(DUR_TAG), dir = tag_dir(oldtags, newtags);
	enum tag_anim kind = srv.conf.tag_animation;
	if (d <= 0 || m->overview || (kind != TAG_ANIM_SLIDE && kind != TAG_ANIM_SLIDE_VERTICAL))
		return;
	wl_list_for_each(c, &srv.clients, link) {
		if (c->mon != m || !c->fx || !c->fx->placed || !c->scene->node.enabled || c->sticky || c->scratch ||
				c->minimized || !(c->tags & oldtags) || (c->tags & newtags))
			continue;
		struct snapshot *s = snapshot_take(c->fx, d, CURVE_TAG);
		if (!s)
			continue;
		if (kind == TAG_ANIM_SLIDE)
			s->dx = -dir * m->m.width;
		else
			s->dy = -dir * m->m.height;
		s->alpha_from = s->alpha_to = c->fx->alpha;
		snapshot_apply(s, 0, 0);
	}
}

void effects_show(struct monitor *m, uint32_t oldtags)
{
	struct client *c;
	int d = dur(DUR_TAG), dir = tag_dir(oldtags, m->tags);
	enum tag_anim kind = srv.conf.tag_animation;
	if (d <= 0 || kind == TAG_ANIM_NONE || m->overview)
		return;
	wl_list_for_each(c, &srv.clients, link) {
		if (c->mon != m || !c->fx || !c->fx->placed || c->sticky || c->scratch || (c->tags & oldtags) ||
				!client_visible(c))
			continue;
		struct wlr_scene_node *n = &c->scene->node;
		if (kind == TAG_ANIM_FADE) {
			fade_in(c->fx, d, CURVE_TAG, 0);
		} else if (n->enabled) {
			int x = c->fx->moving ? c->fx->to_x : n->x, y = c->fx->moving ? c->fx->to_y : n->y;
			int ox = kind == TAG_ANIM_SLIDE ? dir * m->m.width : 0, oy = kind == TAG_ANIM_SLIDE ? 0 : dir * m->m.height;
			start_move(c->fx, x + ox, y + oy, x, y, d, CURVE_TAG);
		}
	}
}

static void close_copy(struct effects *fx, enum anim_kind kind, bool fade, int dx, int dy)
{
	int d = dur(DUR_CLOSE);
	if (d <= 0 || (kind == ANIM_NONE && !fade))
		return;
	struct snapshot *s = snapshot_take(fx, d, CURVE_CLOSE);
	if (!s)
		return;
	s->alpha_from = (fade ? srv.conf.fade_out_opacity : 1) * fx->alpha;
	s->alpha_to = fade ? 0 : s->alpha_from;
	s->alpha_curve = CURVE_FADE_OUT;
	if (kind == ANIM_ZOOM)
		s->scale_to = srv.conf.zoom_end;
	else if (kind == ANIM_SLIDE)
		s->dx = dx, s->dy = dy;
	snapshot_apply(s, 0, 0);
}

void effects_unmap(struct client *c)
{
	struct effects *fx = c->fx;
	if (!fx)
		return;
	bool shown = fx->placed && c->scene->node.enabled && client_visible(c);
	fx->placed = false;
	if (fx->intro)
		snapshot_destroy(fx->intro);
	fx->zoom_pending = false;
	if (fx->moving || fx->fading || fx->focusing)
		stop(fx);
	fx->alpha = fx->alpha_to;
	if (shown) {
		int dx = 0, dy = 0;
		out_of_output(c->geom, &dx, &dy);
		close_copy(fx, srv.conf.anim_close, srv.conf.fade_out, dx, dy);
	}
	fx->surface = NULL;
	set_opacity(fx, 1);
}

void effects_layer_create(struct layer_surface *ls)
{
	struct effects *fx = fx_new();
	if (!fx)
		return;
	fx->ls = ls;
	fx_create_nodes(fx, ls->scene);
	ls->fx = fx;
}

void effects_layer_destroy(struct layer_surface *ls)
{
	if (!ls->fx)
		return;
	fx_destroy(ls->fx);
	ls->fx = NULL;
}

void effects_layer_commit(struct layer_surface *ls)
{
	if (!ls->fx || !ls->mapped)
		return;
	layout(ls->fx);
	float a = opacity_of(ls->fx);
	if (a < 1.0f)
		set_opacity(ls->fx, a);
}

void effects_layer_map(struct layer_surface *ls)
{
	struct effects *fx = ls->fx;
	struct layer_rule r;
	if (!fx)
		return;
	fx->placed = true;
	fx->surface = NULL;
	wlr_scene_node_for_each_buffer(&ls->scene->node, find_surface, fx);
#ifdef WITH_SCENEFX
	if (fx->surface)
		wlr_scene_blur_set_transparency_mask_source(fx->blur, fx->surface);
#endif
	layout(fx);
	config_layer(ls->ls->namespace, &r);
	if (r.animations)
		open_window(fx, (enum anim_kind)r.anim_open, srv.conf.fade_in, dur(DUR_OPEN));
}

void effects_layer_unmap(struct layer_surface *ls)
{
	struct effects *fx = ls->fx;
	struct layer_rule r;
	if (!fx)
		return;
	bool shown = fx->placed && ls->scene->node.enabled;
	fx->placed = false;
	if (fx->intro)
		snapshot_destroy(fx->intro);
	if (fx->moving || fx->fading || fx->focusing)
		stop(fx);
	config_layer(ls->ls->namespace, &r);
	if (shown && r.animations) {
		int dx, dy;
		layer_out(ls, &dx, &dy);
		close_copy(fx, (enum anim_kind)r.anim_close, srv.conf.fade_out, dx, dy);
	}
	fx->surface = NULL;
	set_opacity(fx, 1);
}

void effects_monitor_update(struct monitor *m)
{
#ifdef WITH_SCENEFX
	bool want = srv.conf.blur_optimized && srv.conf.blur_passes > 0 && m->output->enabled;
	if (!want) {
		if (m->optimized_blur)
			wlr_scene_node_destroy(m->optimized_blur);
		m->optimized_blur = NULL;
		return;
	}
	if (!m->optimized_blur) {
		struct wlr_scene_optimized_blur *b = wlr_scene_optimized_blur_create(srv.layers[LYR_BOTTOM], 0, 0);
		if (!b)
			return;
		m->optimized_blur = &b->node;
	}
	wlr_scene_node_lower_to_bottom(m->optimized_blur);
	wlr_scene_node_set_position(m->optimized_blur, m->m.x, m->m.y);
	wlr_scene_optimized_blur_set_size(wlr_scene_optimized_blur_from_node(m->optimized_blur), m->m.width, m->m.height);
#endif
}

void effects_background_changed(struct monitor *m)
{
#ifdef WITH_SCENEFX
	if (m && m->optimized_blur)
		wlr_scene_optimized_blur_mark_dirty(wlr_scene_optimized_blur_from_node(m->optimized_blur));
#endif
}

void effects_frame(void)
{
	struct effects *fx, *tmp;
	struct snapshot *s, *stmp;
	if (wl_list_empty(&active) && wl_list_empty(&snapshots))
		return;
	uint32_t now = now_ms();
	wl_list_for_each_safe(fx, tmp, &active, link) {
		bool changed = false;
		if (fx->moving) {
			float t = progress(now, fx->move_start, fx->move_dur), e = ease(fx->move_curve, t);
			wlr_scene_node_set_position(&tree_of(fx)->node, fx->from_x + (int)lroundf((fx->to_x - fx->from_x) * e),
				fx->from_y + (int)lroundf((fx->to_y - fx->from_y) * e));
			fx->moving = t < 1.0f;
		}
		if (fx->fading) {
			float t = progress(now, fx->fade_start, fx->fade_dur);
			fx->fade = fx->fade_from + (1 - fx->fade_from) * ease(fx->fade_curve, t);
			fx->fading = t < 1.0f;
			changed = true;
		}
		if (fx->focusing) {
			float t = progress(now, fx->focus_start, dur(DUR_FOCUS));
			fx->alpha = fx->alpha_from + (fx->alpha_to - fx->alpha_from) * ease(CURVE_FOCUS, t);
			fx->focusing = t < 1.0f;
			changed = true;
		}
		if (fx->zoom_pending) {
			zoom_ready(fx, now);
			changed = true;
		}
		if (changed)
			set_opacity(fx, opacity_of(fx));
		if (!fx->moving && !fx->fading && !fx->focusing && !fx->zoom_pending && !fx->intro)
			stop(fx);
	}
	wl_list_for_each_safe(s, stmp, &snapshots, link) {
		float t = progress(now, s->start, s->dur);
		if (t >= 1.0f) {
			struct effects *owner = s->owner;
			snapshot_destroy(s);
			if (owner)
				set_opacity(owner, opacity_of(owner));
			continue;
		}
		snapshot_apply(s, ease(s->curve, t), ease(s->alpha_curve, t));
	}
	if (!wl_list_empty(&active) || !wl_list_empty(&snapshots))
		schedule_frames();
}

void effects_apply_config(void)
{
	struct client *c;
	struct monitor *m;
	int r = srv.conf.shadow_size, blur = srv.conf.shadow_blur;
	bool full = srv.conf.shadow_x || srv.conf.shadow_y;
	for (int i = 0; i < 2; i++) {
		if (dim_buf[i])
			wlr_buffer_drop(dim_buf[i]);
		dim_buf[i] = solid(i ? srv.conf.dim_unfocused : srv.conf.dim_focused);
	}
#ifdef WITH_SCENEFX
	struct blur_data b = blur_data_get_default();
	const struct config *k = &srv.conf;
	wlr_scene_set_blur_data(srv.scene, k->blur_passes, k->blur_radius, k->blur_noise >= 0 ? k->blur_noise : b.noise,
		k->blur_brightness >= 0 ? k->blur_brightness : b.brightness, k->blur_contrast >= 0 ? k->blur_contrast : b.contrast,
		k->blur_saturation >= 0 ? k->blur_saturation : b.saturation);
	goto relayout;
#endif
	if (r == built_size && blur == built_blur && full == built_full &&
			!memcmp(built_color, srv.conf.shadow_color, sizeof(built_color)))
		goto relayout;
	for (int i = 0; i < NPARTS; i++) {
		if (parts[i])
			wlr_buffer_drop(parts[i]);
		parts[i] = r > 0 ? make_part(i, r, blur > 0 ? blur : r / 3.0, full) : NULL;
	}
	if (fill_buf)
		wlr_buffer_drop(fill_buf);
	fill_buf = r > 0 && full ? solid(srv.conf.shadow_color) : NULL;
	built_size = r;
	built_blur = blur;
	built_full = full;
	memcpy(built_color, srv.conf.shadow_color, sizeof(built_color));
relayout:
	wl_list_for_each(c, &srv.clients, link)
		if (c->fx) {
			effects_focus(c);
			effects_resize(c);
		}
	wl_list_for_each(m, &srv.monitors, link) {
		struct layer_surface *ls;
		effects_monitor_update(m);
		for (size_t i = 0; i < LENGTH(m->layers); i++)
			wl_list_for_each(ls, &m->layers[i], link)
				effects_layer_commit(ls);
	}
}

void effects_init(void)
{
	wl_list_init(&active);
	wl_list_init(&snapshots);
	effects_apply_config();
}

void effects_finish(void)
{
	struct snapshot *s, *tmp;
	wl_list_for_each_safe(s, tmp, &snapshots, link)
		snapshot_destroy(s);
	for (int i = 0; i < NPARTS; i++)
		if (parts[i])
			wlr_buffer_drop(parts[i]);
	memset(parts, 0, sizeof(parts));
	if (fill_buf)
		wlr_buffer_drop(fill_buf);
	fill_buf = NULL;
	for (int i = 0; i < 2; i++) {
		if (dim_buf[i])
			wlr_buffer_drop(dim_buf[i]);
		dim_buf[i] = NULL;
	}
}
