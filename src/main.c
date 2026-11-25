#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>
#include <wlr/config.h>
#if WLR_HAS_GLES2_RENDERER
#include <wlr/render/gles2.h>
#endif
#include <wlr/render/pixman.h>
#ifdef WITH_SCENEFX
#include <scenefx/render/fx_renderer/fx_renderer.h>
#endif
#if WLR_HAS_VULKAN_RENDERER
struct wlr_renderer *wlr_vk_renderer_create_with_drm_fd(int drm_fd);
bool wlr_renderer_is_vk(struct wlr_renderer *wlr_renderer);
#endif
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include "swizzle.h"

struct server srv;
static bool log_to_file;
static struct wl_listener request_activate;
static struct wlr_security_context_manager_v1 *security_context;
static const struct wl_global *privileged[32];
static size_t nprivileged;
static struct wl_listener renderer_lost;
static struct wl_event_source *gpu_idle;
static int own_fd = -1;

void privileged_add(const struct wl_global *g)
{
	if (!g)
		return;
	if (nprivileged == LENGTH(privileged))
		abort();
	privileged[nprivileged++] = g;
}

static bool global_filter(const struct wl_client *client, const struct wl_global *global, void *data)
{
	if (!wlr_security_context_manager_v1_lookup_client(security_context, client))
		return true;
	for (size_t i = 0; i < nprivileged; i++)
		if (privileged[i] == global)
			return false;
	return true;
}

pid_t spawn(const char *cmd)
{
	wlr_log(WLR_INFO, "spawn: %s", cmd);
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		if (log_to_file) {
			int null = open("/dev/null", O_WRONLY);
			if (null >= 0)
				dup2(null, STDERR_FILENO), close(null);
		}
		pid_t child = fork();
		if (child == 0) {
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(child < 0 ? 1 : 0);
	}
	if (pid < 0)
		wlr_log_errno(WLR_ERROR, "fork");
	int status = 0;
	while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status)) {
		wlr_log(WLR_ERROR, "spawn: cannot fork for '%s'", cmd);
		return -1;
	}
	return pid;
}

