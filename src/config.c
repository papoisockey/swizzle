#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <libinput.h>
#include <wlr/types/wlr_keyboard.h>
#include "swizzle.h"

static int nerrors, cur_line;
#define conf_error(...) (nerrors++, wlr_log(WLR_ERROR, __VA_ARGS__))

static double fclamp(double v, double lo, double hi)
{
	return v >= lo ? (v <= hi ? v : hi) : lo;
}

static bool parse_num(const char *v, double lo, double hi, double *out)
{
	char *end;
	double n = strtod(v, &end);
	if (end == v || *end || n != n) {
		conf_error("config:%d: expected a number, got '%s'", cur_line, v);
		return false;
	}
	*out = fclamp(n, lo, hi);
	return true;
}

#define SET_NUM(dst, v, lo, hi) do { double n_; if (parse_num(v, lo, hi, &n_)) (dst) = n_; } while (0)

enum section { SEC_NONE, SEC_GENERAL, SEC_INPUT, SEC_OUTPUT, SEC_RULE, SEC_AUTOSTART, SEC_BIND, SEC_ENV, SEC_TOUCHPAD, SEC_MOUSE,
	SEC_LAYER };

static const char *fallback_binds[][2] = {
	{"super+Return", "spawn foot"},
	{"super+q", "close"},
	{"super+shift+e", "quit"},
	{"super+shift+r", "reload"},
};

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static void set_str(char **dst, const char *v)
{
	free(*dst);
	*dst = strdup(v);
}

static int bool_of(const char *v)
{
	if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1"))
		return 1;
	if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0"))
		return 0;
	return -1;
}

#define SET_BOOL(dst, v) do { int b_ = bool_of(v); if (b_ < 0) \
	conf_error("config:%d: expected true or false, got '%s'", cur_line, v); else (dst) = b_; } while (0)

static bool parse_color(const char *v, float out[4])
{
	if (*v == '#')
		v++;
	size_t len = strlen(v);
	if (len != 6 && len != 8)
		return false;
	if (strspn(v, "0123456789abcdefABCDEF") != len)
		return false;
	unsigned long n = strtoul(v, NULL, 16);
	if (len == 6)
		n = (n << 8) | 0xff;
	float a = (n & 0xff) / 255.0f;
	out[0] = ((n >> 24) & 0xff) / 255.0f * a;
	out[1] = ((n >> 16) & 0xff) / 255.0f * a;
	out[2] = ((n >> 8) & 0xff) / 255.0f * a;
	out[3] = a;
	return true;
}

static void parse_state_color(const char *v, float out[4], int line)
{
	if (!*v)
		out[3] = -1;
	else if (!parse_color(v, out))
		conf_error("config:%d: invalid color '%s'", line, v);
}

static void parse_pair(const char *v, int lo, int hi, int *a, int *b, int line)
{
	double x, y;
	char buf[64], *comma;
	snprintf(buf, sizeof(buf), "%s", v);
	if ((comma = strchr(buf, ','))) {
		*comma = '\0';
		if (parse_num(trim(buf), lo, hi, &x) && parse_num(trim(comma + 1), lo, hi, &y))
			*a = (int)x, *b = (int)y;
	} else if (parse_num(trim(buf), lo, hi, &x)) {
		*a = *b = (int)x;
	}
}

static const struct {
	const char *name;
	struct curve c;
} curve_names[] = {
	{"linear", {0, 0, 1, 1}},
	{"ease", {0.25f, 0.1f, 0.25f, 1}},
	{"ease-in", {0.42f, 0, 1, 1}},
	{"ease-out", {0.33f, 1, 0.68f, 1}},
	{"ease-in-out", {0.42f, 0, 0.58f, 1}},
	{"snappy", {0.46f, 1, 0.29f, 1}},
};

static void parse_curve(const char *v, struct curve *out, int line)
{
	for (size_t i = 0; i < LENGTH(curve_names); i++)
		if (!strcmp(v, curve_names[i].name)) {
			*out = curve_names[i].c;
			return;
		}
	struct curve c;
	char tail;
	if (sscanf(v, "%f , %f , %f , %f %c", &c.x1, &c.y1, &c.x2, &c.y2, &tail) != 4 || !(c.x1 >= 0 && c.x1 <= 1) ||
			!(c.x2 >= 0 && c.x2 <= 1) || !(c.y1 >= -2 && c.y1 <= 3) || !(c.y2 >= -2 && c.y2 <= 3)) {
		conf_error("config:%d: invalid curve '%s', use a name such as ease-out or x1,y1,x2,y2", line, v);
		return;
	}
	*out = c;
}

float curve_ease(const struct curve *c, float t)
{
	if (!(t > 0))
		return 0;
	if (t >= 1)
		return 1;
	float lo = 0, hi = 1, s = t;
	for (int i = 0; i < 24; i++) {
		s = (lo + hi) / 2;
		float u = 1 - s, x = 3 * u * u * s * c->x1 + 3 * u * s * s * c->x2 + s * s * s;
		if (x < t)
			lo = s;
		else
			hi = s;
	}
	float u = 1 - s;
	return 3 * u * u * s * c->y1 + 3 * u * s * s * c->y2 + s * s * s;
}

static int parse_anim(const char *v, int line)
{
	if (!strcmp(v, "none") || !strcmp(v, "fade"))
		return ANIM_NONE;
	if (!strcmp(v, "slide"))
		return ANIM_SLIDE;
	if (!strcmp(v, "zoom"))
		return ANIM_ZOOM;
	conf_error("config:%d: invalid animation '%s', use none, fade, slide or zoom", line, v);
	return -1;
}

static uint32_t parse_mod(const char *s)
{
	if (!strcasecmp(s, "super") || !strcasecmp(s, "logo") || !strcasecmp(s, "mod4"))
		return WLR_MODIFIER_LOGO;
	if (!strcasecmp(s, "alt") || !strcasecmp(s, "mod1"))
		return WLR_MODIFIER_ALT;
	if (!strcasecmp(s, "ctrl") || !strcasecmp(s, "control"))
		return WLR_MODIFIER_CTRL;
	if (!strcasecmp(s, "shift"))
		return WLR_MODIFIER_SHIFT;
	if (!strcasecmp(s, "altgr") || !strcasecmp(s, "mod5"))
		return WLR_MODIFIER_MOD5;
	return 0;
}

