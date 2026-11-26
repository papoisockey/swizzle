#ifdef __linux__
#define _GNU_SOURCE
#else
#undef _POSIX_C_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "swizzle.h"

typedef bool (*action_fn)(const char *arg, char *err, size_t errlen);

static struct client *target;

static struct client *selected(void)
{
	return target ? target : client_focused();
}

static bool reload_config(char *err, size_t len)
{
	struct config next;
	char *path = srv.conf_arg ? strdup(srv.conf_arg) : config_default_path();
	bool ok = config_load(&next, path);
	free(path);
	if (!ok) {
		config_free(&next);
		snprintf(err, len, "config reload failed");
		return false;
	}
	if (next.renderer != srv.conf.renderer)
		wlr_log(WLR_INFO, "renderer change takes effect after restart");
	bool mfact = next.mfact != srv.conf.mfact, nmaster = next.nmaster != srv.conf.nmaster;
	config_apply_env(&srv.conf, &next);
	config_free(&srv.conf);
	srv.conf = next;
	output_update_tag_names();

	input_apply_config();
	portal_export();
	struct monitor *m;
	wl_list_for_each(m, &srv.monitors, link) {
		for (int t = 0; t <= TAGCOUNT; t++) {
			if (mfact)
				m->tag_mfact[t] = srv.conf.mfact;
			if (nmaster)
				m->tag_nmaster[t] = srv.conf.nmaster;
		}
		if (mfact)
			m->mfact = srv.conf.mfact;
		if (nmaster)
			m->nmaster = srv.conf.nmaster;
		int layout = m->layout;
		output_apply_config(m);
		if (m->layout != layout)
			ipc_event("layout %s %s", m->output->name, layout_name(m->layout));
	}
	client_update_borders();
	effects_apply_config();
	output_update_all();
	ipc_event("reload");
	wlr_log(WLR_INFO, "config reloaded");
	if (srv.conf.errors) {
		snprintf(err, len, "config reloaded with %d errors, see swizzle --validate", srv.conf.errors);
		return false;
	}
	return true;
}

static int dir_arg(const char *arg)
{
	return !arg || !strcmp(arg, "next") ? 1 : !strcmp(arg, "prev") ? -1 : 0;
}

static uint32_t tag_arg(const char *arg, bool word)
{
	char *end;
	if (!arg)
		return 0;
	if (!strncmp(arg, "all", 3) && (!arg[3] || (word && (arg[3] == ' ' || arg[3] == '\t'))))
		return tag_mask();
	long n = strtol(arg, &end, 10);
	if (end == arg || (*end && (!word || (*end != ' ' && *end != '\t'))))
		return 0;
	return n >= 1 && n <= srv.conf.tag_count ? 1u << (n - 1) : 0;
}

static uint32_t valid_tag(const char *arg, bool word, char *err, size_t len)
{
	uint32_t t = tag_arg(arg, word);
	if (!t)
		snprintf(err, len, "invalid tag '%s'", arg ? arg : "");
	return t;
}

static bool a_spawn(const char *arg, char *err, size_t len)
{
	if (!arg || !*arg) {
		snprintf(err, len, "spawn needs a command");
		return false;
	}
	if (spawn(arg) < 0) {
		snprintf(err, len, "cannot spawn '%s'", arg);
		return false;
	}
	return true;
}

static bool a_close(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	if (c)
		client_close(c);
	return true;
}

static bool a_quit(const char *arg, char *err, size_t len)
{
	quit();
	return true;
}

static bool a_reload(const char *arg, char *err, size_t len)
{
	return reload_config(err, len);
}

static const struct {
	const char *name;
	int dx, dy;
} dirs[] = {{"left", -1, 0}, {"right", 1, 0}, {"up", 0, -1}, {"down", 0, 1}};

static bool dir_of(const char *arg, int *dx, int *dy)
{
	for (size_t i = 0; arg && i < LENGTH(dirs); i++)
		if (!strcmp(arg, dirs[i].name)) {
			*dx = dirs[i].dx, *dy = dirs[i].dy;
			return true;
		}
	return false;
}

