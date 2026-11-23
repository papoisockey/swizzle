#include <math.h>
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

static struct wlr_tablet_v2_tablet_tool *tool_v2(struct wlr_tablet_tool *tool)
{
	if (!tool->data)
		tool->data = wlr_tablet_tool_create(tablet_mgr, srv.seat, tool);
	return tool->data;
}

static bool tool_motion(struct wlr_tablet *tablet, struct wlr_tablet_tool *tool, uint32_t time)
{
	struct wlr_tablet_v2_tablet_tool *t = tool_v2(tool);
	struct wlr_surface *surf;
	double sx, sy;
	if (!t || !tablet->data)
		return false;
	client_at(srv.cursor->x, srv.cursor->y, &surf, &sx, &sy);
	if (!surf) {
		wlr_send_tablet_v2_tablet_tool_proximity_out(t);
		input_motion(time, false);
		return true;
	}
	if (t->focused_surface != surf)
		wlr_send_tablet_v2_tablet_tool_proximity_in(t, tablet->data, surf);
	if (t->focused_surface != surf) {
		if (t->focused_surface)
			wlr_send_tablet_v2_tablet_tool_proximity_out(t);
		input_motion(time, false);
		return false;
	}
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
	struct client *c = client_at(srv.cursor->x, srv.cursor->y, &surf, &sx, &sy);
	if (down)
		input_click_focus(c, surf);
	if (t->focused_surface && t->focused_surface == surf) {
		if (down)
			wlr_send_tablet_v2_tablet_tool_down(t);
		else
			wlr_send_tablet_v2_tablet_tool_up(t);
		return;
	}
	wlr_seat_pointer_notify_button(srv.seat, ev->time_msec, BTN_LEFT,
		down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_notify_frame(srv.seat);
}

static void handle_tool_button(struct wl_listener *l, void *data)
{
	struct wlr_tablet_tool_button_event *ev = data;
	struct wlr_tablet_v2_tablet_tool *t = tool_v2(ev->tool);
	if (t && t->focused_surface)
		wlr_send_tablet_v2_tablet_tool_button(t, ev->button, (enum zwp_tablet_pad_v2_button_state)ev->state);
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
