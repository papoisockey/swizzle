#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include "swizzle.h"

static struct wlr_ext_foreign_toplevel_list_v1 *list;
static struct wl_listener capture_request;
static unsigned gpu_gen;

static void drop_capture(struct client *c)
{
	struct wlr_scene_tree *old = c->scene, *t = wlr_scene_tree_create(old->node.parent);
	struct wlr_scene_node *n, *tmp;
	wl_list_for_each_safe(n, tmp, &old->children, link)
		wlr_scene_node_reparent(n, t);
	t->node.data = c;
	wlr_scene_node_set_position(&t->node, old->node.x, old->node.y);
	wlr_scene_node_set_enabled(&t->node, old->node.enabled);
	wlr_scene_node_place_above(&t->node, &old->node);
	wlr_scene_node_destroy(&old->node);
	c->scene = t;
	c->capture = NULL;
}

void share_gpu_reset(void)
{
	struct client *c;
	gpu_gen++;
	wl_list_for_each(c, &srv.clients, link)
		if (c->capture)
			drop_capture(c);
}

static struct wlr_ext_foreign_toplevel_handle_v1_state state(struct client *c)
{
	return (struct wlr_ext_foreign_toplevel_handle_v1_state){.title = client_title(c), .app_id = client_app_id(c)};
}

void share_map(struct client *c)
{
	struct wlr_ext_foreign_toplevel_handle_v1_state st = state(c);
	c->ext_ftl = wlr_ext_foreign_toplevel_handle_v1_create(list, &st);
	if (c->ext_ftl)
		c->ext_ftl->data = c;
}

void share_unmap(struct client *c)
{
	if (c->ext_ftl)
		wlr_ext_foreign_toplevel_handle_v1_destroy(c->ext_ftl);
	c->ext_ftl = NULL;
}

void share_update(struct client *c)
{
	struct wlr_ext_foreign_toplevel_handle_v1_state st = state(c);
	if (c->ext_ftl)
		wlr_ext_foreign_toplevel_handle_v1_update_state(c->ext_ftl, &st);
}

static void handle_capture_request(struct wl_listener *l, void *data)
{
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req = data;
	struct client *c = req->toplevel_handle->data;
	if (!c)
		return;
	if (c->capture && c->capture_gen != gpu_gen)
		drop_capture(c);
	if (!c->capture) {
		c->capture = wlr_ext_image_capture_source_v1_create_with_scene_node(&c->scene->node, srv.loop,
			srv.allocator, srv.renderer);
		c->capture_gen = gpu_gen;
	}
	if (c->capture)
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(req, c->capture);
}

void share_init(void)
{
	list = wlr_ext_foreign_toplevel_list_v1_create(srv.display, 1);
	privileged_add(list->global);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *capture =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(srv.display, 1);
	privileged_add(capture->global);
	LISTEN(&capture->events.new_request, &capture_request, handle_capture_request);
}

void share_finish(void)
{
	wl_list_remove(&capture_request.link);
}
