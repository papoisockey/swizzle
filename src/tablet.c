#include <math.h>
#include <stdlib.h>
#ifdef __linux__
#include <linux/input-event-codes.h>
#else
#include <dev/evdev/input-event-codes.h>
#endif
#include <libinput.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include "swizzle.h"

struct tablet {
	struct wl_list link;
	struct wlr_input_device *dev;
	struct wlr_tablet_v2_tablet *v2;
	struct wl_listener destroy;
};

struct pad {
	struct wl_list link;
	struct wlr_input_device *dev;
	struct wlr_tablet_v2_tablet_pad *v2;
	struct wlr_surface *focus;
	struct wl_listener button, ring, strip, destroy, focus_destroy;
};

static struct wl_listener tool_axis, tool_proximity, tool_tip, tool_button;
static struct wl_list tablets, pads;
static struct wlr_tablet_manager_v2 *tablet_mgr;
static bool tip_swallowed, tip_down, tip_pointer;
static uint32_t pointer_buttons[8];
static double tip_dx, tip_dy;
static int tip_ox, tip_oy;

static uint32_t pointer_button(uint32_t button)
{
	return button == BTN_STYLUS ? BTN_RIGHT : button == BTN_STYLUS2 ? BTN_MIDDLE : button;
}

static void release_pointer(uint32_t time)
{
	bool released = tip_pointer;
	if (tip_pointer)
		input_button(time, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	tip_pointer = false;
	for (size_t i = 0; i < LENGTH(pointer_buttons); i++)
		if (pointer_buttons[i]) {
			input_button(time, pointer_button(pointer_buttons[i]), WL_POINTER_BUTTON_STATE_RELEASED);
			pointer_buttons[i] = 0;
			released = true;
		}
	if (released)
		wlr_seat_pointer_notify_frame(srv.seat);
}

struct tool {
	struct wlr_tablet_v2_tablet_tool *v2;
	struct wl_listener set_cursor;
	struct wl_listener destroy;
};

static void handle_tool_set_cursor(struct wl_listener *l, void *data)
{
	struct tool *t = wl_container_of(l, t, set_cursor);
	struct wlr_tablet_v2_event_cursor *ev = data;
	struct wlr_surface *focused = t->v2->focused_surface;
	if (srv.cursor_mode != CURSOR_NORMAL || tip_pointer || !focused || !ev->seat_client ||
			ev->seat_client->client != wl_resource_get_client(focused->resource))
		return;
	wlr_cursor_set_surface(srv.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void handle_tool_destroy(struct wl_listener *l, void *data)
{
	struct tool *t = wl_container_of(l, t, destroy);
	wl_list_remove(&t->set_cursor.link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

static struct wlr_tablet_v2_tablet_tool *tool_v2(struct wlr_tablet_tool *tool)
{
	struct tool *t = tool->data;
	if (t)
		return t->v2;
	if (!(t = calloc(1, sizeof(*t))))
		return NULL;
	LISTEN(&tool->events.destroy, &t->destroy, handle_tool_destroy);
	t->v2 = wlr_tablet_tool_create(tablet_mgr, srv.seat, tool);
	if (!t->v2) {
		wl_list_remove(&t->destroy.link);
		free(t);
		return NULL;
	}
	LISTEN(&t->v2->events.set_cursor, &t->set_cursor, handle_tool_set_cursor);
	tool->data = t;
	return t->v2;
}

static bool tool_motion(struct wlr_tablet *tablet, struct wlr_tablet_tool *tool, uint32_t time)
{
	struct wlr_tablet_v2_tablet_tool *t = tool_v2(tool);
	struct wlr_surface *surf;
	double sx, sy;
	if (!t || !tablet->data)
		return false;
	if (tip_down && srv.locked) {
		tip_down = false;
		tip_swallowed = true;
	}
	if (tip_down && t->focused_surface) {
		int nx, ny;
		input_surface_origin(t->focused_surface, &nx, &ny);
		wlr_scene_node_set_position(&srv.layers[LYR_DRAG]->node, (int)srv.cursor->x, (int)srv.cursor->y);
		wlr_send_tablet_v2_tablet_tool_motion(t, srv.cursor->x - tip_dx - (nx - tip_ox),
			srv.cursor->y - tip_dy - (ny - tip_oy));
		return true;
	}
	if (srv.cursor_mode != CURSOR_NORMAL || tip_pointer) {
		input_motion(time, false);
		return false;
	}
	client_at(srv.cursor->x, srv.cursor->y, &surf, &sx, &sy);
	if (!surf) {
		wlr_send_tablet_v2_tablet_tool_proximity_out(t);
		input_motion(time, false);
		return false;
	}
	if (t->focused_surface != surf)
		wlr_send_tablet_v2_tablet_tool_proximity_in(t, tablet->data, surf);
	if (t->focused_surface != surf) {
		if (t->focused_surface)
			wlr_send_tablet_v2_tablet_tool_proximity_out(t);
		input_motion(time, false);
		return false;
	}
	input_hover(time);
	if (t->focused_surface != surf)
		return false;
	wlr_send_tablet_v2_tablet_tool_motion(t, sx, sy);
	return true;
}

static void handle_tool_axis(struct wl_listener *l, void *data)
{
	struct wlr_tablet_tool_axis_event *ev = data;
	if (ev->tool->type == WLR_TABLET_TOOL_TYPE_MOUSE || ev->tool->type == WLR_TABLET_TOOL_TYPE_LENS)
		wlr_cursor_move(srv.cursor, &ev->tablet->base, ev->dx, ev->dy);
	else if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y))
		wlr_cursor_warp_absolute(srv.cursor, &ev->tablet->base, ev->updated_axes & WLR_TABLET_TOOL_AXIS_X ? ev->x : NAN,
			ev->updated_axes & WLR_TABLET_TOOL_AXIS_Y ? ev->y : NAN);
	input_activity();
	if (!tool_motion(ev->tablet, ev->tool, ev->time_msec))
		return;
	struct wlr_tablet_v2_tablet_tool *t = tool_v2(ev->tool);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE)
		wlr_send_tablet_v2_tablet_tool_pressure(t, ev->pressure);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE)
		wlr_send_tablet_v2_tablet_tool_distance(t, ev->distance);
	if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
		wlr_send_tablet_v2_tablet_tool_tilt(t, ev->tilt_x, ev->tilt_y);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION)
		wlr_send_tablet_v2_tablet_tool_rotation(t, ev->rotation);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER)
		wlr_send_tablet_v2_tablet_tool_slider(t, ev->slider);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL)
		wlr_send_tablet_v2_tablet_tool_wheel(t, ev->wheel_delta, 0);
}

