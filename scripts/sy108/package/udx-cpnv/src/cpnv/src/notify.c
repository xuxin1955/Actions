/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include "refnotify.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct notifier;

struct notify_client {
	struct uloop_fd io;
	struct notifier *owner;
	unsigned index;
};

struct notifier {
	struct cpnv *cpnv;
	struct uloop_fd port, listener;
	struct notify_client clients[RN_CLIENTS];
	struct uloop_timeout timer;
	struct rn_linux linux;
	struct rn_stream stream;
	struct rn_hub hub;
	uint8_t tx[RN_MAX_REPLY];
	size_t length, used;
	uint64_t partial_since, tx_since;
};

static void client_drop(struct notifier *n, unsigned i)
{
	struct uloop_fd *fd = &n->clients[i].io;
	if (fd->fd < 0)
		return;
	rn_hub_remove(&n->hub, fd->fd);
	uloop_fd_delete(fd);
	close(fd->fd);
	fd->fd = -1;
}

static int dispatch(void *arg, const uint8_t *bytes, size_t length)
{
	struct notifier *n = arg;
	struct rn_ops ops = {.read_calendar = rn_linux_read_calendar, .uptime = rn_linux_uptime};
	struct rn_result result;
	int e = rn_dispatch(bytes, length, &ops, &n->linux, &result);
	if (e) {
		fprintf(stderr, "notify: command=%u error=%d\n", result.command, e);
		return e == -EMSGSIZE || e == -EBADMSG || e == -EINVAL || e == -EPROTO ? e : 0;
	}
	if (result.reply_len) {
		if (n->length)
			return -ENOBUFS;
		memcpy(n->tx, result.reply, result.reply_len);
		n->length = result.reply_len;
		n->used = 0;
		n->tx_since = cpnv_now();
	}
	if (result.association_valid) {
		int evicted[RN_CLIENTS];
		e = rn_hub_publish(&n->hub, n->cpnv->epoch, result.association, evicted);
		if (e)
			return e;
		for (unsigned i = 0; i < RN_CLIENTS; i++) {
			if (evicted[i] >= 0)
				client_drop(n, i);
			else if (n->clients[i].io.fd >= 0)
				uloop_fd_add(&n->clients[i].io,
					     ULOOP_READ |
						 (n->hub.clients[i].queued ? ULOOP_WRITE : 0));
		}
	}
	return 0;
}

