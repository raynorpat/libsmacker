/*
 * libsmacker - A C library for decoding .smk Smacker Video files
 * Copyright (C) 2012-2020 Greg Kennedy
 *
 * See smacker.h for more information.
 *
 * smk_gl_player.c - OpenGL 3.3 playback example
 *
 * The frame stays palettized: an 8-bit index texture and a 256x1 palette
 * texture go to the GPU, and the shaders resolve the palette and scale the
 * image. See smk_sdl_player.c for the CPU-side reference player.
 *
 * Build with:
 *   cmake -S . -B build -DSMACKER_BUILD_TOOLS=ON
 *   cmake --build build
 *
 * Usage:
 *   smk_gl_player [--frames N] file.smk
 *
 * Keys: Esc/q quit. --frames 0 (the default) plays to the end.
 */

#include "smacker.h"

#ifdef SMK_USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#else
#include <SDL.h>
#include <SDL_opengl.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Event names and key access moved in SDL3. */
#ifdef SMK_USE_SDL3
#define SMK_EVENT_QUIT		SDL_EVENT_QUIT
#define SMK_EVENT_KEY_DOWN	SDL_EVENT_KEY_DOWN
#define SMK_EVENT_KEY(event)	((event).key.key)
#define SMK_KEY_Q		SDLK_Q
#define SMK_GL_DESTROY_CONTEXT	SDL_GL_DestroyContext
#define SMK_GL_SET_ATTR(attr, value) \
	(!SDL_GL_SetAttribute((attr), (value)))
typedef SDL_AudioStream *smk_audio_out;
#else
#define SMK_EVENT_QUIT		SDL_QUIT
#define SMK_EVENT_KEY_DOWN	SDL_KEYDOWN
#define SMK_EVENT_KEY(event)	((event).key.keysym.sym)
#define SMK_KEY_Q		SDLK_q
#define SMK_GL_DESTROY_CONTEXT	SDL_GL_DeleteContext
#define SMK_GL_SET_ATTR(attr, value) \
	(SDL_GL_SetAttribute((attr), (value)) != 0)
typedef SDL_AudioDeviceID smk_audio_out;
#endif

struct options {
	const char     *path;
	unsigned long	frame_limit;	/* 0 means play to the end */
};

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [--frames N] file.smk\n",
		prog != NULL ? prog : "smk_gl_player");
}

/* Parse the command line. Returns 0 on success, 1 after printing help, and
 * -1 after printing an error. */
static int
parse_options(int argc, char *argv[], struct options *out)
{
	int		i;

	out->path = NULL;
	out->frame_limit = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 ||
			strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 1;
		}

		if (strcmp(argv[i], "--frames") == 0) {
			char           *end = NULL;
			unsigned long	v;

			if (i + 1 >= argc) {
				fputs("--frames needs a value\n", stderr);
				usage(argv[0]);
				return -1;
			}
			if (argv[i + 1][0] == '-') {
				fprintf(stderr, "invalid --frames value: %s\n",
					argv[i + 1]);
				usage(argv[0]);
				return -1;
			}
			errno = 0;
			v = strtoul(argv[++i], &end, 10);
			if (errno != 0 || end == argv[i] || *end != '\0') {
				fprintf(stderr, "invalid --frames value: %s\n",
					argv[i]);
				usage(argv[0]);
				return -1;
			}
			out->frame_limit = v;
			continue;
		}

		if (argv[i][0] == '-') {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return -1;
		}

		if (out->path != NULL) {
			fputs("only one input file may be given\n", stderr);
			usage(argv[0]);
			return -1;
		}
		out->path = argv[i];
	}

	if (out->path == NULL) {
		usage(argv[0]);
		return -1;
	}

	return 0;
}

/* Initialize the video subsystem. Returns 0 on success. */
static int
video_init(void)
{
#ifdef SMK_USE_SDL3
	return SDL_Init(SDL_INIT_VIDEO) ? 0 : -1;
#else
	return SDL_Init(SDL_INIT_VIDEO) == 0 ? 0 : -1;
#endif
}

