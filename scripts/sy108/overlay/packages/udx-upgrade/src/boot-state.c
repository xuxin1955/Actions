/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "misc-state.h"
#define MISC_BYTES (9u * 126976u)
#include "root-health.h"
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include "management-ready.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/magic.h>
#include <mtd/ubi-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>

struct volume {
	int fd;
	char sys[256], node[320];
	size_t capacity;
};

static int text(const char *p, char *b, size_t n)
{
	int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	ssize_t r = read(fd, b, n - 1);
	close(fd);
	if (r < 0 || (size_t)r == n - 1)
		return -1;
	b[r] = 0;
	return 0;
}

static int property(const char *path, const char *expected)
{
	char b[128];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, b, sizeof(b));
	close(fd);
	return n == (ssize_t)strlen(expected) + 1 && !memcmp(b, expected, n) ? 0 : -1;
}

static int number(const char *base, const char *name, unsigned long *v)
{
	char p[320], b[64], *e;
	snprintf(p, sizeof(p), "%s/%s", base, name);
	if (text(p, b, sizeof(b)))
		return -1;
	errno = 0;
	*v = strtoul(b, &e, 10);
	return errno || strcmp(e, "\n") ? -1 : 0;
}

static int find_volume(const char *name, unsigned lebs, struct volume *out)
{
	glob_t g = {0};
	char p[320], b[64];
	unsigned found = 0;
	memset(out, 0, sizeof(*out));
	out->fd = -1;
	if (glob("/sys/class/ubi/ubi[0-9]*_[0-9]*", 0, 0, &g))
		return -1;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		snprintf(p, sizeof(p), "%s/name", g.gl_pathv[i]);
		if (text(p, b, sizeof(b)))
			continue;
		b[strcspn(b, "\n")] = 0;
		if (!strcmp(b, name)) {
			if (strlen(g.gl_pathv[i]) >= sizeof(out->sys)) {
				found = 2;
				break;
			}
			strcpy(out->sys, g.gl_pathv[i]);
			found++;
		}
	}
	globfree(&g);
	if (found != 1)
		return -1;
	unsigned long n, l, corrupt, update;
	snprintf(p, sizeof(p), "%s/type", out->sys);
	if (text(p, b, sizeof(b)) || strcmp(b, "dynamic\n") ||
	    number(out->sys, "reserved_ebs", &n) || n != lebs ||
	    number(out->sys, "usable_eb_size", &l) || l != 126976 ||
	    number(out->sys, "corrupted", &corrupt) || corrupt > 1 ||
	    number(out->sys, "upd_marker", &update) || update > 1)
		return -1;
	/* A full signed replacement is the recovery for an interrupted boot/rootfs
	 * update. misc requires intact bytes because its unrelated keys are kept. */
	if (!strcmp(name, "misc") && (corrupt || update))
		return -1;
	snprintf(p, sizeof(p), "%s/dev", out->sys);
	if (text(p, b, sizeof(b)))
		return -1;
	unsigned maj, min;
	char extra;
	if (sscanf(b, "%u:%u %c", &maj, &min, &extra) != 2)
		return -1;
	const char *node = strrchr(out->sys, '/') + 1;
	snprintf(p, sizeof(p), "/sys/class/block/ubiblock%s", node + 3);
	if (!access(p, F_OK))
		return -1; /* Do not begin a transaction against a mapped root. */
	snprintf(p, sizeof(p), "/dev/%s", strrchr(out->sys, '/') + 1);
	snprintf(out->node, sizeof(out->node), "%s", p);
	struct stat st;
	if (lstat(p, &st) || !S_ISCHR(st.st_mode) || st.st_rdev != makedev(maj, min))
		return -1;
	out->capacity = n * l;
	out->fd = open(p, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (out->fd < 0)
		return -1;
	if (fstat(out->fd, &st) || !S_ISCHR(st.st_mode) || st.st_rdev != makedev(maj, min)) {
		close(out->fd);
		out->fd = -1;
		return -1;
	}
	out->capacity = n * l;
	return 0;
}

