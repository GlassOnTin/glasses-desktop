#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "presenter.h"

static PFNEGLCREATEIMAGEKHRPROC p_CreateImage;
static PFNEGLDESTROYIMAGEKHRPROC p_DestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_ImageTargetTexture;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC p_QueryMods;

static const char *VS =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *FS =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform bool u_swap_rb;\n"
	"void main() {\n"
	"  vec4 c = texture2D(u_tex, v_uv);\n"
	"  gl_FragColor = vec4(u_swap_rb ? c.bgr : c.rgb, 1.0);\n"
	"}\n";

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "presenter: shader: %s\n", log);
		return 0;
	}
	return s;
}

bool render_init_egl(struct app *a)
{
	for (int i = 128; i < 132; i++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
		a->drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (a->drm_fd < 0)
			continue;
		a->gbm = gbm_create_device(a->drm_fd);
		if (a->gbm)
			break;
		close(a->drm_fd);
		a->drm_fd = -1;
	}
	if (!a->gbm) {
		fprintf(stderr, "presenter: no usable render node\n");
		return false;
	}

	a->edpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, a->gbm, NULL);
	if (a->edpy == EGL_NO_DISPLAY || !eglInitialize(a->edpy, NULL, NULL)) {
		fprintf(stderr, "presenter: eglInitialize (gbm) failed\n");
		return false;
	}
	const char *exts = eglQueryString(a->edpy, EGL_EXTENSIONS);
	if (!exts || !strstr(exts, "EGL_KHR_surfaceless_context") ||
	    !strstr(exts, "EGL_KHR_no_config_context")) {
		fprintf(stderr, "presenter: missing surfaceless/no-config EGL\n");
		return false;
	}
	p_CreateImage = (void *)eglGetProcAddress("eglCreateImageKHR");
	p_DestroyImage = (void *)eglGetProcAddress("eglDestroyImageKHR");
	p_ImageTargetTexture =
		(void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	p_QueryMods = (void *)eglGetProcAddress("eglQueryDmaBufModifiersEXT");

	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	a->ectx = eglCreateContext(a->edpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
		ctx_attrs);
	if (a->ectx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(a->edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, a->ectx)) {
		fprintf(stderr, "presenter: surfaceless context failed\n");
		return false;
	}

	GLuint vs = compile(GL_VERTEX_SHADER, VS);
	GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
	if (!vs || !fs)
		return false;
	a->prog = glCreateProgram();
	glAttachShader(a->prog, vs);
	glAttachShader(a->prog, fs);
	glLinkProgram(a->prog);
	GLint ok;
	glGetProgramiv(a->prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "presenter: program link failed\n");
		return false;
	}
	a->a_pos = glGetAttribLocation(a->prog, "a_pos");
	a->a_uv = glGetAttribLocation(a->prog, "a_uv");
	a->u_tex = glGetUniformLocation(a->prog, "u_tex");
	a->u_swap_rb = glGetUniformLocation(a->prog, "u_swap_rb");
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	return true;
}

/* ---- swapchain ---- */

static void swapbuf_release(void *d, struct wl_buffer *b)
{
	((struct swapbuf *)d)->busy = false;
}
static const struct wl_buffer_listener swapbuf_listener = {
	.release = swapbuf_release,
};

static void swapbuf_destroy(struct app *a, struct swapbuf *b)
{
	if (b->fbo)
		glDeleteFramebuffers(1, &b->fbo);
	if (b->tex)
		glDeleteTextures(1, &b->tex);
	if (b->image)
		p_DestroyImage(a->edpy, b->image);
	if (b->wlbuf)
		wl_buffer_destroy(b->wlbuf);
	if (b->bo)
		gbm_bo_destroy(b->bo);
	memset(b, 0, sizeof *b);
}

static bool swapbuf_create(struct app *a, struct swapbuf *b, int w, int h)
{
	uint64_t mods[64];
	EGLint nmods = 0;
	if (p_QueryMods)
		p_QueryMods(a->edpy, DRM_FORMAT_XRGB8888, 64,
			(EGLuint64KHR *)mods, NULL, &nmods);
	if (nmods > 0)
		b->bo = gbm_bo_create_with_modifiers2(a->gbm, w, h,
			DRM_FORMAT_XRGB8888, mods, nmods, GBM_BO_USE_RENDERING);
	if (!b->bo)
		b->bo = gbm_bo_create(a->gbm, w, h, DRM_FORMAT_XRGB8888,
			GBM_BO_USE_RENDERING);
	if (!b->bo)
		return false;

	uint32_t stride = gbm_bo_get_stride(b->bo);
	uint64_t mod = gbm_bo_get_modifier(b->bo);
	int fd = gbm_bo_get_fd(b->bo);
	if (fd < 0)
		return false;

	EGLint attrs[] = {
		EGL_WIDTH, w,
		EGL_HEIGHT, h,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
		EGL_NONE
	};
	b->image = p_CreateImage(a->edpy, EGL_NO_CONTEXT,
		EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	if (!b->image) {
		LOGF(a, 0, "render buffer EGLImage failed (0x%x)", eglGetError());
		close(fd);
		return false;
	}

	glGenTextures(1, &b->tex);
	glBindTexture(GL_TEXTURE_2D, b->tex);
	p_ImageTargetTexture(GL_TEXTURE_2D, b->image);
	glGenFramebuffers(1, &b->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, b->tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		LOGF(a, 0, "render FBO incomplete");
		close(fd);
		return false;
	}

	struct zwp_linux_buffer_params_v1 *params =
		zwp_linux_dmabuf_v1_create_params(a->dmabuf);
	zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride,
		mod >> 32, mod & 0xffffffff);
	b->wlbuf = zwp_linux_buffer_params_v1_create_immed(params, w, h,
		DRM_FORMAT_XRGB8888, 0);
	zwp_linux_buffer_params_v1_destroy(params);
	wl_buffer_add_listener(b->wlbuf, &swapbuf_listener, b);
	close(fd);

	b->w = w;
	b->h = h;
	b->busy = false;
	LOGF(a, 1, "render buffer %dx%d mod %llx", w, h,
		(unsigned long long)mod);
	return true;
}

