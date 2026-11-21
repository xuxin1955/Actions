/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPNV_H
#define CPNV_H
#include <stdint.h>
#include "cpnv_io.h"
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include "nv_ubi_backend.h"
#include <libubox/uloop.h>
#include <libubus.h>

struct firmware {
	unsigned segment, format;
	uint64_t offset, length, target_offset;
};

struct cpnv_policy {
	unsigned enabled, firmware_ubi, count;

	struct {
		unsigned storage, enabled, write_policy, timeout_ms, startup_ms;
	} nv;
	struct firmware firmware[6];
};

struct volume {
	int fd;
	unsigned number;
	struct nv_ubi_identity identity;
};

struct worker_status {
	bool modem_started, nv_ready, committing, stopped, discarded;
	int error;
};

struct nv_worker {
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	int event, cancel;
	bool nv_requested, stop_requested;
	struct worker_status status;
	struct cpnv_policy policy;
	struct volume volumes[4];
	int nv_fd;
	uint64_t deadline;
};
struct modem;
struct notifier;

struct cpnv {
	struct ubus_auto_conn bus;
	struct ubus_object object;
	struct ubus_request_data request;
	struct uloop_fd worker_event;
	struct uloop_timeout stop_timeout;
	struct uloop_signal term, interrupt;
	struct nv_worker worker;
	struct modem *modem;
	struct notifier *notify;
	bool halo_ready, nv_ready, committing, discarded, accepting;
	bool quiescing, quiesced, waiting, terminate, joined, worker_started, notify_started;
	int error, stop_error, result;
	uint64_t epoch;
};

uint64_t cpnv_now(void);
int cpnv_policy_parse(const char *, size_t, struct cpnv_policy *);
int cpnv_policy_load(struct cpnv_policy *);
int volume_open(struct volume *, const char *, int, uint64_t);
void volume_close(struct volume *);
int worker_start(struct nv_worker *, const struct cpnv_policy *);
void worker_request_nv(struct nv_worker *);
void worker_request_stop(struct nv_worker *);
bool worker_stopping(struct nv_worker *);
void worker_snapshot(struct nv_worker *, struct worker_status *);
void worker_publish(struct nv_worker *, const struct worker_status *);
int worker_prepare(struct nv_worker *);
int nv_service_run(struct nv_worker *);
void worker_join(struct nv_worker *);
int modem_start(struct cpnv *);
void modem_close(struct cpnv *);
void modem_stop(struct cpnv *);
void cpnv_fail(struct cpnv *, int);
void cpnv_halo_ready(struct cpnv *);
int notify_start(struct cpnv *);
void notify_close(struct cpnv *);
int cpnv_service_run(struct cpnv *, const struct cpnv_policy *);
#endif
