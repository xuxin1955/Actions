/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded 3GPP SMS codec; no device
 * access. */
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint16_t gsm[128] = {
    '@',   0xa3, '$',  0xa5,  0xe8, 0xe9,  0xf9,  0xec,	 0xf2,	0xc7,  '\n',  0xd8,  0xf8,
    '\r',  0xc5, 0xe5, 0x394, '_',  0x3a6, 0x393, 0x39b, 0x3a9, 0x3a0, 0x3a8, 0x3a3, 0x398,
    0x39e, 27,	 0xc6, 0xe6,  0xdf, 0xc9,  ' ',	  '!',	 '"',	'#',   0xa4,  '%',   '&',
    '\'',  '(',	 ')',  '*',   '+',  ',',   '-',	  '.',	 '/',	'0',   '1',   '2',   '3',
    '4',   '5',	 '6',  '7',   '8',  '9',   ':',	  ';',	 '<',	'=',   '>',   '?',   0xa1,
    'A',   'B',	 'C',  'D',   'E',  'F',   'G',	  'H',	 'I',	'J',   'K',   'L',   'M',
    'N',   'O',	 'P',  'Q',   'R',  'S',   'T',	  'U',	 'V',	'W',   'X',   'Y',   'Z',
    0xc4,  0xd6, 0xd1, 0xdc,  0xa7, 0xbf,  'a',	  'b',	 'c',	'd',   'e',   'f',   'g',
    'h',   'i',	 'j',  'k',   'l',  'm',   'n',	  'o',	 'p',	'q',   'r',   's',   't',
    'u',   'v',	 'w',  'x',   'y',  'z',   0xe4,  0xf6,	 0xf1,	0xfc,  0xe0};

static uint32_t extension(unsigned c)
{
	switch (c) {
	case 10:
		return '\f';
	case 20:
		return '^';
	case 40:
		return '{';
	case 41:
		return '}';
	case 47:
		return '\\';
	case 60:
		return '[';
	case 61:
		return '~';
	case 62:
		return ']';
	case 64:
		return '|';
	case 101:
		return 0x20ac;
	default:
		return 0xfffd;
	}
}

static size_t put_utf8(char *out, uint32_t c)
{
	if (c < 128) {
		out[0] = (char)c;
		return 1;
	}
	if (c < 2048) {
		out[0] = (char)(0xc0 | (c >> 6));
		out[1] = (char)(0x80 | (c & 63));
		return 2;
	}
	if (c < 65536) {
		out[0] = (char)(0xe0 | (c >> 12));
		out[1] = (char)(0x80 | ((c >> 6) & 63));
		out[2] = (char)(0x80 | (c & 63));
		return 3;
	}
	out[0] = (char)(0xf0 | (c >> 18));
	out[1] = (char)(0x80 | ((c >> 12) & 63));
	out[2] = (char)(0x80 | ((c >> 6) & 63));
	out[3] = (char)(0x80 | (c & 63));
	return 4;
}

/* Decode scalars first: byte boundaries are not Unicode character boundaries.
 */
static int utf8_scalars(const char *text, uint32_t out[918])
{
	const unsigned char *p = (const unsigned char *)text;
	size_t count = 0;
	if (!text || !*text)
		return errorf(CELL_INVALID, "SMS text must not be empty");
	while (*p) {
		uint32_t c;
		unsigned n;
		uint32_t minimum;
		if (*p < 128) {
			c = *p++;
			n = 0;
			minimum = 0;
		} else if (*p >= 0xc2 && *p <= 0xdf) {
			c = *p++ & 31;
			n = 1;
			minimum = 128;
		} else if (*p >= 0xe0 && *p <= 0xef) {
			c = *p++ & 15;
			n = 2;
			minimum = 2048;
		} else if (*p >= 0xf0 && *p <= 0xf4) {
			c = *p++ & 7;
			n = 3;
			minimum = 65536;
		} else
			return errorf(CELL_INVALID, "Invalid UTF-8 SMS text");
		while (n--) {
			if ((*p & 0xc0) != 0x80)
				return errorf(CELL_INVALID, "Invalid UTF-8 SMS text");
			c = (c << 6) | (*p++ & 63);
		}
		if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
			return errorf(CELL_INVALID, "Invalid UTF-8 SMS text");
		if (count == 918)
			return errorf(CELL_INVALID, "SMS exceeds six parts");
		out[count++] = c;
	}
	return (int)count;
}

