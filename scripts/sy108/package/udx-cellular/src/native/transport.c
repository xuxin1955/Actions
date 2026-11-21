/* SPDX-License-Identifier: GPL-2.0-only */
/* One AT writer, separately drained URC
 * channel. */
#include "cellular.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

int transport_admit(struct transport *t)
{
	if (atomic_load(&t->stopping))
		return errorf(CELL_SYSTEM, "AT service is stopping");
	if (t->session_dead)
		return errorf(CELL_SYSTEM, "AT communication session ended");
	struct pollfd p[2] = {{t->fd, 0, 0}, {t->urc_fd, 0, 0}};
	if (poll(p, 2, 0) < 0 && errno != EINTR)
		return errorf(CELL_SYSTEM, "AT poll: %s", strerror(errno));
	for (unsigned i = 0; i < 2; i++)
		if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			t->session_dead = true;
			return errorf(CELL_SYSTEM, "AT communication session ended");
		}
	return 0;
}

static int mark(struct transport *t, const char *state)
{
	J *o = json_object_new_object();
	string(o, "boot_id", t->boot);
	string(o, "wwan", t->wwan);
	string(o, "state", state);
	real(o, "uptime", mono());
	if (strcmp(state, "owned") && strcmp(state, "clean"))
		string(o, "command", t->command);
	if (atomic_load(&t->poisoned))
		string(o, "error", cell_error);
	int r = atomic_json(CELL_RUN "/transport.json", o, false);
	json_object_put(o);
	return r;
}

int transport_poison(struct transport *t, const char *reason)
{
	char copy[512];
	snprintf(copy, sizeof(copy), "%s", reason);
	atomic_store(&t->poisoned, true);
	if (t->fd >= 0) {
		close(t->fd);
		t->fd = -1;
	}
	errorf(CELL_UNCERTAIN, "%s", copy);
	mark(t, "uncertain");
	errorf(CELL_UNCERTAIN, "%s", copy);
	if (mono() - t->last_log >= 60) {
		syslog(LOG_ERR, "udx-cellular at-uncertain: %s (%s)", copy, t->command);
		t->last_log = mono();
	}
	return -1;
}

static int append(struct at_channel *channel, const char *p, size_t n)
{
	if (n > sizeof(channel->pending) - 1 - channel->used)
		return errorf(CELL_INVALID, "AT line too large");
	memcpy(channel->pending + channel->used, p, n);
	channel->used += n;
	channel->pending[channel->used] = 0;
	return 0;
}

static void consume(struct at_channel *channel, size_t n)
{
	memmove(channel->pending, channel->pending + n, channel->used - n);
	channel->used -= n;
	channel->pending[channel->used] = 0;
}

/* Return one complete line; empty CR/LF records are harmless. */
static int line_take(struct at_channel *channel, char *line)
{
	size_t n = 0;
	while (n < channel->used && channel->pending[n] != '\r' && channel->pending[n] != '\n')
		n++;
	if (n == channel->used)
		return 0;
	memcpy(line, channel->pending, n);
	line[n] = 0;
	size_t end = n;
	while (end < channel->used &&
	       (channel->pending[end] == '\r' || channel->pending[end] == '\n'))
		end++;
	consume(channel, end);
	return 1;
}

static void queue_event(struct transport *t, J *event)
{
	if (json_object_array_length(t->sms_events) >= 128) {
		syslog(LOG_WARNING,
		       "udx-cellular SMS event queue full; delivery remains unacknowledged");
		json_object_put(event);
		return;
	}
	json_object_array_add(t->sms_events, event);
}

