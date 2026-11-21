/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_REFNOTIFY_H
#define UDX_REFNOTIFY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#define RN_MAX_FRAME 128u
#define RN_MAX_REPLY 14u
#define RN_CLIENTS 3u

struct rn_calendar {
	int year, month, day, hour, minute, second, weekday;
};

struct rn_message {
	uint32_t command;

	union {
		struct {
			unsigned second, minute, hour;
		} time;

		struct {
			unsigned day, month, year, weekday;
		} date;

		uint32_t cp_counter; /* Units/wrap meaning unknown: do not extrapolate. */
	} data;
};

/* Only read-only RTC queries and the CP time-association record are served. */
struct rn_ops {
	int (*read_calendar)(void *, struct rn_calendar *);
	int (*uptime)(void *, uint64_t *);
};

struct rn_result {
	uint32_t command;
	uint8_t reply[RN_MAX_REPLY];
	size_t reply_len;
	uint8_t association[8];
	bool association_valid;
	int error; /* Local error ONLY. No vendor wire ACK/error format was found. */
};

uint32_t rn_get_le32(const uint8_t *);
void rn_put_le32(uint8_t *, uint32_t);
int rn_calendar_epoch(const struct rn_calendar *, int64_t *, int *);
int rn_decode(const uint8_t *, size_t, struct rn_message *);
int rn_dispatch(const uint8_t *, size_t, const struct rn_ops *, void *, struct rn_result *);

struct rn_stream {
	uint8_t data[RN_MAX_FRAME];
	size_t have, target;
	int error;
};

typedef int (*rn_frame_sink)(void *, const uint8_t *, size_t);
void rn_stream_init(struct rn_stream *);
int rn_stream_feed(struct rn_stream *, const uint8_t *, size_t, size_t *, rn_frame_sink, void *);
int rn_stream_finish(struct rn_stream *);

/* Time-association stream: three owner-authorized clients; no bind/accept here.
 * Each queue retains complete 8-byte record order. Slow peers are evicted rather
 * than corrupting record boundaries or pretending to deliver every update.
 * FDs are borrowed; the owner must close every evicted fd before reusing it. */
struct rn_client {
	int fd;
	uint8_t pending[8];
	size_t offset;
	bool queued;
};

struct rn_hub {
	uint64_t epoch;
	bool active, valid;
	uint8_t latest[8];
	struct rn_client clients[RN_CLIENTS];
};

typedef ssize_t (*rn_send_fn)(void *, int, const void *, size_t); /* returns bytes or -errno */
void rn_hub_init(struct rn_hub *);
int rn_hub_begin(struct rn_hub *, uint64_t);
int rn_hub_end(struct rn_hub *, int evicted[RN_CLIENTS]);
int rn_hub_subscribe(struct rn_hub *, uint64_t, int);
int rn_hub_remove(struct rn_hub *, int);
int rn_hub_publish(struct rn_hub *, uint64_t, const uint8_t[8], int evicted[RN_CLIENTS]);
int rn_hub_flush(struct rn_hub *, uint64_t, int, rn_send_fn, void *);

/* The main loop owns the read-only RTC descriptor. */
struct rn_linux {
	int rtc_fd;
};

int rn_linux_read_calendar(void *, struct rn_calendar *);
int rn_linux_uptime(void *, uint64_t *);
ssize_t rn_linux_send(void *, int, const void *, size_t);
/* Pure address construction; caller alone decides whether to bind/accept. */
int rn_linux_timesync_address(struct sockaddr_un *, socklen_t *);
int rn_linux_check_client(int fd, uid_t allowed_uid);
#endif