static bool config_parse_bind_key(const char *s, uint32_t *mods, xkb_keysym_t *sym)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", s);
	*mods = 0;
	*sym = XKB_KEY_NoSymbol;
	char *save = NULL;
	for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
		tok = trim(tok);
		uint32_t m = parse_mod(tok);
		if (m) {
			*mods |= m;
			continue;
		}
		if (*sym != XKB_KEY_NoSymbol)
			return false;
		static const char *wheel[] = {"wheel_up", "wheel_down", "wheel_left", "wheel_right"};
		static const char *buttons[] = {"button_left", "button_right", "button_middle", "button_side", "button_extra"};
		static const char *dirs[] = {"left", "right", "up", "down"};
		int fingers, n = 0;
		char dir[16];
		for (size_t i = 0; i < LENGTH(wheel); i++)
			if (!strcasecmp(tok, wheel[i]))
				*sym = SYM_WHEEL + (uint32_t)i;
		for (size_t i = 0; i < LENGTH(buttons); i++)
			if (!strcasecmp(tok, buttons[i]))
				*sym = SYM_BUTTON + 0x110 + (uint32_t)i;
		if (sscanf(tok, "swipe:%d:%15[a-z]%n", &fingers, dir, &n) == 2 && !tok[n] && fingers >= 3 && fingers <= 5)
			for (size_t i = 0; i < LENGTH(dirs); i++)
				if (!strcmp(dir, dirs[i]))
					*sym = SYM_SWIPE_OF(fingers, i);
		if (*sym != XKB_KEY_NoSymbol)
			continue;
		*sym = xkb_keysym_to_lower(xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE));
		if (*sym == XKB_KEY_NoSymbol)
			return false;
	}
	return *sym != XKB_KEY_NoSymbol;
}

static uint32_t parse_tags(const char *v)
{
	uint32_t tags = 0;
	char buf[64];
	if (snprintf(buf, sizeof(buf), "%s", v) >= (int)sizeof(buf))
		conf_error("config:%d: tag list '%s' is too long", cur_line, v);
	char *save = NULL;
	for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *end;
		tok = trim(tok);
		long n = strtol(tok, &end, 10);
		if (end != tok && !*end && n >= 1 && n <= TAGCOUNT)
			tags |= 1u << (n - 1);
		else
			conf_error("config:%d: invalid tag '%s', use 1 to %d", cur_line, tok, TAGCOUNT);
	}
	return tags;
}

static int parse_transform(const char *v)
{
	static const char *names[] = {"normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270"};
	for (size_t i = 0; i < LENGTH(names); i++)
		if (!strcasecmp(v, names[i]))
			return (int)i;
	return -1;
}

static void *grow(void *arr, size_t *n, size_t size)
{
	void *p = realloc(arr, (*n + 1) * size);
	if (!p) {
		wlr_log(WLR_ERROR, "out of memory");
		abort();
	}
	memset((char *)p + *n * size, 0, size);
	(*n)++;
	return p;
}

static void add_bind(struct config *c, const char *key, const char *cmd, int line)
{
	uint32_t mods;
	xkb_keysym_t sym;
	bool locked = !strncmp(key, "locked+", 7);
	if (locked)
		key += 7;
#ifdef WITH_LID
	bool lid_close = !strcmp(key, "lid:close");
	if (lid_close || !strcmp(key, "lid:open")) {
		set_str(lid_close ? &c->lid_close : &c->lid_open, cmd);
		*(lid_close ? &c->lid_close_locked : &c->lid_open_locked) = locked;
		if (!action_known(cmd))
			conf_error("config:%d: unknown command in '%s'", line, cmd);
		return;
	}
#endif
	if (!config_parse_bind_key(key, &mods, &sym)) {
		conf_error("config:%d: invalid key '%s'", line, key);
		return;
	}
	if (!action_known(cmd))
		conf_error("config:%d: unknown command in '%s'", line, cmd);
	for (size_t i = 0; i < c->nbinds; i++)
		if (c->binds[i].mods == mods && c->binds[i].sym == sym) {
			set_str(&c->binds[i].cmd, cmd);
			c->binds[i].locked = locked;
			return;
		}
	c->binds = grow(c->binds, &c->nbinds, sizeof(*c->binds));
	struct bind *b = &c->binds[c->nbinds - 1];
	b->mods = mods;
	b->sym = sym;
	b->locked = locked;
	b->cmd = strdup(cmd);
}

static void pointer_unset(struct pointer_conf *p)
{
	p->tap = p->tap_drag = p->dwt = p->natural_scroll = p->left_handed = p->middle_emulation = -1;
	p->accel_profile = p->scroll_method = p->click_method = p->drag_lock = p->enabled = -1;
	p->has_accel = false;
}

