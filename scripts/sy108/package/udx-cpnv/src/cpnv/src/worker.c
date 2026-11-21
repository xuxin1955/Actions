/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static void signal_event(int fd)
{
	uint64_t one = 1;
	if (write(fd, &one, sizeof one) < 0 && errno != EAGAIN)
		perror("cpnv eventfd");
}

bool worker_stopping(struct nv_worker *w)
{
	pthread_mutex_lock(&w->mutex);
	bool stop = w->stop_requested;
	pthread_mutex_unlock(&w->mutex);
	return stop;
}

void worker_snapshot(struct nv_worker *w, struct worker_status *out)
{
	pthread_mutex_lock(&w->mutex);
	*out = w->status;
	pthread_mutex_unlock(&w->mutex);
}

void worker_publish(struct nv_worker *w, const struct worker_status *state)
{
	pthread_mutex_lock(&w->mutex);
	w->status = *state;
	pthread_mutex_unlock(&w->mutex);
	signal_event(w->event);
}

static void *run(void *arg)
{
	struct nv_worker *w = arg;
	struct worker_status state = {0};
	state.error = worker_prepare(w);
	if (state.error)
		goto done;
	state.modem_started = true;
	worker_publish(w, &state);
	pthread_mutex_lock(&w->mutex);
	while (!w->nv_requested && !w->stop_requested)
		pthread_cond_wait(&w->condition, &w->mutex);
	bool stop = w->stop_requested;
	pthread_mutex_unlock(&w->mutex);
	int run_error = stop ? 0 : nv_service_run(w);
	worker_snapshot(w, &state);
	if (run_error)
		state.error = run_error;
done:
	for (unsigned i = 0; i < 4; i++)
		volume_close(&w->volumes[i]);
	if (w->nv_fd >= 0) {
		if (close(w->nv_fd) && !state.error)
			state.error = -errno;
		w->nv_fd = -1;
	}
	state.nv_ready = state.committing = false;
	state.stopped = true;
	if (state.error == -ECANCELED && worker_stopping(w))
		state.error = 0;
	worker_publish(w, &state);
	return NULL;
}

int worker_start(struct nv_worker *w, const struct cpnv_policy *policy)
{
	struct cpnv_policy saved = *policy;
	memset(w, 0, sizeof *w);
	w->event = w->cancel = w->nv_fd = -1;
	for (unsigned i = 0; i < 4; i++)
		w->volumes[i].fd = -1;
	w->policy = saved;
	int e = pthread_mutex_init(&w->mutex, NULL);
	if (e)
		return -e;
	e = pthread_cond_init(&w->condition, NULL);
	if (e) {
		pthread_mutex_destroy(&w->mutex);
		return -e;
	}
	w->event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	w->cancel = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (w->event < 0 || w->cancel < 0) {
		e = errno;
		goto failed;
	}
	e = pthread_create(&w->thread, NULL, run, w);
	if (!e)
		return 0;
failed:
	if (w->event >= 0)
		close(w->event);
	if (w->cancel >= 0)
		close(w->cancel);
	pthread_cond_destroy(&w->condition);
	pthread_mutex_destroy(&w->mutex);
	return -e;
}

void worker_request_nv(struct nv_worker *w)
{
	pthread_mutex_lock(&w->mutex);
	w->nv_requested = true;
	pthread_cond_signal(&w->condition);
	pthread_mutex_unlock(&w->mutex);
}

void worker_request_stop(struct nv_worker *w)
{
	pthread_mutex_lock(&w->mutex);
	w->stop_requested = true;
	pthread_cond_signal(&w->condition);
	pthread_mutex_unlock(&w->mutex);
	signal_event(w->cancel);
}

void worker_join(struct nv_worker *w)
{
	pthread_join(w->thread, NULL);
	close(w->event);
	close(w->cancel);
	pthread_cond_destroy(&w->condition);
	pthread_mutex_destroy(&w->mutex);
}
