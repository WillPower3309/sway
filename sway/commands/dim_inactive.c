#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "log.h"
#include "sway/output.h"

static void rebuild_textures_iterator(struct sway_container *con, void *data) {
	container_update_marks_textures(con);
	container_update_title_textures(con);
}

struct cmd_results *cmd_dim_inactive(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "dim_inactive", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}

	char *err;
	float val = strtof(argv[0], &err);
	if (*err || val < 0.0f || val > 1.0f) {
		return cmd_results_new(CMD_INVALID, "dim_inactive float invalid");
	}

	config->dim_inactive = val;

	if (config->active) {
		root_for_each_container(rebuild_textures_iterator, NULL);

		for (int i = 0; i < root->outputs->length; ++i) {
			struct sway_output *output = root->outputs->items[i];
			output_damage_whole(output);
		}
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