static void set_defaults(struct config *c)
{
	memset(c, 0, sizeof(*c));
	pointer_unset(&c->pointer);
	pointer_unset(&c->touchpad);
	pointer_unset(&c->mouse);
	c->renderer = RENDERER_AUTO;
	c->border_width = 2;
	parse_color("#000000", c->border_outer);
	parse_color("#88c0d0", c->border_focus);
	parse_color("#3b4252", c->border_normal);
	parse_color("#bf616a", c->border_urgent);
	c->border_maximized[3] = c->border_scratchpad[3] = c->border_sticky[3] = c->border_overlay[3] = -1;
	parse_color("#000000", c->root_color);
	parse_color("#88c0d055", c->drop_color);
	c->mfact = 0.55f;
	c->nmaster = 1;
	c->tag_count = TAGCOUNT;
	c->focus_follows_mouse = true;
	c->focus_on_activate = true;
	c->prefer_no_csd = true;
	c->mouse_mod = WLR_MODIFIER_LOGO;
	c->cursor_size = 24;
	c->scroll_width = 0.5f;
	const float presets[] = {1.0f / 3, 0.5f, 2.0f / 3, 1.0f};
	memcpy(c->scroll_presets, presets, sizeof(presets));
	c->nscroll_presets = LENGTH(presets);
	c->overview_gap_inner = 5;
	c->overview_gap_outer = 30;
	c->hotarea_corner = CORNER_BOTTOM_LEFT;
	c->axis_timeout = 100;
	c->animation_duration = 150;
	for (int i = 0; i < DUR_COUNT; i++)
		c->durations[i] = i == DUR_FOCUS ? 0 : -1;
	for (int i = 0; i < CURVE_COUNT; i++)
		c->curves[i] = (struct curve){0.33f, 1, 0.68f, 1};
	c->fade_in = c->fade_out = true;
	c->fade_out_opacity = 1;
	c->zoom_start = c->zoom_end = 0.5f;
	c->opacity_focused = c->opacity_unfocused = 1;
	parse_color("#00000080", c->shadow_color);
	c->blur_radius = 5;
	c->blur_noise = c->blur_brightness = c->blur_contrast = c->blur_saturation = -1;
	c->swipe_threshold = 100;
	c->repeat_rate = 25;
	c->repeat_delay = 600;
	c->pointer.tap = 1;
	c->pointer.tap_drag = 1;
	c->pointer.dwt = 1;
	c->pointer.natural_scroll = 0;
	c->pointer.accel = 0;
	c->pointer.has_accel = true;
	c->gesture_fingers = 3;
}

static void parse_tag_names(struct config *c, const char *v, int line)
{
	for (int i = 0;; i++) {
		size_t n = strcspn(v, ",");
		char name[128];
		snprintf(name, sizeof(name), "%.*s", (int)n, v);
		char *t = trim(name);
		if (i == TAGCOUNT) {
			if (*t || v[n])
				conf_error("config:%d: more than %d tag names", line, TAGCOUNT);
			return;
		}
		if (*t)
			set_str(&c->tag_names[i], t);
		if (!v[n])
			return;
		v += n + 1;
	}
}

static bool *general_bool(struct config *c, const char *k)
{
	const struct {
		const char *name;
		bool *dst;
	} keys[] = {
		{"tag_back_and_forth", &c->tag_back_and_forth}, {"smart_gaps", &c->smart_gaps},
		{"focus_follows_mouse", &c->focus_follows_mouse}, {"prefer_no_csd", &c->prefer_no_csd},
		{"no_border_when_single", &c->no_border_when_single}, {"no_radius_when_single", &c->no_radius_when_single},
		{"monocle_no_border", &c->monocle_no_border}, {"monocle_no_gap", &c->monocle_no_gap},
		{"focus_on_activate", &c->focus_on_activate}, {"warp_cursor", &c->warp_cursor},
		{"focus_cross_monitor", &c->focus_cross_monitor}, {"focus_cross_tag", &c->focus_cross_tag},
		{"focus_overlap_only", &c->focus_overlap_only}, {"drag_tile_to_tile", &c->drag_tile_to_tile},
		{"drag_tile_small", &c->drag_tile_small}, {"hotarea_fullscreen", &c->hotarea_fullscreen},
		{"idle_inhibit_hidden", &c->idle_inhibit_hidden}, {"idle_inhibit_fullscreen", &c->idle_inhibit_fullscreen},
		{"fade_in", &c->fade_in}, {"fade_out", &c->fade_out}, {"layer_animations", &c->layer_animations},
		{"shadow_floating_only", &c->shadow_floating_only}, {"shadow_layers", &c->shadow_layers},
		{"blur_layers", &c->blur_layers}, {"blur_optimized", &c->blur_optimized},
	};
	for (size_t i = 0; i < LENGTH(keys); i++)
		if (!strcmp(k, keys[i].name))
			return keys[i].dst;
	return NULL;
}

static bool parse_animation_key(struct config *c, const char *k, const char *v, int line)
{
	static const char *durs[] = {"open", "close", "move", "tag", "focus"};
	static const char *curves[] = {"open", "close", "move", "tag", "fade_in", "fade_out", "focus"};
	if (!strncmp(k, "animation_duration_", 19)) {
		for (size_t i = 0; i < LENGTH(durs); i++)
			if (!strcmp(k + 19, durs[i])) {
				SET_NUM(c->durations[i], v, -1, 5000);
				return true;
			}
		return false;
	}
	if (!strcmp(k, "animation_curve")) {
		struct curve cv = c->curves[0];
		parse_curve(v, &cv, line);
		for (int i = 0; i < CURVE_COUNT; i++)
			c->curves[i] = cv;
		return true;
	}
	if (!strncmp(k, "animation_curve_", 16)) {
		for (size_t i = 0; i < LENGTH(curves); i++)
			if (!strcmp(k + 16, curves[i])) {
				parse_curve(v, &c->curves[i], line);
				return true;
			}
	}
	return false;
}

