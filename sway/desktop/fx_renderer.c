// The original wlr_renderer was heavily referenced in making this project
// https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/render/gles2

// TODO: add push / pop_gles2_debug(renderer)?

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/box.h>
#include "log.h"
#include "sway/desktop/fx_renderer.h"
#include "sway/output.h"
#include "sway/server.h"

// shaders
#include "quad_vert_src.h"
#include "quad_frag_src.h"
#include "corner_frag_src.h"
#include "tex_vert_src.h"
#include "tex_rgba_frag_src.h"
#include "tex_rgbx_frag_src.h"
#include "tex_external_frag_src.h"

static const GLfloat verts[] = {
	1, 0, // top right
	0, 0, // top left
	1, 1, // bottom right
	0, 1, // bottom left
};

static GLuint compile_shader(GLuint type, const GLchar *src) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE) {
		glDeleteShader(shader);
		shader = 0;
	}

	return shader;
}

static GLuint link_program(const GLchar *vert_src, const GLchar *frag_src) {
	GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
	if (!vert) {
		goto error;
	}

	GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!frag) {
		glDeleteShader(vert);
		goto error;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);
	glLinkProgram(prog);

	glDetachShader(prog, vert);
	glDetachShader(prog, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (ok == GL_FALSE) {
		glDeleteProgram(prog);
		goto error;
	}

	return prog;

error:
	return 0;
}

// initializes a provided fragment shader and returns false if unsuccessful
bool init_frag_shader(struct gles2_tex_shader *shader, GLuint prog) {
	shader->program = prog;
	if (!shader->program) {
		return false;
	}
	shader->proj = glGetUniformLocation(prog, "proj");
	shader->tex = glGetUniformLocation(prog, "tex");
	shader->alpha = glGetUniformLocation(prog, "alpha");
	shader->pos_attrib = glGetAttribLocation(prog, "pos");
	shader->tex_attrib = glGetAttribLocation(prog, "texcoord");
	shader->size = glGetUniformLocation(prog, "size");
	shader->position = glGetUniformLocation(prog, "position");
	shader->radius = glGetUniformLocation(prog, "radius");
	shader->titlebar_color = glGetUniformLocation(prog, "titlebar_color");
	return true;
}

struct fx_renderer *fx_renderer_create(struct wlr_egl *egl) {
	struct fx_renderer *renderer = calloc(1, sizeof(struct fx_renderer));
	if (renderer == NULL) {
		return NULL;
	}

	// TODO: wlr_egl_make_current or eglMakeCurrent?
	// TODO: assert instead of conditional statement?
	if (!wlr_egl_make_current(egl)) {
		sway_log(SWAY_ERROR, "GLES2 RENDERER: Could not make EGL current");
		return NULL;
	}
	// TODO: needed?
	renderer->egl = egl;

	// get extensions
	const char *exts_str = (const char *)glGetString(GL_EXTENSIONS);
	if (exts_str == NULL) {
		sway_log(SWAY_ERROR, "GLES2 RENDERER: Failed to get GL_EXTENSIONS");
		return NULL;
	}

	sway_log(SWAY_INFO, "Creating swayfx GLES2 renderer");
	sway_log(SWAY_INFO, "Using %s", glGetString(GL_VERSION));
	sway_log(SWAY_INFO, "GL vendor: %s", glGetString(GL_VENDOR));
	sway_log(SWAY_INFO, "GL renderer: %s", glGetString(GL_RENDERER));
	sway_log(SWAY_INFO, "Supported GLES2 extensions: %s", exts_str);

	// TODO: gl checks

	// init shaders
	GLuint prog;

	prog = link_program(quad_vert_src, quad_frag_src);
	renderer->shaders.quad.program = prog;
	if (!renderer->shaders.quad.program) {
		goto error;
	}
	renderer->shaders.quad.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.quad.color = glGetUniformLocation(prog, "color");
	renderer->shaders.quad.pos_attrib = glGetAttribLocation(prog, "pos");

	// Border corners
	prog = link_program(quad_vert_src, corner_frag_src);
	renderer->shaders.corner.program = prog;
	if (!renderer->shaders.corner.program) {
		goto error;
	}
	renderer->shaders.corner.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.corner.color = glGetUniformLocation(prog, "color");
	renderer->shaders.corner.pos_attrib = glGetAttribLocation(prog, "pos");
	renderer->shaders.corner.is_top_left = glGetUniformLocation(prog, "is_top_left");
	renderer->shaders.corner.is_top_right = glGetUniformLocation(prog, "is_top_right");
	renderer->shaders.corner.is_bottom_left = glGetUniformLocation(prog, "is_bottom_left");
	renderer->shaders.corner.is_bottom_right = glGetUniformLocation(prog, "is_bottom_right");
	renderer->shaders.corner.position = glGetUniformLocation(prog, "position");
	renderer->shaders.corner.radius = glGetUniformLocation(prog, "radius");
	renderer->shaders.corner.half_size = glGetUniformLocation(prog, "half_size");
	renderer->shaders.corner.half_thickness = glGetUniformLocation(prog, "half_thickness");