int transport_sms_line(struct transport *t, struct at_channel *channel, const char *line)
{
	if (channel->sms_kind[0]) {
		if (!*line)
			return 1;
		size_t n = strlen(line);
		bool valid = n >= 4 && n <= 1024 && !(n & 1);
		for (size_t i = 0; valid && i < n; i++)
			if (!isxdigit((unsigned char)line[i]))
				valid = false;
		if (!valid) {
			channel->sms_kind[0] = 0;
			syslog(LOG_WARNING, "udx-cellular invalid unsolicited SMS PDU");
			return 0;
		}
		char octet[3] = {line[0], line[1], 0};
		unsigned smsc = (unsigned)strtoul(octet, NULL, 16);
		if (n / 2 < 1u + smsc || n / 2 - 1u - smsc != channel->sms_length) {
			channel->sms_kind[0] = 0;
			syslog(LOG_WARNING, "udx-cellular unsolicited SMS TPDU length mismatch");
			return 1;
		}
		char upper[1025];
		for (size_t i = 0; i <= n; i++)
			upper[i] = (char)toupper((unsigned char)line[i]);
		J *event = json_object_new_object();
		string(event, "kind", channel->sms_kind);
		integer(event, "length", channel->sms_length);
		string(event, "pdu", upper);
		channel->sms_kind[0] = 0;
		queue_event(t, event);
		return 1;
	}
	const char *headers[] = {"+CMT:", "+CDS:", "+SPCMT:"};
	for (size_t i = 0; i < 3; i++)
		if (!strncmp(line, headers[i], strlen(headers[i]))) {
			const char *p = strrchr(line, ',');
			if (!p)
				p = line + strlen(headers[i]);
			else
				p++;
			while (*p == ' ' || *p == '\t')
				p++;
			int64_t length;
			if (!integer_text(p, 0, 9999, &length))
				return 0;
			snprintf(channel->sms_kind, sizeof(channel->sms_kind), "%.*s",
				 (int)strlen(headers[i]) - 1, headers[i]);
			channel->sms_length = (unsigned)length;
			return 1;
		}
	const char *stored[] = {"+CMTI:", "+CDSI:"};
	for (size_t i = 0; i < 2; i++)
		if (!strncmp(line, stored[i], strlen(stored[i]))) {
			char memory[3];
			unsigned index;
			int used = 0;
			if (sscanf(line + strlen(stored[i]), " \"%2[A-Z]\", %u%n", memory, &index,
				   &used) != 2 ||
			    line[strlen(stored[i]) + used] ||
			    (strcmp(memory, "SM") && strcmp(memory, "ME")) || !index ||
			    index > 65535)
				return 0;
			J *event = json_object_new_object();
			string(event, "kind", i ? "+CDSI" : "+CMTI");
			string(event, "storage", memory);
			integer(event, "index", index);
			queue_event(t, event);
			return 1;
		}
	return 0;
}

static bool transient(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == ENODATA || errno == EBUSY ||
	       errno == EINTR;
}

static int read_notifications(struct transport *t, char *line)
{
	if (t->urc_fd < 0)
		return 0;
	for (unsigned i = 0; i < 16; i++) {
		char buffer[4096];
		ssize_t n = read(t->urc_fd, buffer, sizeof(buffer));
		if (n < 0 && transient())
			return 0;
		if (n <= 0) {
			t->session_dead = true;
			return errorf(CELL_SYSTEM, "AT notification stream ended: %s",
				      n ? strerror(errno) : "EOF");
		}
		if (append(&t->notifications, buffer, (size_t)n))
			return -1;
		while (line_take(&t->notifications, line)) {
			if (!*line || transport_sms_line(t, &t->notifications, line))
				continue;
			if (t->wait_mode && !strncmp(line, "+SPTESTMODE:", 12)) {
				if (json_object_array_length(t->mode_events) >= 8)
					return errorf(CELL_INVALID,
						      "Too many asynchronous mode completions");
				json_object_array_add(t->mode_events, json_object_new_string(line));
			}
		}
	}
	return 0;
}