int misc_edit(unsigned char *data, size_t size, const char *state)
{
	if (size != MISC_BYTES ||
	    (strcmp(state, "pending") && strcmp(state, "trial") && strcmp(state, "ready")))
		return -1;
	uint32_t expected = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
			    (uint32_t)data[3] << 24;
	if (udx_crc32(data + 4, UDX_MISC_ENV_BYTES - 4) != expected)
		return -1;
	unsigned char *out = calloc(1, UDX_MISC_ENV_BYTES);
	if (!out)
		return -1;
	size_t pos = 4, dst = 4;
	unsigned found = 0;
	int valid = 0;
	while (pos < UDX_MISC_ENV_BYTES) {
		if (!data[pos]) {
			if (pos > 4 && data[pos - 1] == 0)
				valid = 1;
			break;
		}
		size_t end = pos;
		while (end < UDX_MISC_ENV_BYTES && data[end])
			end++;
		if (end == UDX_MISC_ENV_BYTES)
			break;
		const unsigned char *eq = memchr(data + pos, '=', end - pos);
		if (!eq || eq == data + pos)
			break;
		if ((size_t)(eq - data - pos) == strlen(UDX_BOOT_STATE_KEY) &&
		    !memcmp(data + pos, UDX_BOOT_STATE_KEY, strlen(UDX_BOOT_STATE_KEY))) {
			if (found++)
				break;
		} else {
			size_t n = end - pos + 1;
			if (dst + n + 64 >= UDX_MISC_ENV_BYTES)
				break;
			memcpy(out + dst, data + pos, n);
			dst += n;
		}
		pos = end + 1;
	}
	if (!valid || found > 1) {
		free(out);
		return -1;
	}
	int n = snprintf((char *)out + dst, UDX_MISC_ENV_BYTES - dst, "%s=%s", UDX_BOOT_STATE_KEY,
			 state);
	if (n < 0 || dst + n + 2 >= UDX_MISC_ENV_BYTES) {
		free(out);
		return -1;
	}
	uint32_t crc = udx_crc32(out + 4, UDX_MISC_ENV_BYTES - 4);
	for (unsigned i = 0; i < 4; i++)
		out[i] = crc >> (i * 8);
	memcpy(data, out, UDX_MISC_ENV_BYTES);
	free(out);
	return 0;
}

static int readall(int fd, void *data, size_t n)
{
	unsigned char *p = data;
	size_t pos = 0;
	while (pos < n) {
		ssize_t r = pread(fd, p + pos, n - pos, pos);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		pos += r;
	}
	return 0;
}

static int update(struct volume *v, const void *data, size_t size)
{
	if (size > v->capacity)
		return -1;
	int64_t bytes = size;
	if (ioctl(v->fd, UBI_IOCVOLUP, &bytes))
		return -1;
	size_t pos = 0;
	while (pos < size) {
		size_t n = size - pos;
		if (n > 65536)
			n = 65536;
		ssize_t w = write(v->fd, (const char *)data + pos, n);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		pos += w;
	}
	if (fsync(v->fd))
		return -1;
	return 0;
}

static int boot_id(char value[40])
{
	if (text("/proc/sys/kernel/random/boot_id", value, 40) || strlen(value) != 37 ||
	    value[36] != '\n')
		return -1;
	value[36] = 0;
	for (unsigned i = 0; i < 36; i++)
		if ((i == 8 || i == 13 || i == 18 || i == 23)
			? value[i] != '-'
			: !((value[i] >= '0' && value[i] <= '9') ||
			    (value[i] >= 'a' && value[i] <= 'f')))
			return -1;
	return 0;
}

struct runtime_readiness {
	const char *boot;
	int ready;
};

