#include <stdio.h>
#include <stdlib.h>
#include "xdg-shell-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "presenter.h"

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

bool render_init_egl(struct app *a)
{
	a->edpy = eglGetDisplay((EGLNativeDisplayType)a->dpy);
	if (a->edpy == EGL_NO_DISPLAY || !eglInitialize(a->edpy, NULL, NULL)) {
		fprintf(stderr, "presenter: eglInitialize failed\n");
		return false;
	}
	EGLint n, cfg_attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE
	};
	if (!eglChooseConfig(a->edpy, cfg_attrs, &a->ecfg, 1, &n) || n < 1) {
		fprintf(stderr, "presenter: no EGL config\n");
		return false;
	}
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	eglBindAPI(EGL_OPENGL_ES_API);
	a->ectx = eglCreateContext(a->edpy, a->ecfg, EGL_NO_CONTEXT, ctx_attrs);
	if (a->ectx == EGL_NO_CONTEXT) {
		fprintf(stderr, "presenter: eglCreateContext failed\n");
		return false;
	}
	return true;
}

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

bool render_create_surface_objects(struct app *a)
{
	a->esurf = eglCreateWindowSurface(a->edpy, a->ecfg,
		(EGLNativeWindowType)a->egl_win, NULL);
	if (a->esurf == EGL_NO_SURFACE ||
	    !eglMakeCurrent(a->edpy, a->esurf, a->esurf, a->ectx)) {
		fprintf(stderr, "presenter: EGL surface/current failed\n");
		return false;
	}
	eglSwapInterval(a->edpy, 0); /* throttled by wl frame callbacks */

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

/* Draw a desktop-space rect (dx,dy,dw,dh) shifted by off_x px, into the
 * current viewport which maps the desktop (desk_w × desk_h) to NDC. */
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
	float py0 = 1.0f - (y0 / H) * 2.0f;   /* desktop top → NDC +1 */
	float py1 = 1.0f - (y1 / H) * 2.0f;

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

void render_frame(struct app *a)
{
	capture_update_texture(a);
	if (!a->cap.tex_valid)
		return;

	/* until sway tells us the desktop size, assume the capture is it */
	if (!a->have_desk) {
		a->desk_w = a->cap.tex_w;
		a->desk_h = a->cap.tex_h;
	}

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

	eglSwapBuffers(a->edpy, a->esurf);
}

void render_destroy(struct app *a)
{
	if (a->edpy == EGL_NO_DISPLAY)
		return;
	eglMakeCurrent(a->edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (a->esurf != EGL_NO_SURFACE)
		eglDestroySurface(a->edpy, a->esurf);
	if (a->ectx != EGL_NO_CONTEXT)
		eglDestroyContext(a->edpy, a->ectx);
	eglTerminate(a->edpy);
}
