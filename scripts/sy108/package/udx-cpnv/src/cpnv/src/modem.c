/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include "halo.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

enum phase {
	WAIT_ALIVE,
	OPEN_HALO,
	QUERY,
	WAIT_PEER,
	MATERIAL,
	FETCH,
	REPORT,
	FACTORY,
	FEATURE,
	MARK,
	READY,
	MAINTAIN
};

struct modem {
	struct cpnv *cpnv;
	struct uloop_fd status, halo;
	struct uloop_timeout timer;
	enum phase phase;
	uint64_t phase_end, deadline, next;
	uint8_t token[4], modulus[128], feature_key[8], tx[152], rx[152];
	size_t tx_length, tx_used, rx_length, rx_used;
	char scan[32];
	size_t scan_used;
};

static void halo_event(struct uloop_fd *, unsigned);
static void tick(struct uloop_timeout *);

static void schedule(struct modem *m)
{
	uint64_t now = cpnv_now(), at = m->deadline;
	switch (m->phase) {
	case WAIT_ALIVE:
		if (m->status.fd < 0 && at > now + 100)
			at = now + 100;
		break;
	case OPEN_HALO:
		at = m->phase_end < now + 100 ? m->phase_end : now + 100;
		break;
	case WAIT_PEER:
		at = m->next < m->phase_end ? m->next : m->phase_end;
		break;
	case READY:
		at = m->next;
		break;
	default:
		break;
	}
	uloop_timeout_set(&m->timer, at <= now ? 0 : (int)(at - now));
}

static uint64_t query_deadline(struct modem *m)
{
	uint64_t at = cpnv_now() + 5000;
	return at < m->phase_end ? at : m->phase_end;
}

static bool suffix(const struct modem *m, const char *text)
{
	size_t n = strlen(text);
	return m->scan_used >= n && !memcmp(m->scan + m->scan_used - n, text, n);
}

static uint32_t clock_word(void)
{
	return (uint32_t)(uint64_t)clock();
}

static int issue(struct modem *m, enum phase phase, const uint8_t *data, size_t bytes, size_t reply,
		 uint64_t deadline)
{
	m->phase = phase;
	m->tx_length = bytes;
	m->tx_used = m->rx_used = 0;
	m->rx_length = reply;
	m->deadline = deadline;
	memcpy(m->tx, data, bytes);
	memset(m->rx, 0, sizeof m->rx);
	schedule(m);
	return uloop_fd_add(&m->halo, ULOOP_READ | ULOOP_WRITE) < 0 ? -EIO : 0;
}

static void packet(struct modem *m, unsigned command, uint8_t *out, size_t bytes)
{
	uint32_t word = clock_word();
	uint8_t nonce[] = {(uint8_t)word, (uint8_t)(word >> 8)};
	if (command == 0xa1) {
		halo_factory_query_frame(m->token, nonce, 0, out);
		return;
	}
	if (command == 0xa2) {
		halo_feature_query_frame(m->token, nonce, 0, out);
		return;
	}
	memset(out, 0, bytes);
	out[0] = 0xfc;
	out[1] = 0xab;
	out[2] = (uint8_t)command;
	out[3] = (uint8_t)(bytes - 8);
	halo_token_encode(m->token, nonce, out + 4);
}

static int authenticate(struct modem *m)
{
	uint8_t nonce[16], out[152] = {0};
	uint64_t now = cpnv_now();
	m->deadline = now + 15000;
	if (m->deadline > m->phase_end)
		m->deadline = m->phase_end;
	uint32_t word = clock_word();
	if (getrandom(nonce, sizeof nonce, GRND_NONBLOCK) != (ssize_t)sizeof nonce)
		return -EIO;
	for (unsigned i = 0; i < 4; i++)
		m->token[i] = (uint8_t)(word >> (24 - 8 * i));
	out[0] = 0xfc;
	out[1] = 0xab;
	out[2] = 0xa0;
	out[3] = 0x90;
	memcpy(out + 4, m->token, 4);
	memcpy(out + 8, nonce, 16);
	int e = halo_sign_material("/etc/udx-cpnv-warm/halo-private.bin", nonce, clock_word(),
				   out + 24);
	if (e)
		return e;
	if (cpnv_now() >= m->deadline)
		return -ETIMEDOUT;
	return issue(m, MATERIAL, out, sizeof out, 9, m->deadline);
}

