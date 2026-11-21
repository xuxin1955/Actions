/* SPDX-License-Identifier: GPL-2.0-only
 * SY108 service/event owner. It exclusively opens the PMIC watchdog.
 * The same event loop checks health and feeds the standard watchdog device.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/magic.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <syslog.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "root-health.h"
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#define CHOSEN "/sys/firmware/devicetree/base/chosen/"
#define RENEW_MS 20000u
#define QUERY_MS 5000u
#define MAX_PATH 512
#define LOG_TIMEOUT_MS 15000u
#define QUIESCE_TIMEOUT_MS 26000u
#ifndef SYSTEM_QUIESCE
#define SYSTEM_QUIESCE "/usr/sbin/udx-system-quiesce"
#endif
#ifndef TRIAL_ACCEPTED_PATH
#define TRIAL_ACCEPTED_PATH "/run/udx-boot-accepted"
#endif
/* fs/ubifs/ubifs.h; not exported by older libc linux/magic.h headers. */
#define STAGE2_UBIFS_MAGIC 0x24051905

struct cpnv_health {
	int healthy, error;
	char storage[16];
};

struct runtime {
	char rtc[MAX_PATH], board[MAX_PATH], pmsys[MAX_PATH], boot[64];
	int recovery, production, boot_policy;
	unsigned ready_stage;
	int watchdog_fd, closing, core_error, cpnv_error;
	struct ubus_auto_conn bus;
	struct ubus_object object;
	struct uloop_fd signal;
	struct uloop_timeout tick;
	pid_t quiesce_pid;
	uint64_t quiesce_deadline;
	int quiesce_state, quiesce_error; /* 0 idle, 1 running, 2 finished */
	char quiesce_target[24];
	struct cpnv_health cpnv;
	unsigned renewals;
	uint64_t last_renew, next_health, last_health;
	uint64_t trial_deadline;
	unsigned long long event_sequence;
	char event_kind[24];
	pid_t log_pid;
	uint64_t log_started;
	int log_error;
};

static int kernel_log = -1;

/* kmsg supplies the single severity-aware log stream consumed by OpenWrt
 * logd. Keep stderr only as a fallback if kmsg is unavailable; duplicating
 * every message on procd stderr incorrectly labels successful work as ERR.
 */
static void runtime_log(int priority, const char *format, ...)
{
	char text[1024];
	va_list args;
	int prefix = snprintf(text, sizeof(text), "<%d>", LOG_USER | priority);
	va_start(args, format);
	int n = vsnprintf(text + prefix, sizeof(text) - prefix, format, args);
	va_end(args);
	if (n <= 0)
		return;
	size_t size = (size_t)n >= sizeof(text) - prefix ? sizeof(text) - 1 : (size_t)n + prefix;
	if (kernel_log >= 0 && write(kernel_log, text, size) == (ssize_t)size)
		return;
	(void)write(STDERR_FILENO, text + prefix, size - prefix);
}

