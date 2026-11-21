/* SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include <uci.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifndef CELL_UCI_CONFIG
#define CELL_UCI_CONFIG "/etc/config/udx-cellular"
#endif

static struct uci_context *config_open(struct uci_package **package)
{
	struct uci_context *ctx = uci_alloc_context();
	char *path = strdup(CELL_UCI_CONFIG);
	if (!ctx || !path)
		goto fail;
	char *name = strrchr(path, '/');
	if (!name)
		goto fail;
	*name++ = 0;
	if (uci_set_confdir(ctx, path) || uci_load(ctx, name, package))
		goto fail;
	free(path);
	return ctx;
fail:
	free(path);
	if (ctx)
		uci_free_context(ctx);
	errorf(CELL_SYSTEM, "Cannot load cellular configuration");
	return NULL;
}

J *cell_config_sections(void)
{
	struct uci_package *package;
	struct uci_context *ctx = config_open(&package);
	if (!ctx)
		return NULL;
	J *out = json_object_new_object();
	struct uci_element *e, *o, *item;
	uci_foreach_element(&package->sections, e)
	{
		struct uci_section *section = uci_to_section(e);
		J *row = json_object_new_object();
		string(row, ".type", section->type);
		uci_foreach_element(&section->options, o)
		{
			struct uci_option *option = uci_to_option(o);
			if (option->type == UCI_TYPE_STRING) {
				string(row, o->name, option->v.string);
			} else {
				J *list = json_object_new_array();
				uci_foreach_element(&option->v.list, item)
				    json_object_array_add(list, json_object_new_string(item->name));
				json_object_object_add(row, o->name, list);
			}
		}
		json_object_object_add(out, e->name, row);
	}
	uci_free_context(ctx);
	return out;
}

static bool config_key(const char *key)
{
	unsigned parts = 0;
	if (!key)
		return false;
	for (;;) {
		if (!(isalpha((unsigned char)*key) || *key == '_'))
			return false;
		for (key++; isalnum((unsigned char)*key) || *key == '_'; key++) {
		}
		if (!*key)
			return true;
		if (*key++ != '.' || ++parts > 1)
			return false;
	}
}

static bool config_scalar(J *value)
{
	if (!json_object_is_type(value, json_type_string) &&
	    !json_object_is_type(value, json_type_int))
		return false;
	const char *s = json_object_get_string(value);
	return s && !strchr(s, '\n') && !strchr(s, '\r') &&
	       (!json_object_is_type(value, json_type_string) ||
		strlen(s) == (size_t)json_object_get_string_len(value));
}

static int config_change(struct uci_context *ctx, struct uci_package *package, const char *key,
			 J *value)
{
	char *name = NULL;
	if (asprintf(&name, "%s.%s", package->e.name, key) < 0)
		return -1;
	struct uci_ptr ptr = {0};
	int ret = uci_lookup_ptr(ctx, &ptr, name, true);
	if (ret)
		goto done;
	if (!value) {
		ret = (ptr.flags & UCI_LOOKUP_COMPLETE) ? uci_delete(ctx, &ptr) : 0;
	} else if (json_object_is_type(value, json_type_array)) {
		if (ptr.flags & UCI_LOOKUP_COMPLETE) {
			ret = uci_delete(ctx, &ptr);
			if (ret)
				goto done;
		}
		for (size_t i = 0; i < json_object_array_length(value); i++) {
			/* Refresh pointers after replacing an existing option. */
			free(name);
			name = NULL;
			if (asprintf(&name, "%s.%s", package->e.name, key) < 0) {
				ret = -1;
				break;
			}
			memset(&ptr, 0, sizeof(ptr));
			ret = uci_lookup_ptr(ctx, &ptr, name, true);
			if (ret)
				break;
			ptr.value = json_object_get_string(json_object_array_get_idx(value, i));
			if ((ret = uci_add_list(ctx, &ptr)))
				break;
		}
	} else {
		ptr.value = json_object_get_string(value);
		ret = uci_set(ctx, &ptr);
	}
done:
	free(name);
	return ret;
}

int cell_config_commit(J *values, J *deletes)
{
	if ((values && !json_object_is_type(values, json_type_object)) ||
	    (deletes && !json_object_is_type(deletes, json_type_array)))
		return errorf(CELL_INVALID, "Invalid configuration changes");
	if (deletes)
		for (size_t i = 0; i < json_object_array_length(deletes); i++) {
			J *key = json_object_array_get_idx(deletes, i);
			if (!json_object_is_type(key, json_type_string) || !config_scalar(key) ||
			    !config_key(json_object_get_string(key)))
				return errorf(CELL_INVALID, "Invalid configuration key");
		}
	if (values) {
		json_object_object_foreach(values, key, value)
		{
			if (!config_key(key))
				return errorf(CELL_INVALID, "Invalid configuration key");
			if (json_object_is_type(value, json_type_array)) {
				if (!strchr(key, '.'))
					return errorf(CELL_INVALID, "List requires an option");
				for (size_t i = 0; i < json_object_array_length(value); i++)
					if (!config_scalar(json_object_array_get_idx(value, i)))
						return errorf(CELL_INVALID, "Invalid list value");
			} else if (!config_scalar(value)) {
				return errorf(CELL_INVALID, "Invalid configuration value");
			}
		}
	}
	struct uci_package *package;
	struct uci_context *ctx = config_open(&package);
	if (!ctx)
		return -1;
	int ret = 0;
	if (deletes)
		for (size_t i = 0; i < json_object_array_length(deletes) && !ret; i++)
			ret = config_change(
			    ctx, package,
			    json_object_get_string(json_object_array_get_idx(deletes, i)), NULL);
	if (values && !ret) {
		json_object_object_foreach(values, key, value)
		{
			ret = config_change(ctx, package, key, value);
			if (ret)
				break;
		}
	}
	if (!ret)
		ret = uci_commit(ctx, &package, false);
	uci_free_context(ctx);
	if (ret)
		return errorf(CELL_SYSTEM, "Cellular configuration update failed");
	if (chmod(CELL_UCI_CONFIG, 0600))
		return errorf(CELL_SYSTEM, "Cellular configuration permission update failed");
	return 0;
}