static int reply(struct modem *m)
{
	uint8_t out[152];
	unsigned state;
	int e;
	uint64_t bits;
	switch (m->phase) {
	case QUERY:
		if ((e = halo_ap_state_decode(m->rx, 16, &state)))
			return e;
		if (state) {
			m->phase = WAIT_PEER;
			m->next = cpnv_now() + 30000;
			uloop_fd_delete(&m->halo);
			schedule(m);
			return 0;
		}
		return authenticate(m);
	case MATERIAL:
		if (m->rx[8] || !halo_token_check(m->token, m->rx + 4))
			return -EKEYREJECTED;
		packet(m, 0xc0, out, 8);
		return issue(m, FETCH, out, 8, 152, m->deadline);
	case FETCH: {
		uint32_t peer_clock;
		if (m->rx[0] != 0x79 || m->rx[1] != 0xa5 || m->rx[3] != 0x90)
			return -EPROTO;
		if ((e = halo_material_verify(m->rx + 8, m->modulus, &peer_clock)))
			return e;
		packet(m, 0xc1, out, 9);
		return issue(m, REPORT, out, 9, 9, m->deadline);
	}
	case REPORT:
		if (m->rx[8])
			return -EKEYREJECTED;
		packet(m, 0xa1, out, 9);
		return issue(m, FACTORY, out, 9, 9, m->deadline);
	case FACTORY:
		if (m->rx[8] != 1)
			return m->rx[8] == 0xf0 ? -EBADMSG : -EKEYREJECTED;
		packet(m, 0xa2, out, 9);
		return issue(m, FEATURE, out, 9, HALO_FEATURE_REPLY_BYTES, m->deadline);
	case FEATURE:
		if ((e = halo_feature_decode(m->rx, HALO_FEATURE_REPLY_BYTES, m->feature_key, &bits,
					     &state)))
			return e;
		halo_ap_mark_frame(out);
		return issue(m, MARK, out, 16, 16, m->deadline);
	case MARK:
	case MAINTAIN:
		if ((e = halo_ap_state_decode(m->rx, 16, &state)))
			return e;
		if (state > 1)
			return -EPROTO;
		m->phase = READY;
		m->next = cpnv_now() + 30000;
		uloop_fd_delete(&m->halo);
		schedule(m);
		cpnv_halo_ready(m->cpnv);
		return 0;
	default:
		return -EPROTO;
	}
}

static void halo_event(struct uloop_fd *fd, unsigned events)
{
	struct modem *m = container_of(fd, struct modem, halo);
	int e = 0;
	if (fd->error || fd->eof) {
		cpnv_fail(m->cpnv, -EPIPE);
		return;
	}
	if (cpnv_now() >= m->deadline) {
		cpnv_fail(m->cpnv, -ETIMEDOUT);
		return;
	}
	if ((events & ULOOP_WRITE) && m->tx_used < m->tx_length) {
		ssize_t n = write(fd->fd, m->tx + m->tx_used, m->tx_length - m->tx_used);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (n <= 0) {
			cpnv_fail(m->cpnv, n < 0 ? -errno : -EPIPE);
			return;
		}
		m->tx_used += (size_t)n;
		if (m->tx_used == m->tx_length)
			uloop_fd_add(fd, ULOOP_READ);
	}
	if ((events & ULOOP_READ) && m->tx_used == m->tx_length) {
		ssize_t n = read(fd->fd, m->rx + m->rx_used, m->rx_length - m->rx_used);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (n <= 0) {
			cpnv_fail(m->cpnv, n < 0 ? -errno : -EPIPE);
			return;
		}
		m->rx_used += (size_t)n;
		if (m->rx_used == m->rx_length)
			e = reply(m);
	}
	if (e)
		cpnv_fail(m->cpnv, e);
}