	// fragment shaders
	prog = link_program(tex_vert_src, tex_rgba_frag_src);
	if (!init_frag_shader(&renderer->shaders.tex_rgba, prog)) {
		goto error;
	}
	prog = link_program(tex_vert_src, tex_rgbx_frag_src);
	if (!init_frag_shader(&renderer->shaders.tex_rgbx, prog)) {
		goto error;
	}
	prog = link_program(tex_vert_src, tex_external_frag_src);
	if (!init_frag_shader(&renderer->shaders.tex_ext, prog)) {
		goto error;
	}

	wlr_egl_unset_current(renderer->egl);

	sway_log(SWAY_INFO, "GLES2 RENDERER: Shaders Initialized Successfully");
	return renderer;

error:
	glDeleteProgram(renderer->shaders.quad.program);
	glDeleteProgram(renderer->shaders.corner.program);
	glDeleteProgram(renderer->shaders.tex_rgba.program);
	glDeleteProgram(renderer->shaders.tex_rgbx.program);
	glDeleteProgram(renderer->shaders.tex_ext.program);

	wlr_egl_unset_current(renderer->egl);

	// TODO: more freeing?
	free(renderer);

	sway_log(SWAY_ERROR, "GLES2 RENDERER: Error Initializing Shaders");
	return NULL;
}