/* Initialize the audio subsystem. Returns 0 on success. */
static int
audio_init(void)
{
#ifdef SMK_USE_SDL3
	return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 0 : -1;
#else
	return SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 ? 0 : -1;
#endif
}

/* Open an audio output device matching the PCM format libsmacker produces:
 * unsigned 8-bit, or little-endian signed 16-bit. Returns 0 on success. */
static int
audio_open(smk_audio_out *out, unsigned long rate, int channels, int bitdepth)
{
#ifdef SMK_USE_SDL3
	SDL_AudioSpec	spec;

	SDL_zero(spec);
	spec.format = (bitdepth == 16) ? SDL_AUDIO_S16LE : SDL_AUDIO_U8;
	spec.channels = channels;
	spec.freq = (int) rate;

	*out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		&spec, NULL, NULL);
	if (*out == NULL)
		return -1;

	SDL_ResumeAudioStreamDevice(*out);
	return 0;
#else
	SDL_AudioSpec	want;

	SDL_zero(want);
	want.freq = (int) rate;
	want.format = (bitdepth == 16) ? AUDIO_S16LSB : AUDIO_U8;
	want.channels = (Uint8) channels;
	want.samples = 1024;
	want.callback = NULL;

	*out = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	if (*out == 0)
		return -1;

	SDL_PauseAudioDevice(*out, 0);
	return 0;
#endif
}

static void
audio_pause(smk_audio_out *out, int pause)
{
#ifdef SMK_USE_SDL3
	if (pause)
		SDL_PauseAudioStreamDevice(*out);
	else
		SDL_ResumeAudioStreamDevice(*out);
#else
	SDL_PauseAudioDevice(*out, pause);
#endif
}

static void
audio_clear(smk_audio_out *out)
{
#ifdef SMK_USE_SDL3
	SDL_ClearAudioStream(*out);
#else
	SDL_ClearQueuedAudio(*out);
#endif
}

static void
audio_queue(smk_audio_out *out, const unsigned char *data,
	unsigned long size)
{
#ifdef SMK_USE_SDL3
	SDL_PutAudioStreamData(*out, data, (int) size);
#else
	SDL_QueueAudio(*out, data, (Uint32) size);
#endif
}

static void
audio_close(smk_audio_out *out)
{
#ifdef SMK_USE_SDL3
	SDL_DestroyAudioStream(*out);
#else
	SDL_CloseAudioDevice(*out);
#endif
}

/* Sleep until the given SDL tick count, if it is still in the future. */
static void
wait_until(double target_ms)
{
	double		now = (double) SDL_GetTicks();

	if (target_ms > now)
		SDL_Delay((Uint32) (target_ms - now + 0.999));
}

/* GL attributes must be known before SDL creates the window, because some
 * backends pick the pixel format or EGL config at window creation. */
static int
gl_attributes_set(void)
{
	if (SMK_GL_SET_ATTR(SDL_GL_CONTEXT_MAJOR_VERSION, 3) ||
		SMK_GL_SET_ATTR(SDL_GL_CONTEXT_MINOR_VERSION, 3) ||
		SMK_GL_SET_ATTR(SDL_GL_CONTEXT_PROFILE_MASK,
			SDL_GL_CONTEXT_PROFILE_CORE) ||
		SMK_GL_SET_ATTR(SDL_GL_DOUBLEBUFFER, 1) ||
		SMK_GL_SET_ATTR(SDL_GL_DEPTH_SIZE, 0)) {
		fprintf(stderr, "SDL GL attribute setup failed: %s\n",
			SDL_GetError());
		return -1;
	}

	return 0;
}

/* Request an OpenGL 3.3 core context and make it current. */
static int
gl_context_create(SDL_Window *window, SDL_GLContext *context)
{
	*context = SDL_GL_CreateContext(window);
	if (*context == NULL)
		return -1;

#ifdef SMK_USE_SDL3
	if (!SDL_GL_MakeCurrent(window, *context))
		return -1;
#else
	if (SDL_GL_MakeCurrent(window, *context) != 0)
		return -1;
#endif

	SDL_GL_SetSwapInterval(0);
	return 0;
}

/* Function pointers for the entry points newer than GL 1.1. SDL resolves
 * them portably, so the player needs no loader library. */
