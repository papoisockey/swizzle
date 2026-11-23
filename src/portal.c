#include <stdlib.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include "swizzle.h"

void portal_init(void)
{
	struct wlr_xdg_foreign_registry *foreign = wlr_xdg_foreign_registry_create(srv.display);
	wlr_xdg_foreign_v1_create(srv.display, foreign);
	wlr_xdg_foreign_v2_create(srv.display, foreign);
	wlr_xdg_wm_dialog_v1_create(srv.display, 1);
}

void portal_export(void)
{
	if (srv.session && getenv("DBUS_SESSION_BUS_ADDRESS"))
		spawn("command -v dbus-update-activation-environment >/dev/null && dbus-update-activation-environment "
			"$([ -d /run/systemd/system ] && echo --systemd) WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP "
			"XDG_SESSION_TYPE SWIZZLE_SOCKET XCURSOR_THEME XCURSOR_SIZE");
}

bool portal_modal(struct wlr_xdg_toplevel *t)
{
	struct wlr_xdg_dialog_v1 *dialog = wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(t);
	return dialog && dialog->modal;
}
