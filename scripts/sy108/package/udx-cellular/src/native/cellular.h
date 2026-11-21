/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_CELLULAR_H
#define UDX_CELLULAR_H
#include "common.h"
#include <signal.h>
#include <stdatomic.h>
#ifndef CELL_RUN
#define CELL_RUN "/run/udx-cellular"
#endif
#ifndef CELL_STORE
#define CELL_STORE "/etc/udx-cellular"
#endif
#ifndef CELL_BOOT
#define CELL_BOOT "/proc/sys/kernel/random/boot_id"
#endif
#define CELL_TELEMETRY_TASKS 8

struct at_channel {
	char pending[65537];
	size_t used;
	char sms_kind[8];
	unsigned sms_length;
};

struct transport {
	int fd, urc_fd, lock_fd;
	char wwan[32];
	bool session_dead;
	atomic_bool poisoned;
	atomic_bool stopping;
	bool sms_input_pending;
	char boot[37], command[257];
	struct at_channel channel, notifications;
	J *sms_events;
	bool wait_mode;
	J *mode_events;
	double last_log;
};

struct controller {
	struct transport transport;
	J *config, *status, *data, *fallback_profile;
	bool fallback_attempted;
	/* Identity of the candidate that was tried last, as "plmn/apn".  The
	 * candidate list is rebuilt on every attempt, so a positional cursor
	 * would drift whenever the order changes; matching on the identity
	 * keeps the retry sequence stable. */
	char fallback_tried[64];
	size_t fallback_index;
	bool want_data, initialized, ims_initialized;
	bool radio_restore_pending, restore_radio, restore_data;
	char error[512], sms_instance[33], sms_active_request[33];
	bool background, background_deferred;
	bool (*foreground_pending)(void *);
	void *foreground_context;
	int (*publish_state)(void *, J *);
	int (*retire_network)(void *);
	void *service_context;
	double telemetry_due[CELL_TELEMETRY_TASKS];
	unsigned telemetry_failures[CELL_TELEMETRY_TASKS];
	char telemetry_sim[48];
};

int transport_admit(struct transport *t);
int transport_init(struct transport *t);
int transport_reopen(struct transport *t);
int transport_drain(struct transport *t);
void transport_close(struct transport *t);
char *transport_command(struct transport *t, const char *command, double seconds, const char *pdu,
			bool network_mode);
char *transport_debug_command(struct transport *t, const char *command, double seconds,
			      char **failed_response);
int debug_validate(J *args);
J *debug_execute(struct controller *c, J *args);
int transport_poison(struct transport *t, const char *reason);
int transport_sms_line(struct transport *t, struct at_channel *channel, const char *line);
J *controller_config(void);
int controller_init(struct controller *c);
void controller_destroy(struct controller *c);
int controller_publish(struct controller *c);
J *controller_profile(struct controller *c);
J *controller_execute(struct controller *c, const char *operation, J *args);
int controller_validate(const char *operation, J *args);
J *controller_operations(void);
char *controller_at(struct controller *c, const char *command, double seconds);
J *controller_query(struct controller *c, const char *command, double seconds, const char *prefix);
int controller_set_data(struct controller *c, J *session);
J *controller_disconnect(struct controller *c, bool quiesce);
J *controller_connect(struct controller *c, unsigned wait_seconds);
int controller_refresh(struct controller *c);
int controller_radio_on(struct controller *c);

struct radio_change {
	bool active, enabled, wanted;
};

bool radio_enabled(struct controller *c);
int radio_state(struct controller *c);
int radio_command(struct controller *c, const char *wire, double timeout);
int radio_restore(struct controller *c, bool enabled, bool wanted);
int radio_begin(struct controller *c, struct radio_change *change);
int radio_end(struct controller *c, struct radio_change *change, bool success);
char *controller_sim(struct controller *c);
J *registration(const char *raw, const char *prefix);
J *parse_pdp(const char *raw, J *profile);
bool apn_matches(const char *requested, const char *actual);
int profile_validate(J *p);
int bearer_active(const char *raw);
J *bearer_default(const char *raw, J *profile);
J *sms_execute(struct controller *c, const char *operation, J *args);
int sms_settings_validate(J *args);
J *sms_configuration(void);
int sms_configure(struct controller *c, J *settings);
J *sms_publish(struct controller *c);
int sms_process_events(struct controller *c);
J *sms_refresh(struct controller *c);
J *sms_inbox(J *rows);
J *receipt_reserve(const char *operation, J *args, J *row, int *result);
int receipt_finish(J *row);
int ims_initialize(struct controller *c);
int ims_refresh(struct controller *c);
J *advanced_execute(struct controller *c, const char *operation, J *args);
int advanced_validate(const char *operation, J *args);
int advanced_set_mode(struct controller *c, const char *mode, J **result);
int identity_validate(const char *operation, J *args);
J *identity_parse(const char *raw);
J *identity_execute(struct controller *c, const char *operation, J *args);
const char *operator_brand(const char *plmn);
const char *operator_home_brand(struct controller *c);
const char *operator_resolve_brand(struct controller *c, const char *name);
const char *operator_plmn_from_name(const char *name);
J *operator_candidates(struct controller *c);
int operator_next_candidate(struct controller *c);
int operator_validate(const char *op, J *args);
J *operator_catalog(struct controller *c);
J *operator_parse_scan(const char *raw);
J *operator_execute(struct controller *c, const char *op, J *args);
int operator_fallback(struct controller *c);
J *apn_execute(struct controller *c, const char *operation, J *args);
int apn_validate(const char *operation, J *args);
int apn_auth_validate(J *args);
J *apn_public_auth(J *config);
int apn_configure(struct controller *c);
J *apn_publish(struct controller *c);
J *apn_direct(struct controller *c, J *args);
J *info_execute(struct controller *c, const char *operation);
char *info_identifier(const char *raw, const char *command, const char *prefix, size_t minimum,
		      size_t maximum, bool hexadecimal);
int info_sim_identity(struct controller *c);
J *info_signal(struct controller *c);
int telemetry_step(struct controller *c);
int telemetry_publish(struct controller *c);
J *decode_cells(const char *raw, const char *rat, bool serving);
J *mode_set_nr(struct controller *c, const char *mode);
J *mode_execute(struct controller *c, const char *operation, J *args);
int mode_validate(const char *operation, J *args);
#endif