static void parse_general(struct config *c, const char *k, const char *v, int line)
{
	bool *b = general_bool(c, k);
	int anim;
	if (b) {
		SET_BOOL(*b, v);
	} else if (parse_animation_key(c, k, v, line)) {
	} else if (!strcmp(k, "gaps")) {
		double g;
		if (parse_num(v, 0, 500, &g))
			c->gap_ih = c->gap_iv = c->gap_oh = c->gap_ov = (int)g;
	} else if (!strcmp(k, "gaps_inner")) {
		parse_pair(v, 0, 500, &c->gap_ih, &c->gap_iv, line);
	} else if (!strcmp(k, "gaps_outer")) {
		parse_pair(v, 0, 500, &c->gap_oh, &c->gap_ov, line);
	} else if (!strcmp(k, "shadow_offset")) {
		parse_pair(v, -100, 100, &c->shadow_x, &c->shadow_y, line);
	} else if (!strcmp(k, "border_maximized")) {
		parse_state_color(v, c->border_maximized, line);
	} else if (!strcmp(k, "border_scratchpad")) {
		parse_state_color(v, c->border_scratchpad, line);
	} else if (!strcmp(k, "border_sticky")) {
		parse_state_color(v, c->border_sticky, line);
	} else if (!strcmp(k, "border_overlay")) {
		parse_state_color(v, c->border_overlay, line);
	} else if (!strcmp(k, "root_color") || !strcmp(k, "drop_color") || !strcmp(k, "dim_focused") ||
			!strcmp(k, "dim_unfocused")) {
		float *dst = k[0] == 'r' ? c->root_color : k[1] == 'r' ? c->drop_color : k[4] == 'f' ? c->dim_focused :
			c->dim_unfocused;
		if (!parse_color(v, dst))
			conf_error("config:%d: invalid color '%s'", line, v);
	} else if (!strcmp(k, "tag_count")) {
		SET_NUM(c->tag_count, v, 1, TAGCOUNT);
	} else if (!strcmp(k, "snap_distance")) {
		SET_NUM(c->snap_distance, v, 0, 500);
	} else if (!strcmp(k, "scratchpad_width")) {
		SET_NUM(c->scratchpad_width, v, 0, 1);
	} else if (!strcmp(k, "scratchpad_height")) {
		SET_NUM(c->scratchpad_height, v, 0, 1);
	} else if (!strcmp(k, "scroll_width")) {
		SET_NUM(c->scroll_width, v, 0.1, 1);
	} else if (!strcmp(k, "scroll_width_single")) {
		SET_NUM(c->scroll_width_single, v, 0, 1);
	} else if (!strcmp(k, "scroll_presets")) {
		char buf[256], *save = NULL;
		int n = 0;
		snprintf(buf, sizeof(buf), "%s", v);
		for (char *tok = strtok_r(buf, ",", &save); tok && n < (int)LENGTH(c->scroll_presets); tok = strtok_r(NULL, ",", &save)) {
			double p;
			if (parse_num(trim(tok), 0.1, 1, &p))
				c->scroll_presets[n++] = (float)p;
		}
		if (n)
			c->nscroll_presets = n;
	} else if (!strcmp(k, "scroll_center")) {
		if (!strcmp(v, "edge"))
			c->scroll_center = SCROLL_EDGE;
		else if (!strcmp(v, "always"))
			c->scroll_center = SCROLL_ALWAYS;
		else if (!strcmp(v, "outside"))
			c->scroll_center = SCROLL_OUTSIDE;
		else
			conf_error("config:%d: invalid scroll_center '%s', use edge, always or outside", line, v);
	} else if (!strcmp(k, "scroll_struts")) {
		SET_NUM(c->scroll_struts, v, 0, 1000);
	} else if (!strcmp(k, "overview_gap_inner")) {
		SET_NUM(c->overview_gap_inner, v, 0, 1000);
	} else if (!strcmp(k, "overview_gap_outer")) {
		SET_NUM(c->overview_gap_outer, v, 0, 1000);
	} else if (!strcmp(k, "hotarea_size")) {
		SET_NUM(c->hotarea_size, v, 0, 1000);
	} else if (!strcmp(k, "hotarea_corner")) {
		static const char *corners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
		size_t i = 0;
		while (i < LENGTH(corners) && strcmp(v, corners[i]))
			i++;
		if (i < LENGTH(corners))
			c->hotarea_corner = (enum hot_corner)i;
		else
			conf_error("config:%d: invalid hotarea_corner '%s'", line, v);
	} else if (!strcmp(k, "axis_timeout")) {
		SET_NUM(c->axis_timeout, v, 0, 1000);
	} else if (!strcmp(k, "animation_open") || !strcmp(k, "animation_close")) {
		if ((anim = parse_anim(v, line)) >= 0)
			*(k[10] == 'o' ? &c->anim_open : &c->anim_close) = (enum anim_kind)anim;
	} else if (!strcmp(k, "fade_in_opacity")) {
		SET_NUM(c->fade_in_opacity, v, 0, 1);
	} else if (!strcmp(k, "fade_out_opacity")) {
		SET_NUM(c->fade_out_opacity, v, 0, 1);
	} else if (!strcmp(k, "zoom_start")) {
		SET_NUM(c->zoom_start, v, 0.05, 1);
	} else if (!strcmp(k, "zoom_end")) {
		SET_NUM(c->zoom_end, v, 0.05, 1);
	} else if (!strcmp(k, "tag_animation")) {
		static const char *names[] = {"fade", "slide", "slide_vertical", "none"};
		size_t i = 0;
		while (i < LENGTH(names) && strcmp(v, names[i]))
			i++;
		if (i < LENGTH(names))
			c->tag_animation = (enum tag_anim)i;
		else
			conf_error("config:%d: invalid tag_animation '%s', use fade, slide, slide_vertical or none", line, v);
	} else if (!strcmp(k, "opacity_focused")) {
		SET_NUM(c->opacity_focused, v, 0.05, 1);
	} else if (!strcmp(k, "opacity_unfocused")) {
		SET_NUM(c->opacity_unfocused, v, 0.05, 1);
	} else if (!strcmp(k, "shadow_blur")) {
		SET_NUM(c->shadow_blur, v, 0, 100);
	} else if (!strcmp(k, "blur_noise")) {
		SET_NUM(c->blur_noise, v, 0, 1);
	} else if (!strcmp(k, "blur_brightness")) {
		SET_NUM(c->blur_brightness, v, 0, 2);
	} else if (!strcmp(k, "blur_contrast")) {
		SET_NUM(c->blur_contrast, v, 0, 2);
	} else if (!strcmp(k, "blur_saturation")) {
		SET_NUM(c->blur_saturation, v, 0, 2);
	} else if (!strcmp(k, "attach")) {
		if (!strcmp(v, "top"))
			c->attach = ATTACH_TOP;
		else if (!strcmp(v, "aside"))
			c->attach = ATTACH_ASIDE;
		else if (!strcmp(v, "bottom"))
			c->attach = ATTACH_BOTTOM;
		else
			conf_error("config:%d: invalid attach '%s'", line, v);
	} else if (!strcmp(k, "tag_names")) {
		parse_tag_names(c, v, line);
	} else if (!strcmp(k, "renderer")) {
		if (!strcmp(v, "vulkan"))
			c->renderer = RENDERER_VULKAN;
		else if (!strcmp(v, "gles"))
			c->renderer = RENDERER_GLES;
		else if (!strcmp(v, "pixman"))
			c->renderer = RENDERER_PIXMAN;
		else if (!strcmp(v, "auto"))
			c->renderer = RENDERER_AUTO;
		else
			conf_error("config:%d: invalid renderer '%s', use auto, vulkan, gles or pixman", line, v);
	} else if (!strcmp(k, "border_width")) {
		SET_NUM(c->border_width, v, 0, 100);
	} else if (!strcmp(k, "border_outer_width")) {
		SET_NUM(c->border_outer_width, v, 0, 100);
	} else if (!strcmp(k, "border_outer")) {
		if (!parse_color(v, c->border_outer))
			conf_error("config:%d: invalid color '%s'", line, v);
	} else if (!strcmp(k, "border_focus") || !strcmp(k, "border_normal") || !strcmp(k, "border_urgent")) {
		float *dst = k[7] == 'f' ? c->border_focus : k[7] == 'n' ? c->border_normal : c->border_urgent;
		if (!parse_color(v, dst))
			conf_error("config:%d: invalid color '%s'", line, v);
	} else if (!strcmp(k, "mfact")) {
		SET_NUM(c->mfact, v, 0.05, 0.95);
	} else if (!strcmp(k, "nmaster")) {
		SET_NUM(c->nmaster, v, 0, 100);
	} else if (!strcmp(k, "mouse_mod")) {
		uint32_t m = parse_mod(v);
		if (m)
			c->mouse_mod = m;
		else
			conf_error("config:%d: invalid mouse_mod '%s'", line, v);
	} else if (!strcmp(k, "cursor_theme")) {
		free(c->cursor_theme);
		c->cursor_theme = *v ? strdup(v) : NULL;
	} else if (!strcmp(k, "cursor_size")) {
		SET_NUM(c->cursor_size, v, 8, 256);
	} else if (!strcmp(k, "animation_duration")) {
		SET_NUM(c->animation_duration, v, 0, 2000);
	} else if (!strcmp(k, "shadow_size")) {
		SET_NUM(c->shadow_size, v, 0, 100);
	} else if (!strcmp(k, "shadow_color")) {
		if (!parse_color(v, c->shadow_color))
			conf_error("config:%d: invalid color '%s'", line, v);
	} else if (!strcmp(k, "corner_radius")) {
		SET_NUM(c->corner_radius, v, 0, 100);
	} else if (!strcmp(k, "blur_passes")) {
		SET_NUM(c->blur_passes, v, 0, 10);
	} else if (!strcmp(k, "blur_radius")) {
		SET_NUM(c->blur_radius, v, 1, 64);
	} else {
		conf_error("config:%d: unknown key '%s' in [general]", line, k);
	}
}

