#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "util.h"

struct cmd_results *cmd_scratchpad_minimize(int argc, char **argv) {
	struct cmd_results *error = checkarg(argc, "scratchpad_minimize", EXPECTED_AT_LEAST, 1);

	if (error) {
		return error;
	}

	config->scratchpad_minimize = parse_boolean(argv[0], config->scratchpad_minimize);

	return cmd_results_new(CMD_SUCCESS, NULL);
}