void fx_renderer_begin(struct fx_renderer *renderer, uint32_t width, uint32_t height) {
	glViewport(0, 0, width, height);

	// refresh projection matrix
	wlr_matrix_projection(renderer->projection, width, height,
		WL_OUTPUT_TRANSFORM_FLIPPED_180);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void fx_renderer_end() {
	// TODO
}

void fx_renderer_clear(const float color[static 4]) {
		glClearColor(color[0], color[1], color[2], color[3]);
		glClear(GL_COLOR_BUFFER_BIT);
}

void fx_renderer_scissor(struct wlr_box *box) {
		if (box) {
				glScissor(box->x, box->y, box->width, box->height);
				glEnable(GL_SCISSOR_TEST);
		} else {
				glDisable(GL_SCISSOR_TEST);
		}
}

bool fx_render_subtexture_with_matrix(struct fx_renderer *renderer, struct wlr_texture *wlr_texture,
		const struct wlr_fbox *src_box, const struct wlr_box *dst_box, const float matrix[static 9],
		float alpha, int radius, const float titlebar_color[static 4]) {

	assert(wlr_texture_is_gles2(wlr_texture));
	struct wlr_gles2_texture_attribs texture_attrs;
	wlr_gles2_texture_get_attribs(wlr_texture, &texture_attrs);

	struct gles2_tex_shader *shader = NULL;

	switch (texture_attrs.target) {
	case GL_TEXTURE_2D:
		if (texture_attrs.has_alpha) {
			shader = &renderer->shaders.tex_rgba;
		} else {
			shader = &renderer->shaders.tex_rgbx;
		}
		break;
	/*
	case GL_TEXTURE_EXTERNAL_OES:
		shader = &renderer->shaders.tex_ext;

		// TODO: ADD ME ONCE EXTS ADDED TO RENDERER
		if (!renderer->exts.OES_egl_image_external) {
			sway_log(SWAY_ERROR, "Failed to render texture: "
				"GL_TEXTURE_EXTERNAL_OES not supported");
			return false;
		}
		break;
		*/
	default:
		sway_log(SWAY_ERROR, "Aborting render");
		abort();
	}

	float gl_matrix[9];
	wlr_matrix_multiply(gl_matrix, renderer->projection, matrix);

	// OpenGL ES 2 requires the glUniformMatrix3fv transpose parameter to be set
	// to GL_FALSE
	wlr_matrix_transpose(gl_matrix, gl_matrix);

	// if there's no opacity or rounded corners we don't need to blend
	if (!texture_attrs.has_alpha && alpha == 1.0 && !radius && titlebar_color[3] == 1.0) {
		glDisable(GL_BLEND);
	} else {
		glEnable(GL_BLEND);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(texture_attrs.target, texture_attrs.tex);

	glTexParameteri(texture_attrs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	glUseProgram(shader->program);

	glUniformMatrix3fv(shader->proj, 1, GL_FALSE, gl_matrix);
	glUniform1i(shader->tex, 0);
	glUniform1f(shader->alpha, alpha);
	glUniform4f(shader->titlebar_color, titlebar_color[0], titlebar_color[1], titlebar_color[2], titlebar_color[3]);

	// rounded corners
	glUniform2f(shader->size, dst_box->width, dst_box->height);
	glUniform2f(shader->position, dst_box->x, dst_box->y);
	glUniform1f(shader->radius, radius);

	const GLfloat x1 = src_box->x / wlr_texture->width;
	const GLfloat y1 = src_box->y / wlr_texture->height;
	const GLfloat x2 = (src_box->x + src_box->width) / wlr_texture->width;
	const GLfloat y2 = (src_box->y + src_box->height) / wlr_texture->height;
	const GLfloat texcoord[] = {
		x2, y1, // top right
		x1, y1, // top left
		x2, y2, // bottom right
		x1, y2, // bottom left
	};

	glVertexAttribPointer(shader->pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(shader->tex_attrib, 2, GL_FLOAT, GL_FALSE, 0, texcoord);

	glEnableVertexAttribArray(shader->pos_attrib);
	glEnableVertexAttribArray(shader->tex_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(shader->pos_attrib);
	glDisableVertexAttribArray(shader->tex_attrib);

	glBindTexture(texture_attrs.target, 0);

	return true;
}

bool fx_render_texture_with_matrix(struct fx_renderer *renderer, struct wlr_texture *wlr_texture,
		const struct wlr_box *dst_box, const float matrix[static 9], float alpha, int radius, const float titlebar_color[static 4]) {
	struct wlr_fbox src_box = {
		.x = 0,
		.y = 0,
		.width = wlr_texture->width,
		.height = wlr_texture->height,
	};
	return fx_render_subtexture_with_matrix(renderer, wlr_texture, &src_box, dst_box, matrix, alpha, radius, titlebar_color);
}

void fx_render_rect(struct fx_renderer *renderer, const struct wlr_box *box,
		const float color[static 4], const float projection[static 9]) {
	if (box->width == 0 || box->height == 0) {
		return;
	}
	assert(box->width > 0 && box->height > 0);
	float matrix[9];
	wlr_matrix_project_box(matrix, box, WL_OUTPUT_TRANSFORM_NORMAL, 0, projection);

	float gl_matrix[9];
	wlr_matrix_multiply(gl_matrix, renderer->projection, matrix);

	// TODO: investigate why matrix is flipped prior to this cmd
	// wlr_matrix_multiply(gl_matrix, flip_180, gl_matrix);

	wlr_matrix_transpose(gl_matrix, gl_matrix);

	if (color[3] == 1.0) {
		glDisable(GL_BLEND);
	} else {
		glEnable(GL_BLEND);
	}

	glUseProgram(renderer->shaders.quad.program);

	glUniformMatrix3fv(renderer->shaders.quad.proj, 1, GL_FALSE, gl_matrix);
	glUniform4f(renderer->shaders.quad.color, color[0], color[1], color[2], color[3]);

	glVertexAttribPointer(renderer->shaders.quad.pos_attrib, 2, GL_FLOAT, GL_FALSE,
			0, verts);

	glEnableVertexAttribArray(renderer->shaders.quad.pos_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(renderer->shaders.quad.pos_attrib);
}

void fx_render_border_corner(struct fx_renderer *renderer, const struct wlr_box *box,
		const float color[static 4], const float projection[static 9],
		enum corner_location corner_location, int radius, int border_thickness) {
	if (border_thickness == 0 || box->width == 0 || box->height == 0) {
		return;
	}
	assert(box->width > 0 && box->height > 0);
	float matrix[9];
	wlr_matrix_project_box(matrix, box, WL_OUTPUT_TRANSFORM_NORMAL, 0, projection);

	float gl_matrix[9];
	wlr_matrix_multiply(gl_matrix, renderer->projection, matrix);

	// TODO: investigate why matrix is flipped prior to this cmd
	// wlr_matrix_multiply(gl_matrix, flip_180, gl_matrix);

	wlr_matrix_transpose(gl_matrix, gl_matrix);

	if (color[3] == 1.0 && !radius) {
		glDisable(GL_BLEND);
	} else {
		glEnable(GL_BLEND);
	}

	glUseProgram(renderer->shaders.corner.program);

	glUniformMatrix3fv(renderer->shaders.corner.proj, 1, GL_FALSE, gl_matrix);
	glUniform4f(renderer->shaders.corner.color, color[0], color[1], color[2], color[3]);

	glUniform1f(renderer->shaders.corner.is_top_left, corner_location == TOP_LEFT);
	glUniform1f(renderer->shaders.corner.is_top_right, corner_location == TOP_RIGHT);
	glUniform1f(renderer->shaders.corner.is_bottom_left, corner_location == BOTTOM_LEFT);
	glUniform1f(renderer->shaders.corner.is_bottom_right, corner_location == BOTTOM_RIGHT);

	glUniform2f(renderer->shaders.corner.position, box->x, box->y);
	glUniform1f(renderer->shaders.corner.radius, radius);
	glUniform2f(renderer->shaders.corner.half_size, box->width / 2.0, box->height / 2.0);
	glUniform1f(renderer->shaders.corner.half_thickness, border_thickness / 2.0);

	glVertexAttribPointer(renderer->shaders.corner.pos_attrib, 2, GL_FLOAT, GL_FALSE,
			0, verts);

	glEnableVertexAttribArray(renderer->shaders.corner.pos_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(renderer->shaders.corner.pos_attrib);
}
