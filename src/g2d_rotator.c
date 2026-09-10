// SPDX-License-Identifier: MIT
#define _GNU_SOURCE

#include "g2d_rotator.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>

#include "util/logging.h"

// The subset of NXP's g2d.h (BSD-3-Clause, i.MX Graphics 2D API) this file
// uses, declared locally so the library can be dlopen'ed and is not a build
// dependency. Layouts match g2d.h 2.5 on 64-bit Linux.
enum g2d_format {
    G2D_RGB565 = 0,
    G2D_RGBA8888 = 1,
    G2D_RGBX8888 = 2,
    G2D_BGRA8888 = 3,
    G2D_BGRX8888 = 4,
    G2D_BGR565 = 5,
    G2D_ARGB8888 = 6,
    G2D_ABGR8888 = 7,
    G2D_XRGB8888 = 8,
    G2D_XBGR8888 = 9,
};

enum g2d_blend_func { G2D_ZERO = 0, G2D_ONE = 1 };

enum g2d_rotation { G2D_ROTATION_0 = 0, G2D_ROTATION_90 = 1, G2D_ROTATION_180 = 2, G2D_ROTATION_270 = 3 };

enum g2d_feature { G2D_SCALING = 0, G2D_ROTATION };

typedef unsigned long g2d_phys_addr_t;

struct g2d_surface {
    enum g2d_format format;
    g2d_phys_addr_t planes[3];
    int left;
    int top;
    int right;
    int bottom;
    int stride;  // in pixels
    int width;
    int height;
    enum g2d_blend_func blendfunc;
    int global_alpha;
    int clrcolor;
    enum g2d_rotation rot;
};

struct g2d_buf {
    void *buf_handle;
    void *buf_vaddr;
    g2d_phys_addr_t buf_paddr;
    int buf_size;
};

struct g2d_api {
    void *lib;
    int (*open)(void **handle);
    int (*close)(void *handle);
    int (*blit)(void *handle, struct g2d_surface *src, struct g2d_surface *dst);
    int (*finish)(void *handle);
    int (*flush)(void *handle);
    int (*query_feature)(void *handle, enum g2d_feature feature, int *available);
    struct g2d_buf *(*buf_from_fd)(int fd);
    int (*free)(struct g2d_buf *buf);
    int (*create_fence_fd)(void *handle);  // optional
};

#define N_SCANOUT_BUFFERS 4
#define N_SOURCE_CACHE 8

struct g2d_rotator_fb {
    struct g2d_rotator *rotator;
    atomic_flag is_locked;
    struct gbm_bo *bo;
    int fd;
    struct g2d_buf *g2d_buf;
    uint32_t drm_fb_id;
    // Sync fd for the blit queued into this buffer, -1 once waited for (or if
    // the library can't create fences, in which case wait falls back to
    // g2d_finish).
    int fence_fd;
    bool blit_pending;
};

// A render-surface buffer imported into g2d. GBM surfaces cycle through a
// handful of buffers for their whole lifetime, so a tiny cache keyed on the
// bo pointer avoids importing per frame.
struct source_entry {
    struct gbm_bo *bo;
    int fd;
    struct g2d_buf *g2d_buf;
};

struct g2d_rotator {
    struct g2d_api api;
    void *handle;
    struct drmdev *drmdev;
    int width, height;
    enum pixfmt format;
    enum g2d_format g2d_format;
    int bytes_per_pixel;
    enum g2d_rotation rot;
    struct g2d_rotator_fb fbs[N_SCANOUT_BUFFERS];
    struct source_entry sources[N_SOURCE_CACHE];

    // FLUTTERPI_G2D_TIMING=1: log the blit issue + wait cost periodically.
    bool timing;
    long t_blit_us, t_finish_us;
    int n_timed;
};

static bool g2d_format_for_pixfmt(enum pixfmt format, enum g2d_format *out) {
    // DRM fourccs name a little-endian word; g2d names the byte order in memory.
    switch (get_pixfmt_info(format)->drm_format) {
        case DRM_FORMAT_ARGB8888: *out = G2D_BGRA8888; return true;
        case DRM_FORMAT_XRGB8888: *out = G2D_BGRX8888; return true;
        case DRM_FORMAT_ABGR8888: *out = G2D_RGBA8888; return true;
        case DRM_FORMAT_XBGR8888: *out = G2D_RGBX8888; return true;
        default: return false;
    }
}