static int gsm_code(uint32_t c)
{
	for (int i = 0; i < 128; i++)
		if (i != 27 && gsm[i] == c)
			return i;
	for (int i = 0; i < 128; i++)
		if (extension(i) != 0xfffd && extension(i) == c)
			return 128 + i;
	return -1;
}

int sms_validate_number(const char *p)
{
	if (!p)
		return errorf(CELL_INVALID, "Invalid SMS number");
	if (*p == '+')
		p++;
	size_t n = strlen(p);
	if (n < 3 || n > 15)
		return errorf(CELL_INVALID,
			      "The number must contain 3–15 digits, optionally prefixed with +");
	for (; *p; p++)
		if (*p < '0' || *p > '9')
			return errorf(CELL_INVALID, "Invalid SMS number");
	return 0;
}

static unsigned nibble(char c)
{
	return c <= '9' ? (unsigned)(c - '0') : (unsigned)(toupper((unsigned char)c) - 'A' + 10);
}

static int unhex(const char *s, unsigned char out[512])
{
	if (!s)
		return errorf(CELL_INVALID, "Missing SMS PDU");
	size_t n = strlen(s);
	if (n < 4 || n > 1024 || n % 2)
		return errorf(CELL_INVALID, "Invalid SMS PDU length");
	for (size_t i = 0; i < n; i += 2) {
		if (!isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1]))
			return errorf(CELL_INVALID, "Invalid SMS PDU hex");
		out[i / 2] = (unsigned char)((nibble(s[i]) << 4) | nibble(s[i + 1]));
	}
	return (int)(n / 2);
}

J *sms_encode(const char *number, const char *text, unsigned reference)
{
	if (sms_validate_number(number))
		return NULL;
	uint32_t scalars[918];
	int count = utf8_scalars(text, scalars);
	if (count < 0)
		return NULL;
	bool seven = true;
	for (int i = 0; i < count; i++)
		if (gsm_code(scalars[i]) < 0)
			seven = false;
	unsigned char data[3672];
	bool boundary[3673] = {true};
	size_t size = 0;
	for (int i = 0; i < count; i++) {
		uint32_t c = scalars[i];
		if (seven) {
			int code = gsm_code(c);
			if (code >= 128)
				data[size++] = 27;
			data[size++] = code & 127;
		} else {
			if (c > 65535) {
				uint32_t high = 0xd800 + ((c - 0x10000) >> 10);
				data[size++] = high >> 8;
				data[size++] = high;
				c = 0xdc00 + ((c - 0x10000) & 1023);
			}
			data[size++] = c >> 8;
			data[size++] = c;
		}
		boundary[size] = true;
	}
	size_t limit = seven ? 160 : 140, chunk = size <= limit ? limit : seven ? 153 : 134;
	size_t offsets[7] = {0}, total = 0;
	while (offsets[total] < size) {
		if (total == 6) {
			errorf(CELL_INVALID, "SMS exceeds six parts");
			return NULL;
		}
		size_t end = offsets[total] + chunk;
		if (end > size)
			end = size;
		while (!boundary[end])
			end--;
		offsets[++total] = end;
	}
	const char *digits = number + (number[0] == '+');
	size_t ndigit = strlen(digits);
	J *parts = json_object_new_array();
	for (size_t part = 0; part < total; part++) {
		unsigned char pdu[256];
		size_t n = 0, used = offsets[part], take = offsets[part + 1] - used;
		pdu[n++] = 0;
		pdu[n++] = total > 1 ? 0x41 : 1;
		pdu[n++] = 0;
		pdu[n++] = (unsigned char)ndigit;
		pdu[n++] = number[0] == '+' ? 0x91 : 0x81;
		for (size_t i = 0; i < ndigit; i += 2)
			pdu[n++] =
			    (unsigned char)((digits[i] - '0') |
					    ((i + 1 < ndigit ? digits[i + 1] - '0' : 15) << 4));
		pdu[n++] = 0;
		pdu[n++] = seven ? 0 : 8;
		pdu[n++] = (unsigned char)(take + (total > 1 ? (seven ? 7 : 6) : 0));
		size_t payload = n;
		memset(pdu + payload, 0, 140);
		if (total > 1) {
			pdu[n++] = 5;
			pdu[n++] = 0;
			pdu[n++] = 3;
			pdu[n++] = (unsigned char)reference;
			pdu[n++] = (unsigned char)total;
			pdu[n++] = (unsigned char)(part + 1);
		}
		if (seven) {
			size_t bit = total > 1 ? 49 : 0;
			for (size_t i = 0; i < take; i++, bit += 7) {
				pdu[payload + bit / 8] |=
				    (unsigned char)(data[used + i] << (bit % 8));
				if (bit % 8 > 1)
					pdu[payload + bit / 8 + 1] |=
					    data[used + i] >> (8 - bit % 8);
			}
			n = payload + (bit + 7) / 8;
		} else {
			memcpy(pdu + n, data + used, take);
			n += take;
		}
		char hex[513];
		for (size_t i = 0; i < n; i++) {
			hex[i * 2] = "0123456789ABCDEF"[pdu[i] >> 4];
			hex[i * 2 + 1] = "0123456789ABCDEF"[pdu[i] & 15];
		}
		hex[n * 2] = 0;
		J *row = json_object_new_object();
		string(row, "pdu", hex);
		integer(row, "tpdu_length", (int64_t)n - 1);
		integer(row, "part", (int64_t)part + 1);
		integer(row, "total", (int64_t)total);
		json_object_array_add(parts, row);
	}
	return parts;
}

