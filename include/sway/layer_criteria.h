#include <stdbool.h>

struct layer_criteria {
	char *namespace;
	char *cmdlist;

	bool shadow_enabled;
	bool blur_enabled;
	bool blur_xray;
	bool blur_ignore_transparent;
	int corner_radius;
};

void layer_criteria_destroy(struct layer_criteria *criteria);

struct layer_criteria *layer_criteria_add(char *namespace, char *cmdlist);

// Get the matching criteria for a specified `sway_layer_surface`
struct layer_criteria *layer_criteria_for_namespace(char *namespace);