typedef void (*smk_gl_active_texture_fn)(GLenum);
typedef void (*smk_gl_gen_vertex_arrays_fn)(GLsizei, GLuint *);
typedef void (*smk_gl_bind_vertex_array_fn)(GLuint);
typedef void (*smk_gl_delete_vertex_arrays_fn)(GLsizei, const GLuint *);
typedef GLuint (*smk_gl_create_shader_fn)(GLenum);
typedef void (*smk_gl_shader_source_fn)(GLuint, GLsizei,
	const GLchar *const *, const GLint *);
typedef void (*smk_gl_compile_shader_fn)(GLuint);
typedef void (*smk_gl_get_shaderiv_fn)(GLuint, GLenum, GLint *);
typedef void (*smk_gl_get_shader_info_log_fn)(GLuint, GLsizei, GLsizei *,
	GLchar *);
typedef GLuint (*smk_gl_create_program_fn)(void);
typedef void (*smk_gl_attach_shader_fn)(GLuint, GLuint);
typedef void (*smk_gl_link_program_fn)(GLuint);
typedef void (*smk_gl_get_programiv_fn)(GLuint, GLenum, GLint *);
typedef void (*smk_gl_get_program_info_log_fn)(GLuint, GLsizei, GLsizei *,
	GLchar *);
typedef void (*smk_gl_use_program_fn)(GLuint);
typedef GLint (*smk_gl_get_uniform_location_fn)(GLuint, const GLchar *);
typedef void (*smk_gl_uniform1i_fn)(GLint, GLint);
typedef void (*smk_gl_gen_textures_fn)(GLsizei, GLuint *);
typedef void (*smk_gl_delete_textures_fn)(GLsizei, const GLuint *);
typedef void (*smk_gl_bind_texture_fn)(GLenum, GLuint);
typedef void (*smk_gl_tex_image_2d_fn)(GLenum, GLint, GLint, GLsizei,
	GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*smk_gl_tex_sub_image_2d_fn)(GLenum, GLint, GLint, GLint,
	GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*smk_gl_tex_parameteri_fn)(GLenum, GLenum, GLint);
typedef void (*smk_gl_pixel_storei_fn)(GLenum, GLint);
typedef void (*smk_gl_delete_program_fn)(GLuint);
typedef void (*smk_gl_delete_shader_fn)(GLuint);

struct gl_api {
	smk_gl_active_texture_fn active_texture;
	smk_gl_gen_vertex_arrays_fn gen_vertex_arrays;
	smk_gl_bind_vertex_array_fn bind_vertex_array;
	smk_gl_delete_vertex_arrays_fn delete_vertex_arrays;
	smk_gl_create_shader_fn create_shader;
	smk_gl_shader_source_fn shader_source;
	smk_gl_compile_shader_fn compile_shader;
	smk_gl_get_shaderiv_fn get_shaderiv;
	smk_gl_get_shader_info_log_fn get_shader_info_log;
	smk_gl_create_program_fn create_program;
	smk_gl_attach_shader_fn attach_shader;
	smk_gl_link_program_fn link_program;
	smk_gl_get_programiv_fn get_programiv;
	smk_gl_get_program_info_log_fn get_program_info_log;
	smk_gl_use_program_fn use_program;
	smk_gl_get_uniform_location_fn get_uniform_location;
	smk_gl_uniform1i_fn uniform1i;
	smk_gl_gen_textures_fn gen_textures;
	smk_gl_delete_textures_fn delete_textures;
	smk_gl_bind_texture_fn bind_texture;
	smk_gl_tex_image_2d_fn tex_image_2d;
	smk_gl_tex_sub_image_2d_fn tex_sub_image_2d;
	smk_gl_tex_parameteri_fn tex_parameteri;
	smk_gl_pixel_storei_fn pixel_storei;
	smk_gl_delete_program_fn delete_program;
	smk_gl_delete_shader_fn delete_shader;
};

#define SMK_GL_LOAD(api, field, type, name) \
	do { \
		(api)->field = (type) SDL_GL_GetProcAddress(name); \
		if ((api)->field == NULL) { \
			fprintf(stderr, "Missing OpenGL entry point: %s\n", name); \
			return -1; \
		} \
	} while (0)

