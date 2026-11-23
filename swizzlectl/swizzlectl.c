#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_socket(void)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	const char *path = getenv("SWIZZLE_SOCKET");
	if (path) {
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	} else {
		const char *dir = getenv("XDG_RUNTIME_DIR"), *wd = getenv("WAYLAND_DISPLAY");
		if (!dir || !wd) {
			fprintf(stderr, "swizzlectl: SWIZZLE_SOCKET or XDG_RUNTIME_DIR+WAYLAND_DISPLAY required\n");
			return -1;
		}
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/swizzle.%s.sock", dir, wd);
	}
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("swizzlectl: connect");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char *argv[])
{
	if (argc < 2 || !strcmp(argv[1], "-h")) {
		fprintf(stderr, "usage: swizzlectl <command> [args...]\n"
			"  status | subscribe | reload | quit | spawn <cmd> | view <n> | tag <n> ...\n"
			"  layout tile|dwindle|monocle|grid|scroll|float|next|prev | width next|prev|<0.1-1>\n"
			"  gaps on|off|toggle | swap left|right|up|down\n"
			"  scratchpad [move] | sticky | viewnext [occupied] | viewprev [occupied]\n"
			"  on <app_id> close|floating|fullscreen|activate|sticky|width <w>|scratchpad move|tag <n>|toggletag <n>|tagmon prev|next\n");
		return argc < 2;
	}
	int fd = connect_socket();
	if (fd < 0)
		return 1;

	char line[4096];
	size_t n = 0;
	for (int i = 1; i < argc; i++) {
		int w = snprintf(line + n, sizeof(line) - n, "%s%s", i > 1 ? " " : "", argv[i]);
		if (w < 0 || (size_t)w >= sizeof(line) - n - 1) {
			fprintf(stderr, "swizzlectl: command too long\n");
			return 1;
		}
		n += (size_t)w;
	}
	line[n++] = '\n';
	if (write(fd, line, n) != (ssize_t)n) {
		perror("swizzlectl: write");
		return 1;
	}

	int status = 0;
	char buf[4096];
	ssize_t r;
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		if (r >= 7 && !strncmp(buf, "error: ", 7))
			status = 1;
		fwrite(buf, 1, (size_t)r, stdout);
		fflush(stdout);
	}
	close(fd);
	return status;
}
