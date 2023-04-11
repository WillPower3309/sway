/*
	The original wlr_renderer was heavily referenced in making this project
	https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/render/gles2
*/

#include <assert.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/box.h>

#include "log.h"
#include "sway/desktop/fx_renderer/fx_renderer.h"
#include "sway/desktop/fx_renderer/matrix.h"
#include "sway/output.h"
#include "sway/server.h"

// shaders
#include "blur1_frag_src.h"
#include "blur2_frag_src.h"
#include "box_shadow_frag_src.h"
#include "common_vert_src.h"
#include "corner_frag_src.h"
#include "quad_frag_src.h"
#include "quad_round_frag_src.h"
#include "tex_frag_src.h"

static const GLfloat verts[] = {
	1, 0, // top right
	0, 0, // top left
	1, 1, // bottom right
	0, 1, // bottom left
};

static void create_stencil_buffer(struct wlr_output* output, GLuint *buffer_id) {
	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);

	if (*buffer_id == (uint32_t) -1) {
		glGenRenderbuffers(1, buffer_id);
		glBindRenderbuffer(GL_RENDERBUFFER, *buffer_id);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
				GL_RENDERBUFFER, *buffer_id);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			sway_log(SWAY_ERROR, "Stencilbuffer incomplete, couldn't create! (FB status: %i)", status);
			return;
		}
		sway_log(SWAY_DEBUG, "Stencilbuffer created, status %i", status);
	}
}

static void release_stencil_buffer(GLuint *buffer_id) {
	if (*buffer_id != (uint32_t)-1 && buffer_id) {
		glDeleteRenderbuffers(1, buffer_id);
	}
	*buffer_id = -1;
}

static GLuint compile_shader(GLuint type, const GLchar *src) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE) {
		sway_log(SWAY_ERROR, "Failed to compile shader");
		glDeleteShader(shader);
		shader = 0;
	}

	return shader;
}

static GLuint link_program(const GLchar *frag_src) {
	const GLchar *vert_src = common_vert_src;
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
		sway_log(SWAY_ERROR, "Failed to link shader");
		glDeleteProgram(prog);
		goto error;
	}

	return prog;

error:
	return 0;
}

static bool link_tex_program(struct fx_renderer *renderer,
		struct gles2_tex_shader *shader, enum fx_tex_shader_source source) {
	GLchar frag_src[2048];
	snprintf(frag_src, sizeof(frag_src),
		"#define SOURCE %d\n%s", source, tex_frag_src);

	GLuint prog;
	shader->program = prog = link_program(frag_src);
	if (!shader->program) {
		return false;
	}

	shader->proj = glGetUniformLocation(prog, "proj");
	shader->tex = glGetUniformLocation(prog, "tex");
	shader->alpha = glGetUniformLocation(prog, "alpha");
	shader->dim = glGetUniformLocation(prog, "dim");
	shader->dim_color = glGetUniformLocation(prog, "dim_color");
	shader->pos_attrib = glGetAttribLocation(prog, "pos");
	shader->tex_attrib = glGetAttribLocation(prog, "texcoord");
	shader->size = glGetUniformLocation(prog, "size");
	shader->position = glGetUniformLocation(prog, "position");
	shader->radius = glGetUniformLocation(prog, "radius");
	shader->saturation = glGetUniformLocation(prog, "saturation");
	shader->has_titlebar = glGetUniformLocation(prog, "has_titlebar");

	return true;
}

static bool link_rounded_quad_program(struct fx_renderer *renderer,
		struct rounded_quad_shader *shader, enum fx_rounded_quad_shader_source source) {
	GLchar quad_src[2048];
	snprintf(quad_src, sizeof(quad_src),
		"#define SOURCE %d\n%s", source, quad_round_frag_src);

	GLuint prog;
	shader->program = prog = link_program(quad_src);
	if (!shader->program) {
		return false;
	}

	shader->proj = glGetUniformLocation(prog, "proj");
	shader->color = glGetUniformLocation(prog, "color");
	shader->pos_attrib = glGetAttribLocation(prog, "pos");
	shader->size = glGetUniformLocation(prog, "size");
	shader->position = glGetUniformLocation(prog, "position");
	shader->radius = glGetUniformLocation(prog, "radius");

	return true;
}

static bool check_gl_ext(const char *exts, const char *ext) {
	size_t extlen = strlen(ext);
	const char *end = exts + strlen(exts);

	while (exts < end) {
		if (exts[0] == ' ') {
			exts++;
			continue;
		}
		size_t n = strcspn(exts, " ");
		if (n == extlen && strncmp(ext, exts, n) == 0) {
			return true;
		}
		exts += n;
	}
	return false;
}