static int
gl_api_load(struct gl_api *api)
{
	memset(api, 0, sizeof(*api));

	SMK_GL_LOAD(api, active_texture, smk_gl_active_texture_fn,
		"glActiveTexture");
	SMK_GL_LOAD(api, gen_vertex_arrays, smk_gl_gen_vertex_arrays_fn,
		"glGenVertexArrays");
	SMK_GL_LOAD(api, bind_vertex_array, smk_gl_bind_vertex_array_fn,
		"glBindVertexArray");
	SMK_GL_LOAD(api, delete_vertex_arrays, smk_gl_delete_vertex_arrays_fn,
		"glDeleteVertexArrays");
	SMK_GL_LOAD(api, create_shader, smk_gl_create_shader_fn,
		"glCreateShader");
	SMK_GL_LOAD(api, shader_source, smk_gl_shader_source_fn,
		"glShaderSource");
	SMK_GL_LOAD(api, compile_shader, smk_gl_compile_shader_fn,
		"glCompileShader");
	SMK_GL_LOAD(api, get_shaderiv, smk_gl_get_shaderiv_fn,
		"glGetShaderiv");
	SMK_GL_LOAD(api, get_shader_info_log, smk_gl_get_shader_info_log_fn,
		"glGetShaderInfoLog");
	SMK_GL_LOAD(api, create_program, smk_gl_create_program_fn,
		"glCreateProgram");
	SMK_GL_LOAD(api, attach_shader, smk_gl_attach_shader_fn,
		"glAttachShader");
	SMK_GL_LOAD(api, link_program, smk_gl_link_program_fn,
		"glLinkProgram");
	SMK_GL_LOAD(api, get_programiv, smk_gl_get_programiv_fn,
		"glGetProgramiv");
	SMK_GL_LOAD(api, get_program_info_log,
		smk_gl_get_program_info_log_fn, "glGetProgramInfoLog");
	SMK_GL_LOAD(api, use_program, smk_gl_use_program_fn, "glUseProgram");
	SMK_GL_LOAD(api, get_uniform_location,
		smk_gl_get_uniform_location_fn, "glGetUniformLocation");
	SMK_GL_LOAD(api, uniform1i, smk_gl_uniform1i_fn, "glUniform1i");
	SMK_GL_LOAD(api, gen_textures, smk_gl_gen_textures_fn,
		"glGenTextures");
	SMK_GL_LOAD(api, delete_textures, smk_gl_delete_textures_fn,
		"glDeleteTextures");
	SMK_GL_LOAD(api, bind_texture, smk_gl_bind_texture_fn,
		"glBindTexture");
	SMK_GL_LOAD(api, tex_image_2d, smk_gl_tex_image_2d_fn,
		"glTexImage2D");
	SMK_GL_LOAD(api, tex_sub_image_2d, smk_gl_tex_sub_image_2d_fn,
		"glTexSubImage2D");
	SMK_GL_LOAD(api, tex_parameteri, smk_gl_tex_parameteri_fn,
		"glTexParameteri");
	SMK_GL_LOAD(api, pixel_storei, smk_gl_pixel_storei_fn,
		"glPixelStorei");
	SMK_GL_LOAD(api, delete_program, smk_gl_delete_program_fn,
		"glDeleteProgram");
	SMK_GL_LOAD(api, delete_shader, smk_gl_delete_shader_fn,
		"glDeleteShader");

	return 0;
}

static const char * const gl_vertex_shader =
	"#version 330 core\n"
	"out vec2 v_uv;\n"
	"void main()\n"
	"{\n"
	"    vec2 p = vec2(float((gl_VertexID << 1) & 2),\n"
	"                  float(gl_VertexID & 2));\n"
	"    v_uv = vec2(p.x, 1.0 - p.y);\n"
	"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char * const gl_fragment_shader =
	"#version 330 core\n"
	"uniform sampler2D u_frame;\n"
	"uniform sampler2D u_palette;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag_color;\n"
	"void main()\n"
	"{\n"
	"    float index = texture(u_frame, v_uv).r;\n"
	"    int slot = int(index * 255.0 + 0.5);\n"
	"    vec3 rgb = texelFetch(u_palette, ivec2(slot, 0), 0).rgb;\n"
	"    frag_color = vec4(rgb, 1.0);\n"
	"}\n";

