/* SPDX-License-Identifier: GPL-2.0-only */
#include "refnotify.h"
#include <errno.h>
#include <string.h>

uint32_t rn_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void rn_put_le32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; ++i)
		p[i] = (uint8_t)(v >> (8u * i));
}

int rn_decode(const uint8_t *b, size_t n, struct rn_message *m)
{
	if (!b || !m)
		return -EINVAL;
	memset(m, 0, sizeof(*m));
	if (n < 8 || n > RN_MAX_FRAME)
		return -EMSGSIZE;
	if (rn_get_le32(b + 4) != n)
		return -EBADMSG;
	m->command = rn_get_le32(b);
	switch (m->command) {
	case 1:
		if (n != 11 && n != 12)
			return -EMSGSIZE;
		m->data.time.second = b[8];
		m->data.time.minute = b[9];
		m->data.time.hour = b[10];
		if (b[8] > 59 || b[9] > 59 || b[10] > 23)
			return -ERANGE;
		break;
	case 2:
		if (n != 13 && n != 14)
			return -EMSGSIZE;
		m->data.date.day = b[8];
		m->data.date.month = b[9];
		m->data.date.year = (unsigned)b[10] | (unsigned)b[11] << 8;
		m->data.date.weekday = b[12];
		{
			struct rn_calendar c = {
			    .year = (int)m->data.date.year, .month = (int)b[9], .day = (int)b[8]};
			int w, ret;
			int64_t e;
			ret = rn_calendar_epoch(&c, &e, &w);
			if (ret)
				return ret;
			if (b[12] > 6 || (int)b[12] != w)
				return -ERANGE;
		}
		break;
	case 3:
	case 4:
	case 5:
	case 8:
	case 9:
		if (n != 8)
			return -EMSGSIZE;
		break;
	case 10:
		if (n != 12)
			return -EMSGSIZE;
		m->data.cp_counter = rn_get_le32(b + 8);
		break;
	default:
		return -EOPNOTSUPP; /* Includes inbound display-only 6/7. */
	}
	return 0;
}

static int checked(int ret)
{
	return ret > 0 || ret < -4095 ? -EPROTO : ret;
}

static void header(uint8_t *b, uint32_t cmd, uint32_t len)
{
	rn_put_le32(b, cmd);
	rn_put_le32(b + 4, len);
}

int rn_dispatch(const uint8_t *bytes, size_t length, const struct rn_ops *ops, void *context,
		struct rn_result *result)
{
	struct rn_message message = {0};
	struct rn_calendar calendar = {0};
	int weekday, error;
	int64_t epoch;
	uint64_t uptime;
	if (!result || !ops)
		return -EINVAL;
	memset(result, 0, sizeof *result);
	error = rn_decode(bytes, length, &message);
	result->command = message.command;
	if (error)
		goto done;
	if (message.command == 10) {
		if (!ops->uptime) {
			error = -EOPNOTSUPP;
			goto done;
		}
		error = checked(ops->uptime(context, &uptime));
		if (error)
			goto done;
		rn_put_le32(result->association, message.data.cp_counter);
		rn_put_le32(result->association + 4, (uint32_t)uptime);
		result->association_valid = true;
		goto done;
	}
	if (message.command != 3 && message.command != 4) {
		error = -EPERM;
		goto done;
	}
	if (!ops->read_calendar) {
		error = -EOPNOTSUPP;
		goto done;
	}
	error = checked(ops->read_calendar(context, &calendar));
	if (error)
		goto done;
	error = rn_calendar_epoch(&calendar, &epoch, &weekday);
	if (error)
		goto done;
	if (message.command == 3) {
		header(result->reply, 3, 11);
		result->reply[8] = (uint8_t)calendar.second;
		result->reply[9] = (uint8_t)calendar.minute;
		result->reply[10] = (uint8_t)calendar.hour;
		result->reply_len = 11;
	} else {
		header(result->reply, 4, 14);
		result->reply[8] = (uint8_t)calendar.day;
		result->reply[9] = (uint8_t)calendar.month;
		result->reply[10] = (uint8_t)calendar.year;
		result->reply[11] = (uint8_t)((unsigned)calendar.year >> 8);
		result->reply[12] = (uint8_t)weekday;
		result->reply_len = 14;
	}
done:
	result->error = error;
	return error;
}
