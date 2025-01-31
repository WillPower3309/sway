#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"

bool cmd_corner_radius_parse_value(char *arg, int* result) {
	char *inv;
	int value = strtol(arg, &inv, 10);
	if (*inv != '\0' || value < 0 || value > 99) {
		return false;
	}
	*result = value;
	return true;
}

static void arrange_corner_radius_iter(struct sway_container *con, void *data) {
	con->corner_radius = config->corner_radius;
}

// TODO: handle setting per container
struct cmd_results *cmd_corner_radius(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "corner_radius", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}

	int value = 0;
	if (!cmd_corner_radius_parse_value(argv[0], &value)) {
		return cmd_results_new(CMD_FAILURE, "Invalid size specified");
	}

	config->corner_radius = value;

	if (!config->handler_context.container) {
		// Config reload: reset all containers to config value
		root_for_each_container(arrange_corner_radius_iter, NULL);
		arrange_root();
	}

	/*
	 titlebar padding depends on corner_radius to
	 ensure that titlebars are rendered nicely
	*/
	if (value > config->titlebar_h_padding) {
		config->titlebar_h_padding = value;
	}
	if (value > (int)container_titlebar_height()) {
		config->titlebar_v_padding = (value - config->font_height) / 2;
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