static bool a_focus(const char *arg, char *err, size_t len)
{
	int dx, dy, d = dir_arg(arg);
	if (dir_of(arg, &dx, &dy))
		focus_dir(dx, dy);
	else if (d)
		focus_stack(d);
	else {
		snprintf(err, len, "focus needs next, prev, left, right, up or down");
		return false;
	}
	return true;
}

static bool a_swap(const char *arg, char *err, size_t len)
{
	int dx, dy;
	if (!dir_of(arg, &dx, &dy)) {
		snprintf(err, len, "swap needs left, right, up or down");
		return false;
	}
	swap_dir(dx, dy);
	return true;
}

static bool a_layout(const char *arg, char *err, size_t len)
{
	struct monitor *m = srv.selmon;
	if (!m)
		return true;
	int n = layout_count();
	int l = !arg || !strcmp(arg, "next") ? (m->layout + 1) % n :
		!strcmp(arg, "prev") ? (m->layout + n - 1) % n : layout_from_name(arg);
	if (l < 0) {
		snprintf(err, len, "layout '%s' is unknown or not built in", arg);
		return false;
	}
	m->layout = l;
	arrange(m);
	ipc_event("layout %s %s", m->output->name, layout_name(m->layout));
	return true;
}

#if defined(WITH_SCROLL) || defined(WITH_VERTICAL_SCROLL)
static bool a_width(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	char *end = NULL;
	bool step = !arg || !strcmp(arg, "next") || !strcmp(arg, "prev");
	double w = step ? 0 : strtod(arg, &end);
	if (!step && (end == arg || *end || !(w >= 0.1 && w <= 1))) {
		snprintf(err, len, "width needs next, prev or a share from 0.1 to 1");
		return false;
	}
	if (!c)
		return true;
	scroll_width(c, arg);
	arrange(c->mon);
	return true;
}
#endif

#ifdef WITH_STICKY
static bool a_sticky(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	if (!c)
		return true;
	bool was_focused = c == client_focused();
	bool retag = c->sticky && c->mon && !(c->tags & c->mon->tags);
	if (retag)
		client_leave_column(c);
	c->sticky = !c->sticky;
	if (retag) {
		c->tags = c->mon->tags;
		client_join_strip(c);
	}
	client_update_border(c);
	arrange(c->mon);
	if (was_focused && !client_reachable(c))
		focus_top(c->mon);
	return true;
}
#endif

static bool a_gaps(const char *arg, char *err, size_t len)
{
	char *end = NULL;
	long step = arg && (*arg == '+' || *arg == '-') ? strtol(arg, &end, 10) : 0;
	if (!arg || !strcmp(arg, "toggle"))
		srv.gaps_off = !srv.gaps_off;
	else if (!strcmp(arg, "on"))
		srv.gaps_off = false;
	else if (!strcmp(arg, "off"))
		srv.gaps_off = true;
	else if (end && end != arg + 1 && !*end)
		srv.gaps_delta = (int)MAX(-500, MIN(500, srv.gaps_delta + MAX(-500, MIN(500, step))));
	else {
		snprintf(err, len, "gaps needs on, off, toggle, +N or -N");
		return false;
	}
	arrange_all();
	return true;
}

static bool a_activate(const char *arg, char *err, size_t len)
{
	client_reveal(selected());
	return true;
}

static bool a_focusmon(const char *arg, char *err, size_t len)
{
	if (!dir_arg(arg)) {
		snprintf(err, len, "focusmon needs next or prev");
		return false;
	}
	struct monitor *m = output_dir(srv.selmon, dir_arg(arg));
	if (m && m != srv.selmon) {
		srv.selmon = m;
		focus_top(m);
	}
	return true;
}

static bool a_tagmon(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	if (!dir_arg(arg)) {
		snprintf(err, len, "tagmon needs next or prev");
		return false;
	}
	struct monitor *m = c ? output_dir(c->mon, dir_arg(arg)) : NULL;
	if (c && m && m != c->mon) {
		bool focused = c == client_focused();
		client_send_to_monitor(c, m, m->tags);
		if (!target)
			focus_top(srv.selmon);
		else if (focused) {
			srv.selmon = m;
			if (client_visible(c))
				focus_client(c, true);
			else
				focus_top(m);
		}
	}
	return true;
}