static void load_gl_proc(void *proc_ptr, const char *name) {
	void *proc = (void *)eglGetProcAddress(name);
	if (proc == NULL) {
		sway_log(SWAY_ERROR, "GLES2 RENDERER: eglGetProcAddress(%s) failed", name);
		abort();
	}
	*(void **)proc_ptr = proc;
}

struct fx_renderer *fx_renderer_create(struct wlr_egl *egl) {
	struct fx_renderer *renderer = calloc(1, sizeof(struct fx_renderer));
	if (renderer == NULL) {
		return NULL;
	}

	// TODO: wlr_egl_make_current or eglMakeCurrent?
	// TODO: assert instead of conditional statement?
	if (!eglMakeCurrent(wlr_egl_get_display(egl), EGL_NO_SURFACE, EGL_NO_SURFACE,
			wlr_egl_get_context(egl))) {
		sway_log(SWAY_ERROR, "GLES2 RENDERER: Could not make EGL current");
		return NULL;
	}
	// TODO: needed?
	renderer->egl = egl;

	renderer->main_buffer.fb = -1;

	renderer->blur_buffer.fb = -1;
	renderer->effects_buffer.fb = -1;
	renderer->effects_buffer_swapped.fb = -1;
	renderer->stencil_buffer_id = -1;

	renderer->blur_buffer_dirty = true;
	renderer->blur_optimize_should_render = false;

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

	// TODO: the rest of the gl checks
	if (check_gl_ext(exts_str, "GL_OES_EGL_image_external")) {
		renderer->exts.OES_egl_image_external = true;
		load_gl_proc(&renderer->procs.glEGLImageTargetTexture2DOES,
			"glEGLImageTargetTexture2DOES");
	}

	// init shaders
	GLuint prog;

	// quad fragment shader
	prog = link_program(quad_frag_src);
	renderer->shaders.quad.program = prog;
	if (!renderer->shaders.quad.program) {
		goto error;
	}
	renderer->shaders.quad.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.quad.color = glGetUniformLocation(prog, "color");
	renderer->shaders.quad.pos_attrib = glGetAttribLocation(prog, "pos");

	// rounded quad fragment shaders
	if (!link_rounded_quad_program(renderer, &renderer->shaders.rounded_quad,
			SHADER_SOURCE_QUAD_ROUND)) {
		goto error;
	}
	if (!link_rounded_quad_program(renderer, &renderer->shaders.rounded_tl_quad,
			SHADER_SOURCE_QUAD_ROUND_TOP_LEFT)) {
		goto error;
	}
	if (!link_rounded_quad_program(renderer, &renderer->shaders.rounded_tr_quad,
			SHADER_SOURCE_QUAD_ROUND_TOP_RIGHT)) {
		goto error;
	}

	// Border corner shader
	prog = link_program(corner_frag_src);
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

	// box shadow shader
	prog = link_program(box_shadow_frag_src);
	renderer->shaders.box_shadow.program = prog;
	if (!renderer->shaders.box_shadow.program) {
		goto error;
	}
	renderer->shaders.box_shadow.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.box_shadow.color = glGetUniformLocation(prog, "color");
	renderer->shaders.box_shadow.pos_attrib = glGetAttribLocation(prog, "pos");
	renderer->shaders.box_shadow.position = glGetUniformLocation(prog, "position");
	renderer->shaders.box_shadow.size = glGetUniformLocation(prog, "size");
	renderer->shaders.box_shadow.blur_sigma = glGetUniformLocation(prog, "blur_sigma");
	renderer->shaders.box_shadow.corner_radius = glGetUniformLocation(prog, "corner_radius");

	// Blur 1
	prog = link_program(blur1_frag_src);
	renderer->shaders.blur1.program = prog;
	if (!renderer->shaders.blur1.program) {
		goto error;
	}
	renderer->shaders.blur1.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.blur1.tex = glGetUniformLocation(prog, "tex");
	renderer->shaders.blur1.pos_attrib = glGetAttribLocation(prog, "pos");
	renderer->shaders.blur1.tex_attrib = glGetAttribLocation(prog, "texcoord");
	renderer->shaders.blur1.radius = glGetUniformLocation(prog, "radius");
	renderer->shaders.blur1.halfpixel = glGetUniformLocation(prog, "halfpixel");