static int read_channels(struct transport *t, char *line)
{
	if (transport_admit(t))
		return -1;
	if (read_notifications(t, line))
		return -1;
	char buffer[4096];
	ssize_t n = read(t->fd, buffer, sizeof(buffer));
	if (n < 0 && transient())
		return 0;
	if (n <= 0) {
		t->session_dead = true;
		return errorf(CELL_SYSTEM, "AT command stream ended: %s",
			      n ? strerror(errno) : "EOF");
	}
	return append(&t->channel, buffer, (size_t)n);
}

static void wait_io(struct transport *t, unsigned ms)
{
	struct pollfd p[2] = {{t->fd, POLLIN, 0}, {t->urc_fd, POLLIN, 0}};
	poll(p, 2, (int)ms);
}

int transport_drain(struct transport *t)
{
	char *line = malloc(65537);
	if (!line)
		return errorf(CELL_SYSTEM, "AT buffer allocation failed");
	double end = mono() + 1;
	int result = 0;
	for (;;) {
		if (read_channels(t, line)) {
			result = transport_poison(t, cell_error);
			break;
		}
		while (line_take(&t->channel, line))
			if (*line)
				transport_sms_line(t, &t->channel, line);
		if (!t->channel.used && !t->channel.sms_kind[0])
			break;
		if (mono() >= end) {
			result =
			    transport_poison(t, "Incomplete unsolicited line at command boundary");
			break;
		}
		wait_io(t, 20);
	}
	free(line);
	return result;
}

/* Resolve the board's single modem parent once; global WWAN N may change. */
static int find_wwan(struct transport *t)
{
	DIR *dir = opendir("/sys/class/wwan");
	if (!dir)
		return errorf(CELL_SYSTEM, "WWAN class unavailable");
	struct dirent *entry;
	unsigned matches = 0;
	while ((entry = readdir(dir))) {
		const char *n = entry->d_name;
		if (strncmp(n, "wwan", 4) || !n[4] || strspn(n + 4, "0123456789") != strlen(n + 4))
			continue;
		char path[384], compatible[128];
		snprintf(path, sizeof(path), "/sys/class/wwan/%s/device/of_node/compatible", n);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		ssize_t len = read(fd, compatible, sizeof(compatible));
		close(fd);
		const char expected[] = "sprd,udx710-modem";
		if (len < (ssize_t)sizeof(expected) ||
		    memcmp(compatible, expected, sizeof(expected)))
			continue;
		if (strlen(n) >= sizeof(t->wwan))
			continue;
		memcpy(t->wwan, n, strlen(n) + 1);
		matches++;
	}
	closedir(dir);
	return matches == 1
		   ? 0
		   : errorf(CELL_SYSTEM, "Expected one UDX710 WWAN modem, found %u", matches);
}

static int open_channel(struct transport *t, unsigned index)
{
	char path[96];
	snprintf(path, sizeof(path), "/dev/%sat%u", t->wwan, index);
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	struct stat st;
	if (fd < 0)
		return errorf(CELL_SYSTEM, "Open AT channel: %s", strerror(errno));
	if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
		close(fd);
		return errorf(CELL_SYSTEM, "AT endpoint is not a character device");
	}
	return fd;
}

/* Only negative EAGAIN/EINTR means the driver has not accepted the request.
 * Once accepted, wait for its response; never replay a positive prefix. */
static int send_request(struct transport *t, const void *data, size_t size, double end, char *line)
{
	for (;;) {
		if (transport_admit(t))
			return -1;
		ssize_t n = write(t->fd, data, size);
		if (n == (ssize_t)size)
			return 0;
		if (n >= 0)
			return errorf(CELL_UNCERTAIN, "Partial AT write; no replay");
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			t->session_dead = true;
			return errorf(CELL_SYSTEM, "AT write failed: %s", strerror(errno));
		}
		if (mono() >= end)
			return errorf(CELL_SYSTEM, "AT request was not accepted before deadline");
		if (read_notifications(t, line))
			return -1;
		struct pollfd p[2] = {{t->fd, POLLOUT, 0}, {t->urc_fd, POLLIN, 0}};
		int ms = (int)((end - mono()) * 1000);
		if (ms > 50)
			ms = 50;
		if (ms > 0 && poll(p, 2, ms) < 0 && errno != EINTR)
			return errorf(CELL_SYSTEM, "AT write wait: %s", strerror(errno));
	}
}

