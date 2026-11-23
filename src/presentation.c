#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_content_type_v1.h>
#include "swizzle.h"

static struct wlr_content_type_manager_v1 *content_type;

bool client_wants_vrr(struct client *c)
{
	struct wlr_surface *s = client_surface(c);
	if (!s || !content_type)
		return false;
	enum wp_content_type_v1_type t = wlr_surface_get_content_type_v1(content_type, s);
	return t == WP_CONTENT_TYPE_V1_TYPE_GAME || t == WP_CONTENT_TYPE_V1_TYPE_VIDEO;
}

void presentation_init(void)
{
	content_type = wlr_content_type_manager_v1_create(srv.display, 1);
	wlr_alpha_modifier_v1_create(srv.display);
}