	// Blur 2
	prog = link_program(blur2_frag_src);
	renderer->shaders.blur2.program = prog;
	if (!renderer->shaders.blur2.program) {
		goto error;
	}
	renderer->shaders.blur2.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.blur2.tex = glGetUniformLocation(prog, "tex");
	renderer->shaders.blur2.pos_attrib = glGetAttribLocation(prog, "pos");
	renderer->shaders.blur2.tex_attrib = glGetAttribLocation(prog, "texcoord");
	renderer->shaders.blur2.radius = glGetUniformLocation(prog, "radius");
	renderer->shaders.blur2.halfpixel = glGetUniformLocation(prog, "halfpixel");

	// fragment shaders
	if (!link_tex_program(renderer, &renderer->shaders.tex_rgba,
			SHADER_SOURCE_TEXTURE_RGBA)) {
		goto error;
	}
	if (!link_tex_program(renderer, &renderer->shaders.tex_rgbx,
			SHADER_SOURCE_TEXTURE_RGBX)) {
		goto error;
	}
	if (!link_tex_program(renderer, &renderer->shaders.tex_ext,
			SHADER_SOURCE_TEXTURE_EXTERNAL)) {
		goto error;
	}


	if (!eglMakeCurrent(wlr_egl_get_display(renderer->egl),
				EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
		sway_log(SWAY_ERROR, "GLES2 RENDERER: Could not unset current EGL");
		goto error;
	}

	sway_log(SWAY_INFO, "GLES2 RENDERER: Shaders Initialized Successfully");
	return renderer;

error:
	glDeleteProgram(renderer->shaders.quad.program);
	glDeleteProgram(renderer->shaders.rounded_quad.program);
	glDeleteProgram(renderer->shaders.rounded_tl_quad.program);
	glDeleteProgram(renderer->shaders.rounded_tr_quad.program);
	glDeleteProgram(renderer->shaders.corner.program);
	glDeleteProgram(renderer->shaders.box_shadow.program);
	glDeleteProgram(renderer->shaders.blur1.program);
	glDeleteProgram(renderer->shaders.blur2.program);
	glDeleteProgram(renderer->shaders.tex_rgba.program);
	glDeleteProgram(renderer->shaders.tex_rgbx.program);
	glDeleteProgram(renderer->shaders.tex_ext.program);

	if (!eglMakeCurrent(wlr_egl_get_display(renderer->egl),
				EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
		sway_log(SWAY_ERROR, "GLES2 RENDERER: Could not unset current EGL");
	}

	// TODO: more freeing?
	free(renderer);

	sway_log(SWAY_ERROR, "GLES2 RENDERER: Error Initializing Shaders");
	return NULL;
}

void fx_renderer_fini(struct fx_renderer *renderer) {
	fx_framebuffer_release(&renderer->main_buffer);
	fx_framebuffer_release(&renderer->blur_buffer);
	fx_framebuffer_release(&renderer->effects_buffer);
	fx_framebuffer_release(&renderer->effects_buffer_swapped);
	release_stencil_buffer(&renderer->stencil_buffer_id);
}

void fx_renderer_begin(struct fx_renderer *renderer, struct sway_output *sway_output) {
	struct wlr_output *output = sway_output->wlr_output;

	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);

	renderer->sway_output = sway_output;
	// Store the wlr framebuffer
	GLint wlr_fb = -1;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &wlr_fb);
	if (wlr_fb < 0) {
		sway_log(SWAY_ERROR, "Failed to get wlr framebuffer!");
		abort();
	}
	renderer->wlr_buffer.fb = wlr_fb;

	// Create the main framebuffer
	fx_framebuffer_create(output, &renderer->main_buffer, true);
	// Create the stencil buffer and attach it to our main_buffer
	create_stencil_buffer(output, &renderer->stencil_buffer_id);

	// Create a new blur/effects framebuffers
	fx_framebuffer_create(output, &renderer->effects_buffer, false);
	fx_framebuffer_create(output, &renderer->effects_buffer_swapped, false);

	// refresh projection matrix
	matrix_projection(renderer->projection, width, height,
		WL_OUTPUT_TRANSFORM_FLIPPED_180);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	// Bind to our main framebuffer
	fx_framebuffer_bind(&renderer->main_buffer, width, height);
}

void fx_renderer_end(struct fx_renderer *renderer) {
	// Release the main buffer
	fx_framebuffer_release(&renderer->main_buffer);
	release_stencil_buffer(&renderer->stencil_buffer_id);
}

