#include <ctype.h>
#include <string.h>
#include "list.h"
#include "log.h"
#include "stringop.h"
#include "sway/commands.h"
#include "sway/layer_criteria.h"
#include "util.h"

static void init_criteria_effects(struct layer_criteria *criteria) {
	criteria->corner_radius = 0;
	criteria->blur_enabled = false;
	criteria->blur_xray = false;
	criteria->blur_ignore_transparent = false;
	criteria->shadow_enabled = false;
}

static void copy_criteria_effects(struct layer_criteria *dst, struct layer_criteria *src) {
	dst->corner_radius = src->corner_radius;
	dst->blur_enabled = src->blur_enabled;
	dst->blur_xray = src->blur_xray;
	dst->blur_ignore_transparent = src->blur_ignore_transparent;
	dst->shadow_enabled = src->shadow_enabled;
}

static bool layer_criteria_find(char *namespace,
		struct layer_criteria **criteria, int *index) {
	*index = -1;
	*criteria = NULL;

	list_t *criterias = config->layer_criteria;
	for (int i = 0; i < criterias->length; ++i) {
		struct layer_criteria *existing = criterias->items[i];
		if (strcmp(namespace, existing->namespace) == 0) {
			*index = i;
			*criteria = existing;
			return true;
		}
	}
	return false;
}

static bool layer_criteria_parse(struct layer_criteria *criteria) {
	char matched_delim = ';';
	char *head = malloc(strlen(criteria->cmdlist) + 1);
	strcpy(head, criteria->cmdlist);
	do {
		// Trim leading whitespaces
		for (; isspace(*head); ++head) {}
		// Split command list
		char *cmd = argsep(&head, ";,", &matched_delim);
		for (; isspace(*cmd); ++cmd) {}

		if (strcmp(cmd, "") == 0) {
			sway_log(SWAY_INFO, "Ignoring empty layer effect.");
			continue;
		}
		sway_log(SWAY_INFO, "Handling layer effect '%s'", cmd);

		int argc;
		char **argv = split_args(cmd, &argc);
		// Strip all quotes from each token
		for (int i = 1; i < argc; ++i) {
			if (*argv[i] == '\"' || *argv[i] == '\'') {
				strip_quotes(argv[i]);
			}
		}
		if (strcmp(argv[0], "reset") == 0) {
			// Reset all args
			init_criteria_effects(criteria);
			return true;
		} else if (strcmp(argv[0], "blur") == 0) {
			criteria->blur_enabled = parse_boolean(argv[1], true);
			continue;
		} if (strcmp(argv[0], "blur_xray") == 0) {
			criteria->blur_xray = parse_boolean(argv[1], true);
			continue;
		} if (strcmp(argv[0], "blur_ignore_transparent") == 0) {
			criteria->blur_ignore_transparent = parse_boolean(argv[1], true);
			continue;
		} else if (strcmp(argv[0], "shadows") == 0) {
			criteria->shadow_enabled = parse_boolean(argv[1], true);
			continue;
		} else if (strcmp(argv[0], "corner_radius") == 0) {
			int value;
			if (cmd_corner_radius_parse_value(argv[1], &value)) {
				criteria->corner_radius = value;
				continue;
			}
			sway_log(SWAY_ERROR,
					"Invalid layer_effects corner_radius size! Got \"%s\"",
					argv[1]);
			return false;
		} else {
			sway_log(SWAY_ERROR, "Invalid layer_effects effect! Got \"%s\"", cmd);
			return false;
		}
	} while(head);

	return true;
}

struct layer_criteria *layer_criteria_add(char *namespace, char *cmdlist) {
	struct layer_criteria *criteria = calloc(1, sizeof(*criteria));
	init_criteria_effects(criteria);

	// Check if the rule already exists
	int existing_index;
	struct layer_criteria *existing = NULL;
	if (layer_criteria_find(namespace, &existing, &existing_index)) {
		sway_log(SWAY_DEBUG, "layer_effect '%s' already exists: '%s'. Replacing with: '%s'",
				namespace, existing->cmdlist, cmdlist);
		// Clone the effects into the new criteria
		copy_criteria_effects(criteria, existing);
		layer_criteria_destroy(existing);
		list_del(config->layer_criteria, existing_index);
	}

	// Create a new criteria
	if (!criteria) {
		sway_log(SWAY_ERROR, "Could not allocate new layer_criteria");
		return NULL;
	}
	criteria->namespace = malloc(strlen(namespace) + 1);
	strcpy(criteria->namespace, namespace);
	criteria->cmdlist = cmdlist;

	// Parsing
	if (!layer_criteria_parse(criteria)) {
		// Failed to parse
		layer_criteria_destroy(criteria);
		return NULL;
	}

	list_add(config->layer_criteria, criteria);

	return criteria;
}

struct layer_criteria *layer_criteria_for_namespace(char *namespace) {
	list_t *criterias = config->layer_criteria;
	for (int i = criterias->length - 1; i >= 0; i--) {
		struct layer_criteria *criteria = criterias->items[i];
		if (strcmp(criteria->namespace, namespace) == 0) {
			return criteria;
		}
	}
	return NULL;
}

void layer_criteria_destroy(struct layer_criteria *criteria) {
	free(criteria->namespace);
	free(criteria->cmdlist);
	free(criteria);
}
