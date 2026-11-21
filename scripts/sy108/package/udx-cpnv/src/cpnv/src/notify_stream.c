/* SPDX-License-Identifier: GPL-2.0-only */
#include "refnotify.h"
#include <errno.h>
#include <string.h>

void rn_stream_init(struct rn_stream *s)
{
	memset(s, 0, sizeof(*s));
	s->target = 8;
}

int rn_stream_feed(struct rn_stream *s, const uint8_t *b, size_t n, size_t *used,
		   rn_frame_sink sink, void *ctx)
{
	size_t take;
	int ret;
	if (!s || (!b && n) || !used || !sink)
		return -EINVAL;
	*used = 0;
	if (s->error)
		return s->error;
	if (s->target < 8 || s->target > RN_MAX_FRAME || s->have >= s->target)
		return s->error = -EINVAL;
	while (*used < n) {
		take = s->target - s->have;
		if (take > n - *used)
			take = n - *used;
		memcpy(s->data + s->have, b + *used, take);
		s->have += take;
		*used += take;
		if (s->have != s->target)
			continue;
		if (s->target == 8) {
			s->target = rn_get_le32(s->data + 4);
			if (s->target < 8 || s->target > RN_MAX_FRAME)
				return s->error = -EMSGSIZE;
			if (s->target > 8)
				continue;
		}
		ret = sink(ctx, s->data, s->target);
		if (ret)
			return s->error = ret < 0 ? ret : -EPROTO;
		s->have = 0;
		s->target = 8;
	}
	return 0;
}

int rn_stream_finish(struct rn_stream *s)
{
	if (!s)
		return -EINVAL;
	if (s->error)
		return s->error;
	return s->have ? (s->error = -EBADMSG) : 0;
}