void fx_renderer_clear(const float color[static 4]) {
	glClearColor(color[0], color[1], color[2], color[3]);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void fx_renderer_scissor(struct wlr_box *box) {
	if (box) {
		glScissor(box->x, box->y, box->width, box->height);
		glEnable(GL_SCISSOR_TEST);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
}

bool fx_render_subtexture_with_matrix(struct fx_renderer *renderer, struct fx_texture *fx_texture,
		const struct wlr_fbox *src_box, const struct wlr_box *dst_box, const float matrix[static 9],
		struct decoration_data deco_data) {

	struct gles2_tex_shader *shader = NULL;

	switch (fx_texture->target) {
	case GL_TEXTURE_2D:
		if (fx_texture->has_alpha) {
			shader = &renderer->shaders.tex_rgba;
		} else {
			shader = &renderer->shaders.tex_rgbx;
		}
		break;
	case GL_TEXTURE_EXTERNAL_OES:
		shader = &renderer->shaders.tex_ext;

		if (!renderer->exts.OES_egl_image_external) {
			sway_log(SWAY_ERROR, "Failed to render texture: "
				"GL_TEXTURE_EXTERNAL_OES not supported");
			return false;
		}
		break;
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
	if (!fx_texture->has_alpha && deco_data.alpha == 1.0 && !deco_data.corner_radius) {
		glDisable(GL_BLEND);
	} else {
		glEnable(GL_BLEND);
	}

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(fx_texture->target, fx_texture->id);

	glTexParameteri(fx_texture->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	glUseProgram(shader->program);

	float* dim_color = deco_data.dim_color;

	glUniformMatrix3fv(shader->proj, 1, GL_FALSE, gl_matrix);
	glUniform1i(shader->tex, 0);
	glUniform2f(shader->size, dst_box->width, dst_box->height);
	glUniform2f(shader->position, dst_box->x, dst_box->y);
	glUniform1f(shader->alpha, deco_data.alpha);
	glUniform1f(shader->dim, deco_data.dim);
	glUniform4f(shader->dim_color, dim_color[0], dim_color[1], dim_color[2], dim_color[3]);
	glUniform1f(shader->has_titlebar, deco_data.has_titlebar);
	glUniform1f(shader->saturation, deco_data.saturation);
	glUniform1f(shader->radius, deco_data.corner_radius);

	const GLfloat x1 = src_box->x / fx_texture->width;
	const GLfloat y1 = src_box->y / fx_texture->height;
	const GLfloat x2 = (src_box->x + src_box->width) / fx_texture->width;
	const GLfloat y2 = (src_box->y + src_box->height) / fx_texture->height;
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

	glBindTexture(fx_texture->target, 0);

	return true;
}

bool fx_render_texture_with_matrix(struct fx_renderer *renderer, struct fx_texture *texture,
		const struct wlr_box *dst_box, const float matrix[static 9],
		struct decoration_data deco_data) {
	struct wlr_fbox src_box = {
		.x = 0,
		.y = 0,
		.width = texture->width,
		.height = texture->height,
	};
	return fx_render_subtexture_with_matrix(renderer, texture, &src_box,
			dst_box, matrix, deco_data);
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

void fx_render_rounded_rect(struct fx_renderer *renderer, const struct wlr_box *box,
		const float color[static 4], const float projection[static 9],
		int radius, enum corner_location corner_location) {
	if (box->width == 0 || box->height == 0) {
		return;
	}
	assert(box->width > 0 && box->height > 0);

	struct rounded_quad_shader *shader = NULL;

	switch (corner_location) {
		case ALL:
			shader = &renderer->shaders.rounded_quad;
			break;
		case TOP_LEFT:
			shader = &renderer->shaders.rounded_tl_quad;
			break;
		case TOP_RIGHT:
			shader = &renderer->shaders.rounded_tr_quad;
			break;
		default:
			sway_log(SWAY_ERROR, "Invalid Corner Location. Aborting render");
			abort();
	}

	float matrix[9];
	wlr_matrix_project_box(matrix, box, WL_OUTPUT_TRANSFORM_NORMAL, 0, projection);

	float gl_matrix[9];
	wlr_matrix_multiply(gl_matrix, renderer->projection, matrix);

	// TODO: investigate why matrix is flipped prior to this cmd
	// wlr_matrix_multiply(gl_matrix, flip_180, gl_matrix);

	wlr_matrix_transpose(gl_matrix, gl_matrix);

	glEnable(GL_BLEND);

	glUseProgram(shader->program);

	glUniformMatrix3fv(shader->proj, 1, GL_FALSE, gl_matrix);
	glUniform4f(shader->color, color[0], color[1], color[2], color[3]);

	// rounded corners
	glUniform2f(shader->size, box->width, box->height);
	glUniform2f(shader->position, box->x, box->y);
	glUniform1f(shader->radius, radius);

	glVertexAttribPointer(shader->pos_attrib, 2, GL_FLOAT, GL_FALSE,
			0, verts);

	glEnableVertexAttribArray(shader->pos_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(shader->pos_attrib);
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

// TODO: alpha input arg?
void fx_render_box_shadow(struct fx_renderer *renderer, const struct wlr_box *box,
		const float color[static 4], const float projection[static 9],
		int corner_radius, float blur_sigma) {
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

	// Init stencil work
	// NOTE: Alpha needs to be set to 1.0 to be able to discard any "empty" pixels
	const float col[4] = {0.0, 0.0, 0.0, 1.0};
	struct wlr_box inner_box;
	memcpy(&inner_box, box, sizeof(struct wlr_box));
	inner_box.x += blur_sigma;
	inner_box.y += blur_sigma;
	inner_box.width -= blur_sigma * 2;
	inner_box.height -= blur_sigma * 2;

	glEnable(GL_STENCIL_TEST);
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);

	glStencilFunc(GL_ALWAYS, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	// Disable writing to color buffer
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	// Draw the rounded rect as a mask
	fx_render_rounded_rect(renderer, &inner_box, col, projection, corner_radius, ALL);
	// Close the mask
	glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	// Reenable writing to color buffer
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	// blending will practically always be needed (unless we have a madman
	// who uses opaque shadows with zero sigma), so just enable it
	glEnable(GL_BLEND);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(renderer->shaders.box_shadow.program);

	glUniformMatrix3fv(renderer->shaders.box_shadow.proj, 1, GL_FALSE, gl_matrix);
	glUniform4f(renderer->shaders.box_shadow.color, color[0], color[1], color[2], color[3]);
	glUniform1f(renderer->shaders.box_shadow.blur_sigma, blur_sigma);
	glUniform1f(renderer->shaders.box_shadow.corner_radius, corner_radius);

	glUniform2f(renderer->shaders.box_shadow.size, box->width, box->height);
	glUniform2f(renderer->shaders.box_shadow.position, box->x, box->y);

	glVertexAttribPointer(renderer->shaders.box_shadow.pos_attrib, 2, GL_FLOAT, GL_FALSE,
			0, verts);

	glEnableVertexAttribArray(renderer->shaders.box_shadow.pos_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(renderer->shaders.box_shadow.pos_attrib);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	glDisable(GL_STENCIL_TEST);
}

void fx_render_blur(struct fx_renderer *renderer, struct sway_output *output,
		const float matrix[static 9], struct fx_framebuffer **buffer,
		struct blur_shader *shader, const struct wlr_box *box, int blur_radius) {
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);

	int width, height;
	wlr_output_transformed_resolution(output->wlr_output, &width, &height);

	glActiveTexture(GL_TEXTURE0);

	glBindTexture((*buffer)->texture.target, (*buffer)->texture.id);

	glTexParameteri((*buffer)->texture.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	glUseProgram(shader->program);

	// OpenGL ES 2 requires the glUniformMatrix3fv transpose parameter to be set
	// to GL_FALSE
	float gl_matrix[9];
	wlr_matrix_transpose(gl_matrix, matrix);
	glUniformMatrix3fv(shader->proj, 1, GL_FALSE, gl_matrix);

	glUniform1i(shader->tex, 0);
	glUniform1f(shader->radius, blur_radius);

	if (shader == &renderer->shaders.blur1) {
		glUniform2f(shader->halfpixel,
				0.5f / (width / 2.0f),
				0.5f / (height / 2.0f));
	} else {
		glUniform2f(shader->halfpixel,
				0.5f / (width * 2.0f),
				0.5f / (height * 2.0f));
	}

	glVertexAttribPointer(shader->pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(shader->tex_attrib, 2, GL_FLOAT, GL_FALSE, 0, verts);

	glEnableVertexAttribArray(shader->pos_attrib);
	glEnableVertexAttribArray(shader->tex_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(shader->pos_attrib);
	glDisableVertexAttribArray(shader->tex_attrib);
}