static bool a_view(const char *arg, char *err, size_t len)
{
	if (arg && !strcmp(arg, "prev")) {
		view(0);
		return true;
	}
	uint32_t t = valid_tag(arg, false, err, len);
	if (!t)
		return false;
	if (srv.conf.tag_back_and_forth && srv.selmon && t == srv.selmon->tags)
		t = 0;
	view(t);
	return true;
}

static bool step_tags(int dir, const char *arg, char *err, size_t len)
{
	if (arg && strcmp(arg, "occupied")) {
		snprintf(err, len, "takes no argument or occupied");
		return false;
	}
	view(tags_shift(dir, arg != NULL));
	return true;
}

static bool a_viewnext(const char *arg, char *err, size_t len)
{
	return step_tags(1, arg, err, len);
}

static bool a_viewprev(const char *arg, char *err, size_t len)
{
	return step_tags(-1, arg, err, len);
}

#ifdef WITH_SCRATCHPAD
static bool a_scratchpad(const char *arg, char *err, size_t len)
{
	if (!arg) {
		if (target) {
			snprintf(err, len, "only 'scratchpad move' can target a window");
			return false;
		}
		scratch_toggle();
		return true;
	}
	if (strcmp(arg, "move")) {
		snprintf(err, len, "scratchpad takes no argument or move");
		return false;
	}
	struct client *c = selected();
	scratch_set(c, c && !c->scratch);
	return true;
}
#endif

static bool a_toggleview(const char *arg, char *err, size_t len)
{
	uint32_t t = valid_tag(arg, false, err, len);
	if (t)
		toggle_view(t);
	return t != 0;
}

static bool a_tag(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	int d = arg && (!strcmp(arg, "next") || !strcmp(arg, "prev")) ? dir_arg(arg) : 0;
	bool follow = !target || c == client_focused();
	if (d) {
		tag_step(c, d, follow);
		return true;
	}
	const char *word = arg ? arg + strcspn(arg, " \t") : NULL;
	while (word && (*word == ' ' || *word == '\t'))
		word++;
	if (word && *word && strcmp(word, "follow")) {
		snprintf(err, len, "tag takes N, all, next, prev or N follow");
		return false;
	}
	uint32_t t = valid_tag(arg, true, err, len);
	if (!t)
		return false;
	tag(c, t);
	if (word && *word && c && c->mon && follow) {
		srv.selmon = c->mon;
		view(t);
		if (client_visible(c))
			focus_client(c, true);
	}
	return true;
}

static bool a_minimize(const char *arg, char *err, size_t len)
{
	client_set_minimized(selected(), true);
	return true;
}

static bool a_restore(const char *arg, char *err, size_t len)
{
	if (target) {
		client_reveal(target);
		return true;
	}
	client_restore();
	return true;
}

static bool a_maximize(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	if (c)
		client_set_maximized(c, !c->maximized);
	return true;
}

static bool a_fakefullscreen(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	if (c)
		client_set_fake_fullscreen(c, !c->fake_fullscreen);
	return true;
}

static bool a_overlay(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	if (c)
		client_set_overlay(c, !c->overlay);
	return true;
}

static bool nudge(const char *arg, char *err, size_t len, bool resize)
{
	char *e1 = NULL, *e2 = NULL;
	long a = arg ? strtol(arg, &e1, 10) : 0, b = e1 && e1 != arg ? strtol(e1, &e2, 10) : 0;
	while (e2 && (*e2 == ' ' || *e2 == '\t'))
		e2++;
	if (!e2 || e2 == e1 || *e2 || a < -10000 || a > 10000 || b < -10000 || b > 10000) {
		snprintf(err, len, "%s needs two numbers of pixels such as -50 0", resize ? "resize" : "move");
		return false;
	}
	client_nudge(selected(), resize ? 0 : (int)a, resize ? 0 : (int)b, resize ? (int)a : 0, resize ? (int)b : 0);
	return true;
}

static bool a_move(const char *arg, char *err, size_t len)
{
	return nudge(arg, err, len, false);
}

static bool a_resize(const char *arg, char *err, size_t len)
{
	return nudge(arg, err, len, true);
}

static bool a_stack(const char *arg, char *err, size_t len)
{
	int dx, dy;
	if (!dir_of(arg, &dx, &dy)) {
		snprintf(err, len, "stack needs left, right, up or down");
		return false;
	}
	scroll_stack(dx, dy);
	return true;
}