static int handshake(struct transport *t)
{
	const char *wire = "ATE0Q0V1\r";
	snprintf(t->command, sizeof(t->command), "ATE0Q0V1");
	char *line = malloc(65537);
	if (!line)
		return errorf(CELL_SYSTEM, "AT handshake buffer allocation");
	int result = -1;
	for (unsigned attempt = 0; attempt < 8; attempt++) {
		if (transport_admit(t) || read_channels(t, line))
			goto fail;
		while (line_take(&t->channel, line))
			if (*line)
				transport_sms_line(t, &t->channel, line);
		t->channel.used = 0;
		t->channel.pending[0] = 0;
		if (send_request(t, wire, strlen(wire), mono() + .25, line)) {
			goto fail;
		}
		double end = mono() + .25;
		while (mono() < end) {
			if (read_channels(t, line))
				goto fail;
			while (line_take(&t->channel, line)) {
				if (!strcmp(line, "OK")) {
					result = 0;
					goto done;
				}
				if (*line)
					transport_sms_line(t, &t->channel, line);
			}
			wait_io(t, 10);
		}
	}
	errorf(CELL_UNCERTAIN, "AT handshake timed out");
fail:
	transport_poison(t, cell_error);
done:
	free(line);
	return result;
}

int transport_reopen(struct transport *t)
{
	if (t->fd >= 0) {
		close(t->fd);
		t->fd = -1;
	}
	if (transport_admit(t))
		return -1;
	t->fd = open_channel(t, 1);
	if (t->fd < 0)
		return -1;
	if (transport_admit(t))
		return transport_poison(t, cell_error);
	atomic_store(&t->poisoned, false);
	memset(&t->channel, 0, sizeof(t->channel));
	if (t->sms_input_pending) {
		char *line = malloc(65537);
		if (!line)
			return errorf(CELL_SYSTEM, "SMS cancellation buffer allocation");
		int ret = send_request(t, "\033", 1, mono() + 1, line);
		free(line);
		if (ret)
			return transport_poison(t, cell_error);
		delay_ms(100);
		t->sms_input_pending = false;
	}
	if (handshake(t))
		return -1;
	return mark(t, "owned");
}