static bool parse_pointer(struct pointer_conf *p, const char *k, const char *v, int line)
{
	if (!strcmp(k, "tap")) {
		SET_BOOL(p->tap, v);
	} else if (!strcmp(k, "tap_drag")) {
		SET_BOOL(p->tap_drag, v);
	} else if (!strcmp(k, "dwt")) {
		SET_BOOL(p->dwt, v);
	} else if (!strcmp(k, "natural_scroll")) {
		SET_BOOL(p->natural_scroll, v);
	} else if (!strcmp(k, "left_handed")) {
		SET_BOOL(p->left_handed, v);
	} else if (!strcmp(k, "middle_emulation")) {
		SET_BOOL(p->middle_emulation, v);
	} else if (!strcmp(k, "drag_lock")) {
		SET_BOOL(p->drag_lock, v);
	} else if (!strcmp(k, "enabled")) {
		SET_BOOL(p->enabled, v);
	} else if (!strcmp(k, "accel")) {
		double a;
		if (parse_num(v, -1.0, 1.0, &a))
			p->accel = a, p->has_accel = true;
	} else if (!strcmp(k, "accel_profile")) {
		if (!strcmp(v, "flat"))
			p->accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
		else if (!strcmp(v, "adaptive"))
			p->accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
		else
			conf_error("config:%d: invalid accel_profile '%s'", line, v);
	} else if (!strcmp(k, "scroll_method")) {
		if (!strcmp(v, "two_finger"))
			p->scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;
		else if (!strcmp(v, "edge"))
			p->scroll_method = LIBINPUT_CONFIG_SCROLL_EDGE;
		else if (!strcmp(v, "button"))
			p->scroll_method = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
		else if (!strcmp(v, "none"))
			p->scroll_method = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
		else
			conf_error("config:%d: invalid scroll_method '%s'", line, v);
	} else if (!strcmp(k, "click_method")) {
		if (!strcmp(v, "button_areas"))
			p->click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
		else if (!strcmp(v, "clickfinger"))
			p->click_method = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
		else if (!strcmp(v, "none"))
			p->click_method = LIBINPUT_CONFIG_CLICK_METHOD_NONE;
		else
			conf_error("config:%d: invalid click_method '%s'", line, v);
	} else {
		return false;
	}
	return true;
}

static void parse_input(struct config *c, const char *k, const char *v, int line)
{
	char **str = NULL;
	if (!strcmp(k, "kb_layout"))
		str = &c->kb_layout;
	else if (!strcmp(k, "kb_variant"))
		str = &c->kb_variant;
	else if (!strcmp(k, "kb_model"))
		str = &c->kb_model;
	else if (!strcmp(k, "kb_options"))
		str = &c->kb_options;
	if (str)
		set_str(str, v);
	else if (!strcmp(k, "repeat_rate")) {
		SET_NUM(c->repeat_rate, v, 0, 1000);
	} else if (!strcmp(k, "repeat_delay")) {
		SET_NUM(c->repeat_delay, v, 0, 10000);
	} else if (!strcmp(k, "gesture_fingers")) {
		SET_NUM(c->gesture_fingers, v, 0, 5);
	} else if (!strcmp(k, "swipe_threshold")) {
		SET_NUM(c->swipe_threshold, v, 1, 2000);
	} else if (!strcmp(k, "numlock")) {
		SET_BOOL(c->numlock, v);
	} else if (!parse_pointer(&c->pointer, k, v, line)) {
		conf_error("config:%d: unknown key '%s' in [input]", line, k);
	}
}