static bool a_split(const char *arg, char *err, size_t len)
{
	dwindle_toggle_split();
	return true;
}

#ifdef WITH_OVERVIEW
static bool a_overview(const char *arg, char *err, size_t len)
{
	struct monitor *m = srv.selmon;
	if (!m)
		return true;
	if (!arg || !strcmp(arg, "toggle"))
		overview_set(m, !m->overview);
	else if (!strcmp(arg, "on") || !strcmp(arg, "off"))
		overview_set(m, arg[1] == 'n');
	else {
		snprintf(err, len, "overview needs on, off or toggle");
		return false;
	}
	return true;
}
#endif

static bool a_toggletag(const char *arg, char *err, size_t len)
{
	uint32_t t = valid_tag(arg, false, err, len);
	struct client *c = selected();
	if (!t)
		return false;
	if (c && !((c->tags ^ t) & tag_mask())) {
		snprintf(err, len, "a window needs at least one tag");
		return false;
	}
	if (c)
		tag(c, c->tags ^ t);
	return true;
}

static bool a_zoom(const char *arg, char *err, size_t len)
{
	zoom();
	return true;
}

static bool a_floating(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	if (c && !c->scratch)
		client_set_floating(c, !c->floating);
	return true;
}

static bool a_fullscreen(const char *arg, char *err, size_t len)
{
	struct client *c = selected();
	if (c)
		client_set_fullscreen(c, !c->fullscreen);
	return true;
}

static bool a_nmaster(const char *arg, char *err, size_t len)
{
	char *end;
	long step = arg ? strtol(arg, &end, 10) : 0;
	if (!arg || end == arg || *end) {
		snprintf(err, len, "nmaster needs a step such as +1 or -1");
		return false;
	}
	if (!srv.selmon)
		return true;
	srv.selmon->nmaster = MIN(100, MAX(0, srv.selmon->nmaster + MIN(100, MAX(-100, step))));
	arrange(srv.selmon);
	return true;
}

static bool a_mfact(const char *arg, char *err, size_t len)
{
	char *end;
	float step = arg ? strtof(arg, &end) : 0;
	if (!arg || end == arg || *end || step != step) {
		snprintf(err, len, "mfact needs a step such as +0.05 or -0.05");
		return false;
	}
	if (!srv.selmon)
		return true;
	srv.selmon->mfact = MAX(0.05f, MIN(0.95f, srv.selmon->mfact + step));
	arrange(srv.selmon);
	return true;
}

#ifdef WITH_TARGETS
static bool a_on(const char *arg, char *err, size_t len);
#endif

static const struct {
	const char *name;
	action_fn fn;
	bool targeted;
} actions[] = {
	{"spawn", a_spawn, false},
	{"close", a_close, true},
	{"quit", a_quit, false},
	{"reload", a_reload, false},
	{"focus", a_focus, false},
	{"swap", a_swap, false},
	{"focusmon", a_focusmon, false},
	{"tagmon", a_tagmon, true},
	{"view", a_view, false},
	{"viewnext", a_viewnext, false},
	{"viewprev", a_viewprev, false},
	{"toggleview", a_toggleview, false},
	{"tag", a_tag, true},
	{"toggletag", a_toggletag, true},
	{"zoom", a_zoom, false},
	{"floating", a_floating, true},
	{"fullscreen", a_fullscreen, true},
	{"activate", a_activate, true},
#ifdef WITH_SCRATCHPAD
	{"scratchpad", a_scratchpad, true},
#else
	{"scratchpad", NULL, true},
#endif
	{"nmaster", a_nmaster, false},
	{"mfact", a_mfact, false},
	{"layout", a_layout, false},
#if defined(WITH_SCROLL) || defined(WITH_VERTICAL_SCROLL)
	{"width", a_width, true},
#else
	{"width", NULL, true},
#endif
#ifdef WITH_STICKY
	{"sticky", a_sticky, true},
#else
	{"sticky", NULL, true},
#endif
	{"gaps", a_gaps, false},
	{"minimize", a_minimize, true},
	{"restore", a_restore, true},
	{"maximize", a_maximize, true},
	{"fakefullscreen", a_fakefullscreen, true},
	{"overlay", a_overlay, true},
	{"move", a_move, true},
	{"resize", a_resize, true},
	{"stack", a_stack, false},
	{"split", a_split, false},
#ifdef WITH_OVERVIEW
	{"overview", a_overview, false},
#else
	{"overview", NULL, false},
#endif
#ifdef WITH_TARGETS
	{"on", a_on, false},
#else
	{"on", NULL, false},
#endif
};

