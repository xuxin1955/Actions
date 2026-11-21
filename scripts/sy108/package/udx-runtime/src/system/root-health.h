/* SPDX-License-Identifier: GPL-2.0-only
 * Read-only checks shared by preinit and the local runtime owner.
 * UBI volume IDs are discovered by names on the ubipac MTD device.
 */
#ifndef UDX_ROOT_HEALTH_H
#define UDX_ROOT_HEALTH_H
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifndef UDX_ROOT_SYS
#define UDX_ROOT_SYS "/sys/class"
#endif
#ifndef UDX_ROOT_MOUNTINFO
#define UDX_ROOT_MOUNTINFO "/proc/self/mountinfo"
#endif
#define UDX_UBIFS_MAGIC 0x24051905

struct udx_root_layout {
	unsigned ubi, mtd, rootfs, data;
};

struct udx_root_mount {
	unsigned major, minor, count;
	char options[256], type[32], source[256];
};

static int udx_root_read(const char *path, char *text, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	ssize_t n = read(fd, text, size - 1);
	int error = errno;
	close(fd);
	if (n < 0)
		return -error;
	if (!n || (size_t)n == size - 1)
		return -EOVERFLOW;
	text[n] = 0;
	if (text[n - 1] == '\n')
		text[n - 1] = 0;
	return 0;
}