static char *unpack7(const unsigned char *p, size_t bytes, unsigned count, unsigned skip)
{
	if ((size_t)count * 7 > bytes * 8 || skip > count) {
		errorf(CELL_INVALID, "Truncated GSM septets");
		return NULL;
	}
	char *out = malloc((size_t)count * 4 + 1);
	if (!out)
		return NULL;
	size_t used = 0;
	bool escaped = false;
	for (unsigned i = skip; i < count; i++) {
		size_t bit = (size_t)i * 7, at = bit / 8;
		unsigned value = p[at];
		if (at + 1 < bytes)
			value |= (unsigned)p[at + 1] << 8;
		value = (value >> (bit % 8)) & 127;
		if (escaped) {
			used += put_utf8(out + used, extension(value));
			escaped = false;
		} else if (value == 27)
			escaped = true;
		else
			used += put_utf8(out + used, gsm[value]);
	}
	out[used] = 0;
	return out;
}

struct pdu_reader {
	unsigned char bytes[512];
	size_t size, offset;
};

static const unsigned char *take(struct pdu_reader *r, size_t n)
{
	if (r->offset > r->size || n > r->size - r->offset) {
		errorf(CELL_INVALID, "Truncated SMS PDU");
		return NULL;
	}
	const unsigned char *p = r->bytes + r->offset;
	r->offset += n;
	return p;
}