struct gl_renderer {
	struct gl_api	api;
	GLuint		program;
	GLuint		vao;
	GLuint		frame_tex;
	GLuint		palette_tex;
	GLint		u_frame;
	GLint		u_palette;
	unsigned char	last_palette[256 * 3];
	int		have_palette;
};

static GLuint
compile_shader(struct gl_api *api, GLenum type, const char *source)
{
	GLuint		shader;
	GLint		ok = 0;
	GLsizei		len = 0;
	char		log[1024];

	shader = api->create_shader(type);
	if (shader == 0)
		return 0;

	api->shader_source(shader, 1, &source, NULL);
	api->compile_shader(shader);
	api->get_shaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		log[0] = '\0';
		api->get_shader_info_log(shader, (GLsizei) sizeof(log), &len,
			log);
		fprintf(stderr, "Shader compile failed: %.*s\n", (int) len,
			log);
		api->delete_shader(shader);
		return 0;
	}

	return shader;
}

static GLuint
link_program(struct gl_api *api, const char *vertex_source,
	const char *fragment_source)
{
	GLuint		vertex, fragment, program;
	GLint		ok = 0;
	GLsizei		len = 0;
	char		log[1024];

	vertex = compile_shader(api, GL_VERTEX_SHADER, vertex_source);
	if (vertex == 0)
		return 0;
	fragment = compile_shader(api, GL_FRAGMENT_SHADER, fragment_source);
	if (fragment == 0) {
		api->delete_shader(vertex);
		return 0;
	}

	program = api->create_program();
	if (program == 0) {
		api->delete_shader(vertex);
		api->delete_shader(fragment);
		return 0;
	}
	api->attach_shader(program, vertex);
	api->attach_shader(program, fragment);
	api->link_program(program);
	api->get_programiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		log[0] = '\0';
		api->get_program_info_log(program, (GLsizei) sizeof(log), &len,
			log);
		fprintf(stderr, "Shader link failed: %.*s\n", (int) len, log);
		api->delete_program(program);
		program = 0;
	}

	api->delete_shader(vertex);
	api->delete_shader(fragment);
	return program;
}

static int
gl_renderer_init(struct gl_renderer *r, unsigned long w, unsigned long h)
{
	memset(r, 0, sizeof(*r));

	if (gl_api_load(&r->api) != 0)
		return -1;

	r->program = link_program(&r->api, gl_vertex_shader,
		gl_fragment_shader);
	if (r->program == 0)
		return -1;

	r->u_frame = r->api.get_uniform_location(r->program, "u_frame");
	r->u_palette = r->api.get_uniform_location(r->program, "u_palette");
	if (r->u_frame < 0 || r->u_palette < 0) {
		fputs("Missing shader uniform\n", stderr);
		return -1;
	}

	r->api.gen_vertex_arrays(1, &r->vao);

	r->api.gen_textures(1, &r->frame_tex);
	r->api.bind_texture(GL_TEXTURE_2D, r->frame_tex);
	r->api.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		GL_NEAREST);
	r->api.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
		GL_NEAREST);
	r->api.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		GL_CLAMP_TO_EDGE);
	r->api.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		GL_CLAMP_TO_EDGE);
	r->api.tex_image_2d(GL_TEXTURE_2D, 0, GL_R8, (GLsizei) w,
		(GLsizei) h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

	r->api.gen_textures(1, &r->palette_tex);
	r->api.bind_texture(GL_TEXTURE_2D, r->palette_tex);
	r->api.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		GL_NEAREST);
	r->api.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
		GL_NEAREST);
	r->api.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		GL_CLAMP_TO_EDGE);
	r->api.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		GL_CLAMP_TO_EDGE);
	r->api.tex_image_2d(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB,
		GL_UNSIGNED_BYTE, NULL);

	r->api.use_program(r->program);
	r->api.uniform1i(r->u_frame, 0);
	r->api.uniform1i(r->u_palette, 1);
	r->api.use_program(0);
	r->api.bind_texture(GL_TEXTURE_2D, 0);

	return 0;
}

