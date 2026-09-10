// SPDX-License-Identifier: MIT
/*
 * 2D-core rotation of the presented frame (i.MX g2d).
 *
 * On SoCs whose display controller cannot rotate a plane (i.MX8MM LCDIF) and
 * whose 3D GPU pays several times the raster cost for a rotated scene
 * transform, the cheapest portrait presentation is: render the scene upright,
 * then rotate-blit it into a scanout buffer on the separate 2D core. The
 * rotator owns a small pool of scanout buffers and imports the render
 * surface's buffers through their dmabuf fds. libg2d is loaded at runtime, so
 * the feature is simply unavailable where the library is absent.
 */

#ifndef _FLUTTERPI_SRC_G2D_ROTATOR_H
#define _FLUTTERPI_SRC_G2D_ROTATOR_H

#include <stdint.h>

#include "modesetting.h"
#include "pixel_format.h"

struct gbm_device;
struct gbm_bo;
struct g2d_rotator;
struct g2d_rotator_fb;

/**
 * @brief Create a rotator that presents view-sized render buffers as
 * @p display_width x @p display_height scanout buffers rotated by @p rotation.
 *
 * @returns NULL (with a log line) if libg2d can't be loaded, the 2D core is not
 * available, or the scanout buffers can't be allocated.
 */
struct g2d_rotator *g2d_rotator_new(
    struct gbm_device *gbm_device,
    struct drmdev *drmdev,
    int display_width,
    int display_height,
    drm_plane_transform_t rotation,
    enum pixfmt format
);

void g2d_rotator_destroy(struct g2d_rotator *rotator);

/**
 * @brief Queue a rotate-blit of @p src into a free scanout buffer and return
 * without waiting. @p src must stay untouched until @ref g2d_rotator_fb_wait
 * has returned for the buffer.
 *
 * The scanout buffer stays locked until @ref g2d_rotator_fb_release is called
 * (from the KMS layer release callback).
 */
int g2d_rotator_blit_async(struct g2d_rotator *rotator, struct gbm_bo *src, struct g2d_rotator_fb **fb_out);

/**
 * @brief Wait until the blit queued into @p fb has landed. Cheap when called a
 * frame later, which is how the render surface uses it: the blit of frame N
 * runs on the 2D core while the 3D core renders frame N+1, and frame N is
 * scanned out when N+1 is presented.
 */
int g2d_rotator_fb_wait(struct g2d_rotator_fb *fb);

void g2d_rotator_fb_release(struct g2d_rotator_fb *fb);

uint32_t g2d_rotator_fb_get_drm_fb_id(const struct g2d_rotator_fb *fb);

uint64_t g2d_rotator_fb_get_modifier(const struct g2d_rotator_fb *fb);

/**
 * @brief The (opaque) pixel format of the scanout buffers.
 */
enum pixfmt g2d_rotator_get_format(const struct g2d_rotator *rotator);

/**
 * @brief The scanout (display) size the rotator was created with.
 */
void g2d_rotator_get_size(const struct g2d_rotator *rotator, int *width_out, int *height_out);

#endif  // _FLUTTERPI_SRC_G2D_ROTATOR_H