static int udx_root_discover(struct udx_root_layout *layout)
{
	glob_t devices = {0}, volumes = {0};
	char path[512], text[256], extra;
	unsigned ubi, mtd, count = 0, roots = 0, data = 0;
	int ret, used;
	memset(layout, 0, sizeof(*layout));
	if (snprintf(path, sizeof(path), "%s/ubi/ubi[0-9]*", UDX_ROOT_SYS) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	ret = glob(path, 0, NULL, &devices);
	if (ret) {
		globfree(&devices);
		return ret == GLOB_NOMATCH ? -ENOENT : -EIO;
	}
	for (size_t i = 0; i < devices.gl_pathc; i++) {
		const char *name = strrchr(devices.gl_pathv[i], '/') + 1;
		used = 0;
		if (sscanf(name, "ubi%u%n", &ubi, &used) != 1 || name[used])
			continue;
		snprintf(path, sizeof(path), "%s/mtd_num", devices.gl_pathv[i]);
		if (udx_root_read(path, text, sizeof(text)) ||
		    sscanf(text, "%u%c", &mtd, &extra) != 1)
			continue;
		snprintf(path, sizeof(path), "%s/mtd/mtd%u/name", UDX_ROOT_SYS, mtd);
		if (udx_root_read(path, text, sizeof(text)) || strcmp(text, "ubipac"))
			continue;
		layout->ubi = ubi;
		layout->mtd = mtd;
		count++;
	}
	globfree(&devices);
	if (count != 1)
		return count ? -ENOTUNIQ : -ENOENT;
	snprintf(path, sizeof(path), "%s/ubi/ubi%u/ro_mode", UDX_ROOT_SYS, layout->ubi);
	if ((ret = udx_root_read(path, text, sizeof(text))))
		return ret;
	if (strcmp(text, "0"))
		return -EROFS;
	snprintf(path, sizeof(path), "%s/ubi/ubi%u_*/name", UDX_ROOT_SYS, layout->ubi);
	ret = glob(path, 0, NULL, &volumes);
	if (ret) {
		globfree(&volumes);
		return ret == GLOB_NOMATCH ? -ENOENT : -EIO;
	}
	for (size_t i = 0; i < volumes.gl_pathc; i++) {
		unsigned found_ubi, volume;
		const char *part = volumes.gl_pathv[i] + strlen(UDX_ROOT_SYS) + strlen("/ubi/");
		used = 0;
		if (sscanf(part, "ubi%u_%u/name%n", &found_ubi, &volume, &used) != 2 ||
		    part[used] || found_ubi != layout->ubi)
			continue;
		if ((ret = udx_root_read(volumes.gl_pathv[i], text, sizeof(text))))
			break;
		if (!strcmp(text, "rootfs")) {
			layout->rootfs = volume;
			roots++;
		}
		if (!strcmp(text, "rootfs_data")) {
			layout->data = volume;
			data++;
		}
	}
	globfree(&volumes);
	if (ret)
		return ret;
	if (roots != 1 || data != 1)
		return roots > 1 || data > 1 ? -ENOTUNIQ : -ENOENT;
	/* fstools intentionally skips UBI volumes which also have a block device. */
	snprintf(path, sizeof(path), "%s/block/ubiblock%u_%u", UDX_ROOT_SYS, layout->ubi,
		 layout->data);
	struct stat st;
	if (!stat(path, &st))
		return -EBUSY;
	if (errno != ENOENT)
		return -errno;
	return 0;
}

static int udx_root_option(const char *options, const char *wanted)
{
	size_t length = strlen(wanted);
	for (const char *p = options; *p;) {
		const char *end = strchr(p, ',');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (n == length && !memcmp(p, wanted, n))
			return 1;
		if (!end)
			break;
		p = end + 1;
	}
	return 0;
}

static int udx_root_mounts(struct udx_root_mount mounts[3])
{
	FILE *file = fopen(UDX_ROOT_MOUNTINFO, "re");
	if (!file)
		return -errno;
	char *line = NULL;
	size_t size = 0;
	int ret = 0;
	while (getline(&line, &size, file) >= 0) {
		char mountpoint[256], root[256], options[256];
		unsigned major, minor;
		if (sscanf(line, "%*u %*u %u:%u %255s %255s %255s", &major, &minor, root,
			   mountpoint, options) != 5) {
			ret = -EPROTO;
			break;
		}
		int index = !strcmp(mountpoint, "/")	      ? 0
			    : !strcmp(mountpoint, "/rom")     ? 1
			    : !strcmp(mountpoint, "/overlay") ? 2
							      : -1;
		if (index < 0)
			continue;
		char *separator = strstr(line, " - ");
		if (!separator || sscanf(separator + 3, "%31s %255s", mounts[index].type,
					 mounts[index].source) != 2) {
			ret = -EPROTO;
			break;
		}
		mounts[index].major = major;
		mounts[index].minor = minor;
		mounts[index].count++;
		strcpy(mounts[index].options, options);
	}
	if (ferror(file) && !ret)
		ret = -EIO;
	free(line);
	fclose(file);
	return ret;
}

static int udx_root_fs(const char *path, long magic, int readonly)
{
	struct statfs fs;
	struct statvfs vfs;
	if (statfs(path, &fs) || statvfs(path, &vfs))
		return -errno;
	if ((long)fs.f_type != magic || !!(vfs.f_flag & ST_RDONLY) != readonly)
		return -EROFS;
	return 0;
}

static int udx_root_check(int preinit, int require_procd, struct udx_root_layout *layout)
{
	struct udx_root_mount mounts[3] = {0};
	char path[512], text[256], source[64], named_source[64], extra;
	unsigned major, minor;
	int ret = udx_root_discover(layout);
	if (ret || (ret = udx_root_mounts(mounts)))
		return ret;
	snprintf(path, sizeof(path), "%s/block/ubiblock%u_%u/dev", UDX_ROOT_SYS, layout->ubi,
		 layout->rootfs);
	if ((ret = udx_root_read(path, text, sizeof(text))))
		return ret;
	if (sscanf(text, "%u:%u%c", &major, &minor, &extra) != 2)
		return -EPROTO;
	struct udx_root_mount *rom = &mounts[preinit ? 0 : 1];
	if (rom->count != 1 || strcmp(rom->type, "squashfs") || rom->major != major ||
	    rom->minor != minor || !udx_root_option(rom->options, "ro"))
		return -EXDEV;
	if ((ret = udx_root_fs(preinit ? "/" : "/rom", SQUASHFS_MAGIC, 1)))
		return ret;
	if (!preinit) {
		snprintf(source, sizeof(source), "/dev/ubi%u_%u", layout->ubi, layout->data);
		snprintf(named_source, sizeof(named_source), "ubi%u:rootfs_data", layout->ubi);
		if (mounts[0].count != 1 || strcmp(mounts[0].type, "overlay") ||
		    !udx_root_option(mounts[0].options, "rw") || mounts[2].count != 1 ||
		    strcmp(mounts[2].type, "ubifs") || !udx_root_option(mounts[2].options, "rw") ||
		    (strcmp(mounts[2].source, source) && strcmp(mounts[2].source, named_source)))
			return -EXDEV;
		if ((ret = udx_root_fs("/", OVERLAYFS_SUPER_MAGIC, 0)) ||
		    (ret = udx_root_fs("/overlay", UDX_UBIFS_MAGIC, 0)))
			return ret;
		struct stat overlay, upper, work;
		if (stat("/overlay", &overlay) || stat("/overlay/upper", &upper) ||
		    stat("/overlay/work", &work))
			return -errno;
		if (!S_ISDIR(upper.st_mode) || !S_ISDIR(work.st_mode) ||
		    overlay.st_dev != upper.st_dev || overlay.st_dev != work.st_dev)
			return -EXDEV;
	}
	if (require_procd) {
		if ((ret = udx_root_read("/proc/1/comm", text, sizeof(text))))
			return ret;
		if (strcmp(text, "procd"))
			return -EHOSTDOWN;
	}
	return 0;
}
#endif