static void
gl_renderer_free(struct gl_renderer *r)
{
	if (r->api.delete_textures != NULL) {
		if (r->frame_tex != 0)
			r->api.delete_textures(1, &r->frame_tex);
		if (r->palette_tex != 0)
			r->api.delete_textures(1, &r->palette_tex);
	}
	if (r->api.delete_vertex_arrays != NULL && r->vao != 0)
		r->api.delete_vertex_arrays(1, &r->vao);
	if (r->api.delete_program != NULL && r->program != 0)
		r->api.delete_program(r->program);

	memset(r, 0, sizeof(*r));
}

static void
gl_renderer_upload(struct gl_renderer *r, const unsigned char *frame,
	const unsigned char *palette, unsigned long w, unsigned long h)
{
	r->api.pixel_storei(GL_UNPACK_ALIGNMENT, 1);

	r->api.active_texture(GL_TEXTURE0);
	r->api.bind_texture(GL_TEXTURE_2D, r->frame_tex);
	r->api.tex_sub_image_2d(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) w,
		(GLsizei) h, GL_RED, GL_UNSIGNED_BYTE, frame);

	if (!r->have_palette ||
		memcmp(r->last_palette, palette,
			sizeof(r->last_palette)) != 0) {
		r->api.active_texture(GL_TEXTURE1);
		r->api.bind_texture(GL_TEXTURE_2D, r->palette_tex);
		r->api.tex_sub_image_2d(GL_TEXTURE_2D, 0, 0, 0, 256, 1,
			GL_RGB, GL_UNSIGNED_BYTE, palette);
		memcpy(r->last_palette, palette,
			sizeof(r->last_palette));
		r->have_palette = 1;
	}
}

/* Largest centred rect inside the window with the source aspect ratio. */
static void
compute_viewport(int win_w, int win_h, unsigned long src_w,
	unsigned long src_h, int *x, int *y, int *vw, int *vh)
{
	double		sx, sy, scale;
	int		w, h;

	*x = 0;
	*y = 0;
	*vw = 0;
	*vh = 0;
	if (win_w <= 0 || win_h <= 0 || src_w == 0 || src_h == 0)
		return;

	sx = (double) win_w / (double) src_w;
	sy = (double) win_h / (double) src_h;
	scale = sx < sy ? sx : sy;

	w = (int) ((double) src_w * scale + 0.5);
	h = (int) ((double) src_h * scale + 0.5);
	if (w < 1)
		w = 1;
	if (w > win_w)
		w = win_w;
	if (h < 1)
		h = 1;
	if (h > win_h)
		h = win_h;

	*vw = w;
	*vh = h;
	*x = (win_w - w) / 2;
	*y = (win_h - h) / 2;
}

