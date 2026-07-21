#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <gbm.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "presenter.h"

static int n_render, n_capture;
static double swap_ms_total, cb_ms_total;
static int cb_n;
static struct timespec swap_t1;

static double ms_since(const struct timespec *t0)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1e3 + (t.tv_nsec - t0->tv_nsec) / 1e6;
}

/* ---- wl_output (v4 gives us the name directly) ---- */

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
	int32_t pw, int32_t ph, int32_t subpix, const char *make,
	const char *model, int32_t transform) {}
static void out_mode(void *d, struct wl_output *o, uint32_t flags,
	int32_t w, int32_t h, int32_t refresh) {}
static void out_done(void *d, struct wl_output *o) {}
static void out_scale(void *d, struct wl_output *o, int32_t f) {}
static void out_name(void *d, struct wl_output *o, const char *name)
{
	struct output *out = d;
	snprintf(out->name, sizeof out->name, "%s", name);
}
static void out_desc(void *d, struct wl_output *o, const char *desc) {}

static const struct wl_output_listener output_listener = {
	.geometry = out_geometry, .mode = out_mode, .done = out_done,
	.scale = out_scale, .name = out_name, .description = out_desc,
};

/* ---- registry ---- */

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
	const char *iface, uint32_t version)
{
	struct app *a = d;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		a->comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		a->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		a->wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 2);
	} else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
		a->dmabuf = wl_registry_bind(reg, name,
			&zwp_linux_dmabuf_v1_interface, version < 3 ? version : 3);
	} else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
		a->screencopy = wl_registry_bind(reg, name,
			&zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		if (version < 4)
			return; /* need the name event */
		struct output *out = calloc(1, sizeof *out);
		out->wl = wl_registry_bind(reg, name, &wl_output_interface, 4);
		out->global_name = name;
		wl_output_add_listener(out->wl, &output_listener, out);
		wl_list_insert(&a->outputs, &out->link);
	}
}

static void reg_global_remove(void *d, struct wl_registry *reg, uint32_t name)
{
	struct app *a = d;
	struct output *out, *tmp;
	wl_list_for_each_safe(out, tmp, &a->outputs, link) {
		if (out->global_name != name)
			continue;
		if (out == a->src || out == a->dst) {
			/* source or target output vanished; the watcher restarts us */
			LOGF(a, 0, "output %s removed, exiting", out->name);
			a->running = false;
		}
		wl_list_remove(&out->link);
		wl_output_release(out->wl);
		free(out);
	}
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_global_remove,
};

/* ---- xdg surface ---- */

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial)
{
	xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void top_configure(void *d, struct xdg_toplevel *top,
	int32_t w, int32_t h, struct wl_array *states)
{
	struct app *a = d;
	LOGF(a, 2, "toplevel configure %dx%d", w, h);
	if (w > 0 && h > 0) {
		a->win_w = w;
		a->win_h = h;
	}
}
static void top_close(void *d, struct xdg_toplevel *top)
{
	((struct app *)d)->running = false;
}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener top_listener = {
	.configure = top_configure, .close = top_close,
	.configure_bounds = top_bounds, .wm_capabilities = top_caps,
};

static void xsurf_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	struct app *a = d;
	xdg_surface_ack_configure(xs, serial);
	if (a->win_w <= 0 || a->win_h <= 0) {
		/* 0x0 = "you choose"; must commit a buffer to map at all */
		a->win_w = 1920;
		a->win_h = 1080;
	}
	a->configured = true;
	LOGF(a, 1, "configured %dx%d (%s)", a->win_w, a->win_h,
		a->win_w >= 2 * a->win_h ? "SBS" : "2D");
}
static const struct xdg_surface_listener xsurf_listener = {
	.configure = xsurf_configure,
};

/* ---- frame callback throttling ---- */

static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
	struct app *a = d;
	wl_callback_destroy(cb);
	a->frame_cb = NULL;
	cb_ms_total += ms_since(&swap_t1);
	cb_n++;
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };


static void rate_log(struct app *a)
{
	static struct timespec t0;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (!t0.tv_sec) {
		t0 = t;
		return;
	}
	double dt = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
	if (dt < 5.0)
		return;
	LOGF(a, 1, "rate: %.1f renders/s, %.1f captures/s, copy %.1f ms, "
		"swap %.1f ms, cb %.1f ms",
		n_render / dt, n_capture / dt,
		cap_ms_n ? cap_ms_total / cap_ms_n : 0.0,
		n_render ? swap_ms_total / n_render : 0.0,
		cb_n ? cb_ms_total / cb_n : 0.0);
	n_render = n_capture = 0;
	cap_ms_total = swap_ms_total = cb_ms_total = 0;
	cap_ms_n = cb_n = 0;
	t0 = t;
}

static void maybe_render(struct app *a)
{
	if (!a->configured || a->frame_cb)
		return;
	if (!a->cap.tex_valid && !a->cap.content_new)
		return; /* nothing to show yet */
	if (!a->cap.ready && !a->tree_dirty)
		return;
	/* the frame request must precede the commit inside render_frame */
	a->frame_cb = wl_surface_frame(a->surf);
	wl_callback_add_listener(a->frame_cb, &frame_listener, a);
	struct timespec r0;
	clock_gettime(CLOCK_MONOTONIC, &r0);
	if (!render_frame(a)) {
		/* no free swapchain buffer or no texture yet — retried on
		 * the next wakeup (buffer release / capture ready) */
		wl_callback_destroy(a->frame_cb);
		a->frame_cb = NULL;
		if (!a->cap.tex_valid)
			a->cap.ready = false; /* let a fresh capture start */
		return;
	}
	a->cap.ready = false;
	a->tree_dirty = false;
	swap_ms_total += ms_since(&r0);
	clock_gettime(CLOCK_MONOTONIC, &swap_t1);
	n_render++;
	rate_log(a);
}

