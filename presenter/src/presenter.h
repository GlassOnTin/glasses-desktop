/* glasses-presenter — SBS depth presenter for the glasses desktop.
 *
 * Captures the desktop output (screencopy → dmabuf/shm), asks sway IPC for
 * window rects + focus order, and draws the desktop into both eye halves of
 * the physical SBS output with a per-window horizontal disparity: the
 * focused window pops toward the viewer. On a <2:1 surface it renders one
 * flat copy (2D mode).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define MAX_WINDOWS 64

struct win {
	/* desktop-relative pixels (source output coords) */
	int32_t x, y, w, h;
	int rank;      /* 0 = focused, 1 = next most recent, ... */
	bool focused;
};

struct capture {
	/* offers from the current screencopy frame */
	bool off_dmabuf; uint32_t dma_format; int dma_w, dma_h;
	bool off_shm; uint32_t shm_format; int shm_w, shm_h, shm_stride;
	bool y_invert;

	/* dmabuf path */
	int drm_fd;
	struct gbm_device *gbm;
	struct gbm_bo *bo;
	struct wl_buffer *dma_buf;
	int bo_w, bo_h; uint32_t bo_format;
	EGLImageKHR image;
	bool dmabuf_broken;   /* copy failed once — stick to shm */

	/* shm path */
	int memfd; void *shm_data; size_t shm_size;
	struct wl_buffer *shm_wlbuf;
	int sb_w, sb_h, sb_stride; uint32_t sb_format;
	bool using_shm;       /* what the in-flight copy targets */

	/* gl */
	GLuint tex; int tex_w, tex_h;
	bool tex_valid, tex_is_shm, content_new, need_reimport;

	struct zwlr_screencopy_frame_v1 *frame;
	bool ready;           /* new content since last render */
};

struct output {
	struct wl_output *wl;
	uint32_t global_name;
	char name[64];
	struct wl_list link;
};

struct app {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct zwlr_screencopy_manager_v1 *screencopy;
	struct wl_list outputs;
	struct output *src, *dst;
	const char *src_name, *dst_name;

	struct wl_surface *surf;
	struct xdg_surface *xsurf;
	struct xdg_toplevel *top;
	struct wl_egl_window *egl_win;
	struct wl_callback *frame_cb;
	int32_t win_w, win_h;
	bool configured, running;

	EGLDisplay edpy;
	EGLConfig ecfg;
	EGLContext ectx;
	EGLSurface esurf;
	GLuint prog;
	GLint a_pos, a_uv, u_tex, u_swap_rb;

	struct capture cap;

	/* sway IPC */
	int ipc_fd;
	char *ipc_buf; size_t ipc_len, ipc_cap;
	bool tree_pending, tree_dirty, want_tree;
	struct win wins[MAX_WINDOWS];
	int nwins;                       /* back-to-front paint order */
	int desk_x, desk_y, desk_w, desk_h;
	bool have_desk;

	/* config */
	float pop, step, floor_d;
	int eye_sign;                    /* +1 normal, -1 swapped */
	int log;
};

#define LOGF(app, lvl, ...) do { if ((app)->log >= (lvl)) { \
	fprintf(stderr, "presenter: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* capture.c */
bool capture_init(struct app *a);
void capture_start(struct app *a);
void capture_update_texture(struct app *a); /* needs GL current */
void capture_destroy(struct app *a);

/* render.c */
bool render_init_egl(struct app *a);
bool render_create_surface_objects(struct app *a);
void render_frame(struct app *a);
void render_destroy(struct app *a);

/* sway.c */
bool sway_connect(struct app *a);
void sway_request_tree(struct app *a);
bool sway_handle_readable(struct app *a);