static void
gl_renderer_draw(struct gl_renderer *r, int win_w, int win_h,
	unsigned long w, unsigned long h, int y_double)
{
	int		x, y, vw, vh;

	glViewport(0, 0, win_w > 0 ? win_w : 1, win_h > 0 ? win_h : 1);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	compute_viewport(win_w, win_h, w, y_double ? h * 2 : h,
		&x, &y, &vw, &vh);
	if (vw <= 0 || vh <= 0)
		return;

	glViewport(x, y, vw, vh);
	r->api.use_program(r->program);
	r->api.active_texture(GL_TEXTURE0);
	r->api.bind_texture(GL_TEXTURE_2D, r->frame_tex);
	r->api.active_texture(GL_TEXTURE1);
	r->api.bind_texture(GL_TEXTURE_2D, r->palette_tex);
	r->api.bind_vertex_array(r->vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Current drawable size in pixels, or 0x0 while minimized. */
static void
drawable_size(SDL_Window *window, int *w, int *h)
{
#ifdef SMK_USE_SDL3
	if (!SDL_GetWindowSizeInPixels(window, w, h)) {
		*w = 0;
		*h = 0;
	}
#else
	SDL_GL_GetDrawableSize(window, w, h);
#endif
}

static const char *
gl_string(GLenum name)
{
	const GLubyte  *value = glGetString(name);

	return value != NULL ? (const char *) value : "(unknown)";
}

int
main(int argc, char *argv[])
{
	struct options	opt;
	smk		s = NULL;
	SDL_Window     *window = NULL;
	SDL_GLContext	context = NULL;
	struct gl_renderer renderer;
	unsigned long	w = 0, h = 0, frames = 0;
	double		usf = 0.0;
	unsigned char	y_scale = SMK_FLAG_Y_NONE;
	int		y_double;
	int		status = 0;
	int		quit = 0;
	unsigned long	presented = 0;
	double		frame_ms;
	char		title[512];
	char		rc;
	int		parsed;
	int		renderer_started = 0;
	int		gl_checked = 0;
	int		draw_w = 0, draw_h = 0;
	GLenum		gl_err;
	smk_audio_out	audio;
	unsigned char	track_mask = 0;
	unsigned char	channels[7], bitdepth[7];
	unsigned long	rates[7];
	int		track = -1;
	int		has_audio = 0;
	int		paused = 0;
	int		redraw = 1;
	int		i;
	double		next_ms;

	parsed = parse_options(argc, argv, &opt);
	if (parsed != 0)
		return parsed > 0 ? 0 : 1;

	s = smk_open_file(opt.path, SMK_MODE_DISK);
	if (s == NULL) {
		fprintf(stderr, "Errors encountered opening %s, exiting.\n",
			opt.path);
		return 1;
	}

	smk_info_all(s, NULL, &frames, &usf);
	smk_info_video(s, &w, &h, &y_scale);
	if (w == 0 || h == 0) {
		fprintf(stderr, "%s has no video track, exiting.\n", opt.path);
		smk_close(s);
		return 1;
	}
	if (usf <= 0.0)
		usf = 1000000.0 / 30.0;
	frame_ms = usf / 1000.0;
	y_double = y_scale != SMK_FLAG_Y_NONE;

	printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
		opt.path, w, h, frames, 1000000.0 / usf);
	if (y_double)
		printf("Y-scale: doubled\n");

	if (video_init() != 0) {
		fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
		status = 1;
		goto done;
	}

	if (gl_attributes_set() != 0) {
		status = 1;
		goto done;
	}

	snprintf(title, sizeof(title), "libsmacker OpenGL player - %s",
		opt.path);
#ifdef SMK_USE_SDL3
	window = SDL_CreateWindow(title, (int) w,
		(int) (y_double ? h * 2 : h),
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
#else
	window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, (int) w,
		(int) (y_double ? h * 2 : h),
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
#endif
	if (window == NULL) {
		fprintf(stderr, "SDL window creation failed: %s\n",
			SDL_GetError());
		status = 1;
		goto done;
	}

	if (gl_context_create(window, &context) != 0) {
		fprintf(stderr, "OpenGL 3.3 core context creation failed: %s\n",
			SDL_GetError());
		status = 1;
		goto done;
	}

	printf("GL_VERSION: %s\nGL_RENDERER: %s\n",
		gl_string(GL_VERSION), gl_string(GL_RENDERER));

	renderer_started = 1;
	if (gl_renderer_init(&renderer, w, h) != 0) {
		fputs("OpenGL renderer initialization failed\n", stderr);
		status = 1;
		goto done;
	}
	gl_err = glGetError();
	if (gl_err != GL_NO_ERROR) {
		fprintf(stderr, "OpenGL error 0x%04X after renderer init\n",
			(unsigned int) gl_err);
		status = 1;
		goto done;
	}

	smk_enable_video(s, 1);
	rc = smk_first(s);
	if (rc == SMK_ERROR) {
		fprintf(stderr, "Failed to decode the first frame of %s.\n",
			opt.path);
		status = 1;
		goto done;
	}

	/* Play the first audio track that exists (normally track 0). */
	smk_info_audio(s, &track_mask, channels, bitdepth, rates);
	for (i = 0; i < 7; i++) {
		if (track_mask & (1u << i)) {
			track = i;
			break;
		}
	}

	if (track >= 0) {
		if (audio_init() != 0) {
			fprintf(stderr,
				"SDL audio init failed, playing silently: %s\n",
				SDL_GetError());
		} else {
			smk_enable_audio(s, (unsigned char) track, 1);
			if (audio_open(&audio, rates[track], channels[track],
				bitdepth[track]) == 0) {
				has_audio = 1;
				printf("Audio track %d: %u bits, %u channels, %luhz\n",
					track, (unsigned int) bitdepth[track],
					(unsigned int) channels[track],
					rates[track]);
			} else {
				fprintf(stderr,
					"SDL audio open failed, playing silently: %s\n",
					SDL_GetError());
			}
		}
	}

	next_ms = (double) SDL_GetTicks();

	while (!quit) {
		SDL_Event	event;

		while (SDL_PollEvent(&event)) {
			int		key;

			if (event.type == SMK_EVENT_QUIT) {
				quit = 1;
				break;
			}
			if (event.type != SMK_EVENT_KEY_DOWN)
				continue;

			key = SMK_EVENT_KEY(event);
			if (key == SDLK_ESCAPE || key == SMK_KEY_Q) {
				quit = 1;
			} else if (key == SDLK_SPACE) {
				paused = !paused;
				if (has_audio)
					audio_pause(&audio, paused);
				next_ms = (double) SDL_GetTicks();
			} else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
				unsigned long	cur = 0;
				long		step;
				long		target;

				smk_info_all(s, &cur, &frames, NULL);
				step = (long) (1000000.0 / usf + 0.5);
				if (step < 1)
					step = 1;
				target = (long) cur +
					(key == SDLK_LEFT ? -step : step);
				if (target < 0)
					target = 0;
				if (frames > 0 && target >= (long) frames)
					target = (long) frames - 1;

				if (smk_seek_keyframe(s,
					(unsigned long) target) == 0) {
					smk_info_all(s, &cur, &frames, NULL);
					rc = (cur + 1 >= frames) ? SMK_LAST
						: SMK_MORE;
					if (has_audio)
						audio_clear(&audio);
					next_ms = (double) SDL_GetTicks();
					redraw = 1;
				}
			}
		}
		if (quit)
			break;

		if (!paused || redraw) {
			draw_w = 0;
			draw_h = 0;
			drawable_size(window, &draw_w, &draw_h);
			gl_renderer_upload(&renderer, smk_get_video(s),
				smk_get_palette(s), w, h);
			gl_renderer_draw(&renderer, draw_w, draw_h, w, h,
				y_double);

			if (!gl_checked) {
				gl_checked = 1;
				gl_err = glGetError();
				if (gl_err != GL_NO_ERROR) {
					fprintf(stderr,
						"OpenGL error 0x%04X after first frame\n",
						(unsigned int) gl_err);
					status = 1;
					break;
				}
			}

			SDL_GL_SwapWindow(window);
			redraw = 0;

			presented++;
			if (opt.frame_limit != 0 &&
				presented >= opt.frame_limit)
				break;

			if (has_audio && !paused) {
				unsigned long	size = smk_get_audio_size(s,
					(unsigned char) track);

				if (size > 0)
					audio_queue(&audio,
						smk_get_audio(s,
						(unsigned char) track),
						size);
			}
		}

		if (paused) {
			SDL_Delay(10);
			continue;
		}

		/* Hold the last frame for one interval, then stop. */
		if (rc == SMK_LAST) {
			next_ms += frame_ms;
			wait_until(next_ms);
			break;
		}

		next_ms += frame_ms;
		wait_until(next_ms);

		rc = smk_next(s);
		if (rc == SMK_ERROR || rc == SMK_DONE)
			break;
	}

done:
	if (has_audio)
		audio_close(&audio);
	if (renderer_started)
		gl_renderer_free(&renderer);
	if (context != NULL)
		SMK_GL_DESTROY_CONTEXT(context);
	if (window != NULL)
		SDL_DestroyWindow(window);
	SDL_Quit();
	if (s != NULL)
		smk_close(s);

	return status;
}