static void parse_output(struct output_conf *o, const char *k, const char *v, int line)
{
	if (!strcmp(k, "mode")) {
		o->refresh = 0;
		int n = 0;
		char *end = NULL;
		bool ok = sscanf(v, "%dx%d%n", &o->width, &o->height, &n) == 2;
		if (ok && v[n] == '@') {
			o->refresh = strtof(v + n + 1, &end);
			ok = end != v + n + 1 && (!*end || !strcasecmp(end, "hz"));
		} else if (ok)
			ok = !v[n];
		if (!ok || o->width <= 0 || o->height <= 0 ||
				o->width > 16384 || o->height > 16384 || !(o->refresh >= 0 && o->refresh <= 1000)) {
			conf_error("config:%d: invalid mode '%s'", line, v);
			o->width = o->height = 0;
			o->refresh = 0;
		}
	} else if (!strcmp(k, "scale")) {
		double s;
		if (parse_num(v, 0.0, 10.0, &s))
			o->scale = s > 0 && s < 0.25 ? 0.25f : (float)s;
	} else if (!strcmp(k, "position")) {
		int n = 0;
		o->has_pos = sscanf(v, "%d,%d%n", &o->x, &o->y, &n) == 2 && !v[n] && o->x > -1000000 && o->x < 1000000 &&
			o->y > -1000000 && o->y < 1000000;
		if (!o->has_pos)
			conf_error("config:%d: invalid position '%s'", line, v);
	} else if (!strcmp(k, "transform")) {
		o->transform = parse_transform(v);
		if (o->transform < 0)
			conf_error("config:%d: invalid transform '%s'", line, v);
	} else if (!strcmp(k, "vrr")) {
		int b = bool_of(v);
		o->vrr = !strcmp(v, "fullscreen") ? VRR_FULLSCREEN : !strcmp(v, "content") ? VRR_CONTENT : b == 1 ? VRR_ON : VRR_OFF;
		if (b < 0 && o->vrr == VRR_OFF) {
			conf_error("config:%d: invalid vrr '%s', use on, off, fullscreen or content", line, v);
			o->vrr = -1;
		}
#ifndef WITH_PRESENTATION
		if (o->vrr == VRR_CONTENT)
			conf_error("config:%d: vrr = content is not built in, use fullscreen", line);
#endif
	} else if (!strcmp(k, "layout")) {
		o->layout = layout_from_name(v);
		if (o->layout < 0)
			conf_error("config:%d: invalid layout '%s'", line, v);
	} else if (!strcmp(k, "tearing")) {
		SET_BOOL(o->tearing, v);
	} else if (!strcmp(k, "enabled")) {
		SET_BOOL(o->enabled, v);
	} else if (!strcmp(k, "direct_scanout")) {
		SET_BOOL(o->direct_scanout, v);
	} else if (!strcmp(k, "hardware_cursor")) {
		if (!strcmp(v, "auto"))
			o->hardware_cursor = 2;
		else
			SET_BOOL(o->hardware_cursor, v);
	} else if (!strcmp(k, "primary")) {
		SET_BOOL(o->primary, v);
	} else if (!strcmp(k, "tag_layouts")) {
		for (int i = 0; i < TAGCOUNT; i++) {
			size_t n = strcspn(v, ",");
			char name[32];
			snprintf(name, sizeof(name), "%.*s", (int)MIN(n, sizeof(name) - 1), v);
			char *t = trim(name);
			o->tag_layout[i] = *t ? layout_from_name(t) : -1;
			if (*t && o->tag_layout[i] < 0)
				conf_error("config:%d: invalid layout '%s'", line, t);
			if (!v[n])
				break;
			v += n + 1;
		}
	} else {
		conf_error("config:%d: unknown key '%s' in [output]", line, k);
	}
}

static void parse_rule(struct rule *r, const char *k, const char *v, int line)
{
	if (!strcmp(k, "app_id")) {
		set_str(&r->app_id, v);
	} else if (!strcmp(k, "title")) {
		set_str(&r->title, v);
	} else if (!strcmp(k, "floating")) {
		SET_BOOL(r->floating, v);
	} else if (!strcmp(k, "tags")) {
		r->tags = parse_tags(v);
	} else if (!strcmp(k, "output")) {
		set_str(&r->output, v);
	} else if (!strcmp(k, "fullscreen")) {
		SET_BOOL(r->fullscreen, v);
#ifdef WITH_SCRATCHPAD
	} else if (!strcmp(k, "scratchpad")) {
		SET_BOOL(r->scratchpad, v);
#endif
#ifdef WITH_STICKY
	} else if (!strcmp(k, "sticky")) {
		SET_BOOL(r->sticky, v);
#endif
	} else if (!strcmp(k, "focus")) {
		SET_BOOL(r->focus, v);
	} else {
		conf_error("config:%d: unknown key '%s' in [rule]", line, k);
	}
}

static void parse_layer_rule(struct layer_rule *r, const char *k, const char *v, int line)
{
	int a;
	if (!strcmp(k, "namespace"))
		set_str(&r->namespace, v);
	else if (!strcmp(k, "animations"))
		SET_BOOL(r->animations, v);
	else if (!strcmp(k, "blur"))
		SET_BOOL(r->blur, v);
	else if (!strcmp(k, "shadow"))
		SET_BOOL(r->shadow, v);
	else if (!strcmp(k, "animation_open") && (a = parse_anim(v, line)) >= 0)
		r->anim_open = a;
	else if (!strcmp(k, "animation_close") && (a = parse_anim(v, line)) >= 0)
		r->anim_close = a;
	else if (strcmp(k, "animation_open") && strcmp(k, "animation_close"))
		conf_error("config:%d: unknown key '%s' in [layer]", line, k);
}

