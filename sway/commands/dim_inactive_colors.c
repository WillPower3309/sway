#include "sway/commands.h"
#include "sway/config.h"
#include "sway/tree/arrange.h"
#include "util.h"

static struct cmd_results *handle_command(int argc, char **argv, char *cmd_name,
		float config_option[4]) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, cmd_name, EXPECTED_AT_LEAST, 1))) {
		return error;
	}

	uint32_t color;
	if (!parse_color(argv[0], &color)) {
		return cmd_results_new(CMD_INVALID, "Invalid %s color %s",
				cmd_name, argv[0]);
	}
	color_to_rgba(config_option, color);

	if (config->active) {
		arrange_root();
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_dim_inactive_colors_unfocused(int argc, char **argv) {
	return handle_command(argc, argv, "dim_inactive_colors.unfocused",
			config->dim_inactive_colors.unfocused);
}

struct cmd_results *cmd_dim_inactive_colors_urgent(int argc, char **argv) {
	return handle_command(argc, argv, "dim_inactive_colors.urgent",
			config->dim_inactive_colors.urgent);
}