static void port_event(struct uloop_fd *fd, unsigned events)
{
	struct notifier *n = container_of(fd, struct notifier, port);
	int e = 0;
	if (fd->error || fd->eof) {
		cpnv_fail(n->cpnv, -EPIPE);
		return;
	}
	if (n->length && (events & ULOOP_WRITE)) {
		ssize_t got = write(fd->fd, n->tx + n->used, n->length - n->used);
		if (got < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (got <= 0) {
			cpnv_fail(n->cpnv, got < 0 ? -errno : -EPIPE);
			return;
		}
		n->used += (size_t)got;
		if (n->used == n->length) {
			n->length = n->used = 0;
			n->tx_since = 0;
			uloop_fd_add(fd, ULOOP_READ);
		}
	} else if (!n->length && (events & ULOOP_READ)) {
		uint8_t data[RN_MAX_FRAME];
		size_t used, want = n->stream.target - n->stream.have;
		ssize_t got = read(fd->fd, data, want);
		if (got < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (got <= 0) {
			cpnv_fail(n->cpnv, got < 0 ? -errno : -EPIPE);
			return;
		}
		if (!n->partial_since)
			n->partial_since = cpnv_now();
		e = rn_stream_feed(&n->stream, data, (size_t)got, &used, dispatch, n);
		if (e) {
			cpnv_fail(n->cpnv, e);
			return;
		}
		if (!n->stream.have)
			n->partial_since = 0;
		if (n->length)
			uloop_fd_add(fd, ULOOP_WRITE);
	}
}

static void timeout(struct uloop_timeout *timer)
{
	struct notifier *n = container_of(timer, struct notifier, timer);
	uint64_t now = cpnv_now();
	if ((n->partial_since && now - n->partial_since >= 5000) ||
	    (n->tx_since && now - n->tx_since >= 5000)) {
		cpnv_fail(n->cpnv, -ETIMEDOUT);
		return;
	}
	uloop_timeout_set(timer, 1000);
}

static void subscriber(struct uloop_fd *fd, unsigned events)
{
	struct notify_client *client = container_of(fd, struct notify_client, io);
	struct notifier *n = client->owner;
	unsigned i = client->index;
	if (fd->error || fd->eof || (events & ULOOP_READ)) {
		client_drop(n, i);
		return;
	}
	if (events & ULOOP_WRITE) {
		int e = rn_hub_flush(&n->hub, n->cpnv->epoch, fd->fd, rn_linux_send, NULL);
		if (e && e != -EAGAIN && e != -EINTR) {
			client_drop(n, i);
			return;
		}
		uloop_fd_add(fd, ULOOP_READ | (n->hub.clients[i].queued ? ULOOP_WRITE : 0));
	}
}

static void accept_client(struct uloop_fd *fd, unsigned events)
{
	struct notifier *n = container_of(fd, struct notifier, listener);
	(void)events;
	int client = accept4(fd->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (client < 0)
		return;
	if (rn_linux_check_client(client, 0) || rn_hub_subscribe(&n->hub, n->cpnv->epoch, client)) {
		close(client);
		return;
	}
	for (unsigned i = 0; i < RN_CLIENTS; i++)
		if (n->hub.clients[i].fd == client) {
			n->clients[i].io.fd = client;
			n->clients[i].io.cb = subscriber;
			uloop_fd_add(&n->clients[i].io,
				     ULOOP_READ | (n->hub.clients[i].queued ? ULOOP_WRITE : 0));
			break;
		}
}

int notify_start(struct cpnv *c)
{
	char policy[32];
	int count =
	    cpnv_private_read("/etc/udx-refnotify/owner-policy", policy, sizeof policy, false);
	if (count == -ENOENT || (count == 10 && !memcmp(policy, "enabled=0\n", 10)))
		return 0;
	if (count != 10 || memcmp(policy, "enabled=1\n", 10))
		return -EINVAL;
	struct notifier *n = calloc(1, sizeof *n);
	if (!n)
		return -ENOMEM;
	n->cpnv = c;
	n->port.fd = n->listener.fd = n->linux.rtc_fd = -1;
	for (unsigned i = 0; i < RN_CLIENTS; i++)
		n->clients[i].io.fd = -1;
	c->notify = n;
	for (unsigned i = 0; i < RN_CLIENTS; i++) {
		n->clients[i].owner = n;
		n->clients[i].index = i;
	}
	n->port.fd = cpnv_port_open("/dev/spipe_lte8", O_RDWR);
	if (n->port.fd < 0) {
		int error = n->port.fd;
		notify_close(c);
		return error;
	}
	n->linux.rtc_fd = cpnv_port_open("/dev/rtc0", O_RDONLY);
	if (n->linux.rtc_fd < 0) {
		int error = n->linux.rtc_fd;
		notify_close(c);
		return error;
	}
	struct sockaddr_un address;
	socklen_t length;
	int e = rn_linux_timesync_address(&address, &length);
	if (e) {
		notify_close(c);
		return e;
	}
	n->listener.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (n->listener.fd < 0)
		goto failed;
	if (bind(n->listener.fd, (struct sockaddr *)&address, length) || listen(n->listener.fd, 3))
		goto failed;
	rn_hub_init(&n->hub);
	rn_hub_begin(&n->hub, c->epoch);
	rn_stream_init(&n->stream);
	n->port.cb = port_event;
	n->listener.cb = accept_client;
	n->timer.cb = timeout;
	uloop_fd_add(&n->port, ULOOP_READ);
	uloop_fd_add(&n->listener, ULOOP_READ);
	uloop_timeout_set(&n->timer, 1000);
	return 0;
failed:
	e = -errno;
	notify_close(c);
	return e;
}

void notify_close(struct cpnv *c)
{
	struct notifier *n = c->notify;
	if (!n)
		return;
	uloop_timeout_cancel(&n->timer);
	uloop_fd_delete(&n->port);
	uloop_fd_delete(&n->listener);
	for (unsigned i = 0; i < RN_CLIENTS; i++)
		client_drop(n, i);
	if (n->port.fd >= 0)
		close(n->port.fd);
	if (n->listener.fd >= 0)
		close(n->listener.fd);
	if (n->linux.rtc_fd >= 0)
		close(n->linux.rtc_fd);
	free(n);
	c->notify = NULL;
}