static int find_action(const char *cmd, const char **arg, char *err, size_t errlen)
{
	char name[32];
	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	size_t n = strcspn(cmd, " \t");
	if (n == 0 || n >= sizeof(name)) {
		snprintf(err, errlen, "empty or invalid command");
		return -1;
	}
	memcpy(name, cmd, n);
	name[n] = '\0';
	const char *a = cmd + n;
	while (*a == ' ' || *a == '\t')
		a++;
	*arg = *a ? a : NULL;
	for (size_t i = 0; i < LENGTH(actions); i++)
		if (!strcmp(actions[i].name, name))
			return (int)i;
	snprintf(err, errlen, "unknown command '%s'", name);
	return -1;
}

#ifdef WITH_TARGETS
static bool a_on(const char *arg, char *err, size_t len)
{
	char app_id[256];
	const char *sub;
	if (!arg) {
		snprintf(err, len, "usage: on <app_id> <command>");
		return false;
	}
	size_t n = strcspn(arg, " \t");
	if (n >= sizeof(app_id)) {
		snprintf(err, len, "app_id too long");
		return false;
	}
	memcpy(app_id, arg, n);
	app_id[n] = '\0';
	int i = find_action(arg + n, &sub, err, len);
	if (i < 0)
		return false;
	if (!actions[i].fn) {
		snprintf(err, len, "'%s' is not built in", actions[i].name);
		return false;
	}
	if (!actions[i].targeted) {
		snprintf(err, len, "'%s' cannot target a window", actions[i].name);
		return false;
	}
	struct client *c, **hits;
	size_t nhits = 0;
	bool ok = true;
	wl_list_for_each(c, &srv.clients, link)
		if (c->mapped && !strcmp(client_app_id(c), app_id))
			nhits++;
	if (!nhits) {
		snprintf(err, len, "no window with app_id '%s'", app_id);
		return false;
	}
	if (!(hits = calloc(nhits, sizeof(*hits)))) {
		snprintf(err, len, "out of memory");
		return false;
	}
	nhits = 0;
	wl_list_for_each(c, &srv.clients, link)
		if (c->mapped && !strcmp(client_app_id(c), app_id))
			hits[nhits++] = c;
	for (size_t k = 0; k < nhits; k++) {
		if (!hits[k]->mapped)
			continue;
		target = hits[k];
		ok = actions[i].fn(sub, err, len) && ok;
		target = NULL;
	}
	free(hits);
	return ok;
}
#endif

bool action_known(const char *cmd)
{
	const char *arg;
	char err[64];
	return find_action(cmd, &arg, err, sizeof(err)) >= 0;
}

void action_exec(const char *cmd)
{
	char *dup = cmd ? strdup(cmd) : NULL, err[256];
	if (dup && !action_run(dup, err, sizeof(err)))
		wlr_log(WLR_ERROR, "'%s': %s", dup, err);
	free(dup);
}