static struct swapbuf *sc_acquire(struct app *a)
{
	for (int i = 0; i < SWAPCHAIN_LEN; i++) {
		struct swapbuf *b = &a->sc[i];
		if (b->busy)
			continue;
		if (b->w != a->win_w || b->h != a->win_h) {
			swapbuf_destroy(a, b);
			if (!swapbuf_create(a, b, a->win_w, a->win_h))
				return NULL;
		}
		return b;
	}
	return NULL; /* all busy — retried when a release event arrives */
}

/* ---- drawing ---- */

/* Draw a desktop-space rect (dx,dy,dw,dh) shifted by off_x px, into the
 * current viewport which maps the desktop (desk_w × desk_h) to NDC.
 * Rendering targets an FBO whose memory consumers read top-down, so the
 * desktop top maps to NDC -1. */
static void draw_rect(struct app *a, float dx, float dy, float dw, float dh,
	float off_x)
{
	float W = a->desk_w, H = a->desk_h;

	/* clip source rect to the desktop so CLAMP_TO_EDGE never smears */
	float x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > W) x1 = W;
	if (y1 > H) y1 = H;
	if (x1 <= x0 || y1 <= y0)
		return;

	float u0 = x0 / W, u1 = x1 / W;
	float v0 = y0 / H, v1 = y1 / H;
	if (a->cap.y_invert) {
		v0 = 1.0f - v0;
		v1 = 1.0f - v1;
	}
	float px0 = (x0 + off_x) / W * 2.0f - 1.0f;
	float px1 = (x1 + off_x) / W * 2.0f - 1.0f;
	float py0 = (y0 / H) * 2.0f - 1.0f;
	float py1 = (y1 / H) * 2.0f - 1.0f;

	const float verts[] = {
		px0, py0, u0, v0,
		px1, py0, u1, v0,
		px0, py1, u0, v1,
		px1, py1, u1, v1,
	};
	glVertexAttribPointer(a->a_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
	glVertexAttribPointer(a->a_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static float disparity(struct app *a, int rank)
{
	float d = a->pop - rank * a->step;
	return d < a->floor_d ? a->floor_d : d;
}

static void draw_eye(struct app *a, float shift_sign)
{
	draw_rect(a, 0, 0, a->desk_w, a->desk_h, 0); /* background at screen depth */
	for (int i = 0; i < a->nwins; i++) {
		struct win *w = &a->wins[i];
		float d = shift_sign * disparity(a, w->rank);
		/* extend the trailing edge by the shift so the quad covers the
		 * window's own baked copy in the background — otherwise a
		 * ghost border sliver peeks out beside every window edge */
		if (d > 0)
			draw_rect(a, w->x - d, w->y, w->w + d, w->h, d);
		else if (d < 0)
			draw_rect(a, w->x, w->y, w->w - d, w->h, d);
		else
			draw_rect(a, w->x, w->y, w->w, w->h, 0);
	}
}

bool render_frame(struct app *a)
{
	struct swapbuf *buf = sc_acquire(a);
	if (!buf)
		return false;

	capture_update_texture(a);
	if (!a->cap.tex_valid)
		return false;

	/* until sway tells us the desktop size, assume the capture is it */
	if (!a->have_desk) {
		a->desk_w = a->cap.tex_w;
		a->desk_h = a->cap.tex_h;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo);
	glViewport(0, 0, a->win_w, a->win_h);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(a->prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, a->cap.tex);
	glUniform1i(a->u_tex, 0);
	glUniform1i(a->u_swap_rb, a->cap.tex_is_shm);
	glEnableVertexAttribArray(a->a_pos);
	glEnableVertexAttribArray(a->a_uv);

	if (a->win_w >= 2 * a->win_h) {
		int half = a->win_w / 2;
		glViewport(0, 0, half, a->win_h);           /* left eye */
		draw_eye(a, +1.0f * a->eye_sign);
		glViewport(half, 0, a->win_w - half, a->win_h); /* right eye */
		draw_eye(a, -1.0f * a->eye_sign);
	} else {
		draw_eye(a, 0.0f); /* 2D: offsets vanish, plain mirror */
	}

	/* v3d attaches an implicit fence on submit; the compositor's read
	 * of the dmabuf orders against it */
	glFlush();

	wl_surface_attach(a->surf, buf->wlbuf, 0, 0);
	wl_surface_damage_buffer(a->surf, 0, 0, buf->w, buf->h);
	wl_surface_commit(a->surf);
	buf->busy = true;
	return true;
}

void render_destroy(struct app *a)
{
	if (a->edpy == EGL_NO_DISPLAY)
		return;
	for (int i = 0; i < SWAPCHAIN_LEN; i++)
		swapbuf_destroy(a, &a->sc[i]);
	eglMakeCurrent(a->edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (a->ectx != EGL_NO_CONTEXT)
		eglDestroyContext(a->edpy, a->ectx);
	eglTerminate(a->edpy);
	if (a->gbm)
		gbm_device_destroy(a->gbm);
	if (a->drm_fd >= 0)
		close(a->drm_fd);
}