static void handle_tool_proximity(struct wl_listener *l, void *data)
{
	struct wlr_tablet_tool_proximity_event *ev = data;
	struct wlr_tablet_v2_tablet_tool *t = tool_v2(ev->tool);
	if (!t)
		return;
	if (ev->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
		tip_down = false;
		release_pointer(ev->time_msec);
		wlr_send_tablet_v2_tablet_tool_proximity_out(t);
		return;
	}
	if (ev->tool->type != WLR_TABLET_TOOL_TYPE_MOUSE && ev->tool->type != WLR_TABLET_TOOL_TYPE_LENS)
		wlr_cursor_warp_absolute(srv.cursor, &ev->tablet->base, ev->x, ev->y);
	tool_motion(ev->tablet, ev->tool, ev->time_msec);
}

static void handle_tool_tip(struct wl_listener *l, void *data)
{
	struct wlr_tablet_tool_tip_event *ev = data;
	struct wlr_tablet_v2_tablet_tool *t = tool_v2(ev->tool);
	struct wlr_surface *surf;
	double sx, sy;
	bool down = ev->state == WLR_TABLET_TOOL_TIP_DOWN;
	if (!t)
		return;
	if (!down && tip_swallowed) {
		tip_swallowed = false;
		return;
	}
	if (!down && tip_down) {
		tip_down = false;
		if (t->focused_surface)
			wlr_send_tablet_v2_tablet_tool_up(t);
		return;
	}
	if (!down) {
		if (tip_pointer) {
			tip_pointer = false;
			input_button(ev->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
			wlr_seat_pointer_notify_frame(srv.seat);
		}
		return;
	}
	struct client *c = client_at(srv.cursor->x, srv.cursor->y, &surf, &sx, &sy);
	if (srv.cursor_mode == CURSOR_NORMAL && t->focused_surface && t->focused_surface == surf) {
		input_surface_origin(surf, &tip_ox, &tip_oy);
		input_click_focus(c, surf);
		if (input_overview_pick(c)) {
			tip_swallowed = true;
			return;
		}
		wlr_send_tablet_v2_tablet_tool_down(t);
		tip_down = true;
		tip_dx = srv.cursor->x - sx;
		tip_dy = srv.cursor->y - sy;
		return;
	}
	tip_pointer = true;
	input_button(ev->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	wlr_seat_pointer_notify_frame(srv.seat);
}

static void handle_tool_button(struct wl_listener *l, void *data)
{
	struct wlr_tablet_tool_button_event *ev = data;
	struct wlr_tablet_v2_tablet_tool *t = tool_v2(ev->tool);
	if (!t)
		return;
	size_t slot = LENGTH(pointer_buttons);
	for (size_t i = 0; i < LENGTH(pointer_buttons); i++)
		if (pointer_buttons[i] == ev->button)
			slot = i;
	if (!ev->state && slot == LENGTH(pointer_buttons)) {
		if (t->focused_surface)
			wlr_send_tablet_v2_tablet_tool_button(t, ev->button, (enum zwp_tablet_pad_v2_button_state)ev->state);
		return;
	}
	if (ev->state && srv.cursor_mode == CURSOR_NORMAL && t->focused_surface) {
		wlr_send_tablet_v2_tablet_tool_button(t, ev->button, (enum zwp_tablet_pad_v2_button_state)ev->state);
		return;
	}
	if (ev->state && slot == LENGTH(pointer_buttons))
		for (size_t i = 0; i < LENGTH(pointer_buttons) && slot == LENGTH(pointer_buttons); i++)
			if (!pointer_buttons[i])
				pointer_buttons[slot = i] = ev->button;
	if (slot == LENGTH(pointer_buttons))
		return;
	if (!ev->state)
		pointer_buttons[slot] = 0;
	input_button(ev->time_msec, pointer_button(ev->button), ev->state ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_notify_frame(srv.seat);
}

static struct libinput_device *libinput_of(struct wlr_input_device *dev)
{
	return wlr_input_device_is_libinput(dev) ? wlr_libinput_get_device_handle(dev) : NULL;
}

static struct tablet *pad_tablet(struct pad *p)
{
	struct libinput_device *d = libinput_of(p->dev);
	struct libinput_device_group *group = d ? libinput_device_get_device_group(d) : NULL;
	struct tablet *t;
	wl_list_for_each(t, &tablets, link) {
		struct libinput_device *td = libinput_of(t->dev);
		if (group && td && libinput_device_get_device_group(td) == group)
			return t;
	}
	return wl_list_empty(&tablets) ? NULL : wl_container_of(tablets.next, t, link);
}

static void handle_pad_focus_destroy(struct wl_listener *l, void *data)
{
	struct pad *p = wl_container_of(l, p, focus_destroy);
	wl_list_remove(&p->focus_destroy.link);
	wl_list_init(&p->focus_destroy.link);
	p->focus = NULL;
}

static void pad_focus(struct pad *p, struct wlr_surface *surface)
{
	if (surface == p->focus)
		return;
	if (p->focus)
		wlr_send_tablet_v2_tablet_pad_leave(p->v2, p->focus);
	wl_list_remove(&p->focus_destroy.link);
	wl_list_init(&p->focus_destroy.link);
	p->focus = NULL;
	struct tablet *t = surface ? pad_tablet(p) : NULL;
	if (!t || !wlr_send_tablet_v2_tablet_pad_enter(p->v2, t->v2, surface))
		return;
	p->focus = surface;
	LISTEN(&surface->events.destroy, &p->focus_destroy, handle_pad_focus_destroy);
}

void tablet_focus(struct wlr_surface *surface)
{
	struct pad *p;
	wl_list_for_each(p, &pads, link)
		pad_focus(p, surface);
}

void tablet_map(void)
{
	struct tablet *t;
	wl_list_for_each(t, &tablets, link) {
		struct libinput_device *d = libinput_of(t->dev);
		const char *name = d ? libinput_device_get_output_name(d) : NULL;
		struct monitor *m = name ? output_by_name(name) : NULL;
		wlr_cursor_map_input_to_output(srv.cursor, t->dev, m ? m->output : NULL);
	}
}

static void handle_pad_button(struct wl_listener *l, void *data)
{
	struct pad *p = wl_container_of(l, p, button);
	struct wlr_tablet_pad_button_event *ev = data;
	input_activity();
	if (!p->focus)
		return;
	wlr_send_tablet_v2_tablet_pad_mode(p->v2, ev->group, ev->mode, ev->time_msec);
	wlr_send_tablet_v2_tablet_pad_button(p->v2, ev->button, ev->time_msec,
		ev->state == WLR_BUTTON_PRESSED ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
}

static void handle_pad_ring(struct wl_listener *l, void *data)
{
	struct pad *p = wl_container_of(l, p, ring);
	struct wlr_tablet_pad_ring_event *ev = data;
	input_activity();
	if (p->focus)
		wlr_send_tablet_v2_tablet_pad_ring(p->v2, ev->ring, ev->position,
			ev->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, ev->time_msec);
}

static void handle_pad_strip(struct wl_listener *l, void *data)
{
	struct pad *p = wl_container_of(l, p, strip);
	struct wlr_tablet_pad_strip_event *ev = data;
	input_activity();
	if (p->focus)
		wlr_send_tablet_v2_tablet_pad_strip(p->v2, ev->strip, ev->position,
			ev->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, ev->time_msec);
}

static void handle_pad_destroy(struct wl_listener *l, void *data)
{
	struct pad *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->focus_destroy.link);
	wl_list_remove(&p->button.link);
	wl_list_remove(&p->ring.link);
	wl_list_remove(&p->strip.link);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	free(p);
}

void tablet_pad_add(struct wlr_input_device *dev)
{
	struct pad *p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->dev = dev;
	p->v2 = wlr_tablet_pad_create(tablet_mgr, srv.seat, dev);
	if (!p->v2) {
		free(p);
		return;
	}
	struct wlr_tablet_pad *pad = wlr_tablet_pad_from_input_device(dev);
	wl_list_init(&p->focus_destroy.link);
	LISTEN(&pad->events.button, &p->button, handle_pad_button);
	LISTEN(&pad->events.ring, &p->ring, handle_pad_ring);
	LISTEN(&pad->events.strip, &p->strip, handle_pad_strip);
	LISTEN(&dev->events.destroy, &p->destroy, handle_pad_destroy);
	wl_list_insert(&pads, &p->link);
	pad_focus(p, srv.seat->keyboard_state.focused_surface);
}

static void handle_tablet_destroy(struct wl_listener *l, void *data)
{
	struct tablet *t = wl_container_of(l, t, destroy);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

void tablet_add(struct wlr_input_device *dev)
{
	struct wlr_tablet_v2_tablet *v2 = wlr_tablet_create(tablet_mgr, srv.seat, dev);
	wlr_tablet_from_input_device(dev)->data = v2;
	wlr_cursor_attach_input_device(srv.cursor, dev);
	struct tablet *t = v2 ? calloc(1, sizeof(*t)) : NULL;
	if (!t)
		return;
	t->dev = dev;
	t->v2 = v2;
	LISTEN(&dev->events.destroy, &t->destroy, handle_tablet_destroy);
	wl_list_insert(tablets.prev, &t->link);
	tablet_map();
	tablet_focus(srv.seat->keyboard_state.focused_surface);
}

void tablet_init(void)
{
	wl_list_init(&tablets);
	wl_list_init(&pads);
	tablet_mgr = wlr_tablet_v2_create(srv.display);
	LISTEN(&srv.cursor->events.tablet_tool_axis, &tool_axis, handle_tool_axis);
	LISTEN(&srv.cursor->events.tablet_tool_proximity, &tool_proximity, handle_tool_proximity);
	LISTEN(&srv.cursor->events.tablet_tool_tip, &tool_tip, handle_tool_tip);
	LISTEN(&srv.cursor->events.tablet_tool_button, &tool_button, handle_tool_button);
}

void tablet_finish(void)
{
	wl_list_remove(&tool_axis.link);
	wl_list_remove(&tool_proximity.link);
	wl_list_remove(&tool_tip.link);
	wl_list_remove(&tool_button.link);
}
