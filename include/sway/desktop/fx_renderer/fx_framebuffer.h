#ifndef FX_FRAMEBUFFER_H
#define FX_FRAMEBUFFER_H

#include <GLES2/gl2.h>
#include <stdbool.h>
#include <wlr/types/wlr_output.h>

#include "sway/desktop/fx_renderer/fx_texture.h"

struct fx_framebuffer {
	struct fx_texture texture;
	GLuint fb;
};

void fx_framebuffer_bind(struct fx_framebuffer *buffer, GLsizei width, GLsizei height);

void fx_framebuffer_create(struct wlr_output *output, struct fx_framebuffer *buffer,
		bool bind);

void fx_framebuffer_release(struct fx_framebuffer *buffer);


#endif
