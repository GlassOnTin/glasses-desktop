#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include "linux-dmabuf-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "presenter.h"

static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

bool capture_init(struct app *a)
{
	struct capture *c = &a->cap;
	c->drm_fd = -1;
	c->memfd = -1;

	p_eglCreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
	p_eglDestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
	p_glEGLImageTargetTexture2DOES =
		(void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

	if (!a->dmabuf || !p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES) {
		c->dmabuf_broken = true;
	} else {
		for (int i = 128; i < 132; i++) {
			char path[32];
			snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
			c->drm_fd = open(path, O_RDWR | O_CLOEXEC);
			if (c->drm_fd < 0)
				continue;
			c->gbm = gbm_create_device(c->drm_fd);
			if (c->gbm)
				break;
			close(c->drm_fd);
			c->drm_fd = -1;
		}
		if (!c->gbm) {
			LOGF(a, 0, "no gbm render node, using shm capture");
			c->dmabuf_broken = true;
		}
	}
	return true;
}

/* ---- dmabuf buffer ---- */

static bool ensure_dmabuf_buffer(struct app *a)
{
	struct capture *c = &a->cap;
	if (c->bo && c->bo_w == c->dma_w && c->bo_h == c->dma_h &&
	    c->bo_format == c->dma_format)
		return true;

	if (c->image) {
		p_eglDestroyImageKHR(a->edpy, c->image);
		c->image = NULL;
	}
	if (c->dma_buf) {
		wl_buffer_destroy(c->dma_buf);
		c->dma_buf = NULL;
	}
	if (c->bo) {
		gbm_bo_destroy(c->bo);
		c->bo = NULL;
	}

	c->bo = gbm_bo_create(c->gbm, c->dma_w, c->dma_h, c->dma_format,
		GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (!c->bo) {
		LOGF(a, 0, "gbm_bo_create %dx%d %08x failed", c->dma_w, c->dma_h,
			c->dma_format);
		return false;
	}
	uint32_t stride = gbm_bo_get_stride(c->bo);
	uint64_t mod = gbm_bo_get_modifier(c->bo);
	int fd = gbm_bo_get_fd(c->bo);
	if (fd < 0)
		return false;

	struct zwp_linux_buffer_params_v1 *params =
		zwp_linux_dmabuf_v1_create_params(a->dmabuf);
	zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride,
		mod >> 32, mod & 0xffffffff);
	c->dma_buf = zwp_linux_buffer_params_v1_create_immed(params,
		c->dma_w, c->dma_h, c->dma_format, 0);
	zwp_linux_buffer_params_v1_destroy(params);
	close(fd);

	c->bo_w = c->dma_w;
	c->bo_h = c->dma_h;
	c->bo_format = c->dma_format;
	c->need_reimport = true;
	LOGF(a, 1, "dmabuf capture buffer %dx%d fmt %08x mod %llx",
		c->bo_w, c->bo_h, c->bo_format, (unsigned long long)mod);
	return true;
}

/* ---- shm buffer ---- */

static bool ensure_shm_buffer(struct app *a)
{
	struct capture *c = &a->cap;
	if (c->shm_wlbuf && c->sb_w == c->shm_w && c->sb_h == c->shm_h &&
	    c->sb_stride == c->shm_stride && c->sb_format == c->shm_format)
		return true;

	if (c->shm_wlbuf) {
		wl_buffer_destroy(c->shm_wlbuf);
		c->shm_wlbuf = NULL;
	}
	if (c->shm_data) {
		munmap(c->shm_data, c->shm_size);
		c->shm_data = NULL;
	}
	if (c->memfd >= 0) {
		close(c->memfd);
		c->memfd = -1;
	}

	size_t size = (size_t)c->shm_stride * c->shm_h;
	c->memfd = memfd_create("glasses-presenter-shm", MFD_CLOEXEC);
	if (c->memfd < 0 || ftruncate(c->memfd, size) < 0)
		return false;
	c->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		c->memfd, 0);
	if (c->shm_data == MAP_FAILED) {
		c->shm_data = NULL;
		return false;
	}
	c->shm_size = size;

	struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, c->memfd, size);
	c->shm_wlbuf = wl_shm_pool_create_buffer(pool, 0, c->shm_w, c->shm_h,
		c->shm_stride, c->shm_format);
	wl_shm_pool_destroy(pool);

	c->sb_w = c->shm_w;
	c->sb_h = c->shm_h;
	c->sb_stride = c->shm_stride;
	c->sb_format = c->shm_format;
	LOGF(a, 1, "shm capture buffer %dx%d stride %d fmt %u",
		c->sb_w, c->sb_h, c->sb_stride, c->sb_format);
	return true;
}

/* ---- screencopy frame events ---- */

static void frame_shm_buffer(void *d, struct zwlr_screencopy_frame_v1 *f,
	uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
	struct app *a = d;
	a->cap.off_shm = true;
	a->cap.shm_format = format;
	a->cap.shm_w = width;
	a->cap.shm_h = height;
	a->cap.shm_stride = stride;
}