/* ---- setup ---- */

static float envf(const char *name, float def)
{
	const char *v = getenv(name);
	return v && *v ? strtof(v, NULL) : def;
}

int main(int argc, char **argv)
{
	struct app a = {
		.src_name = "HEADLESS-1",
		.eye_sign = 1,
		.running = true,
		.drm_fd = -1,
	};
	wl_list_init(&a.outputs);
	a.pop = envf("GLASSES_DEPTH_POP", 8.0f);
	a.step = envf("GLASSES_DEPTH_STEP", 3.0f);
	a.floor_d = envf("GLASSES_DEPTH_FLOOR", 0.0f);
	if (envf("GLASSES_SWAP_EYES", 0) != 0)
		a.eye_sign = -1;
	a.log = (int)envf("GLASSES_PRESENTER_LOG", 1);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc)
			a.src_name = argv[++i];
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			a.dst_name = argv[++i];
		else {
			fprintf(stderr,
				"usage: glasses-presenter [-s source-output] [-o target-output]\n"
				"env: GLASSES_DEPTH_POP/STEP/FLOOR (px), GLASSES_SWAP_EYES,\n"
				"     GLASSES_PRESENTER_LOG\n");
			return !!strcmp(argv[i], "-h");
		}
	}

	a.dpy = wl_display_connect(NULL);
	if (!a.dpy) {
		fprintf(stderr, "presenter: no wayland display\n");
		return 1;
	}
	a.reg = wl_display_get_registry(a.dpy);
	wl_registry_add_listener(a.reg, &reg_listener, &a);
	wl_display_roundtrip(a.dpy);  /* globals */
	wl_display_roundtrip(a.dpy);  /* output names */

	if (!a.comp || !a.wm || !a.screencopy || !a.shm) {
		fprintf(stderr, "presenter: missing globals (compositor/xdg/screencopy/shm)\n");
		return 1;
	}

	struct output *out;
	wl_list_for_each(out, &a.outputs, link) {
		if (!strcmp(out->name, a.src_name))
			a.src = out;
		else if (a.dst_name ? !strcmp(out->name, a.dst_name) : !a.dst)
			a.dst = out;
	}
	if (!a.src || !a.dst) {
		fprintf(stderr, "presenter: outputs not found (source %s%s, target %s%s)\n",
			a.src_name, a.src ? " ok" : " MISSING",
			a.dst_name ? a.dst_name : "<auto>", a.dst ? " ok" : " MISSING");
		return 1;
	}
	LOGF(&a, 0, "source %s → target %s", a.src->name, a.dst->name);

	if (!render_init_egl(&a))
		return 1;
	capture_init(&a);
	if (!sway_connect(&a))
		return 1;

	xdg_wm_base_add_listener(a.wm, &wm_listener, &a);
	a.surf = wl_compositor_create_surface(a.comp);
	a.xsurf = xdg_wm_base_get_xdg_surface(a.wm, a.surf);
	xdg_surface_add_listener(a.xsurf, &xsurf_listener, &a);
	a.top = xdg_surface_get_toplevel(a.xsurf);
	xdg_toplevel_add_listener(a.top, &top_listener, &a);
	xdg_toplevel_set_app_id(a.top, "glasses-presenter");
	xdg_toplevel_set_title(a.top, "glasses-presenter");
	xdg_toplevel_set_fullscreen(a.top, a.dst->wl);
	wl_surface_commit(a.surf);

	sway_request_tree(&a);
	capture_start(&a);

	int wl_fd = wl_display_get_fd(a.dpy);
	while (a.running) {
		while (wl_display_prepare_read(a.dpy) != 0)
			wl_display_dispatch_pending(a.dpy);
		wl_display_flush(a.dpy);

		struct pollfd pfd[2] = {
			{ .fd = wl_fd, .events = POLLIN },
			{ .fd = a.ipc_fd, .events = POLLIN },
		};
		if (poll(pfd, 2, 200) < 0 && errno != EINTR) {
			wl_display_cancel_read(a.dpy);
			break;
		}
		if (pfd[0].revents & POLLIN)
			wl_display_read_events(a.dpy);
		else
			wl_display_cancel_read(a.dpy);
		wl_display_dispatch_pending(a.dpy);

		if (pfd[1].revents & (POLLIN | POLLHUP)) {
			if (!sway_handle_readable(&a)) {
				LOGF(&a, 0, "sway IPC closed, exiting");
				break;
			}
		}

		maybe_render(&a);
		/* pace captures at render rate (a free-running capture loop
		 * spins the compositor flat out), but start the next copy
		 * immediately after rendering so it overlaps presentation
		 * instead of serializing with it */
		if (!a.cap.frame && !a.cap.ready && !a.cap.content_new) {
			capture_start(&a);
			n_capture++;
		}
	}

	capture_destroy(&a);
	render_destroy(&a);
	wl_display_disconnect(a.dpy);
	return 0;
}
