#include <wlr/types/wlr_drm_lease_v1.h>
#include "swizzle.h"

static struct wlr_drm_lease_v1_manager *mgr;
static struct wl_listener lease_request;

static void handle_lease_request(struct wl_listener *l, void *data)
{
	struct wlr_drm_lease_request_v1 *req = data;
	if (!wlr_drm_lease_request_v1_grant(req)) {
		wlr_log(WLR_ERROR, "drm lease request failed");
		wlr_drm_lease_request_v1_reject(req);
	}
}

void lease_offer(struct wlr_output *o)
{
	if (mgr && wlr_drm_lease_v1_manager_offer_output(mgr, o))
		wlr_log(WLR_INFO, "%s: non-desktop output offered for drm lease", o->name);
}

void lease_init(void)
{
	mgr = wlr_drm_lease_v1_manager_create(srv.display, srv.backend);
	if (!mgr)
		return;
	LISTEN(&mgr->events.request, &lease_request, handle_lease_request);
	struct wlr_drm_lease_device_v1 *dev;
	wl_list_for_each(dev, &mgr->devices, link)
		privileged_add(dev->global);
}

void lease_finish(void)
{
	if (mgr)
		wl_list_remove(&lease_request.link);
}