static int load_api(struct g2d_api *api) {
    api->lib = dlopen("libg2d.so.2", RTLD_NOW | RTLD_LOCAL);
    if (api->lib == NULL) {
        LOG_DEBUG("libg2d.so.2 not available, no 2D-core rotation. dlopen: %s\n", dlerror());
        return ENOENT;
    }

#define LOAD(name, sym)                                                                     \
    do {                                                                                    \
        *(void **) &api->name = dlsym(api->lib, sym);                                       \
        if (api->name == NULL) {                                                            \
            LOG_ERROR("libg2d.so.2 has no %s, no 2D-core rotation.\n", sym);                \
            dlclose(api->lib);                                                              \
            return ENOENT;                                                                  \
        }                                                                                   \
    } while (0)
    LOAD(open, "g2d_open");
    LOAD(close, "g2d_close");
    LOAD(blit, "g2d_blit");
    LOAD(finish, "g2d_finish");
    LOAD(flush, "g2d_flush");
    LOAD(query_feature, "g2d_query_feature");
    LOAD(buf_from_fd, "g2d_buf_from_fd");
    LOAD(free, "g2d_free");
#undef LOAD
    *(void **) &api->create_fence_fd = dlsym(api->lib, "g2d_create_fence_fd");
    return 0;
}

static void fb_deinit(struct g2d_rotator *r, struct g2d_rotator_fb *fb) {
    if (fb->drm_fb_id != 0) {
        drmdev_rm_fb(r->drmdev, fb->drm_fb_id);
    }
    if (fb->g2d_buf != NULL) {
        r->api.free(fb->g2d_buf);
    }
    if (fb->fd >= 0) {
        close(fb->fd);
    }
    if (fb->bo != NULL) {
        gbm_bo_destroy(fb->bo);
    }
}