static void runtime_reply(struct ubus_request *request, int type, struct blob_attr *message)
{
	enum { BOOT, OWNER, CORE, ERROR, RENEWALS, LAST, CLOSING, ROOT, QUIESCE, QERROR, N };
	static const struct blobmsg_policy policy[N] = {
	    [BOOT] = {.name = "boot_id", .type = BLOBMSG_TYPE_STRING},
	    [OWNER] = {.name = "owner", .type = BLOBMSG_TYPE_INT32},
	    [CORE] = {.name = "core", .type = BLOBMSG_TYPE_STRING},
	    [ERROR] = {.name = "core_error", .type = BLOBMSG_TYPE_INT32},
	    [RENEWALS] = {.name = "renewals", .type = BLOBMSG_TYPE_INT32},
	    [LAST] = {.name = "last_renew_ms", .type = BLOBMSG_TYPE_INT64},
	    [CLOSING] = {.name = "closing", .type = BLOBMSG_TYPE_BOOL},
	    [ROOT] = {.name = "root", .type = BLOBMSG_TYPE_STRING},
	    [QUIESCE] = {.name = "quiesce", .type = BLOBMSG_TYPE_STRING},
	    [QERROR] = {.name = "quiesce_error", .type = BLOBMSG_TYPE_INT32},
	};
	struct runtime_readiness *s = request->priv;
	struct blob_attr *tb[N];
	struct timespec now;
	(void)type;
	if (!message)
		return;
	blobmsg_parse(policy, N, tb, blob_data(message), blob_len(message));
	for (unsigned i = 0; i < N; i++)
		if (!tb[i])
			return;
	if (strcmp(blobmsg_get_string(tb[BOOT]), s->boot) || blobmsg_get_u32(tb[OWNER]) <= 1 ||
	    strcmp(blobmsg_get_string(tb[CORE]), "ready") || blobmsg_get_u32(tb[ERROR]) ||
	    blobmsg_get_bool(tb[CLOSING]) || blobmsg_get_u32(tb[RENEWALS]) < 2 ||
	    strcmp(blobmsg_get_string(tb[ROOT]), "overlay") ||
	    strcmp(blobmsg_get_string(tb[QUIESCE]), "idle") || blobmsg_get_u32(tb[QERROR]) ||
	    clock_gettime(CLOCK_BOOTTIME, &now))
		return;
	uint64_t milliseconds = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	uint64_t last = blobmsg_get_u64(tb[LAST]);
	s->ready = last <= milliseconds && milliseconds - last <= 25000;
}

static int live_runtime(const char *boot)
{
	struct runtime_readiness state = {.boot = boot};
	struct ubus_context *ctx;
	uint32_t id;
	int ret = -1;
	/* Object lookup itself is synchronous and has no timeout argument. */
	alarm(2);
	ctx = ubus_connect(NULL);
	if (ctx) {
		if (!ubus_lookup_id(ctx, "udx.runtime", &id) &&
		    !ubus_invoke(ctx, id, "status", NULL, runtime_reply, &state, 1000) &&
		    state.ready)
			ret = 0;
		ubus_free(ctx);
	}
	alarm(0);
	return ret;
}

static int management_listener(unsigned port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_in address = {.sin_family = AF_INET,
				      .sin_port = htons(port),
				      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	int result = -1, error = 0;
	socklen_t length = sizeof(error);
	if (fd < 0)
		return -1;
	if (!connect(fd, (void *)&address, sizeof(address)))
		result = 0;
	else if (errno == EINPROGRESS) {
		struct pollfd wait = {.fd = fd, .events = POLLOUT};
		if (poll(&wait, 1, 500) == 1 &&
		    !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) && !error)
			result = 0;
	}
	close(fd);
	return result;
}

static int management_network_ready(void)
{
	/* The same bounded helper serves boot acceptance and live USB status. */
	pid_t child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		setpgid(0, 0);
		execl("/usr/libexec/udx-boot-state", "udx-boot-state", "management-ready",
		      (char *)0);
		_exit(127);
	}
	setpgid(child, child);
	int status;
	for (unsigned i = 0; i < 30; i++) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
		if (result < 0 && errno != EINTR)
			break;
		struct timespec delay = {0, 100000000};
		nanosleep(&delay, NULL);
	}
	kill(-child, SIGKILL);
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	return -1;
}

/* /run/udx-usb/applied starts with the active profile line. Keep every gadget
 * profile the daemon supports readable here, otherwise an ECM/RNDIS boot would
 * be treated as an unmanaged gadget and block the boot trial confirmation. */
static int usb_profile_line(const char *value)
{
	static const char *const profiles[] = {"ncm", "ncm-software", "ecm", "rndis"};

	if (strncmp(value, "profile=", 8))
		return 0;
	value += 8;
	for (size_t i = 0; i < sizeof(profiles) / sizeof(*profiles); i++) {
		size_t len = strlen(profiles[i]);
		if (!strncmp(value, profiles[i], len) && value[len] == '\n')
			return 1;
	}
	return 0;
}