J *sms_decode(const char *hex)
{
	struct pdu_reader r = {0};
	int size = unhex(hex, r.bytes);
	if (size < 0)
		return NULL;
	r.size = (size_t)size;
	r.offset = 1u + r.bytes[0];
	const unsigned char *p = take(&r, 1);
	if (!p)
		return NULL;
	unsigned first = *p, mti = first & 3;
	J *out = json_object_new_object(), *concat = NULL;
	char *peer = NULL, *text = NULL;
	unsigned dcs = 0;
	if (mti > 1) {
		string(out, "kind", mti == 2 ? "status_report" : "unknown");
		string(out, "raw_pdu", hex);
		return out;
	}
	if (mti == 1 && !take(&r, 1))
		goto bad;
	p = take(&r, 2);
	if (!p)
		goto bad;
	unsigned digits = p[0], toa = p[1];
	size_t address_bytes = (digits + 1u) / 2;
	p = take(&r, address_bytes);
	if (!p)
		goto bad;
	if ((toa & 0x70) == 0x50)
		peer = unpack7(p, address_bytes, digits * 4 / 7, 0);
	else {
		peer = malloc(digits + 2u);
		if (!peer)
			goto bad;
		size_t used = 0;
		if ((toa & 0x70) == 0x10)
			peer[used++] = '+';
		for (unsigned i = 0; i < digits; i++) {
			unsigned digit = (p[i / 2] >> ((i % 2) * 4)) & 15;
			if (digit == 15 && i + 1 == digits)
				break;
			if (digit == 15) {
				errorf(CELL_INVALID, "Interior filler in SMS address");
				goto bad;
			}
			peer[used++] = "0123456789*#abc"[digit];
		}
		peer[used] = 0;
	}
	if (!peer)
		goto bad;
	p = take(&r, 2);
	if (!p)
		goto bad;
	dcs = p[1];
	string(out, "kind", mti ? "submitted" : "received");
	string(out, "peer", peer);
	string(out, "timestamp", NULL);
	if (!mti) {
		p = take(&r, 7);
		if (!p)
			goto bad;
		unsigned v[6];
		for (size_t i = 0; i < 6; i++)
			v[i] = (p[i] & 15) * 10 + (p[i] >> 4);
		char stamp[64];
		snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u", 2000 + v[0], v[1],
			 v[2], v[3], v[4], v[5]);
		string(out, "timestamp", stamp);
	} else {
		unsigned vpf = (first >> 3) & 3;
		if (vpf && !take(&r, vpf == 2 ? 1 : 7))
			goto bad;
	}
	p = take(&r, 1);
	if (!p)
		goto bad;
	unsigned udl = *p, alphabet = 3;
	bool supported = true;
	integer(out, "dcs", dcs);
	if (dcs < 0x80) {
		alphabet = (dcs >> 2) & 3;
		supported = !(dcs & 0x20) && alphabet != 3;
		boolean(out, "automatic_deletion", (dcs & 0x40) != 0);
		if (dcs & 0x10)
			integer(out, "message_class", dcs & 3);
	} else if (dcs >= 0xc0 && dcs <= 0xef && !(dcs & 4)) {
		alphabet = dcs >= 0xe0 ? 2 : 0;
		boolean(out, "mwi_active", (dcs & 8) != 0);
		boolean(out, "mwi_discard", dcs < 0xd0);
		integer(out, "mwi_type", dcs & 3);
	} else if (dcs >= 0xf0 && dcs <= 0xf7) {
		alphabet = (dcs & 4) ? 1 : 0;
		integer(out, "message_class", dcs & 3);
	} else
		supported = false;
	if (!supported) {
		string(out, "text", NULL);
		string(out, "raw_pdu", hex);
		string(out, "decode_error", "Unsupported DCS coding group or compression");
		free(peer);
		return out;
	}
	size_t bytes = alphabet == 0 ? (udl * 7u + 7) / 8 : udl;
	p = take(&r, bytes);
	if (!p)
		goto bad;
	size_t header = 0;
	if (first & 64) {
		if (!bytes || (header = (size_t)p[0] + 1) > bytes) {
			errorf(CELL_INVALID, "Truncated SMS user-data header");
			goto bad;
		}
		for (size_t i = 1; i < header;) {
			if (header - i < 2) {
				errorf(CELL_INVALID, "Truncated SMS information element");
				goto bad;
			}
			unsigned tag = p[i++], n = p[i++];
			if (n > header - i) {
				errorf(CELL_INVALID, "Truncated SMS information payload");
				goto bad;
			}
			if ((tag == 0 && n == 3) || (tag == 8 && n == 4)) {
				json_object_put(concat);
				concat = json_object_new_object();
				unsigned ref = tag == 0 ? p[i] : ((unsigned)p[i] << 8) | p[i + 1];
				integer(concat, "reference", ref);
				integer(concat, "total", p[i + n - 2]);
				integer(concat, "part", p[i + n - 1]);
			}
			i += n;
		}
	}
	integer(out, "dcs", dcs);
	json_object_object_add(out, "concat", concat);
	concat = NULL;
	if (alphabet == 1) {
		char binary[1025];
		for (size_t i = header; i < bytes; i++) {
			binary[(i - header) * 2] = "0123456789ABCDEF"[p[i] >> 4];
			binary[(i - header) * 2 + 1] = "0123456789ABCDEF"[p[i] & 15];
		}
		binary[(bytes - header) * 2] = 0;
		string(out, "text", NULL);
		string(out, "binary_hex", binary);
		string(out, "encoding", "8bit");
		free(peer);
		return out;
	}
	string(out, "encoding", alphabet ? "utf16be" : "gsm7");
	if (!alphabet)
		text = unpack7(p, bytes, udl, (unsigned)((header * 8 + 6) / 7));
	else {
		text = malloc(bytes * 4 + 5);
		if (!text)
			goto bad;
		size_t used = 0;
		for (size_t i = header; i < bytes;) {
			uint32_t c = p[i++];
			if (alphabet == 2) {
				if (i == bytes)
					c = 0xfffd;
				else {
					c = (c << 8) | p[i++];
					if (c >= 0xd800 && c <= 0xdbff) {
						if (bytes - i >= 2) {
							uint32_t low =
							    ((uint32_t)p[i] << 8) | p[i + 1];
							if (low >= 0xdc00 && low <= 0xdfff) {
								i += 2;
								c = 0x10000 + ((c - 0xd800) << 10) +
								    low - 0xdc00;
							} else
								c = 0xfffd;
						} else
							c = 0xfffd;
					} else if (c >= 0xdc00 && c <= 0xdfff)
						c = 0xfffd;
				}
			}
			/* NUL is valid UCS2 data; json-c preserves its explicit length. */
			used += put_utf8(text + used, c);
		}
		json_object_object_add(out, "text", json_object_new_string_len(text, (int)used));
		free(text);
		free(peer);
		return out;
	}
	if (!text)
		goto bad;
	string(out, "text", text);
	free(text);
	free(peer);
	return out;
