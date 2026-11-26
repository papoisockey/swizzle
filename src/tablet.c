#include <math.h>
#include <stdlib.h>
#ifdef __linux__
#include <linux/input-event-codes.h>
#else
#include <dev/evdev/input-event-codes.h>
#endif
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include "swizzle.h"

static struct wl_listener tool_axis, tool_proximity, tool_tip, tool_button;
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

void tablet_add(struct wlr_input_device *dev)
{
	wlr_tablet_from_input_device(dev)->data = wlr_tablet_create(tablet_mgr, srv.seat, dev);
	wlr_cursor_attach_input_device(srv.cursor, dev);
}

void tablet_init(void)
{
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