void config_layer(const char *namespace, struct layer_rule *out)
{
	*out = (struct layer_rule){NULL, srv.conf.layer_animations, srv.conf.blur_layers, srv.conf.shadow_layers,
		srv.conf.anim_open, srv.conf.anim_close};
	for (size_t i = 0; i < srv.conf.nlayer_rules; i++) {
		const struct layer_rule *r = &srv.conf.layer_rules[i];
		if (r->namespace && (!namespace || !strstr(namespace, r->namespace)))
			continue;
		if (r->animations >= 0)
			out->animations = r->animations;
		if (r->blur >= 0)
			out->blur = r->blur;
		if (r->shadow >= 0)
			out->shadow = r->shadow;
		if (r->anim_open >= 0)
			out->anim_open = r->anim_open;
		if (r->anim_close >= 0)
			out->anim_close = r->anim_close;
	}
}

static void add_env(struct config *c, const char *k, const char *v, int line)
{
	if (!*k || strchr(k, '=')) {
		conf_error("config:%d: invalid variable name '%s'", line, k);
		return;
	}
	for (size_t i = 0; i < c->nenv; i++)
		if (!strcmp(c->env[i].key, k)) {
			set_str(&c->env[i].value, v);
			return;
		}
	c->env = grow(c->env, &c->nenv, sizeof(*c->env));
	c->env[c->nenv - 1].key = strdup(k);
	c->env[c->nenv - 1].value = strdup(v);
}

static enum section open_section(struct config *c, char *name, int line)
{
	char *arg = strchr(name, ' ');
	if (arg)
		*arg++ = '\0', arg = trim(arg);
	if (!strcmp(name, "general"))
		return SEC_GENERAL;
	if (!strcmp(name, "input")) {
		if (!arg || !*arg)
			return SEC_INPUT;
		if (!strcmp(arg, "touchpad"))
			return SEC_TOUCHPAD;
		if (!strcmp(arg, "pointer"))
			return SEC_MOUSE;
		conf_error("config:%d: unknown input class '%s', use touchpad or pointer", line, arg);
		return SEC_NONE;
	}
	if (!strcmp(name, "autostart"))
		return SEC_AUTOSTART;
	if (!strcmp(name, "bind"))
		return SEC_BIND;
	if (!strcmp(name, "env"))
		return SEC_ENV;
	if (!strcmp(name, "rule")) {
		c->rules = grow(c->rules, &c->nrules, sizeof(*c->rules));
		c->rules[c->nrules - 1].floating = -1;
		c->rules[c->nrules - 1].fullscreen = -1;
		c->rules[c->nrules - 1].scratchpad = -1;
		c->rules[c->nrules - 1].sticky = -1;
		return SEC_RULE;
	}
	if (!strcmp(name, "output")) {
		c->outputs = grow(c->outputs, &c->noutputs, sizeof(*c->outputs));
		struct output_conf *o = &c->outputs[c->noutputs - 1];
		o->name = strdup(arg && *arg ? arg : "*");
		o->transform = o->vrr = o->tearing = o->enabled = o->direct_scanout = o->hardware_cursor = o->layout = -1;
		o->primary = -1;
		for (int i = 0; i < TAGCOUNT; i++)
			o->tag_layout[i] = -1;
		return SEC_OUTPUT;
	}
	if (!strcmp(name, "layer")) {
		c->layer_rules = grow(c->layer_rules, &c->nlayer_rules, sizeof(*c->layer_rules));
		struct layer_rule *r = &c->layer_rules[c->nlayer_rules - 1];
		r->animations = r->blur = r->shadow = r->anim_open = r->anim_close = -1;
		return SEC_LAYER;
	}
	conf_error("config:%d: unknown section '%s'", line, name);
	return SEC_NONE;
}

bool config_user_path(char *buf, size_t len)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg)
		snprintf(buf, len, "%s/swizzle/swizzle.conf", xdg);
	else if (home)
		snprintf(buf, len, "%s/.config/swizzle/swizzle.conf", home);
	else
		return false;
	return true;
}

char *config_default_path(void)
{
	char buf[4096];
	if (config_user_path(buf, sizeof(buf)) && access(buf, R_OK) == 0)
		return strdup(buf);
	static const char *system[] = {"/etc/swizzle/swizzle.conf", SWIZZLE_DATADIR "/swizzle.conf"};
	for (size_t i = 0; i < LENGTH(system); i++)
		if (access(system[i], R_OK) == 0)
			return strdup(system[i]);
	return NULL;
}