static void frame_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f,
	uint32_t format, uint32_t width, uint32_t height)
{
	struct app *a = d;
	a->cap.off_dmabuf = true;
	a->cap.dma_format = format;
	a->cap.dma_w = width;
	a->cap.dma_h = height;
}

static void frame_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f)
{
	struct app *a = d;
	struct capture *c = &a->cap;

	if (!c->dmabuf_broken && c->off_dmabuf && ensure_dmabuf_buffer(a)) {
		c->using_shm = false;
		zwlr_screencopy_frame_v1_copy(f, c->dma_buf);
	} else if (c->off_shm && ensure_shm_buffer(a)) {
		c->using_shm = true;
		zwlr_screencopy_frame_v1_copy(f, c->shm_wlbuf);
	} else {
		LOGF(a, 0, "no usable capture buffer offered");
		zwlr_screencopy_frame_v1_destroy(f);
		c->frame = NULL;
	}
}

static void frame_flags(void *d, struct zwlr_screencopy_frame_v1 *f,
	uint32_t flags)
{
	((struct app *)d)->cap.y_invert =
		flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_ready(void *d, struct zwlr_screencopy_frame_v1 *f,
	uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	struct app *a = d;
	zwlr_screencopy_frame_v1_destroy(f);
	a->cap.frame = NULL;
	a->cap.content_new = true;
	a->cap.ready = true;
}

static void frame_failed(void *d, struct zwlr_screencopy_frame_v1 *f)
{
	struct app *a = d;
	struct capture *c = &a->cap;
	zwlr_screencopy_frame_v1_destroy(f);
	c->frame = NULL;
	if (!c->using_shm && !c->dmabuf_broken) {
		LOGF(a, 0, "dmabuf copy failed, falling back to shm");
		c->dmabuf_broken = true;
	} else {
		LOGF(a, 1, "screencopy failed (transient)");
	}
}

static void frame_damage(void *d, struct zwlr_screencopy_frame_v1 *f,
	uint32_t x, uint32_t y, uint32_t w, uint32_t h) {}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_shm_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

void capture_start(struct app *a)
{
	struct capture *c = &a->cap;
	if (c->frame || !a->src)
		return;
	c->off_dmabuf = c->off_shm = false;
	c->frame = zwlr_screencopy_manager_v1_capture_output(a->screencopy,
		1 /* overlay cursor */, a->src->wl);
	zwlr_screencopy_frame_v1_add_listener(c->frame, &frame_listener, a);
}

/* GL-side upload/import; called from render_frame with GL current */
void capture_update_texture(struct app *a)
{
	struct capture *c = &a->cap;
	if (!c->content_new)
		return;
	c->content_new = false;

	if (!c->tex)
		glGenTextures(1, &c->tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, c->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	if (c->using_shm) {
		/* wl_shm XRGB/ARGB8888 is BGRA in memory; shader swaps R/B */
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->sb_w, c->sb_h, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, c->shm_data);
		c->tex_w = c->sb_w;
		c->tex_h = c->sb_h;
		c->tex_is_shm = true;
	} else {
		if (c->need_reimport) {
			if (c->image)
				p_eglDestroyImageKHR(a->edpy, c->image);
			int fd = gbm_bo_get_fd(c->bo);
			uint64_t mod = gbm_bo_get_modifier(c->bo);
			EGLint attrs[] = {
				EGL_WIDTH, c->bo_w,
				EGL_HEIGHT, c->bo_h,
				EGL_LINUX_DRM_FOURCC_EXT, (EGLint)c->bo_format,
				EGL_DMA_BUF_PLANE0_FD_EXT, fd,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
				EGL_DMA_BUF_PLANE0_PITCH_EXT,
					(EGLint)gbm_bo_get_stride(c->bo),
				EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
					(EGLint)(mod & 0xffffffff),
				EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
					(EGLint)(mod >> 32),
				EGL_NONE
			};
			c->image = p_eglCreateImageKHR(a->edpy, EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
			close(fd);
			if (!c->image) {
				LOGF(a, 0, "EGLImage import failed (0x%x), shm fallback",
					eglGetError());
				c->dmabuf_broken = true;
				return;
			}
			c->need_reimport = false;
		}
		p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, c->image);
		c->tex_w = c->bo_w;
		c->tex_h = c->bo_h;
		c->tex_is_shm = false;
	}
	c->tex_valid = true;
}

void capture_destroy(struct app *a)
{
	struct capture *c = &a->cap;
	if (c->frame)
		zwlr_screencopy_frame_v1_destroy(c->frame);
	if (c->image)
		p_eglDestroyImageKHR(a->edpy, c->image);
	if (c->dma_buf)
		wl_buffer_destroy(c->dma_buf);
	if (c->bo)
		gbm_bo_destroy(c->bo);
	if (c->gbm)
		gbm_device_destroy(c->gbm);
	if (c->drm_fd >= 0)
		close(c->drm_fd);
	if (c->shm_wlbuf)
		wl_buffer_destroy(c->shm_wlbuf);
	if (c->shm_data)
		munmap(c->shm_data, c->shm_size);
	if (c->memfd >= 0)
		close(c->memfd);
}
