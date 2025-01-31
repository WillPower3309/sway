#include "scenefx/render/fx_renderer/fx_effect_framebuffers.h"
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/output.h"

struct cmd_results *cmd_blur_noise(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "blur_noise", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}

	char *inv;
	float value = strtof(argv[0], &inv);
	if (*inv != '\0' || value < 0 || value > 1) {
		return cmd_results_new(CMD_FAILURE, "Invalid noise specified");
	}

	struct wlr_scene *root_scene = root->root_scene;
	struct blur_data blur_data = root_scene->blur_data;
	blur_data.noise = value;
	wlr_scene_set_blur_data(root_scene, blur_data);

	return cmd_results_new(CMD_SUCCESS, NULL);
}