bad:
	free(peer);
	free(text);
	json_object_put(concat);
	json_object_put(out);
	if (!cell_error[0])
		errorf(CELL_INVALID, "Invalid SMS PDU");
	return NULL;
}

J *sms_list(const char *raw)
{
	if (!at_complete(raw)) {
		errorf(CELL_INVALID, "Incomplete SMS listing");
		return NULL;
	}
	char *copy = strdup(raw);
	if (!copy)
		return NULL;
	J *out = json_object_new_array();
	char *save, *line = strtok_r(copy, "\r\n", &save);
	for (; line; line = strtok_r(NULL, "\r\n", &save)) {
		while (isspace((unsigned char)*line))
			line++;
		if (strncmp(line, "+CMGL:", 6))
			continue;
		unsigned index, status;
		int consumed = 0;
		if (sscanf(line + 6, " %u , %u ,%n", &index, &status, &consumed) != 2 ||
		    !consumed || !strchr(line + 6 + consumed, ',') || index < 1 || index > 65535 ||
		    status > 3)
			goto bad;
		for (size_t i = 0; i < json_object_array_length(out); i++)
			if (json_object_get_int(get(json_object_array_get_idx(out, i), "index")) ==
			    (int)index)
				goto bad;
		char *pdu = strtok_r(NULL, "\r\n", &save);
		if (!pdu)
			goto bad;
		while (isspace((unsigned char)*pdu))
			pdu++;
		size_t n = strlen(pdu);
		while (n && isspace((unsigned char)pdu[n - 1]))
			pdu[--n] = 0;
		J *row = sms_decode(pdu);
		if (!row)
			goto bad;
		integer(row, "index", index);
		integer(row, "status", status);
		string(row, "raw_pdu", pdu);
		json_object_array_add(out, row);
	}
	free(copy);
	return out;
bad:
	free(copy);
	json_object_put(out);
	errorf(CELL_INVALID, "Invalid or duplicate SMS listing row");
	return NULL;
}
