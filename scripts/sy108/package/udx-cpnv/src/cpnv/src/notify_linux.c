/* SPDX-License-Identifier: GPL-2.0-only */
#include "refnotify.h"
#include <errno.h>
#include <linux/rtc.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <time.h>
#ifdef __GLIBC__
#define RN_IOCTL_ARG(v) ((unsigned long)(v))
#else
/* musl declares ioctl(int, int, ...); keep the request bit pattern. */
#define RN_IOCTL_ARG(v) ((int)(v))
#endif
int rn_linux_read_calendar(void *ctx, struct rn_calendar *out)
{
	struct rn_linux *l = ctx;
	struct rtc_time tm;
	int ret, w;
	int64_t e;
	if (!l || !out || l->rtc_fd < 0)
		return -EINVAL;
	memset(&tm, 0, sizeof(tm));
	ret = ioctl(l->rtc_fd, RN_IOCTL_ARG(RTC_RD_TIME), &tm);
	if (ret < 0)
		return -errno;
	if (ret)
		return -EPROTO;
	/* Bound before additions; no signed overflow from malformed driver data. */
	if (tm.tm_year < 70 || tm.tm_year > 8099 || tm.tm_mon < 0 || tm.tm_mon > 11)
		return -ERANGE;
	*out = (struct rn_calendar){tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
				    tm.tm_min,	       tm.tm_sec,     0};
	ret = rn_calendar_epoch(out, &e, &w);
	if (ret)
		return ret;
	out->weekday = w;
	return 0;
}

int rn_linux_uptime(void *ctx, uint64_t *out)
{
	struct sysinfo info;
	(void)ctx;
	if (!out)
		return -EINVAL;
	memset(&info, 0, sizeof(info));
	if (sysinfo(&info) < 0)
		return -errno;
	if ((uint64_t)info.uptime > (uint64_t)LONG_MAX)
		return -ERANGE;
	*out = (uint64_t)info.uptime;
	return 0;
}

ssize_t rn_linux_send(void *ctx, int fd, const void *b, size_t n)
{
	ssize_t ret;
	(void)ctx;
	if (fd < 0 || !b || n > 8)
		return -EINVAL;
	ret = send(fd, b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
	return ret < 0 ? -errno : ret;
}

int rn_linux_timesync_address(struct sockaddr_un *a, socklen_t *length)
{
	static const char name[] = "cp_time_sync_server";
	if (!a || !length)
		return -EINVAL;
	memset(a, 0, sizeof(*a));
	a->sun_family = AF_UNIX;
	/* Abstract namespace length excludes a terminal NUL after the name. */
	memcpy(a->sun_path + 1, name, sizeof(name) - 1);
	*length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof(name));
	return 0;
}

int rn_linux_check_client(int fd, uid_t allowed_uid)
{
	int domain, type;
	struct ucred peer;
	socklen_t n;
	if (fd < 0)
		return -EINVAL;
	n = sizeof(domain);
	if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &n) < 0)
		return -errno;
	if (n != sizeof(domain) || domain != AF_UNIX)
		return -EPROTOTYPE;
	n = sizeof(type);
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &n) < 0)
		return -errno;
	if (n != sizeof(type) || type != SOCK_STREAM)
		return -EPROTOTYPE;
	n = sizeof(peer);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &n) < 0)
		return -errno;
	if (n != sizeof(peer) || peer.pid <= 0 || peer.uid != allowed_uid)
		return -EACCES;
	return 0;
}
