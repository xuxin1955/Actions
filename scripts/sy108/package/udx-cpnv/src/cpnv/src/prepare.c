/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include "modem_image.h"
#include "modem_cmdline.h"
#include "nv_vendor.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const images[] = {"modem",	"v3phy",     "nrphy", "nrdsp1", "nrdsp2",
				     "deltanv", "cpcmdline", "fixnv", "runnv"};
static const uint32_t capacity[] = {0x1030000, 0x600000, 0x400000, 0x80000, 0x100000,
				    0x100000,  0x800,	 0x700000, 0x900000};

static int check(struct nv_worker *w)
{
	if (worker_stopping(w))
		return -ECANCELED;
	return cpnv_now() >= w->deadline ? -ETIMEDOUT : 0;
}

static int copy_extent(struct nv_worker *w, int source, int target, const struct mci_extent *extent)
{
	uint8_t *buffer = malloc(65536);
	if (!buffer)
		return -ENOMEM;
	uint64_t done = 0;
	int e = 0;
	while (done < extent->length) {
		if ((e = check(w)))
			break;
		size_t take =
		    extent->length - done > 65536 ? 65536 : (size_t)(extent->length - done);
		ssize_t n = pread(source, buffer, take, (off_t)(extent->source_offset + done));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			e = n < 0 ? -errno : -EIO;
			break;
		}
		size_t sent = 0;
		while (sent < (size_t)n) {
			ssize_t k = pwrite(target, buffer + sent, (size_t)n - sent,
					   (off_t)(extent->target_offset + done + sent));
			if (k < 0 && errno == EINTR)
				continue;
			if (k <= 0) {
				e = k < 0 ? -errno : -EIO;
				break;
			}
			sent += (size_t)k;
		}
		if (e)
			break;
		done += (uint64_t)n;
	}
	free(buffer);
	return e;
}