static int boot_management_ready(void)
{
	char value[512];
	/* Logical gadget readiness is independent of a connected host or SIM.
	 * All run files are from this boot; require the live UDC binding too. */
	if (text("/sys/kernel/config/usb_gadget/udx-usb/UDC", value, sizeof(value)) ||
	    strcmp(value, "29100000.usb\n") || text("/run/udx-usb/applied", value, sizeof(value)) ||
	    !usb_profile_line(value) ||
	    management_listener(22) || (management_listener(80) && management_listener(443)) ||
	    management_network_ready())
		return -1;
	return 0;
}

static int boot_accept(char *why, size_t cap)
{
	struct volume misc = {.fd = -1};
	struct udx_root_layout layout;
	unsigned char *data = NULL;
	char boot[40], value[64];
	const char *failure = "boot-accept-health";
	int result = -1;
	if (text("/etc/udx-openwrt-root", value, sizeof(value)) ||
	    strcmp(value, "OpenWrt production root\n") ||
	    property("/sys/firmware/devicetree/base/chosen/udx,boot-policy", "openwrt-v1") ||
	    property("/sys/firmware/devicetree/base/chosen/udx,boot-target", "openwrt") ||
	    boot_id(boot) || udx_root_check(0, 1, &layout) || live_runtime(boot))
		goto out;
	failure = "boot-accept-management";
	if (boot_management_ready())
		goto out;
	failure = "boot-accept-misc";
	if (find_volume("misc", 9, &misc))
		goto out;
	data = malloc(MISC_BYTES);
	if (!data || readall(misc.fd, data, MISC_BYTES))
		goto out;
	enum udx_upgrade_state state = udx_misc_state(data, UDX_MISC_ENV_BYTES, 0);
	if (state != UDX_UPGRADE_TRIAL && state != UDX_UPGRADE_READY)
		goto out;
	if (state == UDX_UPGRADE_TRIAL &&
	    (misc_edit(data, MISC_BYTES, "ready") || update(&misc, data, MISC_BYTES)))
		goto out;
	/* Existing runtime's deadline is boot-specific. This file never survives
	 * reboot; durable acceptance remains the CRC-protected misc field. */
	failure = "boot-accept-runtime";
	int accepted = open("/run/udx-boot-accepted",
			    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (accepted < 0)
		goto out;
	strcat(boot, "\n");
	int bad = write(accepted, boot, strlen(boot)) != (ssize_t)strlen(boot);
	close(accepted);
	if (bad)
		goto out;
	failure = "boot-accepted";
	result = 0;
out:
	free(data);
	if (misc.fd >= 0)
		close(misc.fd);
	snprintf(why, cap, "%s", failure);
	return result;
}

/* Only the native bootloader's persistent state is device-specific. Firmware data
 * is written by OpenWrt's standard UBI utilities. */
int main(int argc, char **argv)
{
	if (getuid() || argc != 2)
		return 64;
	if (!strcmp(argv[1], "management-ready")) {
		const char *reason;
		int ret = udx_management_ready(&reason);
		printf("dhcp_readiness=%s\ndhcp_reason=%s\n", ret ? "not-ready" : "ready", reason);
		return !!ret;
	}
	if (!strcmp(argv[1], "accept")) {
		char why[80];
		int rc = boot_accept(why, sizeof(why));
		if (rc)
			fprintf(stderr, "%s\n", why);
		return !!rc;
	}
	if (strcmp(argv[1], "pending") && strcmp(argv[1], "trial"))
		return 64;
	if (property("/sys/firmware/devicetree/base/chosen/udx,boot-policy", "openwrt-v1"))
		return 1;
	struct volume v = {.fd = -1};
	unsigned char *data = malloc(MISC_BYTES);
	int rc = 1;
	if (!data || find_volume("misc", 9, &v) || readall(v.fd, data, MISC_BYTES))
		goto done;
	if (!strcmp(argv[1], "trial") &&
	    udx_misc_state(data, UDX_MISC_ENV_BYTES, 0) != UDX_UPGRADE_PENDING)
		goto done;
	if (misc_edit(data, MISC_BYTES, argv[1]) || update(&v, data, MISC_BYTES))
		goto done;
	rc = 0;
done:
	if (v.fd >= 0)
		close(v.fd);
	free(data);
	if (rc)
		perror("SY108 boot state");
	return rc;
}
