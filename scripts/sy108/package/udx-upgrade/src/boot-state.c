/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "misc-state.h"
#include "management-ready.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <mtd/ubi-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

struct volume {
	int fd;
	char sys[256];
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

static int find_volume(const char *name, size_t minimum, struct volume *out)
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
	    number(out->sys, "reserved_ebs", &n) || !n ||
	    number(out->sys, "usable_eb_size", &l) || !l || n > SIZE_MAX / l || n * l < minimum ||
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
	if (size < UDX_MISC_ENV_BYTES ||
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

static int adbd_running(void)
{
	DIR *dir = opendir("/proc");
	struct dirent *entry;
	int found = 0;
	if (!dir)
		return 0;
	while ((entry = readdir(dir))) {
		char path[288], comm[64];
		int fd;
		ssize_t n;
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0)
			continue;
		n = read(fd, comm, sizeof(comm) - 1);
		close(fd);
		if (n <= 0)
			continue;
		comm[n] = 0;
		if (comm[n - 1] == '\n')
			comm[n - 1] = 0;
		if (!strcmp(comm, "adbd")) {
			found = 1;
			break;
		}
	}
	closedir(dir);
	return found;
}

static int adbd_ready(void)
{
	char value[64];
	if (!adbd_running())
		return -1;
	if (access("/dev/usb-ffs/adb/ep1", F_OK))
		return -1;
	if (text("/sys/kernel/config/usb_gadget/udx-usb/UDC", value, sizeof(value)) ||
	    strcmp(value, "29100000.usb\n"))
		return -1;
	return 0;
}


static int boot_accept(char *why, size_t cap)
{
	struct volume misc = {.fd = -1};
	unsigned char *data = NULL;
	char boot[40];
	const char *failure = "boot-accept-adbd";
	int result = -1;
	if (boot_id(boot) || adbd_ready())
		goto out;
	failure = "boot-accept-misc";
	if (find_volume("misc", UDX_MISC_ENV_BYTES, &misc))
		goto out;
	data = malloc(misc.capacity);
	if (!data || readall(misc.fd, data, misc.capacity))
		goto out;
	enum udx_upgrade_state state = udx_misc_state(data, UDX_MISC_ENV_BYTES, 0);
	if (state != UDX_UPGRADE_TRIAL && state != UDX_UPGRADE_READY)
		goto out;
	if (state == UDX_UPGRADE_TRIAL &&
	    (misc_edit(data, misc.capacity, "ready") || update(&misc, data, misc.capacity)))
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
	unsigned char *data = NULL;
	int rc = 1;
	if (find_volume("misc", UDX_MISC_ENV_BYTES, &v))
		goto done;
	data = malloc(v.capacity);
	if (!data || readall(v.fd, data, v.capacity))
		goto done;
	if (!strcmp(argv[1], "trial") &&
	    udx_misc_state(data, UDX_MISC_ENV_BYTES, 0) != UDX_UPGRADE_PENDING)
		goto done;
	if (misc_edit(data, v.capacity, argv[1]) || update(&v, data, v.capacity))
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