bool config_load(struct config *c, const char *path)
{
	set_defaults(c);
	nerrors = 0;
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f) {
		if (path) {
			wlr_log(WLR_ERROR, "cannot open config %s: %s", path, strerror(errno));
			c->path = strdup(path);
		}
		wlr_log(WLR_INFO, "using built-in fallback bindings");
		for (size_t i = 0; i < LENGTH(fallback_binds); i++)
			add_bind(c, fallback_binds[i][0], fallback_binds[i][1], 0);
		return false;
	}
	c->path = strdup(path);

	char *raw = NULL;
	size_t cap = 0;
	int line = 0;
	enum section sec = SEC_NONE;
	while (getline(&raw, &cap, f) > 0) {
		cur_line = ++line;
		char *s = raw;
		if (line == 1 && !strncmp(s, "\xef\xbb\xbf", 3))
			s += 3;
		s = trim(s);
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '[') {
			char *end = strchr(s, ']');
			if (!end) {
				conf_error("config:%d: unterminated section", line);
				sec = SEC_NONE;
				continue;
			}
			*end = '\0';
			sec = open_section(c, trim(s + 1), line);
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) {
			conf_error("config:%d: expected key = value", line);
			continue;
		}
		*eq = '\0';
		char *k = trim(s), *v = trim(eq + 1);
		switch (sec) {
		case SEC_GENERAL:
			parse_general(c, k, v, line);
			break;
		case SEC_INPUT:
			parse_input(c, k, v, line);
			break;
		case SEC_OUTPUT:
			parse_output(&c->outputs[c->noutputs - 1], k, v, line);
			break;
		case SEC_RULE:
			parse_rule(&c->rules[c->nrules - 1], k, v, line);
			break;
		case SEC_LAYER:
			parse_layer_rule(&c->layer_rules[c->nlayer_rules - 1], k, v, line);
			break;
		case SEC_AUTOSTART:
			c->autostart = grow(c->autostart, &c->nautostart, sizeof(*c->autostart));
			c->autostart[c->nautostart - 1] = strdup(v);
			break;
		case SEC_BIND:
			add_bind(c, k, v, line);
			break;
		case SEC_ENV:
			add_env(c, k, v, line);
			break;
		case SEC_TOUCHPAD:
		case SEC_MOUSE:
			if (!parse_pointer(sec == SEC_TOUCHPAD ? &c->touchpad : &c->mouse, k, v, line))
				conf_error("config:%d: unknown key '%s' in [input %s]", line, k,
					sec == SEC_TOUCHPAD ? "touchpad" : "pointer");
			break;
		case SEC_NONE:
			conf_error("config:%d: key outside of a section", line);
			break;
		}
	}
	bool failed = ferror(f);
	int err = errno;
	free(raw);
	fclose(f);
	if (failed) {
		wlr_log(WLR_ERROR, "cannot read config %s: %s", path, strerror(err));
		config_free(c);
		return config_load(c, NULL);
	}
	c->errors = nerrors;
	return true;
}

void config_free(struct config *c)
{
	free(c->path);
	free(c->cursor_theme);
	free(c->kb_layout);
	free(c->kb_variant);
	free(c->kb_model);
	free(c->kb_options);
	for (size_t i = 0; i < TAGCOUNT; i++)
		free(c->tag_names[i]);
	free(c->lid_close);
	free(c->lid_open);
	for (size_t i = 0; i < c->noutputs; i++)
		free(c->outputs[i].name);
	free(c->outputs);
	for (size_t i = 0; i < c->nrules; i++) {
		free(c->rules[i].app_id);
		free(c->rules[i].title);
		free(c->rules[i].output);
	}
	free(c->rules);
	for (size_t i = 0; i < c->nlayer_rules; i++)
		free(c->layer_rules[i].namespace);
	free(c->layer_rules);
	for (size_t i = 0; i < c->nbinds; i++)
		free(c->binds[i].cmd);
	free(c->binds);
	for (size_t i = 0; i < c->nautostart; i++)
		free(c->autostart[i]);
	free(c->autostart);
	for (size_t i = 0; i < c->nenv; i++) {
		free(c->env[i].key);
		free(c->env[i].value);
	}
	free(c->env);
	memset(c, 0, sizeof(*c));
}

void config_output(const char *name, struct output_conf *out)
{
	struct output_conf merged;
	memset(&merged, 0, sizeof(merged));
	merged.transform = merged.enabled = -1;
	merged.vrr = VRR_FULLSCREEN;
	merged.tearing = 0;
	merged.direct_scanout = 0;
	merged.hardware_cursor = 2;
	merged.layout = 0;
	merged.primary = 0;
	for (int t = 0; t < TAGCOUNT; t++)
		merged.tag_layout[t] = -1;
	for (size_t i = 0; i < 2 * srv.conf.noutputs; i++) {
		const struct output_conf *o = &srv.conf.outputs[i % srv.conf.noutputs];
		if (strcmp(o->name, i < srv.conf.noutputs ? "*" : name))
			continue;
		if (o->width)
			merged.width = o->width, merged.height = o->height, merged.refresh = o->refresh;
		if (o->scale > 0)
			merged.scale = o->scale;
		if (o->has_pos)
			merged.has_pos = true, merged.x = o->x, merged.y = o->y;
		if (o->transform >= 0)
			merged.transform = o->transform;
		if (o->vrr >= 0)
			merged.vrr = o->vrr;
		if (o->tearing >= 0)
			merged.tearing = o->tearing;
		if (o->enabled >= 0)
			merged.enabled = o->enabled;
		if (o->direct_scanout >= 0)
			merged.direct_scanout = o->direct_scanout;
		if (o->hardware_cursor >= 0)
			merged.hardware_cursor = o->hardware_cursor;
		if (o->layout >= 0)
			merged.layout = o->layout;
		if (o->primary >= 0)
			merged.primary = o->primary;
		for (int t = 0; t < TAGCOUNT; t++)
			if (o->tag_layout[t] >= 0)
				merged.tag_layout[t] = o->tag_layout[t];
	}
	*out = merged;
	out->name = NULL;
}

static struct env_var *orig_env;
static size_t norig_env;

static struct env_var *orig_of(const char *key)
{
	for (size_t i = 0; i < norig_env; i++)
		if (!strcmp(orig_env[i].key, key))
			return &orig_env[i];
	return NULL;
}

static void remember_env(const char *key)
{
	if (orig_of(key))
		return;
	const char *v = getenv(key);
	orig_env = grow(orig_env, &norig_env, sizeof(*orig_env));
	orig_env[norig_env - 1].key = strdup(key);
	orig_env[norig_env - 1].value = v ? strdup(v) : NULL;
}

void config_apply_env(const struct config *old, const struct config *conf)
{
	for (size_t i = 0; old && i < old->nenv; i++) {
		bool kept = false;
		for (size_t j = 0; j < conf->nenv && !kept; j++)
			kept = !strcmp(old->env[i].key, conf->env[j].key);
		if (kept)
			continue;
		const struct env_var *o = orig_of(old->env[i].key);
		if (o && o->value)
			setenv(o->key, o->value, 1);
		else
			unsetenv(old->env[i].key);
	}
	for (size_t i = 0; i < conf->nenv; i++) {
		remember_env(conf->env[i].key);
		if (setenv(conf->env[i].key, conf->env[i].value, 1) < 0)
			wlr_log_errno(WLR_ERROR, "setenv %s", conf->env[i].key);
	}
}