static pid_t spawn_group(const char *cmd)
{
	wlr_log(WLR_INFO, "spawn: %s", cmd);
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		if (log_to_file) {
			int null = open("/dev/null", O_WRONLY);
			if (null >= 0)
				dup2(null, STDERR_FILENO), close(null);
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		wlr_log_errno(WLR_ERROR, "fork");
	return pid;
}

void quit(void)
{
	wl_display_terminate(srv.display);
}

static int handle_signal(int sig, void *data)
{
	quit();
	return 0;
}

static bool read_param(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(buf, (int)len, f) != NULL;
	fclose(f);
	return ok;
}

static void nvidia_sanity_checks(void)
{
#ifdef __linux__
	char buf[256];
	if (read_param("/sys/module/nvidia_drm/parameters/modeset", buf, sizeof(buf)) && buf[0] != 'Y')
		wlr_log(WLR_ERROR, "nvidia: nvidia_drm.modeset=1 is required, add it to the kernel cmdline");
	FILE *f = fopen("/proc/driver/nvidia/params", "r");
	if (f) {
		while (fgets(buf, sizeof(buf), f))
			if (!strncmp(buf, "PreserveVideoMemoryAllocations: 0", 33))
				wlr_log(WLR_INFO, "nvidia: NVreg_PreserveVideoMemoryAllocations=0, suspend may corrupt windows");
		fclose(f);
	}
#endif
}

static int open_render_node(void)
{
	drmDevicePtr devs[16];
	int n = drmGetDevices2(0, devs, LENGTH(devs)), fd = -1;
	for (int i = 0; i < n && fd < 0; i++)
		if (devs[i]->available_nodes & (1 << DRM_NODE_RENDER))
			fd = open(devs[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
	if (n > 0)
		drmFreeDevices(devs, n);
	return fd;
}

static void detect_driver(void)
{
	srv.drm_fd = wlr_backend_get_drm_fd(srv.backend);
	if (srv.drm_fd < 0) {
		srv.drm_fd = own_fd = open_render_node();
		wlr_log(WLR_INFO, "no DRM backend, running nested");
		if (srv.drm_fd < 0)
			return;
	}
	drmVersion *v = drmGetVersion(srv.drm_fd);
	if (!v)
		return;
	srv.is_nvidia = !strcmp(v->name, "nvidia-drm");
	static const char *soft[] = {"vmwgfx", "vboxvideo", "qxl", "bochs", "bochs-drm", "cirrus", "simpledrm"};
	for (size_t i = 0; i < LENGTH(soft); i++)
		if (!strcmp(v->name, soft[i]))
			srv.is_virtual = srv.soft_gpu = true;
	if (!strcmp(v->name, "virtio_gpu"))
		srv.is_virtual = true;
	if (srv.is_virtual)
		wlr_log(WLR_INFO, "virtual gpu %s: software cursor%s by default", v->name,
			srv.soft_gpu ? ", pixman renderer" : "");
	char *node = drmGetDeviceNameFromFd2(srv.drm_fd);
	wlr_log(WLR_INFO, "rendering GPU: %s, driver %s %d.%d.%d", node ? node : "unknown", v->name, v->version_major,
		v->version_minor, v->version_patchlevel);
	free(node);
	drmFreeVersion(v);
	if (srv.is_nvidia)
		nvidia_sanity_checks();
}

static struct wlr_renderer *try_renderer(enum renderer_pref pref)
{
	switch (pref) {
	case RENDERER_VULKAN:
#if WLR_HAS_VULKAN_RENDERER
		return wlr_vk_renderer_create_with_drm_fd(srv.drm_fd);
#else
		return NULL;
#endif
	case RENDERER_GLES:
#if WLR_HAS_GLES2_RENDERER
		return wlr_gles2_renderer_create_with_drm_fd(srv.drm_fd);
#else
		return NULL;
#endif
	case RENDERER_PIXMAN:
		return wlr_pixman_renderer_create();
	default:
		return NULL;
	}
}

static struct wlr_renderer *create_renderer(void)
{
#ifdef WITH_SCENEFX
	if (srv.conf.renderer == RENDERER_VULKAN || getenv("WLR_RENDERER"))
		wlr_log(WLR_INFO, "built with scenefx, renderer = vulkan and WLR_RENDERER are ignored");
	struct wlr_renderer *fx = srv.drm_fd >= 0 ? fx_renderer_create_with_drm_fd(srv.drm_fd) : fx_renderer_create(srv.backend);
	wlr_log(WLR_INFO, "renderer: scenefx gles%s", fx ? "" : " failed");
	if (srv.soft_gpu)
		wlr_log(WLR_ERROR, "scenefx needs gles, which is slow on this virtual gpu: build with -Dscenefx=disabled");
	return fx;
#endif
	if (getenv("WLR_RENDERER") || srv.drm_fd < 0)
		return wlr_renderer_autocreate(srv.backend);

	static const char *names[] = {"auto", "vulkan", "gles", "pixman"};
	enum renderer_pref pref = srv.conf.renderer;
	if (pref == RENDERER_AUTO)
		pref = srv.is_nvidia ? RENDERER_VULKAN : srv.soft_gpu ? RENDERER_PIXMAN : RENDERER_GLES;
	enum renderer_pref other = pref == RENDERER_GLES ? RENDERER_VULKAN : RENDERER_GLES;

	struct wlr_renderer *r = try_renderer(pref);
	if (!r) {
		wlr_log(WLR_ERROR, "%s renderer failed, falling back", names[pref]);
		r = try_renderer(other);
	}
	if (!r)
		r = wlr_renderer_autocreate(srv.backend);
	if (!r)
		return NULL;
	const char *name = wlr_renderer_is_pixman(r) ? "pixman" : "other";
#if WLR_HAS_GLES2_RENDERER
	if (wlr_renderer_is_gles2(r))
		name = "gles2";
#endif
#if WLR_HAS_VULKAN_RENDERER
	if (wlr_renderer_is_vk(r))
		name = "vulkan";
#endif
	wlr_log(WLR_INFO, "renderer: %s", name);
	return r;
}

static void gpu_handle_lost(struct wl_listener *l, void *data);

static void gpu_recreate(void *data)
{
	gpu_idle = NULL;
	wlr_log(WLR_ERROR, "GPU reset detected, recreating renderer");
	struct wlr_renderer *old_r = srv.renderer;
	struct wlr_allocator *old_a = srv.allocator;

	struct wlr_renderer *r = create_renderer();
	if (!r) {
		wlr_log(WLR_ERROR, "cannot recreate renderer");
		quit();
		return;
	}
	struct wlr_allocator *a = wlr_allocator_autocreate(srv.backend, r);
	if (!a) {
		wlr_log(WLR_ERROR, "cannot recreate allocator");
		wlr_renderer_destroy(r);
		quit();
		return;
	}

	wl_list_remove(&renderer_lost.link);
	srv.renderer = r;
	srv.allocator = a;
	LISTEN(&r->events.lost, &renderer_lost, gpu_handle_lost);
	wlr_compositor_set_renderer(srv.compositor, r);

	struct monitor *m;
	bool all = true;
	wl_list_for_each(m, &srv.monitors, link) {
		if (wlr_output_init_render(m->output, a, r))
			continue;
		wlr_log(WLR_ERROR, "%s: cannot render with the new renderer, disabling it", m->output->name);
		all = false;
		if (m->output->enabled) {
			struct wlr_output_state st;
			wlr_output_state_init(&st);
			wlr_output_state_set_enabled(&st, false);
			wlr_output_commit_state(m->output, &st);
			wlr_output_state_finish(&st);
		}
	}
	share_gpu_reset();

	if (!all) {
		wlr_log(WLR_ERROR, "keeping the lost renderer alive for outputs that still reference it");
		return;
	}
	wlr_allocator_destroy(old_a);
	wlr_renderer_destroy(old_r);
}

static void gpu_handle_lost(struct wl_listener *l, void *data)
{
	if (!gpu_idle)
		gpu_idle = wl_event_loop_add_idle(srv.loop, gpu_recreate, NULL);
}

static bool gpu_init(void)
{
	detect_driver();
	srv.renderer = create_renderer();
	if (!srv.renderer)
		return false;
	srv.allocator = wlr_allocator_autocreate(srv.backend, srv.renderer);
	if (!srv.allocator)
		return false;
	LISTEN(&srv.renderer->events.lost, &renderer_lost, gpu_handle_lost);
	return true;
}

static void gpu_finish(void)
{
	wl_list_remove(&renderer_lost.link);
	if (gpu_idle)
		wl_event_source_remove(gpu_idle);
	gpu_idle = NULL;
	if (own_fd >= 0)
		close(own_fd);
	own_fd = -1;
}

static void gpu_init_syncobj(void)
{
	if (!srv.renderer->features.timeline || !srv.backend->features.timeline) {
		wlr_log(WLR_INFO, "explicit sync unavailable (renderer %d, backend %d)",
			srv.renderer->features.timeline, srv.backend->features.timeline);
		return;
	}
	int fd = wlr_renderer_get_drm_fd(srv.renderer);
	if (fd < 0)
		return;
	srv.syncobj = wlr_linux_drm_syncobj_manager_v1_create(srv.display, 1, fd);
	wlr_log(WLR_INFO, "explicit sync enabled");
}

static void handle_request_activate(struct wl_listener *l, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *ev = data;
	client_request_activate(client_from_surface(ev->surface), ev->token->seat != NULL);
}

static void create_protocols(void)
{
	srv.compositor = wlr_compositor_create(srv.display, 6, srv.renderer);
	wlr_subcompositor_create(srv.display);
	wlr_data_device_manager_create(srv.display);

	if (wlr_renderer_get_texture_formats(srv.renderer, WLR_BUFFER_CAP_DMABUF)) {
		srv.dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(srv.display, 5, srv.renderer);
		wlr_scene_set_linux_dmabuf_v1(srv.scene, srv.dmabuf);
	}
	gpu_init_syncobj();

	wlr_viewporter_create(srv.display);
	wlr_single_pixel_buffer_manager_v1_create(srv.display);
	wlr_fractional_scale_manager_v1_create(srv.display, 1);
	wlr_presentation_create(srv.display, srv.backend, 2);
	srv.tearing = wlr_tearing_control_manager_v1_create(srv.display, 1);
	presentation_init();

	privileged_add(wlr_export_dmabuf_manager_v1_create(srv.display)->global);
	privileged_add(wlr_screencopy_manager_v1_create(srv.display)->global);
	privileged_add(wlr_ext_image_copy_capture_manager_v1_create(srv.display, 1)->global);
	privileged_add(wlr_ext_output_image_capture_source_manager_v1_create(srv.display, 1)->global);

	privileged_add(wlr_data_control_manager_v1_create(srv.display)->global);
	privileged_add(wlr_ext_data_control_manager_v1_create(srv.display, 1)->global);
	wlr_primary_selection_v1_device_manager_create(srv.display);

	portal_init();
	struct wlr_xdg_activation_v1 *act = wlr_xdg_activation_v1_create(srv.display);
	LISTEN(&act->events.request_activate, &request_activate, handle_request_activate);
}

static void mkdir_p(char *path)
{
	for (char *p = path + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(path, 0700);
			*p = '/';
		}
	mkdir(path, 0700);
}

static void redirect_log(void)
{
	char dir[4000], path[4096], old[4096];
	const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
	if (state && *state)
		snprintf(dir, sizeof(dir), "%s/swizzle", state);
	else if (home)
		snprintf(dir, sizeof(dir), "%s/.local/state/swizzle", home);
	else
		return;
	mkdir_p(dir);
	struct stat st;
	if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 022)) {
		fprintf(stderr, "swizzle: %s is not a private directory of this user, logging to stderr\n", dir);
		return;
	}
	snprintf(path, sizeof(path), "%s/swizzle.log", dir);
	snprintf(old, sizeof(old), "%s/swizzle.log.old", dir);
	rename(path, old);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	fchmod(fd, 0600);
	if (isatty(STDERR_FILENO))
		fprintf(stderr, "swizzle: log in %s\n", path);
	dup2(fd, STDERR_FILENO);
	close(fd);
	log_to_file = true;
}

static void usage(const char *argv0)
{
	printf("usage: %s [options]\n"
		"  -c, --config PATH  use this config instead of the default one\n"
		"      --validate     check the config, print its errors and exit\n"
		"      --init         copy the default config to ~/.config/swizzle and exit\n"
		"  -d, --debug        verbose log\n"
		"  -v, --version      print the version\n", argv0);
}

static int init_config(void)
{
	char dst[4096], buf[4096];
	const char *src = SWIZZLE_DATADIR "/swizzle.conf";
	if (!config_user_path(dst, sizeof(dst))) {
		fprintf(stderr, "swizzle: neither XDG_CONFIG_HOME nor HOME is set\n");
		return 1;
	}
	FILE *in = fopen(src, "r");
	if (!in) {
		fprintf(stderr, "swizzle: cannot read %s: %s\n", src, strerror(errno));
		return 1;
	}
	char *slash = strrchr(dst, '/');
	*slash = '\0';
	mkdir_p(dst);
	*slash = '/';
	int fd = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (fd < 0 && errno == EEXIST) {
		fprintf(stderr, "swizzle: %s already exists\n", dst);
		fclose(in);
		return 1;
	}
	FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (fd >= 0 && !out)
		close(fd);
	size_t n;
	bool ok = out != NULL;
	int err = ok ? 0 : errno;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		if (!(ok = fwrite(buf, 1, n, out) == n))
			err = errno;
	if (ok && ferror(in))
		ok = false, err = EIO;
	fclose(in);
	if (out && fclose(out) != 0 && ok)
		ok = false, err = errno;
	if (!ok) {
		fprintf(stderr, "swizzle: cannot write %s: %s\n", dst, strerror(err));
		return 1;
	}
	printf("wrote %s\n", dst);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *conf_path = NULL;
	bool debug = false, check = false;
	int opt;
	static const struct option longopts[] = {
		{"config", required_argument, NULL, 'c'},
		{"validate", no_argument, NULL, 'C'},
		{"init", no_argument, NULL, 'I'},
		{"debug", no_argument, NULL, 'd'},
		{"version", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{0},
	};

	while ((opt = getopt_long(argc, argv, "c:dvh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'I':
			return init_config();
		case 'c':
			conf_path = optarg;
			break;
		case 'C':
			check = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'v':
			printf("swizzle %s\n", SWIZZLE_VERSION);
			return 0;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}
	if (check) {
		wlr_log_init(WLR_ERROR, NULL);
		char *p = conf_path ? strdup(conf_path) : config_default_path();
		bool opened = config_load(&srv.conf, p);
		int errors = srv.conf.errors;
		printf("%s: %s\n", p ? p : "no config found", !opened ? "cannot be read" : errors ? "has errors" : "ok");
		free(p);
		config_free(&srv.conf);
		return opened && !errors ? 0 : 1;
	}
	if (!getenv("XDG_RUNTIME_DIR")) {
		fprintf(stderr, "XDG_RUNTIME_DIR must be set\n");
		return 1;
	}
	char bus[4096];
	snprintf(bus, sizeof(bus), "unix:path=%s/bus", getenv("XDG_RUNTIME_DIR"));
	if (!getenv("DBUS_SESSION_BUS_ADDRESS") && access(bus + strlen("unix:path="), F_OK) != 0) {
		char **args = calloc((size_t)argc + 3, sizeof(*args));
		if (args) {
			args[0] = "dbus-run-session";
			args[1] = "--";
			memcpy(args + 2, argv, (size_t)argc * sizeof(*args));
			execvp(args[0], args);
			free(args);
		}
	}
	redirect_log();
	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, NULL);

	srv.conf_arg = conf_path;
	char *path = conf_path ? strdup(conf_path) : config_default_path();
	config_load(&srv.conf, path);
	free(path);
	config_apply_env(NULL, &srv.conf);
	srv.ipc_fd = -1;

	srv.display = wl_display_create();
	srv.loop = wl_display_get_event_loop(srv.display);
	srv.backend = wlr_backend_autocreate(srv.loop, &srv.session);
	if (!srv.backend) {
		wlr_log(WLR_ERROR, "cannot create backend");
		return 1;
	}

	srv.scene = wlr_scene_create();
	for (size_t i = 0; i < LYR_COUNT; i++)
		srv.layers[i] = wlr_scene_tree_create(&srv.scene->tree);

	if (!gpu_init()) {
		wlr_log(WLR_ERROR, "cannot create renderer");
		return 1;
	}
	wlr_renderer_init_wl_shm(srv.renderer, srv.display);
	create_protocols();

	output_init();
	wlr_xdg_output_manager_v1_create(srv.display, srv.output_layout);
	input_init();
	client_init();
	shell_init();
	effects_init();
	security_context = wlr_security_context_manager_v1_create(srv.display);
	privileged_add(security_context->global);
	wl_display_set_global_filter(srv.display, global_filter, NULL);

	const char *socket = wl_display_add_socket_auto(srv.display);
	if (!socket) {
		wlr_log(WLR_ERROR, "cannot open wayland socket");
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, 1);
	setenv("XDG_CURRENT_DESKTOP", "swizzle:wlroots", 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	if (!getenv("DBUS_SESSION_BUS_ADDRESS") && access(bus + strlen("unix:path="), F_OK) == 0) {
		setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1);
		wlr_log(WLR_INFO, "session bus %s", bus);
	}

	if (!wlr_backend_start(srv.backend)) {
		wlr_log(WLR_ERROR, "cannot start backend");
		return 1;
	}
	ipc_init();
	portal_export();

	signal(SIGPIPE, SIG_IGN);
	struct wl_event_source *sigint = wl_event_loop_add_signal(srv.loop, SIGINT, handle_signal, NULL);
	struct wl_event_source *sigterm = wl_event_loop_add_signal(srv.loop, SIGTERM, handle_signal, NULL);

	if (srv.selmon)
		wlr_cursor_warp_closest(srv.cursor, NULL, srv.selmon->m.x + srv.selmon->m.width / 2.0,
			srv.selmon->m.y + srv.selmon->m.height / 2.0);
	pid_t *autostart = calloc(srv.conf.nautostart ? srv.conf.nautostart : 1, sizeof(*autostart));
	size_t nautostart = srv.conf.nautostart;
	for (size_t i = 0; i < nautostart; i++) {
		pid_t pg = spawn_group(srv.conf.autostart[i]);
		if (autostart)
			autostart[i] = pg;
	}

	wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(srv.display);

	for (size_t i = 0; autostart && i < nautostart; i++)
		if (autostart[i] > 0) {
			kill(-autostart[i], SIGTERM);
			waitpid(autostart[i], NULL, WNOHANG);
		}
	free(autostart);
	ipc_finish();
	wl_event_source_remove(sigint);
	wl_event_source_remove(sigterm);
	wl_display_destroy_clients(srv.display);
	client_finish();
	shell_finish();
	effects_finish();
	wl_list_remove(&request_activate.link);
	output_finish();
	input_finish();
	gpu_finish();
	wlr_backend_destroy(srv.backend);
	wlr_scene_node_destroy(&srv.scene->tree.node);
	wlr_xcursor_manager_destroy(srv.xcursor_mgr);
	wlr_cursor_destroy(srv.cursor);
	wlr_allocator_destroy(srv.allocator);
	wlr_renderer_destroy(srv.renderer);
	wl_display_destroy(srv.display);
	config_free(&srv.conf);
	return 0;
}
