/* SPDX-License-Identifier: GPL-2.0-only */
#include "refnotify.h"
#include <errno.h>
#include <string.h>

void rn_hub_init(struct rn_hub *h)
{
	memset(h, 0, sizeof(*h));
	for (size_t i = 0; i < RN_CLIENTS; ++i)
		h->clients[i].fd = -1;
}

int rn_hub_begin(struct rn_hub *h, uint64_t epoch)
{
	if (!h || !epoch)
		return -EINVAL;
	if (h->active)
		return -EBUSY;
	if (epoch <= h->epoch)
		return -ESTALE;
	h->epoch = epoch;
	h->active = true;
	h->valid = false;
	return 0;
}

int rn_hub_end(struct rn_hub *h, int evicted[RN_CLIENTS])
{
	if (!h || !evicted)
		return -EINVAL;
	for (size_t i = 0; i < RN_CLIENTS; ++i) {
		evicted[i] = h->clients[i].fd;
		h->clients[i].fd = -1;
		h->clients[i].queued = false;
	}
	h->active = h->valid = false;
	memset(h->latest, 0, 8);
	return 0;
}

static int epoch_check(const struct rn_hub *h, uint64_t epoch)
{
	return !h->active || h->epoch != epoch ? -ESTALE : 0;
}

int rn_hub_subscribe(struct rn_hub *h, uint64_t epoch, int fd)
{
	if (!h || fd < 0)
		return -EINVAL;
	if (epoch_check(h, epoch))
		return -ESTALE;
	for (size_t i = 0; i < RN_CLIENTS; ++i)
		if (h->clients[i].fd == fd)
			return -EEXIST;
	for (size_t i = 0; i < RN_CLIENTS; ++i)
		if (h->clients[i].fd < 0) {
			struct rn_client *c = &h->clients[i];
			c->fd = fd;
			c->offset = 0;
			c->queued = h->valid;
			if (h->valid)
				memcpy(c->pending, h->latest, 8);
			return 0;
		}
	return -ENOSPC;
}

int rn_hub_remove(struct rn_hub *h, int fd)
{
	if (!h || fd < 0)
		return -EINVAL;
	for (size_t i = 0; i < RN_CLIENTS; ++i)
		if (h->clients[i].fd == fd) {
			h->clients[i].fd = -1;
			h->clients[i].queued = false;
			return 0;
		}
	return -ENOENT;
}

int rn_hub_publish(struct rn_hub *h, uint64_t epoch, const uint8_t sample[8],
		   int evicted[RN_CLIENTS])
{
	if (!h || !sample || !evicted)
		return -EINVAL;
	for (size_t i = 0; i < RN_CLIENTS; ++i)
		evicted[i] = -1;
	if (epoch_check(h, epoch))
		return -ESTALE;
	memcpy(h->latest, sample, 8);
	h->valid = true;
	for (size_t i = 0; i < RN_CLIENTS; ++i) {
		struct rn_client *c = &h->clients[i];
		if (c->fd < 0)
			continue;
		if (c->queued) {
			evicted[i] = c->fd;
			c->fd = -1;
			c->queued = false;
			continue;
		}
		memcpy(c->pending, sample, 8);
		c->offset = 0;
		c->queued = true;
	}
	return 0; /* Caller must inspect evictions; does NOT mean delivered. */
}

int rn_hub_flush(struct rn_hub *h, uint64_t epoch, int fd, rn_send_fn send_fn, void *ctx)
{
	if (!h || fd < 0 || !send_fn)
		return -EINVAL;
	if (epoch_check(h, epoch))
		return -ESTALE;
	for (size_t i = 0; i < RN_CLIENTS; ++i)
		if (h->clients[i].fd == fd) {
			struct rn_client *c = &h->clients[i];
			ssize_t n;
			if (!c->queued)
				return 0;
			/* Exactly one send per call bounds work even with malicious callbacks. */
			n = send_fn(ctx, fd, c->pending + c->offset, 8 - c->offset);
			if (n < 0) {
				if (n == -EAGAIN || n == -EINTR)
					return (int)n;
				c->fd = -1;
				c->queued = false;
				return n < -4095 ? -EIO : (int)n;
			}
			if (!n || (size_t)n > 8 - c->offset) {
				c->fd = -1;
				c->queued = false;
				return -EIO;
			}
			c->offset += (size_t)n;
			if (c->offset < 8)
				return -EAGAIN;
			c->queued = false;
			c->offset = 0;
			return 0;
		}
	return -ENOENT;
}