static void status_event(struct uloop_fd *fd, unsigned events)
{
	struct modem *m = container_of(fd, struct modem, status);
	(void)events;
	if (fd->error || fd->eof) {
		cpnv_fail(m->cpnv, -EPIPE);
		return;
	}
	for (unsigned pass = 0; pass < 16; pass++) {
		char bytes[256];
		ssize_t n = read(fd->fd, bytes, sizeof bytes);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (n <= 0) {
			cpnv_fail(m->cpnv, n < 0 ? -errno : -EPIPE);
			return;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (m->scan_used == sizeof m->scan - 1) {
				memmove(m->scan, m->scan + 1, --m->scan_used);
			}
			m->scan[m->scan_used++] = bytes[i];
			m->scan[m->scan_used] = 0;
			static const char *const faults[] = {"Modem Assert", "Modem Reset", "HUNG",
							     "Modem Blocked", "AGDSP Assert"};
			for (unsigned i = 0; i < sizeof faults / sizeof faults[0]; i++)
				if (suffix(m, faults[i])) {
					fprintf(stderr, "modem: received %s\n", faults[i]);
					cpnv_fail(m->cpnv, -EHOSTDOWN);
					return;
				}
			if (m->phase == WAIT_ALIVE && suffix(m, "Modem Alive")) {
				m->phase = OPEN_HALO;
				m->phase_end = cpnv_now() + 120000;
				worker_request_nv(&m->cpnv->worker);
				uloop_timeout_set(&m->timer, 0);
			}
		}
	}
}

static void tick(struct uloop_timeout *timer)
{
	struct modem *m = container_of(timer, struct modem, timer);
	uint64_t now = cpnv_now();
	int e = 0;
	if (m->cpnv->error || m->cpnv->quiescing)
		return;
	if (m->phase == WAIT_ALIVE) {
		if (now >= m->deadline) {
			e = -ETIMEDOUT;
			goto done;
		}
		if (m->status.fd < 0) {
			m->status.fd = cpnv_port_open("/dev/spipe_lte2", O_RDONLY);
			if (m->status.fd >= 0)
				uloop_fd_add(&m->status, ULOOP_READ);
			else if (!cpnv_port_pending(m->status.fd)) {
				e = m->status.fd;
				goto done;
			}
		}
	} else if (m->phase == OPEN_HALO) {
		if (now >= m->phase_end) {
			e = -ETIMEDOUT;
			goto done;
		}
		m->halo.fd = cpnv_port_open("/dev/spipe_lte3", O_RDWR);
		if (m->halo.fd >= 0) {
			uint8_t verify[136];
			int n = cpnv_private_read("/etc/udx-cpnv-warm/halo-verification.bin",
						  verify, sizeof verify, true);
			if (n != 136) {
				e = n < 0 ? n : -EIO;
				goto done;
			}
			memcpy(m->modulus, verify, 128);
			memcpy(m->feature_key, verify + 128, 8);
			uint8_t out[16];
			halo_ap_query_frame(out);
			e = issue(m, QUERY, out, 16, 16, query_deadline(m));
		} else if (!cpnv_port_pending(m->halo.fd))
			e = m->halo.fd;
	} else if (m->phase == WAIT_PEER) {
		if (now >= m->phase_end) {
			e = -ETIMEDOUT;
			goto done;
		}
		if (now >= m->next) {
			uint8_t out[16];
			halo_ap_query_frame(out);
			e = issue(m, QUERY, out, 16, 16, query_deadline(m));
		}
	} else if (m->phase == READY) {
		if (now >= m->next) {
			uint8_t out[16];
			halo_ap_mark_frame(out);
			e = issue(m, MAINTAIN, out, 16, 16, now + 5000);
		}
	} else if (now >= m->deadline)
		e = -ETIMEDOUT;
done:
	if (e)
		cpnv_fail(m->cpnv, e);
	else
		schedule(m);
}

int modem_start(struct cpnv *c)
{
	struct modem *m = calloc(1, sizeof *m);
	if (!m)
		return -ENOMEM;
	m->cpnv = c;
	m->status.fd = m->halo.fd = -1;
	m->status.cb = status_event;
	m->halo.cb = halo_event;
	m->timer.cb = tick;
	m->phase = WAIT_ALIVE;
	m->deadline = cpnv_now() + 60000;
	c->modem = m;
	uloop_timeout_set(&m->timer, 0);
	return 0;
}

void modem_stop(struct cpnv *c)
{
	struct modem *m = c->modem;
	if (!m)
		return;
	uloop_timeout_cancel(&m->timer);
	uloop_fd_delete(&m->status);
	uloop_fd_delete(&m->halo);
	if (m->status.fd >= 0)
		close(m->status.fd);
	if (m->halo.fd >= 0)
		close(m->halo.fd);
	m->status.fd = m->halo.fd = -1;
}

void modem_close(struct cpnv *c)
{
	modem_stop(c);
	free(c->modem);
	c->modem = NULL;
}
