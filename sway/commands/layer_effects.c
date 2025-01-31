#include <string.h>
#include "log.h"
#include "stringop.h"
#include "sway/commands.h"
#include "sway/layers.h"
#include "sway/output.h"
#include "sway/scene_descriptor.h"
#include "sway/tree/arrange.h"

struct cmd_results *cmd_layer_effects(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "layer_effects", EXPECTED_AT_LEAST, 2))) {
		return error;
	}

	struct layer_criteria *criteria = layer_criteria_add(argv[0], join_args(argv + 1, argc - 1));
	if (criteria) {
		sway_log(SWAY_DEBUG, "layer_effect: '%s' '%s' added",
				criteria->namespace, criteria->cmdlist);

		// Apply the criteria to all applicable layer surfaces
		for (int i = 0; i < root->outputs->length; ++i) {
			struct sway_output *output = root->outputs->items[i];
			struct wlr_scene_tree *layers[] = {
				output->layers.shell_background,
				output->layers.shell_bottom,
				output->layers.shell_overlay,
				output->layers.shell_top,
			};
			size_t nlayers = sizeof(layers) / sizeof(layers[0]);
			struct wlr_scene_node *node;
			for (size_t i = 0; i < nlayers; ++i) {
				wl_list_for_each_reverse(node, &layers[i]->children, link) {
					struct sway_layer_surface *surface = scene_descriptor_try_get(node,
						SWAY_SCENE_DESC_LAYER_SHELL);
					if (!surface) {
						continue;
					}
					if (strcmp(surface->layer_surface->namespace, criteria->namespace) == 0) {
						layer_apply_criteria(surface, criteria);
					}
				}
			}

			arrange_layers(output);
		}
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
