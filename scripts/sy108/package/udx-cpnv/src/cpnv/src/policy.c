/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include "modem_image.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int num(const char *s, uint64_t *v)
{
	uint64_t n = 0;
	if (!s || !*s)
		return -1;
	while (*s) {
		unsigned d = (unsigned)(*s++ - '0');
		if (d > 9 || n > (UINT64_MAX - d) / 10)
			return -1;
		n = n * 10 + d;
	}
	*v = n;
	return 0;
}

static int firmware_record(char *text, struct firmware *f)
{
	static const char *const names[] = {"modem", "v3phy", "nrphy", "nrdsp1", "nrdsp2", "deltanv"};
	static const char *const formats[] = {"raw", "sci", "sci-signed", "sci-payload", "dhtb"};
	char *token[6], *save;
	unsigned count = 0;
	for (char *v = strtok_r(text, " \t", &save); v; v = strtok_r(NULL, " \t", &save)) {
		if (count == 6)
			return -EINVAL;
		token[count++] = v;
	}
	if (count < 2)
		return -EINVAL;
	memset(f, 0, sizeof(*f));
	if (token[0][0] >= '0' && token[0][0] <= '9') {
		/* Existing conffiles used six columns, including an unused digest.
		 * Keep them readable and migrate the old DHTB payload slices. */
		uint64_t segment, format;
		if (count != 6 || num(token[0], &segment) || segment >= 6 ||
		    num(token[1], &format) || format > MCI_SCI_PAYLOAD ||
		    num(token[3], &f->offset) || num(token[4], &f->length) ||
		    num(token[5], &f->target_offset))
			return -EINVAL;
		f->segment = (unsigned)segment;
		f->format = (unsigned)format;
		if (f->format == MCI_RAW && segment >= 1 && segment <= 4 &&
		    f->offset == MCI_PREFIX_BYTES && !f->target_offset) {
			f->format = MCI_DHTB;
			f->offset = f->length = 0;
		}
	} else {
		for (f->segment = 0; f->segment < 6; f->segment++)
			if (!strcmp(token[0], names[f->segment]))
				break;
		for (f->format = 0; f->format <= MCI_DHTB; f->format++)
			if (!strcmp(token[1], formats[f->format]))
				break;
		if (f->segment == 6 || f->format > MCI_DHTB)
			return -EINVAL;
		if (f->format == MCI_RAW) {
			if (count != 5 || num(token[2], &f->offset) || num(token[3], &f->length) ||
			    num(token[4], &f->target_offset))
				return -EINVAL;
		} else if (count != 2) {
			return -EINVAL;
		}
	}
	if (f->format == MCI_RAW)
		return f->length ? 0 : -EINVAL;
	return f->offset || f->length || f->target_offset ? -EINVAL : 0;
}

int cpnv_policy_parse(const char *text, size_t n, struct cpnv_policy *p)
{
	if (!text || !p || !n || n >= 4096 || memchr(text, 0, n))
		return -EINVAL;
	char b[4096];
	memcpy(b, text, n);
	b[n] = 0;
	memset(p, 0, sizeof *p);
	unsigned seen = 0, fw = 0, storage = 0, firmware_source = 0;
	char *save, *s;
	static const char *keys[] = {"enabled", "write_policy", "timeout_ms", "startup_ms"};
	for (s = strtok_r(b, "\n", &save); s; s = strtok_r(NULL, "\n", &save)) {
		if (*s == '#')
			continue;
		if (!strcmp(s, "storage=ubi")) {
			if (storage++)
				return -EINVAL;
			p->nv.storage = 1;
			continue;
		}
		if (!strcmp(s, "firmware_source=ubi") || !strcmp(s, "firmware_source=files")) {
			if (firmware_source++)
				return -EINVAL;
			p->firmware_ubi = !strcmp(s, "firmware_source=ubi");
			continue;
		}
		if (!strncmp(s, "firmware=", 9)) {
			if (p->count >= 6)
				return -EINVAL;
			struct firmware *f = &p->firmware[p->count];
			if (firmware_record(s + 9, f) || (fw & (1u << f->segment)))
				return -EINVAL;
			p->count++;
			fw |= 1u << f->segment;
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq)
			return -EINVAL;
		*eq++ = 0;
		unsigned i;
		for (i = 0; i < 4; i++)
			if (!strcmp(s, keys[i]))
				break;
		uint64_t v;
		if (i == 4 || (seen & (1u << i)) || num(eq, &v) || v > UINT_MAX)
			return -EINVAL;
		seen |= 1u << i;
		switch (i) {
		case 0:
			p->enabled = (unsigned)v;
			break;
		case 1:
			p->nv.write_policy = (unsigned)v;
			break;
		case 2:
			p->nv.timeout_ms = (unsigned)v;
			break;
		case 3:
			p->nv.startup_ms = (unsigned)v;
			break;
		}
	}
	if (seen != 15 || (p->count && fw != 63))
		return -EINVAL;
	p->nv.enabled = p->enabled;
	return 0;
}