bool action_run(const char *cmd, char *err, size_t errlen)
{
	const char *arg;
	int i = find_action(cmd, &arg, err, errlen);
	if (i >= 0 && !actions[i].fn)
		snprintf(err, errlen, "'%s' is not built in", actions[i].name);
	struct client *before = client_focused();
	bool ok = i >= 0 && actions[i].fn && actions[i].fn(arg, err, errlen);
	struct client *after = client_focused();
	if (ok && srv.conf.warp_cursor && after && after != before)
		input_warp_to(after);
	return ok;
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define IPC_BUF 4096
#define IPC_MAX_CLIENTS 64
#define IPC_WAIT_MS 20

struct ipc_client {
	struct wl_list link;
	int fd;
	struct wl_event_source *src;
	char buf[IPC_BUF];
	size_t len;
	bool subscribed;
	bool needs_current;
	bool dead;
};

static struct sockaddr_un addr;
static struct wl_event_source *listen_src;
static struct wl_list ipc_clients;
static char last_state[IPC_BUF];
static int nsubscribers, nclients;

static void client_free(struct ipc_client *c)
{
	if (c->subscribed)
		nsubscribers--;
	nclients--;
	wl_event_source_remove(c->src);
	close(c->fd);
	wl_list_remove(&c->link);
	free(c);
}

static bool client_write(struct ipc_client *c, const char *s, size_t n)
{
	if (c->dead)
		return false;
	while (n) {
		ssize_t w = send(c->fd, s, n, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !c->subscribed) {
			struct pollfd p = {.fd = c->fd, .events = POLLOUT};
			if (poll(&p, 1, IPC_WAIT_MS) > 0 && (p.revents & POLLOUT))
				continue;
		}
		if (w <= 0) {
			c->dead = true;
			shutdown(c->fd, SHUT_RDWR);
			return false;
		}
		s += w;
		n -= (size_t)w;
	}
	return true;
}

static void blank(char *s, const char *chars)
{
	for (; *s; s++)
		if (strchr(chars, *s))
			*s = ' ';
}

static void field(char *dst, size_t n, const char *s)
{
	size_t len = (size_t)snprintf(dst, n, "%s", s);
	if (len >= n) {
		size_t k = n - 1;
		while (k > 0 && ((unsigned char)dst[k - 1] & 0xc0) == 0x80)
			k--;
		unsigned char lead = k > 0 ? (unsigned char)dst[k - 1] : 0;
		size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
		if (k > 0 && n - k < need)
			dst[k - 1] = '\0';
	}
	blank(dst, "\t\n\r");
}

static size_t format_state(char *out, size_t cap)
{
	size_t n = 0;
	struct monitor *m;
	struct client *c;
	wl_list_for_each(m, &srv.monitors, link) {
		uint32_t occ = 0, urg = 0;
		wl_list_for_each(c, &srv.clients, link)
			if (c->mon == m && !c->scratch) {
				occ |= c->tags;
				if (c->urgent)
					urg |= c->tags;
			}
		int w = snprintf(out + n, cap - n, "tags %s %u %u %u %d\n", m->output->name, m->tags, occ, urg, m == srv.selmon);
		if (w < 0 || (size_t)w >= cap - n)
			break;
		n += (size_t)w;
	}
	out[n] = '\0';
	return n;
}

static void send_status(struct ipc_client *ic)
{
	char line[2048];
	struct monitor *m;
	struct client *c, *sel = client_focused();
	wl_list_for_each(m, &srv.monitors, link) {
		snprintf(line, sizeof(line), "monitor %s %dx%d+%d+%d tags=%u vrr=%d selected=%d state=%s layout=%s overview=%d\n",
			m->output->name, m->m.width, m->m.height, m->m.x, m->m.y, m->tags,
			m->output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED, m == srv.selmon,
			m->output->enabled ? "on" : m->asleep ? "asleep" : "off", layout_name(m->layout), m->overview);
		client_write(ic, line, strlen(line));
	}
	wl_list_for_each(c, &srv.clients, link) {
		char app[256], title[512];
		field(app, sizeof(app), client_app_id(c));
		field(title, sizeof(title), client_title(c));
		snprintf(line, sizeof(line), "client %s\t%s\tmonitor=%s tags=%u floating=%d fullscreen=%d focused=%d scratchpad=%d sticky=%d"
			" minimized=%d maximized=%d overlay=%d\n",
			app, title, c->mon ? c->mon->output->name : "-", c->tags, c->floating, c->fullscreen, c == sel, c->scratch,
			c->sticky, c->minimized, c->maximized, c->overlay);
		client_write(ic, line, strlen(line));
	}
	snprintf(line, sizeof(line), "gpu nvidia=%d explicit_sync=%d\n", srv.is_nvidia, srv.syncobj != NULL);
	client_write(ic, line, strlen(line));
}

static void send_current(struct ipc_client *ic)
{
	char line[1024], app[256], title[512];
	struct monitor *m;
	struct client *c = client_focused();
	bool fullscreen = false;
	wl_list_for_each(m, &srv.monitors, link) {
		snprintf(line, sizeof(line), "layout %s %s\noverview %s %d\n", m->output->name, layout_name(m->layout),
			m->output->name, m->overview);
		client_write(ic, line, strlen(line));
		fullscreen = fullscreen || m->fullscreen;
	}
	snprintf(line, sizeof(line), "fullscreen %d\n", fullscreen);
	client_write(ic, line, strlen(line));
	field(app, sizeof(app), c ? client_app_id(c) : "");
	field(title, sizeof(title), c ? client_title(c) : "");
	snprintf(line, sizeof(line), "focus %s\t%s\n", app, title);
	client_write(ic, line, strlen(line));
}

static bool handle_line(struct ipc_client *ic, char *line)
{
	char err[256];
	if (srv.locked && strcmp(line, "subscribe")) {
		client_write(ic, "error: the screen is locked\n", 28);
		return false;
	}
	if (!strcmp(line, "subscribe")) {
		char st[IPC_BUF];
		if (srv.locked) {
			if (!ic->subscribed)
				nsubscribers++;
			ic->subscribed = true;
			ic->needs_current = true;
			last_state[0] = '\0';
			return true;
		}
		size_t n = format_state(st, sizeof(st));
		if (!nsubscribers)
			memcpy(last_state, st, n + 1);
		if (!ic->subscribed)
			nsubscribers++;
		ic->subscribed = true;
		ic->needs_current = false;
		if (!client_write(ic, st, n))
			return false;
		send_current(ic);
		return true;
	}
	if (!strcmp(line, "status")) {
		send_status(ic);
		return false;
	}
	if (action_run(line, err, sizeof(err)))
		client_write(ic, "ok\n", 3);
	else {
		client_write(ic, "error: ", 7);
		client_write(ic, err, strlen(err));
		client_write(ic, "\n", 1);
	}
	return false;
}

static int handle_readable(int fd, uint32_t mask, void *data)
{
	struct ipc_client *ic = data;
	if (ic->dead || (mask & WL_EVENT_ERROR)) {
		client_free(ic);
		return 0;
	}
	ssize_t r = read(fd, ic->buf + ic->len, sizeof(ic->buf) - 1 - ic->len);
	if (r <= 0) {
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			return 0;
		client_free(ic);
		return 0;
	}
	ic->len += (size_t)r;
	ic->buf[ic->len] = '\0';

	char *nl;
	bool done = false;
	while ((nl = memchr(ic->buf, '\n', ic->len))) {
		*nl = '\0';
		if (nl > ic->buf && nl[-1] == '\r')
			nl[-1] = '\0';
		bool keep = handle_line(ic, ic->buf);
		if (ic->dead) {
			client_free(ic);
			return 0;
		}
		if (!keep)
			done = true;
		size_t used = (size_t)(nl - ic->buf) + 1;
		memmove(ic->buf, nl + 1, ic->len - used + 1);
		ic->len -= used;
	}
	if ((done && !ic->subscribed) || ic->len >= sizeof(ic->buf) - 1)
		client_free(ic);
	return 0;
}

static bool peer_is_us(int fd)
{
#ifdef __linux__
	struct ucred cred;
	socklen_t len = sizeof(cred);
	return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == getuid();
#else
	uid_t uid;
	gid_t gid;
	return getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
#endif
}

static void accept_one(int cfd)
{
	if (!peer_is_us(cfd)) {
		wlr_log(WLR_ERROR, "ipc: rejected connection from another user");
		close(cfd);
		return;
	}
	if (nclients >= IPC_MAX_CLIENTS) {
		wlr_log(WLR_ERROR, "ipc: too many connections, rejecting one");
		close(cfd);
		return;
	}
	fcntl(cfd, F_SETFD, FD_CLOEXEC);
	fcntl(cfd, F_SETFL, O_NONBLOCK);
	struct ipc_client *ic = calloc(1, sizeof(*ic));
	if (!ic) {
		close(cfd);
		return;
	}
	ic->fd = cfd;
	ic->src = wl_event_loop_add_fd(srv.loop, cfd, WL_EVENT_READABLE, handle_readable, ic);
	if (!ic->src) {
		close(cfd);
		free(ic);
		return;
	}
	nclients++;
	wl_list_insert(&ipc_clients, &ic->link);
}

static int handle_accept(int fd, uint32_t mask, void *data)
{
	for (int i = 0; i < 64; i++) {
		int cfd = accept(fd, NULL, NULL);
		if (cfd < 0)
			break;
		accept_one(cfd);
	}
	return 0;
}

bool ipc_init(void)
{
	wl_list_init(&ipc_clients);
	const char *dir = getenv("XDG_RUNTIME_DIR");
	const char *wd = getenv("WAYLAND_DISPLAY");
	if (!dir || !wd)
		return false;

	struct stat st;
	if (stat(dir, &st) == 0 && (st.st_uid != getuid() || (st.st_mode & 077)))
		wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR %s is not private, other users may see swizzle sockets", dir);

	addr.sun_family = AF_UNIX;
	int plen = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/swizzle.%s.sock", dir, wd);
	if (plen < 0 || (size_t)plen >= sizeof(addr.sun_path)) {
		wlr_log(WLR_ERROR, "ipc: socket path in %s is too long", dir);
		return false;
	}
	unlink(addr.sun_path);

	srv.ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv.ipc_fd < 0)
		return false;
	fcntl(srv.ipc_fd, F_SETFD, FD_CLOEXEC);
	fcntl(srv.ipc_fd, F_SETFL, O_NONBLOCK);
	mode_t old_mask = umask(077);
	int bound = bind(srv.ipc_fd, (struct sockaddr *)&addr, sizeof(addr));
	umask(old_mask);
	if (bound < 0 || listen(srv.ipc_fd, SOMAXCONN) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc socket");
		close(srv.ipc_fd);
		srv.ipc_fd = -1;
		return false;
	}
	listen_src = wl_event_loop_add_fd(srv.loop, srv.ipc_fd, WL_EVENT_READABLE, handle_accept, NULL);
	if (!listen_src) {
		wlr_log(WLR_ERROR, "ipc: cannot watch the socket");
		close(srv.ipc_fd);
		unlink(addr.sun_path);
		srv.ipc_fd = -1;
		return false;
	}
	setenv("SWIZZLE_SOCKET", addr.sun_path, 1);
	wlr_log(WLR_INFO, "ipc listening on %s", addr.sun_path);
	return true;
}