int transport_init(struct transport *t)
{
	memset(t, 0, sizeof(*t));
	t->fd = t->urc_fd = t->lock_fd = -1;
	t->last_log = -60;
	atomic_init(&t->poisoned, false);
	atomic_init(&t->stopping, false);
	t->sms_events = json_object_new_array();
	t->mode_events = json_object_new_array();
	char *boot = read_text(CELL_BOOT, 64, true);
	if (!boot || strlen(boot) != 36) {
		free(boot);
		return errorf(CELL_SYSTEM, "Boot identity unavailable");
	}
	memcpy(t->boot, boot, 37);
	free(boot);
	if (transport_admit(t) || mkdir_private(CELL_RUN))
		return -1;
	t->lock_fd = open("/run/udx-cpnv-warm/at-query.lock",
			  O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (t->lock_fd < 0 || flock(t->lock_fd, LOCK_EX | LOCK_NB))
		return errorf(CELL_SYSTEM, "AT owner is already active");
	if (find_wwan(t))
		return -1;
	t->urc_fd = open_channel(t, 0);
	if (t->urc_fd < 0)
		return -1;
	return transport_reopen(t);
}

static char *command_exchange(struct transport *t, const char *command, double seconds,
			      const char *pdu, bool network_mode, char **failed_response)
{
	if (atomic_load(&t->poisoned)) {
		errorf(CELL_UNCERTAIN, "AT stream is uncertain; commands blocked");
		return NULL;
	}
	if (!command || strncmp(command, "AT", 2) || strlen(command) > 256 ||
	    strpbrk(command, "\r\n") || seconds <= 0 || seconds > 200) {
		errorf(CELL_INVALID, "Invalid internal AT command");
		return NULL;
	}
	if (pdu) {
		size_t n = strlen(pdu);
		unsigned tpdu;
		int used = 0;
		if (sscanf(command, "AT+CMGS=%u%n", &tpdu, &used) != 1 || command[used] || !tpdu ||
		    tpdu > 999 || n < 4 || n > 512 || n % 2) {
			errorf(CELL_INVALID, "Invalid SMS submission");
			return NULL;
		}
		for (size_t i = 0; i < n; i++)
			if (!strchr("0123456789ABCDEF", pdu[i])) {
				errorf(CELL_INVALID, "Invalid SMS PDU");
				return NULL;
			}
		char first[3] = {pdu[0], pdu[1], 0};
		if (n / 2 != (size_t)tpdu + 1 + strtoul(first, NULL, 16)) {
			errorf(CELL_INVALID, "SMS TPDU length mismatch");
			return NULL;
		}
	}
	if (transport_admit(t) || transport_drain(t))
		return NULL;
	snprintf(t->command, sizeof(t->command), "%s",
		 !strncmp(command, "AT+CGPCO=", 9) ? "AT+CGPCO=<redacted>" : command);
	if (mark(t, "inflight"))
		return NULL;
	char *line = malloc(65537), *response = calloc(65537, 1);
	if (!line || !response) {
		free(line);
		free(response);
		errorf(CELL_SYSTEM, "AT response allocation failed");
		return NULL;
	}
	t->wait_mode = network_mode;
	json_object_array_del_idx(t->mode_events, 0, json_object_array_length(t->mode_events));
	char wire[258];
	snprintf(wire, sizeof(wire), "%s\r", command);
	if (pdu)
		t->sms_input_pending = true;
	bool submitted = false, prompt = false;
	size_t response_size = 0;
	unsigned terminals = 0, completions = 0;
	bool terminal_ok = false, mode_ok = false, cmgs = false;
	char terminal[160] = {0};
	double end = mono() + seconds;
	if (send_request(t, wire, strlen(wire), end, line)) {
		transport_poison(t, cell_error);
		goto bad;
	}
	while (mono() < end) {
		if (atomic_load(&t->stopping)) {
			transport_poison(t, "AT interrupted by service stop; result unknown");
			goto bad;
		}
		if (read_channels(t, line)) {
			transport_poison(t, cell_error);
			goto bad;
		}
		for (;;) {
			bool from_mode = false;
			if (!line_take(&t->channel, line)) {
				if (!json_object_array_length(t->mode_events))
					break;
				snprintf(line, 65537, "%s",
					 json_object_get_string(
					     json_object_array_get_idx(t->mode_events, 0)));
				json_object_array_del_idx(t->mode_events, 0, 1);
				from_mode = true;
			}
			if (!*line || (!from_mode && transport_sms_line(t, &t->channel, line)))
				continue;
			if (!strcmp(line, ">") || !strcmp(line, "> ")) {
				prompt = true;
				continue;
			}
			size_t n = strlen(line);
			if (n + 2 > 65536 - response_size) {
				transport_poison(t, "AT response too large");
				goto bad;
			}
			memcpy(response + response_size, line, n);
			response_size += n;
			memcpy(response + response_size, "\r\n", 3);
			response_size += 2;
			bool term =
			    !strcmp(line, "OK") || !strcmp(line, "ERROR") ||
			    !strncmp(line, "+CME ERROR:", 11) ||
			    !strncmp(line, "+CMS ERROR:", 11) ||
			    (!strncmp(command, "AT+CGDATA=", 10) && !strcmp(line, "CONNECT"));
			if (term) {
				terminals++;
				snprintf(terminal, sizeof(terminal), "%.159s", line);
				terminal_ok = !strcmp(line, "OK") || !strcmp(line, "CONNECT");
			}
			if (network_mode && !strncmp(line, "+SPTESTMODE:", 12)) {
				completions++;
				const char *p = line + 12;
				while (isspace((unsigned char)*p))
					p++;
				mode_ok = !strcmp(p, "1");
			}
			if (!strncmp(line, "+CMGS:", 6))
				cmgs = true;
		}
		if (pdu && !submitted &&
		    (prompt || !strcmp(t->channel.pending, ">") ||
		     !strcmp(t->channel.pending, "> "))) {
			if (transport_admit(t)) {
				transport_poison(t, cell_error);
				goto bad;
			}
			char payload[514];
			size_t n = strlen(pdu);
			memcpy(payload, pdu, n);
			payload[n++] = 26;
			if (send_request(t, payload, n, end, line)) {
				transport_poison(t, cell_error);
				goto bad;
			}
			submitted = true;
			t->sms_input_pending = false;
			response_size = 0;
			response[0] = 0;
			t->channel.used = 0;
			t->channel.pending[0] = 0;
			terminals = completions = 0;
			cmgs = false;
			continue;
		}
		if (terminals > 1 || completions > 1) {
			transport_poison(t, "Ambiguous AT completion");
			goto bad;
		}
		if (terminals && (!terminal_ok || !network_mode || completions)) {
			if (pdu)
				t->sms_input_pending = false;
			if (!terminal_ok) {
				mark(t, "owned");
				errorf(CELL_MODEM, "Modem rejected command: %s", terminal);
				goto bad;
			}
			if (pdu && (!submitted || !cmgs)) {
				transport_poison(t, "SMS confirmation missing; no replay");
				goto bad;
			}
			if (network_mode && !mode_ok) {
				mark(t, "owned");
				errorf(CELL_MODEM,
				       "Modem asynchronous network-mode completion failed");
				goto bad;
			}
			if (mark(t, "owned")) {
				transport_poison(
				    t,
				    "Command completed but transport state could not be recorded");
				goto bad;
			}
			t->wait_mode = false;
			free(line);
			return response;
		}
		wait_io(t, 20);
	}
	transport_poison(t, pdu ? (submitted ? "AT response timed out while waiting "
					       "for SMS confirmation; no replay"
					     : "AT response timed out while waiting "
					       "for SMS prompt; no replay")
				: "AT response timed out; no command replay");
bad:
	t->wait_mode = false;
	json_object_array_del_idx(t->mode_events, 0, json_object_array_length(t->mode_events));
	free(line);
	if (failed_response)
		*failed_response = response;
	else
		free(response);
	return NULL;
}

char *transport_command(struct transport *t, const char *command, double seconds, const char *pdu,
			bool network_mode)
{
	return command_exchange(t, command, seconds, pdu, network_mode, NULL);
}

char *transport_debug_command(struct transport *t, const char *command, double seconds,
			      char **failed_response)
{
	*failed_response = NULL;
	return command_exchange(t, command, seconds, NULL,
				!strncasecmp(command, "AT+SPTESTMODE=", 14), failed_response);
}

void transport_close(struct transport *t)
{
	if (t->fd >= 0)
		close(t->fd);
	if (t->urc_fd >= 0)
		close(t->urc_fd);
	t->fd = t->urc_fd = -1;
	if (t->boot[0])
		mark(t, atomic_load(&t->poisoned) ? "closed" : "clean");
	if (t->lock_fd >= 0)
		close(t->lock_fd);
	t->lock_fd = -1;
	json_object_put(t->sms_events);
	json_object_put(t->mode_events);
	t->sms_events = t->mode_events = NULL;
}