static uint64_t milliseconds(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_BOOTTIME, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int read_text(const char *path, char *out, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	ssize_t n = read(fd, out, size - 1);
	int error = errno;
	close(fd);
	if (n < 0)
		return -error;
	if (!n || (size_t)n == size - 1)
		return -EOVERFLOW;
	out[n] = 0;
	return 0;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	size_t size = strlen(text);
	ssize_t n = write(fd, text, size);
	int error = errno;
	close(fd);
	return n < 0 ? -error : (size_t)n == size ? 0 : -EIO;
}

static int exact_file(const char *path, const char *value)
{
	char text[256];
	int ret = read_text(path, text, sizeof(text));
	return ret ? ret : strcmp(text, value) ? -EPROTO : 0;
}

static int unique_path(const char *pattern, char *path, size_t size)
{
	glob_t matches;
	int ret = glob(pattern, GLOB_NOSORT, NULL, &matches);
	if (ret) {
		globfree(&matches);
		return ret == GLOB_NOMATCH ? -ENOENT : -EIO;
	}
	if (matches.gl_pathc != 1 || strlen(matches.gl_pathv[0]) >= size)
		ret = -EEXIST;
	else {
		strcpy(path, matches.gl_pathv[0]);
		ret = 0;
	}
	globfree(&matches);
	return ret;
}

static void cpnv_status(struct ubus_request *request, int type, struct blob_attr *message)
{
	(void)type;
	struct cpnv_health *h = request->priv;

	enum { READY, NV, HALO, ERROR, STORAGE, COUNT };
	static const struct blobmsg_policy policy[COUNT] = {
	    [READY] = {.name = "ready", .type = BLOBMSG_TYPE_BOOL},
	    [NV] = {.name = "nv_ready", .type = BLOBMSG_TYPE_BOOL},
	    [HALO] = {.name = "halo_ready", .type = BLOBMSG_TYPE_BOOL},
	    [ERROR] = {.name = "last_error", .type = BLOBMSG_TYPE_INT32},
	    [STORAGE] = {.name = "storage", .type = BLOBMSG_TYPE_STRING},
	};
	struct blob_attr *tb[COUNT];
	if (!message)
		return;
	blobmsg_parse(policy, COUNT, tb, blob_data(message), blob_len(message));
	if (!tb[READY] || !tb[NV] || !tb[HALO] || !tb[ERROR] || !tb[STORAGE])
		return;
	h->error = (int32_t)blobmsg_get_u32(tb[ERROR]);
	const char *storage = blobmsg_get_string(tb[STORAGE]);
	if (h->error > 0 || strlen(storage) >= sizeof h->storage) {
		h->error = -EPROTO;
		return;
	}
	strcpy(h->storage, storage);
	h->healthy = blobmsg_get_bool(tb[READY]) && blobmsg_get_bool(tb[NV]) &&
		     blobmsg_get_bool(tb[HALO]) && !h->error;
	if (!h->healthy && !h->error)
		h->error = -EHOSTDOWN;
}

static int runtime_cpnv_query(struct runtime *r)
{
	if (r->recovery)
		return 0;
	memset(&r->cpnv, 0, sizeof r->cpnv);
	r->cpnv.error = -EPROTO;
	struct ubus_context *ctx = ubus_connect(NULL);
	if (!ctx)
		return -ECONNREFUSED;
	uint32_t id;
	int ret = ubus_lookup_id(ctx, "udx.cpnv", &id);
	if (!ret)
		ret = ubus_invoke(ctx, id, "status", NULL, cpnv_status, &r->cpnv, 500);
	ubus_free(ctx);
	ret = ret ? -EIO : r->cpnv.error;
	if (!ret && r->production && strcmp(r->cpnv.storage, "ubi"))
		ret = -EXDEV;
	if (ret)
		r->cpnv.healthy = 0;
	return ret;
}

static int root_health(struct runtime *r)
{
	struct statfs fs;
	int ret;
	if (r->recovery) {
		if ((ret = exact_file("/etc/udx-recovery-root", "OpenWrt Recovery RAM root\n")) ||
		    (ret = exact_file("/proc/1/comm", "procd\n")))
			return ret;
		if (statfs("/", &fs))
			return -errno;
		if (fs.f_type != TMPFS_MAGIC)
			return -EROFS;
	} else if (r->production) {
		struct udx_root_layout layout;
		if ((ret = exact_file("/etc/udx-openwrt-root", "OpenWrt production root\n")) ||
		    (ret = udx_root_check(0, 1, &layout)))
			return ret;
	} else
		return -ENODEV;
	return 0;
}

static int core_health(struct runtime *r)
{
	char text[256];
	int ret;
	if ((ret = read_text(r->pmsys, text, sizeof(text))))
		return ret;
	if (strcmp(text, "running\n"))
		return -EHOSTDOWN;
	/* The board event endpoint is a required live kernel dependency. Sensor
	 * values, temperature sampling and USB attachment are not health gates.
	 */
	if ((ret = read_text(r->board, text, sizeof(text))))
		return ret;
	return 0;
}

static int rtc_path(struct runtime *r, const char *name, char *path, size_t size)
{
	int n = snprintf(path, size, "%s/%s", r->rtc, name);
	return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

static int rtc_write(struct runtime *r, const char *name, const char *text)
{
	char path[MAX_PATH];
	int ret = rtc_path(r, name, path, sizeof(path));
	return ret ? ret : write_text(path, text);
}

static int trial_pending(struct runtime *r)
{
	/* Acceptance is specific to this boot; missing/partial/stale files cannot
	 * cancel a deadline. The budget is measured from kernel boot, not owner start.
	 */
	return r->trial_deadline && exact_file(TRIAL_ACCEPTED_PATH, r->boot);
}

static int trial_budget(struct runtime *r, uint64_t now)
{
	return trial_pending(r) && now >= r->trial_deadline ? -ETIME : 0;
}

static int trial_configure(struct runtime *r)
{
	char text[32], extra;
	unsigned seconds;
	int ret = read_text("/etc/udx-trial-seconds", text, sizeof(text));
	if (ret)
		return ret;
	if (sscanf(text, "%u %c", &seconds, &extra) != 1 ||
	    (seconds && (seconds < 30 || seconds > 600)))
		return -EINVAL;
	r->trial_deadline = (uint64_t)seconds * 1000u;
	return 0;
}

static int watchdog_timeout(struct runtime *r, int seconds)
{
	return ioctl(r->watchdog_fd, WDIOC_SETTIMEOUT, &seconds) ? -errno : 0;
}

static int watchdog_open(struct runtime *r)
{
	/* DT aliases reserve watchdog0 for procd and watchdog1 for the PMIC.
	 * Check sysfs identity before opening, since open itself starts a watchdog.
	 */
	int ret = exact_file("/sys/class/watchdog/watchdog1/identity", "SC2730 PMIC Watchdog\n");
	if (ret)
		return ret;
	r->watchdog_fd = open("/dev/watchdog1", O_WRONLY | O_CLOEXEC);
	if (r->watchdog_fd < 0)
		return -errno;
	ret = watchdog_timeout(r, 60);
	return ret ? ret : rtc_write(r, "udx_trial_stage", "runtime\n");
}

static int watchdog_feed(struct runtime *r)
{
	int ret;
	if (r->closing)
		return -ESHUTDOWN;
	if ((ret = trial_budget(r, milliseconds())))
		return ret;
	if (ioctl(r->watchdog_fd, WDIOC_KEEPALIVE, 0))
		return -errno;
	r->renewals++;
	r->last_renew = milliseconds();
	return 0;
}

static int log_step(struct runtime *r)
{
	int status;
	if (!r->log_pid)
		return 0;
	pid_t result = waitpid(r->log_pid, &status, WNOHANG);
	if (result < 0) {
		r->log_pid = 0;
		return -errno;
	}
	if (!result) {
		if (milliseconds() - r->log_started < LOG_TIMEOUT_MS)
			return 0;
		(void)kill(-r->log_pid, SIGKILL);
		return -ETIMEDOUT;
	}
	r->log_pid = 0;
	r->log_error = WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
	if (r->log_error)
		runtime_log(LOG_ERR, "udx-runtime: persistent checkpoint failed status=%d\n",
			    status);
	return 0;
}

static int log_start(struct runtime *r)
{
	if (r->log_pid)
		return 0;
	pid_t parent = getpid(), child = fork();
	if (child < 0)
		return -errno;
	if (!child) {
		sigset_t empty;
		sigemptyset(&empty);
		if (sigprocmask(SIG_SETMASK, &empty, NULL) || setpgid(0, 0) ||
		    prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
			_exit(74);
		/* A routing wrapper may use this flag to avoid sending this one worker
		 * back into its own supervisor method. All other callers use --checkpoint.
		 */
		setenv("UDX_RUNTIME_LOG_WORKER", "1", 1);
		execl("/usr/sbin/save-debug-log", "save-debug-log", (char *)NULL);
		_exit(127);
	}
	r->log_pid = child;
	r->log_started = milliseconds();
	return 0;
}

static void log_failure(struct runtime *r, int error)
{
	if (r->log_error != error)
		runtime_log(LOG_WARNING,
			    "udx-runtime: diagnostic checkpoint unavailable error=%d\n", error);
	r->log_error = error;
}

static int persist_now(struct runtime *r)
{
	int ret;
	/* Finish the one existing worker before creating another .new/rotation. */
	while (r->log_pid) {
		if ((ret = log_step(r)))
			return ret;
		if (r->log_pid)
			(void)poll(NULL, 0, 20);
	}
	if ((ret = log_start(r)))
		return ret;
	while (r->log_pid) {
		if ((ret = log_step(r)))
			return ret;
		if (r->log_pid)
			(void)poll(NULL, 0, 20);
	}
	return r->log_error;
}

static int system_quiesce_start(struct runtime *r, const char *target)
{
	int ret;
	if (!r->production)
		return -EOPNOTSUPP;
	if (r->closing)
		return -ESHUTDOWN;
	if (r->quiesce_state == 1)
		return strcmp(target, r->quiesce_target) ? -EBUSY : 0;
	if (r->quiesce_state == 2)
		return 0;
	ret = core_health(r);
	if (!ret)
		ret = watchdog_feed(r);
	if (ret)
		return ret;
	strcpy(r->quiesce_target, target);
	if (access(SYSTEM_QUIESCE, X_OK)) {
		ret = -errno;
		goto failed;
	}
	pid_t parent = getpid(), child = fork();
	if (child < 0) {
		ret = -errno;
		goto failed;
	}
	if (!child) {
		sigset_t empty;
		sigemptyset(&empty);
		if (sigprocmask(SIG_SETMASK, &empty, NULL) || setpgid(0, 0) ||
		    prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
			_exit(3);
		execl(SYSTEM_QUIESCE, "udx-system-quiesce", (char *)NULL);
		_exit(3);
	}
	r->quiesce_pid = child;
	r->quiesce_deadline = milliseconds() + QUIESCE_TIMEOUT_MS;
	r->quiesce_state = 1;
	runtime_log(LOG_INFO,
		    "udx-runtime: bounded system quiesce started target=%s; core watchdog remains "
		    "locally checked\n",
		    *target ? target : "prepare");
	return 0;
failed:
	/* A helper launch failure also completes the bounded shutdown attempt. */
	r->quiesce_state = 1;
	r->quiesce_pid = 0;
	r->quiesce_error = ret;
	return 0;
}

static int queue_reboot(struct runtime *r, const char *target)
{
	char command[80];
	if (!r->boot_policy || (strcmp(target, "openwrt") && strcmp(target, "recovery")))
		return -EINVAL;
	if (r->production && r->quiesce_state != 2)
		return system_quiesce_start(r, target);
	int ret = core_health(r);
	if (!ret)
		ret = watchdog_feed(r);
	/* An expired budget cannot buy another minute. The existing PMIC window
	 * still permits best-effort orderly Recovery return. Every other preparation
	 * failure also stays closed rather than resuming the normal feed loop.
	 */
	if (ret == -ETIME && !strcmp(target, "recovery"))
		ret = 0;
	r->closing = 1;
	if (!ret) {
		runtime_log(LOG_INFO, "udx-runtime: preparing reboot %s\n", target);
	}
	if (ret)
		return ret;
	/* Explicit maintenance runs in this same loop. A stuck sync cannot be
	 * hidden by a separate heartbeat thread. No new writes begin afterward.
	 */
	sync();
	snprintf(command, sizeof(command), "%s\n", target);
	ret = rtc_write(r, "udx_trial_reboot", command);
	if (!ret)
		ret = watchdog_timeout(r, 12);
	/* Already closed before maintenance; a refused transaction also expires. */
	return ret;
}

static int upgrade_admit(struct runtime *r)
{
	if (!r->boot_policy)
		return -EOPNOTSUPP;
	if (r->closing)
		return -ESHUTDOWN;
	/* An already accepted runtime reboot still owns its live quiesce child. */
	if (r->quiesce_state == 1)
		return -EBUSY;
	return 0;
}

static int prepare_upgrade(struct runtime *r)
{
	int ret = upgrade_admit(r);
	if (ret)
		return ret;
	/* Complete ordinary maintenance before the terminal window. There is no
	 * owner transfer: this PID performs one NORMAL/60s transaction and closes.
	 * The writer has already durably marked misc pending; any failure reboots
	 * into Recovery unless all image/configuration work later reaches ready.
	 */
	ret = core_health(r);
	if (!ret)
		ret = watchdog_feed(r);
	r->closing = 1;
	if (ret)
		return ret;
	sync();
	ret = rtc_write(r, "udx_trial_reboot", "upgrade\n");
	if (!ret)
		ret = watchdog_timeout(r, 60);
	if (!ret)
		runtime_log(LOG_INFO, "udx-runtime: final NORMAL 60s upgrade window; no further "
				      "renewal or owner handoff\n");
	return ret;
}

static int board_poweroff(struct runtime *r)
{
	/* BusyBox poweroff uses this procd signal. Let init run the normal stop
	 * sequence and filesystem shutdown; the MCU ACK belongs to the driver.
	 */
	if (kill(1, SIGUSR2))
		return -errno;
	r->closing = 1;
	runtime_log(LOG_INFO, "udx-runtime: orderly MCU poweroff requested from procd\n");
	return 0;
}

static int board_event(struct runtime *r)
{
	char text[128], kind[24], extra;
	unsigned long long sequence;
	int ret;

	if (r->closing)
		return 0;
	if ((ret = read_text(r->board, text, sizeof(text))))
		return ret;
	if (!strcmp(text, "none\n"))
		return 0;
	if (sscanf(text, "seq=%llu kind=%23s %c", &sequence, kind, &extra) != 2 || !sequence)
		return -EPROTO;
	/* USB notifications go directly through the existing gadget controller. */
	if (!strcmp(kind, "usb-reset"))
		return 0;
	if (strcmp(kind, "poweroff"))
		return -EPROTO;
	if (sequence == r->event_sequence)
		return 0;
	r->event_sequence = sequence;
	strcpy(r->event_kind, kind);
	if (trial_pending(r)) {
		runtime_log(LOG_WARNING,
			    "udx-runtime: unaccepted trial MCU poweroff redirected to recovery\n");
		return queue_reboot(r, "recovery");
	}
	if (r->production && r->quiesce_state != 2)
		return system_quiesce_start(r, "poweroff");
	return board_poweroff(r);
}

static int system_quiesce_step(struct runtime *r, uint64_t now)
{
	int status, ret;
	if (!r->production || r->quiesce_state != 1)
		return 0;
	if (!r->quiesce_pid) {
		ret = r->quiesce_error;
		goto failed;
	}
	pid_t result = waitpid(r->quiesce_pid, &status, WNOHANG);
	if (result < 0) {
		ret = -errno;
		goto failed;
	}
	if (!result) {
		if (now < r->quiesce_deadline)
			return 0;
		(void)kill(-r->quiesce_pid, SIGTERM);
		ret = -ETIMEDOUT;
		goto failed;
	}
	r->quiesce_pid = 0;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		ret = -EIO;
		goto failed;
	}
	ret = 0;
failed:
	r->quiesce_error = ret;
	r->quiesce_state = 2;
	r->cpnv.healthy = 0;
	r->cpnv_error = -ESHUTDOWN;
	runtime_log(
	    ret ? LOG_ERR : LOG_INFO,
	    "udx-runtime: userspace quiesce finished error=%d; continuing orderly shutdown\n", ret);
	if (!strcmp(r->quiesce_target, "poweroff"))
		return board_poweroff(r);
	if (*r->quiesce_target)
		return queue_reboot(r, r->quiesce_target);
	return 0;
}

static void runtime_describe(struct runtime *r, struct blob_buf *b)
{
	char boot[64];
	strcpy(boot, r->boot);
	boot[strcspn(boot, "\n")] = 0;
	static const char *const states[] = {"idle", "running", "finished"};
	blobmsg_add_string(b, "boot_id", boot);
	blobmsg_add_u32(b, "owner", getpid());
	blobmsg_add_string(b, "core", r->core_error ? "failed" : "ready");
	blobmsg_add_u32(b, "core_error", r->core_error);
	blobmsg_add_string(b, "baseband",
			   r->recovery	     ? "disabled"
			   : r->cpnv.healthy ? "ready"
					     : "degraded");
	blobmsg_add_u32(b, "cpnv_error", r->cpnv_error);
	blobmsg_add_u32(b, "renewals", r->renewals);
	blobmsg_add_u64(b, "last_renew_ms", r->last_renew);
	blobmsg_add_u8(b, "closing", r->closing);
	blobmsg_add_u64(b, "board_event", r->event_sequence);
	blobmsg_add_string(b, "board_event_kind", *r->event_kind ? r->event_kind : "none");
	blobmsg_add_u32(b, "log_error", r->log_error);
	blobmsg_add_string(b, "root", r->production ? "overlay" : "ram");
	blobmsg_add_string(b, "quiesce", states[r->quiesce_state]);
	blobmsg_add_u32(b, "quiesce_error", r->quiesce_error);
}

static const struct blobmsg_policy reboot_policy[] = {
    {.name = "target", .type = BLOBMSG_TYPE_STRING},
};

static int runtime_method(struct ubus_context *ctx, struct ubus_object *object,
			  struct ubus_request_data *request, const char *method,
			  struct blob_attr *message)
{
	struct runtime *r = container_of(object, struct runtime, object);
	struct blob_buf b = {};
	const char *target = NULL;
	int ret = 0;
	blob_buf_init(&b, 0);
	if (!strcmp(method, "status")) {
		runtime_describe(r, &b);
		goto reply;
	}
	if (r->closing && strcmp(method, "stop")) {
		ret = -ESHUTDOWN;
	} else if (!strcmp(method, "checkpoint")) {
		ret = persist_now(r);
	} else if (!strcmp(method, "reboot")) {
		struct blob_attr *tb[1] = {};
		if (message)
			blobmsg_parse(reboot_policy, 1, tb, blob_data(message), blob_len(message));
		if (!tb[0]) {
			blob_buf_free(&b);
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
		target = blobmsg_get_string(tb[0]);
		ret = queue_reboot(r, target);
	} else if (!strcmp(method, "prepare_upgrade")) {
		ret = prepare_upgrade(r);
	} else if (!strcmp(method, "mcu_poweroff")) {
		ret = board_event(r);
	} else if (!strcmp(method, "stop")) {
		r->closing = 1;
		runtime_log(LOG_INFO, "udx-runtime: explicit watchdog feed stop\n");
	} else {
		ret = -EINVAL;
	}
	blobmsg_add_u32(&b, "error", ret);
	blobmsg_add_u8(&b, "accepted", !ret);
	if (!ret) {
		char boot[64];
		strcpy(boot, r->boot);
		boot[strcspn(boot, "\n")] = 0;
		blobmsg_add_string(&b, "boot_id", boot);
		if (target)
			blobmsg_add_string(&b, "target", target);
		if (!strcmp(method, "prepare_upgrade"))
			blobmsg_add_u32(&b, "seconds", 60);
	}
reply:
	ret = ubus_send_reply(ctx, request, b.head);
	blob_buf_free(&b);
	return ret;
}
static const struct ubus_method runtime_methods[] = {
    UBUS_METHOD_NOARG("status", runtime_method),
    UBUS_METHOD_NOARG("checkpoint", runtime_method),
    UBUS_METHOD("reboot", runtime_method, reboot_policy),
    UBUS_METHOD_NOARG("prepare_upgrade", runtime_method),
    UBUS_METHOD_NOARG("stop", runtime_method),
    UBUS_METHOD_NOARG("mcu_poweroff", runtime_method),
};
static struct ubus_object_type runtime_type = UBUS_OBJECT_TYPE("udx.runtime", runtime_methods);

static void runtime_connected(struct ubus_context *ctx)
{
	struct runtime *r =
	    container_of(container_of(ctx, struct ubus_auto_conn, ctx), struct runtime, bus);
	/* ubusd's local ACL defaults deny non-root access; no HTTP ACL grants this object. */
	if (ubus_add_object(ctx, &r->object)) {
		r->core_error = -EIO;
		uloop_end();
	}
}

static void runtime_signal(struct uloop_fd *fd, unsigned int events)
{
	struct runtime *r = container_of(fd, struct runtime, signal);
	(void)events;
	r->core_error = -ECANCELED;
	uloop_end();
}

static void runtime_tick(struct uloop_timeout *timer)
{
	struct runtime *r = container_of(timer, struct runtime, tick);
	uint64_t now = milliseconds();
	int ret = 0;
	if (!now) {
		ret = -EIO;
		goto fail;
	}
	int error = log_step(r);
	if (error)
		log_failure(r, error);
	if (!r->closing && trial_budget(r, now)) {
		runtime_log(
		    LOG_WARNING,
		    "udx-runtime: absolute trial budget reached; requesting recovery return\n");
		if ((ret = queue_reboot(r, "recovery")))
			goto fail;
	}
	if (!r->closing) {
		if ((ret = system_quiesce_step(r, now)))
			goto fail;
		if (r->closing)
			goto again;
		if (now >= r->next_health) {
			if ((ret = core_health(r)))
				goto fail;
			int before = r->cpnv_error;
			r->cpnv_error = runtime_cpnv_query(r);
			if (r->cpnv_error)
				r->cpnv.healthy = 0;
			if (before != r->cpnv_error)
				runtime_log(r->cpnv_error ? LOG_WARNING : LOG_INFO,
					    "udx-runtime: baseband=%s error=%d\n",
					    r->cpnv.healthy ? "ready" : "degraded", r->cpnv_error);
			r->last_health = milliseconds();
			r->next_health = r->last_health + QUERY_MS;
		}
		if (now - r->last_renew >= RENEW_MS &&
		    ((ret = core_health(r)) || (ret = watchdog_feed(r))))
			goto fail;
	}
again:
	uloop_timeout_set(timer, 1000);
	return;
fail:
	r->core_error = ret;
	uloop_end();
}

static int discover(struct runtime *r)
{
	char status[MAX_PATH];
	int ret;
	if ((ret = read_text("/proc/sys/kernel/random/boot_id", r->boot, sizeof(r->boot))))
		return ret;
	if ((ret = unique_path("/sys/bus/platform/devices/*/udx_boot_policy", status,
			       sizeof(status))))
		return ret;
	char *slash = strrchr(status, '/');
	if (!slash)
		return -EINVAL;
	*slash = 0;
	strcpy(r->rtc, status);
	r->recovery = !access("/etc/udx-recovery-root", F_OK);
	r->production = !access("/etc/udx-openwrt-root", F_OK);
	if (r->recovery && r->production)
		return -EPROTO;
	r->ready_stage = 0xbe5a;
	char policy_path[MAX_PATH], policy[128], target[24];
	if ((ret = rtc_path(r, "udx_boot_policy", policy_path, sizeof(policy_path))))
		return ret;
	ret = read_text(policy_path, policy, sizeof(policy));
	if (ret)
		return ret;
	int used = 0;
	if (sscanf(policy, "policy=openwrt-v1 target=%23s ready_stage=%x%n", target,
		   &r->ready_stage, &used) != 2 ||
	    strcmp(policy + used, "\n") || strcmp(target, r->recovery ? "recovery" : "openwrt") ||
	    r->ready_stage != (r->recovery ? 0xbc5a : 0xbe5a) || (!r->production && !r->recovery))
		return -EPROTO;
	r->boot_policy = 1;

	if ((ret = unique_path(
		 "/sys/bus/platform/drivers/udx710-pmsys/*/remoteproc/remoteproc*/state", r->pmsys,
		 sizeof(r->pmsys))))
		return ret;
	return unique_path("/sys/bus/serial/drivers/sy108-mcu/*/event", r->board, sizeof(r->board));
}

static int run_owner(void)
{
	struct runtime r = {.signal = {.fd = -1}, .watchdog_fd = -1, .cpnv_error = -EAGAIN};
	sigset_t mask;
	int ret;
	if (geteuid() || access(CHOSEN "udx,trial-local-lease", F_OK))
		return 77;
	kernel_log = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
	umask(077);
	uloop_handle_sigchld = false; /* Existing log/quiesce workers retain waitpid ownership. */
	if (uloop_init())
		return 74;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &mask, NULL)) {
		ret = -errno;
		goto fail;
	}
	r.signal.fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	r.signal.cb = runtime_signal;
	if (r.signal.fd < 0 || uloop_fd_add(&r.signal, ULOOP_READ)) {
		ret = -errno;
		goto fail;
	}
	if ((ret = discover(&r)) || (ret = trial_configure(&r)) || (ret = root_health(&r)) ||
	    (ret = core_health(&r)))
		goto fail;
	r.cpnv_error = runtime_cpnv_query(&r);
	if (r.cpnv_error)
		r.cpnv.healthy = 0;
	r.last_health = milliseconds();
	r.next_health = r.last_health + QUERY_MS;
	if ((ret = watchdog_open(&r)) || (ret = watchdog_feed(&r)))
		goto fail;
	runtime_log(r.cpnv_error ? LOG_WARNING : LOG_INFO,
		    "udx-runtime: opened PMIC watchdog pid=%ld baseband=%s error=%d\n",
		    (long)getpid(),
		    r.recovery	     ? "disabled"
		    : r.cpnv.healthy ? "ready"
				     : "degraded",
		    r.cpnv_error);
	if ((ret = board_event(&r)))
		goto fail;
	r.object = (struct ubus_object){.name = "udx.runtime",
					.type = &runtime_type,
					.methods = runtime_methods,
					.n_methods = ARRAY_SIZE(runtime_methods)};
	r.bus.cb = runtime_connected;
	ubus_auto_connect(&r.bus);
	r.tick.cb = runtime_tick;
	uloop_timeout_set(&r.tick, 0);
	if (!r.core_error)
		uloop_run();
	ret = r.core_error ? r.core_error : -ECANCELED;
	ubus_auto_shutdown(&r.bus);
fail:
	if (r.quiesce_pid > 0)
		(void)kill(-r.quiesce_pid, SIGTERM);
	runtime_log(LOG_ERR, "udx-runtime: core failure error=%d; no further PMIC renewal\n", ret);
	if (r.watchdog_fd >= 0)
		close(r.watchdog_fd);
	if (r.signal.fd >= 0) {
		uloop_fd_delete(&r.signal);
		close(r.signal.fd);
	}
	uloop_timeout_cancel(&r.tick);
	uloop_done();
	if (kernel_log >= 0)
		close(kernel_log);
	return 74;
}

struct cli_result {
	const char *method;
	int received, error;
};

static int cli_status(struct blob_attr *message)
{
	enum {
		BOOT,
		OWNER,
		CORE,
		CORE_ERROR,
		BASEBAND,
		CPNV_ERROR,
		RENEWALS,
		LAST,
		CLOSING,
		EVENT,
		KIND,
		LOG_ERROR,
		ROOT,
		QUIESCE,
		QUIESCE_ERROR,
		N
	};
	static const struct blobmsg_policy policy[N] = {
	    [BOOT] = {.name = "boot_id", .type = BLOBMSG_TYPE_STRING},
	    [OWNER] = {.name = "owner", .type = BLOBMSG_TYPE_INT32},
	    [CORE] = {.name = "core", .type = BLOBMSG_TYPE_STRING},
	    [CORE_ERROR] = {.name = "core_error", .type = BLOBMSG_TYPE_INT32},
	    [BASEBAND] = {.name = "baseband", .type = BLOBMSG_TYPE_STRING},
	    [CPNV_ERROR] = {.name = "cpnv_error", .type = BLOBMSG_TYPE_INT32},
	    [RENEWALS] = {.name = "renewals", .type = BLOBMSG_TYPE_INT32},
	    [LAST] = {.name = "last_renew_ms", .type = BLOBMSG_TYPE_INT64},
	    [CLOSING] = {.name = "closing", .type = BLOBMSG_TYPE_BOOL},
	    [EVENT] = {.name = "board_event", .type = BLOBMSG_TYPE_INT64},
	    [KIND] = {.name = "board_event_kind", .type = BLOBMSG_TYPE_STRING},
	    [LOG_ERROR] = {.name = "log_error", .type = BLOBMSG_TYPE_INT32},
	    [ROOT] = {.name = "root", .type = BLOBMSG_TYPE_STRING},
	    [QUIESCE] = {.name = "quiesce", .type = BLOBMSG_TYPE_STRING},
	    [QUIESCE_ERROR] = {.name = "quiesce_error", .type = BLOBMSG_TYPE_INT32},
	};
	struct blob_attr *tb[N];
	blobmsg_parse(policy, N, tb, blob_data(message), blob_len(message));
	for (unsigned i = 0; i < N; i++)
		if (!tb[i])
			return -EPROTO;
	printf("RUNTIME_STATUS1 boot=%s owner=%u core=%s core_error=%d baseband=%s "
	       "cpnv_error=%d renewals=%u last_renew_ms=%llu closing=%d board_event=%llu:%s "
	       "log_error=%d",
	       blobmsg_get_string(tb[BOOT]), blobmsg_get_u32(tb[OWNER]),
	       blobmsg_get_string(tb[CORE]), (int32_t)blobmsg_get_u32(tb[CORE_ERROR]),
	       blobmsg_get_string(tb[BASEBAND]), (int32_t)blobmsg_get_u32(tb[CPNV_ERROR]),
	       blobmsg_get_u32(tb[RENEWALS]), (unsigned long long)blobmsg_get_u64(tb[LAST]),
	       blobmsg_get_bool(tb[CLOSING]), (unsigned long long)blobmsg_get_u64(tb[EVENT]),
	       blobmsg_get_string(tb[KIND]), (int32_t)blobmsg_get_u32(tb[LOG_ERROR]));
	if (!strcmp(blobmsg_get_string(tb[ROOT]), "overlay"))
		printf(" root=overlay quiesce=%s quiesce_error=%d", blobmsg_get_string(tb[QUIESCE]),
		       (int32_t)blobmsg_get_u32(tb[QUIESCE_ERROR]));
	putchar('\n');
	return 0;
}

static void cli_reply(struct ubus_request *request, int type, struct blob_attr *message)
{
	struct cli_result *result = request->priv;
	(void)type;
	result->received = 1;
	result->error = -EPROTO;
	if (!message)
		return;
	if (!strcmp(result->method, "status")) {
		result->error = cli_status(message);
		return;
	}

	enum { ERROR, BOOT, TARGET, SECONDS, N };
	static const struct blobmsg_policy policy[N] = {
	    [ERROR] = {.name = "error", .type = BLOBMSG_TYPE_INT32},
	    [BOOT] = {.name = "boot_id", .type = BLOBMSG_TYPE_STRING},
	    [TARGET] = {.name = "target", .type = BLOBMSG_TYPE_STRING},
	    [SECONDS] = {.name = "seconds", .type = BLOBMSG_TYPE_INT32},
	};
	struct blob_attr *tb[N];
	blobmsg_parse(policy, N, tb, blob_data(message), blob_len(message));
	if (!tb[ERROR])
		return;
	result->error = (int32_t)blobmsg_get_u32(tb[ERROR]);
	if (result->error) {
		printf("ERROR %d\n", result->error);
		return;
	}
	if (!strcmp(result->method, "reboot")) {
		if (!tb[BOOT] || !tb[TARGET]) {
			result->error = -EPROTO;
			return;
		}
		printf("REBOOT_QUEUED %s %s\n", blobmsg_get_string(tb[TARGET]),
		       blobmsg_get_string(tb[BOOT]));
	} else if (!strcmp(result->method, "prepare_upgrade")) {
		if (!tb[BOOT] || !tb[SECONDS]) {
			result->error = -EPROTO;
			return;
		}
		printf("UPGRADE_WINDOW_READY seconds=%u boot=%s\n", blobmsg_get_u32(tb[SECONDS]),
		       blobmsg_get_string(tb[BOOT]));
	} else if (!strcmp(result->method, "checkpoint")) {
		puts("CHECKPOINT_SAVED result=0");
	} else if (!strcmp(result->method, "stop")) {
		puts("LEASE_STOPPED result=0");
	} else if (!strcmp(result->method, "mcu_poweroff")) {
		puts("MCU_EVENT_ACCEPTED result=0");
	}
}

static int control_client(const char *method, const char *target)
{
	alarm(45); /* Includes synchronous lookup in the existing control budget. */
	struct ubus_context *ctx = ubus_connect(NULL);
	struct blob_buf b = {};
	struct cli_result result = {.method = method};
	uint32_t id;
	if (!ctx) {
		fputs("runtime control: ubus connection failed\n", stderr);
		alarm(0);
		return 74;
	}
	blob_buf_init(&b, 0);
	if (target)
		blobmsg_add_string(&b, "target", target);
	int ret = ubus_lookup_id(ctx, "udx.runtime", &id);
	if (!ret)
		ret = ubus_invoke(ctx, id, method, b.head, cli_reply, &result, 45000);
	if (ret)
		fprintf(stderr, "runtime control: %s\n", ubus_strerror(ret));
	blob_buf_free(&b);
	ubus_free(ctx);
	alarm(0);
	return ret || !result.received || result.error ? 74 : 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--serve"))
		return run_owner();
	if (argc == 2 && !strcmp(argv[1], "--status"))
		return control_client("status", NULL);
	if (argc == 2 && !strcmp(argv[1], "--checkpoint"))
		return control_client("checkpoint", NULL);
	if (argc == 2 && !strcmp(argv[1], "--stop"))
		return control_client("stop", NULL);
	if (argc == 2 && !strcmp(argv[1], "--mcu-poweroff"))
		return control_client("mcu_poweroff", NULL);
	if (argc == 2 && !strcmp(argv[1], "--prepare-upgrade"))
		return control_client("prepare_upgrade", NULL);
	if (argc == 3 && !strcmp(argv[1], "--reboot") &&
	    (!strcmp(argv[2], "openwrt") || !strcmp(argv[2], "recovery")))
		return control_client("reboot", argv[2]);
	fputs("usage: udx-runtime --serve | --status | --checkpoint | --stop | "
	      "--mcu-poweroff | --prepare-upgrade | --reboot openwrt|recovery\n",
	      stderr);
	return 64;
}
