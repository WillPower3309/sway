#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/tree/arrange.h"
#include "sway/tree/view.h"
#include "sway/tree/container.h"
#include "util.h"

static void arrange_shadow_iter(struct sway_container *con, void *data) {
	con->shadow_enabled = config->shadow_enabled;
}

struct cmd_results *cmd_shadows(int argc, char **argv) {
	struct cmd_results *error = checkarg(argc, "shadows", EXPECTED_AT_LEAST, 1);

	if (error) {
		return error;
	}

	struct sway_container *con = config->handler_context.container;

	bool result = parse_boolean(argv[0], true);
	if (con == NULL) {
		config->shadow_enabled = result;
		// Config reload: reset all containers to config value
		root_for_each_container(arrange_shadow_iter, NULL);
	} else {
		con->shadow_enabled = result;
	}

	arrange_root();

	return cmd_results_new(CMD_SUCCESS, NULL);
}
