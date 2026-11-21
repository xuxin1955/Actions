/* SPDX-License-Identifier: GPL-2.0-only */
#include "refnotify.h"
#include <errno.h>
#include <time.h>

int rn_calendar_epoch(const struct rn_calendar *c, int64_t *epoch, int *weekday)
{
	if (!c || !epoch || !weekday)
		return -EINVAL;
	if (c->year < 1970 || c->year > 9999 || c->month < 1 || c->month > 12 || c->day < 1 ||
	    c->day > 31 || c->hour < 0 || c->hour > 23 || c->minute < 0 || c->minute > 59 ||
	    c->second < 0 || c->second > 59)
		return -ERANGE;
	struct tm tm = {.tm_year = c->year - 1900,
			.tm_mon = c->month - 1,
			.tm_mday = c->day,
			.tm_hour = c->hour,
			.tm_min = c->minute,
			.tm_sec = c->second};
	errno = 0;
	time_t t = timegm(&tm);
	if (t == (time_t)-1 || errno || tm.tm_year != c->year - 1900 || tm.tm_mon != c->month - 1 ||
	    tm.tm_mday != c->day || tm.tm_hour != c->hour || tm.tm_min != c->minute ||
	    tm.tm_sec != c->second)
		return -ERANGE;
	*epoch = (int64_t)t;
	*weekday = tm.tm_wday;
	return 0;
}