static int fb_init(struct g2d_rotator *r, struct gbm_device *gbm_device, struct g2d_rotator_fb *fb) {
    uint32_t gbm_format;

    fb->rotator = r;
    atomic_flag_clear(&fb->is_locked);
    fb->bo = NULL;
    fb->fd = -1;
    fb->g2d_buf = NULL;
    fb->drm_fb_id = 0;
    fb->fence_fd = -1;
    fb->blit_pending = false;

    gbm_format = get_pixfmt_info(r->format)->gbm_format;
    fb->bo = gbm_bo_create(gbm_device, r->width, r->height, gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    if (fb->bo == NULL) {
        fb->bo = gbm_bo_create(gbm_device, r->width, r->height, gbm_format, GBM_BO_USE_SCANOUT);
    }
    if (fb->bo == NULL) {
        LOG_ERROR("Could not allocate a %dx%d scanout buffer for 2D-core rotation. gbm_bo_create: %s\n", r->width, r->height, strerror(errno));
        return errno ? errno : EIO;
    }

    fb->fd = gbm_bo_get_fd(fb->bo);
    if (fb->fd < 0) {
        LOG_ERROR("Could not export scanout buffer as dmabuf. gbm_bo_get_fd: %s\n", strerror(errno));
        goto fail;
    }

    fb->g2d_buf = r->api.buf_from_fd(fb->fd);
    if (fb->g2d_buf == NULL) {
        LOG_ERROR("Could not import scanout buffer into g2d. g2d_buf_from_fd failed.\n");
        goto fail;
    }

    fb->drm_fb_id = drmdev_add_fb_from_gbm_bo(r->drmdev, fb->bo, /* cast_opaque */ true);
    if (fb->drm_fb_id == 0) {
        LOG_ERROR("Could not add scanout buffer as DRM framebuffer.\n");
        goto fail;
    }

    return 0;

fail:
    fb_deinit(r, fb);
    return EIO;
}

struct g2d_rotator *g2d_rotator_new(
    struct gbm_device *gbm_device,
    struct drmdev *drmdev,
    int display_width,
    int display_height,
    drm_plane_transform_t rotation,
    enum pixfmt format
) {
    struct g2d_rotator *r;
    const char *env;
    int available, ok;

    r = calloc(1, sizeof *r);
    if (r == NULL) {
        return NULL;
    }

    ok = load_api(&r->api);
    if (ok != 0) {
        goto fail_free;
    }

    if (!g2d_format_for_pixfmt(format, &r->g2d_format)) {
        LOG_ERROR("Pixel format %s has no g2d equivalent, no 2D-core rotation.\n", get_pixfmt_info(format)->name);
        goto fail_close_lib;
    }

    ok = r->api.open(&r->handle);
    if (ok != 0) {
        LOG_ERROR("Could not open the 2D core. g2d_open: %d\n", ok);
        goto fail_close_lib;
    }

    available = 0;
    ok = r->api.query_feature(r->handle, G2D_ROTATION, &available);
    if (ok != 0 || !available) {
        LOG_ERROR("The 2D core reports no rotation support, no 2D-core rotation.\n");
        goto fail_close_g2d;
    }

    r->drmdev = drmdev_ref(drmdev);
    r->width = display_width;
    r->height = display_height;
    r->format = format;
    r->bytes_per_pixel = get_pixfmt_info(format)->bits_per_pixel / 8;

    if (rotation.rotate_90) {
        r->rot = G2D_ROTATION_90;
    } else if (rotation.rotate_180) {
        r->rot = G2D_ROTATION_180;
    } else if (rotation.rotate_270) {
        r->rot = G2D_ROTATION_270;
    } else {
        r->rot = G2D_ROTATION_0;
    }
    // Bring-up aid: g2d's rotation sense differs between drivers.
    env = getenv("FLUTTERPI_G2D_TIMING");
    r->timing = env != NULL && strcmp(env, "1") == 0;

    env = getenv("FLUTTERPI_G2D_ROTATION");
    if (env != NULL && *env != '\0') {
        r->rot = (enum g2d_rotation) (atoi(env) & 3);
    }

    for (int i = 0; i < N_SCANOUT_BUFFERS; i++) {
        ok = fb_init(r, gbm_device, &r->fbs[i]);
        if (ok != 0) {
            for (int j = 0; j < i; j++) {
                fb_deinit(r, &r->fbs[j]);
            }
            goto fail_unref_drmdev;
        }
    }

    for (int i = 0; i < N_SOURCE_CACHE; i++) {
        r->sources[i].bo = NULL;
        r->sources[i].fd = -1;
        r->sources[i].g2d_buf = NULL;
    }

    LOG_DEBUG("Presenting through the 2D core: %dx%d, g2d rotation %d.\n", display_width, display_height, r->rot);
    return r;

fail_unref_drmdev:
    drmdev_unref(r->drmdev);

fail_close_g2d:
    r->api.close(r->handle);

fail_close_lib:
    dlclose(r->api.lib);

fail_free:
    free(r);
    return NULL;
}

void g2d_rotator_destroy(struct g2d_rotator *r) {
    for (int i = 0; i < N_SOURCE_CACHE; i++) {
        if (r->sources[i].g2d_buf != NULL) {
            r->api.free(r->sources[i].g2d_buf);
        }
        if (r->sources[i].fd >= 0) {
            close(r->sources[i].fd);
        }
    }
    for (int i = 0; i < N_SCANOUT_BUFFERS; i++) {
        fb_deinit(r, &r->fbs[i]);
    }
    r->api.close(r->handle);
    drmdev_unref(r->drmdev);
    dlclose(r->api.lib);
    free(r);
}

static struct g2d_buf *import_source(struct g2d_rotator *r, struct gbm_bo *bo) {
    struct source_entry *entry = NULL;

    for (int i = 0; i < N_SOURCE_CACHE; i++) {
        if (r->sources[i].bo == bo) {
            return r->sources[i].g2d_buf;
        }
        if (entry == NULL && r->sources[i].bo == NULL) {
            entry = &r->sources[i];
        }
    }

    if (entry == NULL) {
        LOG_ERROR("More than %d distinct render buffers seen, can't cache another for the 2D core.\n", N_SOURCE_CACHE);
        return NULL;
    }

    entry->fd = gbm_bo_get_fd(bo);
    if (entry->fd < 0) {
        LOG_ERROR("Could not export render buffer as dmabuf. gbm_bo_get_fd: %s\n", strerror(errno));
        return NULL;
    }

    entry->g2d_buf = r->api.buf_from_fd(entry->fd);
    if (entry->g2d_buf == NULL) {
        LOG_ERROR("Could not import render buffer into g2d. g2d_buf_from_fd failed.\n");
        close(entry->fd);
        entry->fd = -1;
        return NULL;
    }

    entry->bo = bo;
    return entry->g2d_buf;
}

int g2d_rotator_blit_async(struct g2d_rotator *r, struct gbm_bo *src_bo, struct g2d_rotator_fb **fb_out) {
    struct g2d_rotator_fb *fb = NULL;
    struct g2d_buf *src_buf;
    struct g2d_surface src, dst;
    struct timespec t0, t1;
    int ok;

    for (int i = 0; i < N_SCANOUT_BUFFERS; i++) {
        if (atomic_flag_test_and_set(&r->fbs[i].is_locked) == false) {
            fb = &r->fbs[i];
            break;
        }
    }
    if (fb == NULL) {
        LOG_ERROR("All %d scanout buffers are still queued for scanout.\n", N_SCANOUT_BUFFERS);
        return EBUSY;
    }

    src_buf = import_source(r, src_bo);
    if (src_buf == NULL) {
        ok = EIO;
        goto fail_release;
    }

    memset(&src, 0, sizeof src);
    src.format = r->g2d_format;
    src.planes[0] = src_buf->buf_paddr;
    src.left = 0;
    src.top = 0;
    src.right = (int) gbm_bo_get_width(src_bo);
    src.bottom = (int) gbm_bo_get_height(src_bo);
    src.stride = (int) gbm_bo_get_stride(src_bo) / r->bytes_per_pixel;
    src.width = src.right;
    src.height = src.bottom;
    src.blendfunc = G2D_ONE;
    src.global_alpha = 255;
    src.rot = G2D_ROTATION_0;

    memset(&dst, 0, sizeof dst);
    dst.format = r->g2d_format;
    dst.planes[0] = fb->g2d_buf->buf_paddr;
    dst.left = 0;
    dst.top = 0;
    dst.right = r->width;
    dst.bottom = r->height;
    dst.stride = (int) gbm_bo_get_stride(fb->bo) / r->bytes_per_pixel;
    dst.width = r->width;
    dst.height = r->height;
    dst.blendfunc = G2D_ZERO;
    dst.global_alpha = 255;
    dst.rot = r->rot;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    ok = r->api.blit(r->handle, &src, &dst);
    if (ok != 0) {
        LOG_ERROR("2D-core rotate blit failed. g2d_blit: %d\n", ok);
        ok = EIO;
        goto fail_release;
    }

    fb->fence_fd = -1;
    if (r->api.create_fence_fd != NULL) {
        fb->fence_fd = r->api.create_fence_fd(r->handle);
    }
    if (fb->fence_fd < 0) {
        // No fence: make sure the blit is at least submitted, the wait uses g2d_finish.
        r->api.flush(r->handle);
    }
    fb->blit_pending = true;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (r->timing) {
        r->t_blit_us += (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    }

    *fb_out = fb;
    return 0;

fail_release:
    atomic_flag_clear(&fb->is_locked);
    return ok;
}

int g2d_rotator_fb_wait(struct g2d_rotator_fb *fb) {
    struct g2d_rotator *r = fb->rotator;
    struct timespec t0, t1;
    int ok = 0;

    if (!fb->blit_pending) {
        return 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (fb->fence_fd >= 0) {
        struct pollfd pfd = { .fd = fb->fence_fd, .events = POLLIN };
        int n = poll(&pfd, 1, 200);
        if (n <= 0) {
            LOG_ERROR("2D-core rotate blit did not signal its fence within 200 ms%s.\n", n < 0 ? strerror(errno) : "");
            ok = ETIMEDOUT;
        }
        close(fb->fence_fd);
        fb->fence_fd = -1;
    } else {
        ok = r->api.finish(r->handle);
        if (ok != 0) {
            LOG_ERROR("2D-core rotate blit did not complete. g2d_finish: %d\n", ok);
            ok = EIO;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fb->blit_pending = false;

    if (r->timing) {
        r->t_finish_us += (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
        if (++r->n_timed == 120) {
            LOG_ERROR("g2d rotate: issue %ld us + wait %ld us per frame (avg of 120, %s)\n", r->t_blit_us / 120, r->t_finish_us / 120, r->api.create_fence_fd ? "fence" : "finish");
            r->n_timed = 0;
            r->t_blit_us = 0;
            r->t_finish_us = 0;
        }
    }

    return ok;
}

void g2d_rotator_fb_release(struct g2d_rotator_fb *fb) {
    if (fb->fence_fd >= 0) {
        close(fb->fence_fd);
        fb->fence_fd = -1;
    }
    fb->blit_pending = false;
    atomic_flag_clear(&fb->is_locked);
}

uint32_t g2d_rotator_fb_get_drm_fb_id(const struct g2d_rotator_fb *fb) {
    return fb->drm_fb_id;
}

uint64_t g2d_rotator_fb_get_modifier(const struct g2d_rotator_fb *fb) {
    return gbm_bo_get_modifier(fb->bo);
}

enum pixfmt g2d_rotator_get_format(const struct g2d_rotator *r) {
    return pixfmt_opaque(r->format);
}

void g2d_rotator_get_size(const struct g2d_rotator *r, int *width_out, int *height_out) {
    *width_out = r->width;
    *height_out = r->height;
}