static int output_open(unsigned segment)
{
	char path[128];
	snprintf(path, sizeof path, "/run/udx-cpnv-warm/%s.bin", images[segment]);
	return open(path, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
}

static int read_header(struct nv_worker *w, int fd, uint8_t *data, size_t size, uint64_t off)
{
	for (size_t done = 0; done < size;) {
		int e = check(w);
		if (e)
			return e;
		ssize_t n = pread(fd, data + done, size - done, (off_t)(off + done));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n < 0 ? -errno : -EIO;
		done += (size_t)n;
	}
	return 0;
}

static int firmware(struct nv_worker *w, const struct firmware *f)
{
	struct volume volume = {.fd = -1};
	uint64_t bytes = 0;
	int in = -1, out = -1, e = 0;
	if (w->policy.firmware_ubi) {
		char name[64];
		snprintf(name, sizeof name, "nr_%s", images[f->segment]);
		if ((e = volume_open(&volume, name, O_RDONLY, 1)))
			return e;
		in = volume.fd;
		bytes = volume.identity.capacity;
	} else {
		char path[128];
		snprintf(path, sizeof path, "/usr/lib/firmware/udx710/%s.bin", images[f->segment]);
		in = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (in < 0)
			return -errno;
		off_t n = lseek(in, 0, SEEK_END);
		if (n <= 0) {
			e = -EIO;
			goto done;
		}
		bytes = (uint64_t)n;
	}
	struct mci_plan plan = {0};
	if (f->format == MCI_RAW) {
		if (f->offset > bytes || f->length > bytes - f->offset ||
		    f->target_offset > capacity[f->segment] ||
		    f->length > capacity[f->segment] - f->target_offset) {
			e = -ERANGE;
			goto done;
		}
		plan.extent_count = 1;
		plan.extents[0] = (struct mci_extent){f->offset, f->target_offset, f->length};
	} else if (f->format == MCI_DHTB) {
		uint8_t header[MCI_PREFIX_BYTES];
		e = read_header(w, in, header, sizeof header, 0);
		if (e)
			goto done;
		if (mci_parse_dhtb_header(header, sizeof header, bytes, capacity[f->segment], &plan)) {
			e = -EBADMSG;
			goto done;
		}
	} else {
		uint8_t header[MCI_HEADER_BYTES];
		unsigned offset = f->format == MCI_SCI ? 0 : MCI_PREFIX_BYTES;
		e = read_header(w, in, header, sizeof header, offset);
		if (e)
			goto done;
		int parsed = f->format == MCI_SCI_PAYLOAD
			     ? mci_parse_sci_payload(header, sizeof header, offset, bytes,
					     capacity[f->segment], &plan)
			     : mci_parse_sci_header(header, sizeof header, offset, bytes,
					    capacity[f->segment], &plan);
		if (parsed) {
			e = -EBADMSG;
			goto done;
		}
	}
	out = output_open(f->segment);
	if (out < 0) {
		e = -errno;
		goto done;
	}
	for (unsigned i = 0; i < plan.extent_count && !e; i++)
		e = copy_extent(w, in, out, &plan.extents[i]);
	if (!e && fsync(out))
		e = -errno;
done:
	if (out >= 0)
		close(out);
	if (in >= 0)
		close(in);
	return e;
}

/* Startup repair already verified the payload. Do not copy unchecked volume
 * tail bytes into CP RAM; the original RAM disk uses erased-byte padding. */
static int nv_copy_payload(struct nv_worker *worker, int source, int output, uint32_t capacity)
{
	uint8_t header[NV_HEADER], pad[4096];
	int e = read_header(worker, source, header, sizeof header, 0);
	if (e)
		return e;
	uint32_t size = nv_vendor_payload_size(header, capacity);
	if (!size)
		return -EBADMSG;
	struct mci_extent extent = {.source_offset = NV_HEADER, .length = size};
	e = copy_extent(worker, source, output, &extent);
	memset(pad, 0xff, sizeof pad);
	for (uint32_t off = size; !e && off < capacity;) {
		if ((e = check(worker)))
			break;
		size_t n = capacity - off < sizeof pad ? capacity - off : sizeof pad;
		ssize_t done = pwrite(output, pad, n, off);
		if (done < 0 && errno == EINTR)
			continue;
		if (done <= 0)
			return done < 0 ? -errno : -EIO;
		off += (uint32_t)done;
	}
	return e;
}

static int nv_output(struct nv_worker *worker, unsigned pair)
{
	int output = output_open(7 + pair);
	if (output < 0)
		return -errno;
	/* Startup repair has already validated both copies. Keep primary-first boot
	 * selection, and stream only the payload into the firmware-loader material. */
	int error = nv_copy_payload(worker, worker->volumes[pair * 2].fd, output, capacity[7 + pair]);
	if (error && error != -ECANCELED && error != -ETIMEDOUT) {
		if (ftruncate(output, 0))
			error = -errno;
		else
			error =
			    nv_copy_payload(worker, worker->volumes[pair * 2 + 1].fd, output,
					    capacity[7 + pair]);
	}
	if (!error && fsync(output))
		error = -errno;
	close(output);
	return error;
}

static int commandline(void)
{
	char ap[4096], built[2048];
	int n = cpnv_read("/proc/cmdline", ap, sizeof ap);
	size_t length;
	if (n <= 0)
		return n ? n : -EIO;
	int ap_bytes = n;
	enum mcc_boot_hint hint;
	if (mcc_boot_hint_parse(ap, (size_t)n, &hint) || hint != MCC_BOOT_SHUTDOWN)
		return -EINVAL;
	struct mcc_log_tuple tuples[3];
	const char *logs[] = {"slog_lte", "slog_nr_phy", "slog_v3_phy"};
	unsigned dst[] = {5, 7, 8};
	for (unsigned i = 0; i < 3; i++) {
		char path[128], text[128], tail;
		unsigned d, ch, addr, pre, tx, txs, rx, rxs;
		int used = 0;
		snprintf(path, sizeof path, "/sys/class/spool/%s/device/base_addr", logs[i]);
		n = cpnv_read(path, text, sizeof text - 1);
		if (n <= 0)
			return n ? n : -EIO;
		text[n] = 0;
		if (sscanf(text, "%u %u 0x%x %u %u %u %u %u%c%n", &d, &ch, &addr, &pre, &tx, &txs,
			   &rx, &rxs, &tail, &used) != 9 ||
		    used != n || tail != '\n' || d != dst[i] || ch != 5 || !addr || pre != 1 ||
		    tx || txs || rxs != 2048 || rx != (i ? 1000u : 800u))
			return -EINVAL;
		tuples[i] = (struct mcc_log_tuple){d, ch, addr};
	}
	if (mcc_build_nr(ap, (size_t)ap_bytes, tuples, built, sizeof built, &length))
		return -EINVAL;
	int out = output_open(6);
	if (out < 0)
		return -errno;
	int e = write(out, built, length) == (ssize_t)length && !fsync(out) ? 0 : -EIO;
	close(out);
	return e;
}

static int remoteproc(char path[256])
{
	glob_t list = {0};
	int e = -ENODEV;
	if (glob("/sys/class/remoteproc/remoteproc*/name", 0, NULL, &list))
		return e;
	for (size_t i = 0; i < list.gl_pathc; i++) {
		char name[64];
		int n = cpnv_read(list.gl_pathv[i], name, sizeof name);
		if (n != 13 || memcmp(name, "udx710-modem\n", 13))
			continue;
		snprintf(path, 256, "%.*sstate", (int)strlen(list.gl_pathv[i]) - 4,
			 list.gl_pathv[i]);
		e = 0;
		break;
	}
	globfree(&list);
	return e;
}

int worker_prepare(struct nv_worker *w)
{
	char state_path[256], state[32];
	int e = remoteproc(state_path);
	if (e)
		return e;
	int n = cpnv_read(state_path, state, sizeof state);
	if (n != 8 || memcmp(state, "offline\n", 8))
		return -EBUSY;
	w->deadline = cpnv_now() + w->policy.nv.startup_ms;
	const char *names[] = {"nr_fixnv1", "nr_fixnv2", "nr_runtimenv1", "nr_runtimenv2"};
	/* Name aliases follow the installed UBI layout; no raw MTD access. */
	for (unsigned i = 0; i < 4; i++) {
		e = volume_open(&w->volumes[i], names[i], O_RDWR,
				(uint64_t)capacity[7 + i / 2] + NV_HEADER);
		if (e)
			goto done;
	}
	for (unsigned i = 0; i < 2; i++) {
		int fds[] = {w->volumes[2 * i].fd, w->volumes[2 * i + 1].fd};
		e = nv_vendor_repair_pair(fds, capacity[7 + i], w->deadline);
		if (e)
			goto done;
	}
	for (unsigned i = 0; i < 6; i++) {
		if ((e = check(w)) || (e = firmware(w, &w->policy.firmware[i])))
			goto done;
	}
	if ((e = commandline()) || (e = nv_output(w, 0)) || (e = nv_output(w, 1)) || (e = check(w)))
		goto done;
	int start = open(state_path, O_WRONLY | O_CLOEXEC);
	if (start < 0) {
		e = -errno;
		goto done;
	}
	ssize_t written = write(start, "start", 5);
	if (written != 5)
		e = written < 0 ? -errno : -EIO;
	close(start);
	if (!e) {
		n = cpnv_read(state_path, state, sizeof state);
		if (n != 8 || memcmp(state, "running\n", 8))
			e = -EIO;
	}
done:
	for (unsigned i = 0; i < 9; i++) {
		char path[128];
		snprintf(path, sizeof path, "/run/udx-cpnv-warm/%s.bin", images[i]);
		unlink(path);
	}
	return e;
}
