#include <stdlib.h>
#include <wlr/types/wlr_switch.h>
#include "swizzle.h"

struct lid {
	struct wl_listener toggle;
	struct wl_listener destroy;
};

static void handle_lid_toggle(struct wl_listener *l, void *data)
{
	struct wlr_switch_toggle_event *ev = data;
	bool lid_close = ev->switch_state == WLR_SWITCH_STATE_ON;
	if (ev->switch_type != WLR_SWITCH_TYPE_LID)
		return;
	input_activity();
	if (srv.locked && !(lid_close ? srv.conf.lid_close_locked : srv.conf.lid_open_locked))
		return;
	action_exec(lid_close ? srv.conf.lid_close : srv.conf.lid_open);
}

static void handle_lid_destroy(struct wl_listener *l, void *data)
{
	struct lid *s = wl_container_of(l, s, destroy);
	wl_list_remove(&s->toggle.link);
	wl_list_remove(&s->destroy.link);
	free(s);
}

void lid_add(struct wlr_input_device *dev)
{
	struct lid *s = calloc(1, sizeof(*s));
	if (!s)
		return;
	LISTEN(&wlr_switch_from_input_device(dev)->events.toggle, &s->toggle, handle_lid_toggle);
	LISTEN(&dev->events.destroy, &s->destroy, handle_lid_destroy);
}