void ipc_finish(void)
{
	struct ipc_client *ic, *tmp;
	if (srv.ipc_fd < 0)
		return;
	wl_list_for_each_safe(ic, tmp, &ipc_clients, link)
		client_free(ic);
	wl_event_source_remove(listen_src);
	close(srv.ipc_fd);
	unlink(addr.sun_path);
	srv.ipc_fd = -1;
}

static void broadcast(const char *s, size_t n)
{
	struct ipc_client *ic, *tmp;
	if (srv.ipc_fd < 0)
		return;
	wl_list_for_each_safe(ic, tmp, &ipc_clients, link)
		if (ic->subscribed)
			client_write(ic, s, n);
}

void ipc_event(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	if (!nsubscribers || srv.locked)
		return;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n > sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n] = '\0';
	blank(buf, "\n\r");
	buf[n++] = '\n';
	broadcast(buf, (size_t)n);
}

void ipc_client_event(const char *ev, struct client *c)
{
	char app[256], title[512];
	if (!nsubscribers || srv.locked)
		return;
	field(app, sizeof(app), client_app_id(c));
	field(title, sizeof(title), client_title(c));
	ipc_event("%s %s\t%s", ev, app, title);
}

void ipc_title_event(struct client *c)
{
	char title[512];
	if (!nsubscribers || srv.locked)
		return;
	field(title, sizeof(title), client_title(c));
	ipc_event("title %s", title);
}

void ipc_resync(void)
{
	struct ipc_client *ic;
	if (srv.ipc_fd < 0)
		return;
	last_state[0] = '\0';
	wl_list_for_each(ic, &ipc_clients, link)
		if (ic->subscribed)
			ic->needs_current = true;
}

void ipc_notify_state(void)
{
	char st[IPC_BUF];
	if (!nsubscribers || srv.locked)
		return;
	size_t n = format_state(st, sizeof(st));
	if (strcmp(st, last_state)) {
		memcpy(last_state, st, n + 1);
		broadcast(st, n);
	}
	struct ipc_client *ic;
	wl_list_for_each(ic, &ipc_clients, link)
		if (ic->needs_current) {
			ic->needs_current = false;
			send_current(ic);
		}
}
