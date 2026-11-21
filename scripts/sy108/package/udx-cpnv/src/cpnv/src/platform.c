/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

uint64_t cpnv_now(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int number(const char *dir, const char *leaf, uint64_t *out)
{
	char path[256], text[64], *end;
	snprintf(path, sizeof path, "%s/%s", dir, leaf);
	int n = cpnv_read(path, text, sizeof text - 1);
	if (n <= 0)
		return n ? n : -EIO;
	text[n] = 0;
	errno = 0;
	unsigned long long value = strtoull(text, &end, 10);
	if (errno || end == text || (*end && strcmp(end, "\n")))
		return -EINVAL;
	*out = value;
	return 0;
}

int volume_open(struct volume *v, const char *name, int access, uint64_t minimum)
{
	glob_t list = {0};
	int result = -ENOENT;
	v->fd = -1;
	if (glob("/sys/class/ubi/ubi0_*/name", 0, NULL, &list))
		return result;
	for (size_t i = 0; i < list.gl_pathc; i++) {
		char value[128], dir[192], path[256];
		int n = cpnv_read(list.gl_pathv[i], value, sizeof value - 1);
		if (n <= 0)
			continue;
		value[n] = 0;
		if (strlen(name) + 1 != (size_t)n || memcmp(value, name, strlen(name)) ||
		    value[n - 1] != '\n')
			continue;
		snprintf(dir, sizeof dir, "%.*s", (int)strlen(list.gl_pathv[i]) - 5,
			 list.gl_pathv[i]);
		const char *base = strrchr(dir, '/');
		unsigned id;
		if (!base || sscanf(base, "/ubi0_%u", &id) != 1) {
			result = -EINVAL;
			break;
		}
		uint64_t blocks, leb, minio, corrupt, updating, ro;
		if (number(dir, "reserved_ebs", &blocks) || number(dir, "usable_eb_size", &leb) ||
		    number(dir, "corrupted", &corrupt) || number(dir, "upd_marker", &updating) ||
		    number("/sys/class/ubi/ubi0", "min_io_size", &minio) ||
		    number("/sys/class/ubi/ubi0", "ro_mode", &ro) || !leb ||
		    blocks > UINT64_MAX / leb || blocks * leb < minimum || corrupt || updating ||
		    (access == O_RDWR && ro)) {
			result = -EIO;
			break;
		}
		snprintf(path, sizeof path, "%s/type", dir);
		n = cpnv_read(path, value, sizeof value);
		unsigned type = n == 8 && !memcmp(value, "dynamic\n", 8)  ? 1
				: n == 7 && !memcmp(value, "static\n", 7) ? 2
									  : 0;
		if (!type || (access == O_RDWR && type != 1)) {
			result = -EINVAL;
			break;
		}
		snprintf(path, sizeof path, "/dev/ubi0_%u", id);
		int fd = open(path, access | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			result = -errno;
			break;
		}
		struct stat st;
		unsigned ma, mi;
		snprintf(path, sizeof path, "%s/dev", dir);
		n = cpnv_read(path, value, sizeof value - 1);
		if (n <= 0 || n >= (int)sizeof value || fstat(fd, &st)) {
			close(fd);
			result = -EIO;
			break;
		}
		value[n] = 0;
		if (sscanf(value, "%u:%u", &ma, &mi) != 2 || !S_ISCHR(st.st_mode) ||
		    st.st_rdev != makedev(ma, mi)) {
			close(fd);
			result = -ENODEV;
			break;
		}
		if (access == O_RDWR) {
			struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
			if (fcntl(fd, F_SETLK, &lock)) {
				result = -errno;
				close(fd);
				break;
			}
		}
		v->fd = fd;
		v->number = id;
		v->identity = (struct nv_ubi_identity){.inode = st.st_ino,
						       .device = st.st_dev,
						       .rdev = st.st_rdev,
						       .capacity = blocks * leb,
						       .leb_size = leb,
						       .min_io_size = minio,
						       .owner_uid = st.st_uid,
						       .volume_type = type,
						       .health = ro ? 6 : NVU_HEALTH_REQUIRED};
		result = 0;
		break;
	}
	globfree(&list);
	return result;
}

void volume_close(struct volume *v)
{
	if (v->fd >= 0)
		close(v->fd);
	v->fd = -1;
}

int cpnv_policy_load(struct cpnv_policy *p)
{
	char text[4096];
	int n = cpnv_private_read("/etc/udx-cpnv-warm/profile", text, sizeof text, false);
	if (n <= 0)
		return n ? n : -EINVAL;
	int e = cpnv_policy_parse(text, (size_t)n, p);
	if (e)
		return e;
	if (p->enabled != 1 || p->nv.storage != 1 || p->count != 6 || p->nv.write_policy != 3 ||
	    !p->nv.timeout_ms || p->nv.timeout_ms > 60000 || !p->nv.startup_ms ||
	    p->nv.startup_ms > 120000)
		return -EINVAL;
	return 0;
}
