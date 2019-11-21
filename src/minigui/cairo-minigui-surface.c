/* Cairo - a vector graphics library with display and print output
 *
 * Copyright (C) 2019 FMSoft Technologies
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is FMSoft Technologies
 *
 * Contributor(s):
 *        WEI Yongming <vincent@minigui.org>
 */

#include "cairoint.h"

#include "cairo-minigui.h"

#if CAIRO_HAS_MINIGUI_SURFACE

#include "cairo-clip-private.h"
#include "cairo-image-surface-inline.h"
#include "cairo-damage-private.h"
#include "cairo-surface-fallback-private.h"
#include "cairo-default-context-private.h"

#include <minigui/common.h>
#include <minigui/minigui.h>
#include <minigui/gdi.h>

/**
 * SECTION:cairo-minigui
 * @Title: MiniGUI Surfaces
 * @Short_Description: MiniGUI surface support
 * @See_Also: #cairo_surface_t
 *
 * The MiniGUI surface is used to render cairo graphics to
 * MiniGUI graphics device contexts.
 *
 * The surface returned by the constructors is of surface type
 * %CAIRO_SURFACE_TYPE_MINIGUI and is a raster surface type.
 **/

/**
 * CAIRO_HAS_MINIGUI_SURFACE:
 *
 * Defined if the MiniGUI surface backend is available.
 * This macro can be used to conditionally compile backend-specific code.
 *
 * Since: 2.18
 **/

static const cairo_surface_backend_t cairo_minigui_surface_backend;

typedef struct _cairo_minigui_surface {
    cairo_surface_t base;

    cairo_format_t format;

    /* We always create off-screen surface as memory DC */
    HDC dc;

    /* We construct the BITMAP structure with the attributes of the DC */
    BITMAP bitmap;

    /* We also construct an equivalent image surface */
    cairo_surface_t *image;

    /* Use fallback surface for non memory DC */
    cairo_surface_t *fallback;

    cairo_rectangle_int_t extents;

#ifdef _SAVE_INITIAL_CLIP
    /* Initial clip bits
     * We need these kept around so that we maintain
     * whatever clip was set on the original DC at creation
     * time when cairo is asked to reset the surface clip.
     */
    RECT clip_rect;
    PCLIPRGN initial_clip_rgn;
    cairo_bool_t had_simple_clip;
#endif

    cairo_bool_t new_memdc;
} cairo_minigui_surface_t;

#define to_minigui_surface(S) ((cairo_minigui_surface_t *)(S))

static cairo_status_t
_cairo_minigui_print_gdi_error (const char *context)
{
    fprintf (stderr, "%s: MiniGUI GDI error.\n", context);
    return _cairo_error (CAIRO_STATUS_NO_MEMORY);
}

static cairo_format_t
_cairo_format_from_dc (HDC dc)
{
    switch (GetGDCapability(dc, GDCAP_DEPTH)) {
    case 8:
        return CAIRO_FORMAT_A8;
        break;

    case 16:
        return CAIRO_FORMAT_RGB16_565;
        break;

    case 24:
        return CAIRO_FORMAT_RGB24;
        break;

    case 32:
        if (GetGDCapability(dc, GDCAP_AMASK)) {
            return CAIRO_FORMAT_ARGB32;
        }
        else {
            return CAIRO_FORMAT_RGB24;
        }
        break;

    default:
        break;
    }

    return CAIRO_FORMAT_INVALID;
}

#ifdef _SAVE_INITIAL_CLIP
static cairo_int_status_t
_cairo_minigui_save_initial_clip (HDC hdc, cairo_minigui_surface_t *surface)
{
    int clipBoxType;

    clipBoxType = GetClipBox (hdc, &surface->clip_rect);
    if (clipBoxType < 0) {
        _cairo_minigui_print_gdi_error ("cairo_minigui_surface_create");
        return _cairo_error (CAIRO_STATUS_NO_MEMORY);
    }

    surface->initial_clip_rgn = NULL;
    surface->had_simple_clip = FALSE;

    if (clipBoxType == COMPLEXREGION) {
        surface->initial_clip_rgn = CreateClipRgn ();
        if (GetClipRegion (hdc, surface->initial_clip_rgn) <= 0) {
            DestroyClipRgn (surface->initial_clip_rgn);
            surface->initial_clip_rgn = NULL;
        }
    } else {
        surface->had_simple_clip = TRUE;
    }

    return CAIRO_STATUS_SUCCESS;
}

static cairo_int_status_t
_cairo_minigui_restore_initial_clip (cairo_minigui_surface_t *surface)
{
    cairo_int_status_t status = CAIRO_STATUS_SUCCESS;

    /* initial_clip_rgn will either be a real region or NULL
       (which means reset to no clip region) */
    SelectClipRegion (surface->dc, surface->initial_clip_rgn);

    if (surface->had_simple_clip) {
        /* then if we had a simple clip, intersect */
        ClipRectIntersect (surface->dc, &surface->clip_rect);
    }

    return status;
}
#else
static inline cairo_int_status_t
_cairo_minigui_save_initial_clip (HDC hdc, cairo_minigui_surface_t *surface)
{
    return CAIRO_STATUS_SUCCESS;
}

static inline cairo_int_status_t
_cairo_minigui_restore_initial_clip (cairo_minigui_surface_t *surface)
{
    return CAIRO_STATUS_SUCCESS;
}
#endif

static PBITMAP
construct_bmp_from_dc (HDC memdc, PBITMAP bmp)
{
    RECT rc = {0, 0, 1, 1};

    bmp->bmType = BMP_TYPE_NORMAL;
    bmp->bmBitsPerPixel = GetGDCapability (memdc, GDCAP_BITSPP);
    bmp->bmBytesPerPixel = GetGDCapability (memdc, GDCAP_BPP);
    bmp->bmAlpha = 0;
    bmp->bmColorKey = 0;
    bmp->bmWidth = GetGDCapability (memdc, GDCAP_HPIXEL);
    bmp->bmHeight = GetGDCapability (memdc, GDCAP_VPIXEL);
    bmp->bmBits = LockDC (memdc, &rc, NULL, NULL, (int*)&bmp->bmPitch);
    UnlockDC (memdc);

    return bmp;
}

static HDC
_create_memdc (cairo_format_t   format,
               int              width,
               int              height)
{
    HDC dc = HDC_INVALID;

    switch (format) {
    case CAIRO_FORMAT_RGB16_565:
        dc = CreateMemDC (width, height,
                        16, MEMDC_FLAG_HWSURFACE,
                        0xF800, 0x07E0, 0x001F, 0x0000);
        break;

    case CAIRO_FORMAT_RGB24:
        /* We treat RGB24 format like 32bpp. */
        dc = CreateMemDC (width, height,
                        32, MEMDC_FLAG_HWSURFACE,
                        0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000);
        break;

    case CAIRO_FORMAT_ARGB32:
        dc = CreateMemDC (width, height,
                        32, MEMDC_FLAG_HWSURFACE,
                        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        break;

    case CAIRO_FORMAT_A8:
        dc = CreateMemDC (width, height,
                        8, MEMDC_FLAG_HWSURFACE,
                        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        {
            int i;
            GAL_Color cmap [256];

            for (i = 0; i < 256; i++) {
                cmap[i].r = i;
                cmap[i].g = i;
                cmap[i].b = i;
                cmap[i].a = 0;
            }
            SetPalette (dc, 0, 256, cmap);
        }
        break;

    case CAIRO_FORMAT_RGB30:
    case CAIRO_FORMAT_RGB96F:
    case CAIRO_FORMAT_RGBA128F:
    case CAIRO_FORMAT_A1:
    case CAIRO_FORMAT_INVALID:
        return HDC_SCREEN;
    }

    return dc;
}

static cairo_status_t
_create_memdc_and_bitmap (cairo_minigui_surface_t *surface,
                          cairo_format_t           format,
                          int                      width,
                          int                      height,
                          unsigned char          **bits_out,
                          int                     *rowstride_out)
{
    cairo_status_t status;

    width = (width <= 0) ? 1 : width;
    height = (height <= 0) ? 1 : height;
    surface->dc = _create_memdc (format, width, height);

    if (surface->dc == HDC_SCREEN)
        return CAIRO_STATUS_INVALID_FORMAT;
    else if (surface->dc == HDC_INVALID)
        goto FAIL;

    construct_bmp_from_dc (surface->dc, &surface->bitmap);
    if (bits_out)
        *bits_out = surface->bitmap.bmBits;
    if (rowstride_out)
        *rowstride_out = surface->bitmap.bmPitch;

    return CAIRO_STATUS_SUCCESS;

FAIL:
    status = _cairo_minigui_print_gdi_error (__func__);

    if (surface->dc && HDC_INVALID != surface->dc) {
        DeleteMemDC (surface->dc);
        surface->dc = HDC_INVALID;
    }

    return status;
}

static cairo_surface_t *
_cairo_minigui_surface_create_internal (cairo_format_t  format,
                                        int             width,
                                        int             height)
{
    cairo_status_t status;
    cairo_minigui_surface_t *surface;
    unsigned char *bits = NULL;
    int rowstride = 0;

    surface = _cairo_malloc (sizeof (*surface));
    if (surface == NULL)
        return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_NO_MEMORY));

    surface->fallback = NULL;

    status = _create_memdc_and_bitmap (surface, format,
                    width, height, &bits, &rowstride);
    if (status)
        goto FAIL;

    surface->new_memdc = TRUE;
    surface->image = cairo_image_surface_create_for_data (bits, format,
                              width, height, rowstride);
    status = surface->image->status;
    if (status)
        goto FAIL;

    _cairo_image_surface_set_parent (to_image_surface(surface->image),
                     &surface->base);

    surface->format = format;

    surface->extents.x = 0;
    surface->extents.y = 0;
    surface->extents.width = width;
    surface->extents.height = height;

#ifdef _SAVE_INITIAL_CLIP
    surface->initial_clip_rgn = NULL;
    surface->had_simple_clip = FALSE;
#endif

    _cairo_surface_init (&surface->base,
             &cairo_minigui_surface_backend,
             NULL,
             _cairo_content_from_format (format),
             FALSE); /* is_vector */

    return &surface->base;

FAIL:
    if (surface->bitmap.bmBits) {
        DeleteMemDC (surface->dc);
    }
    free (surface);

    return _cairo_surface_create_in_error (status);
}

static cairo_surface_t *
_cairo_minigui_surface_create_on_dc (cairo_device_t* device,
                                     HDC             memdc,
                                     cairo_format_t  format,
                                     cairo_bool_t    is_new)
{
    cairo_status_t status;
    cairo_minigui_surface_t *surface;

    surface = _cairo_malloc (sizeof (*surface));
    if (surface == NULL)
        return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_NO_MEMORY));

    surface->dc = memdc;
    surface->new_memdc = is_new;
    surface->format = format;
    surface->fallback = NULL;

    construct_bmp_from_dc (surface->dc, &surface->bitmap);
    surface->image = cairo_image_surface_create_for_data (surface->bitmap.bmBits,
                                format,
                                surface->bitmap.bmWidth,
                                surface->bitmap.bmHeight,
                                surface->bitmap.bmPitch);

    status = surface->image->status;
    if (status)
        goto FAIL;

    _cairo_image_surface_set_parent (to_image_surface(surface->image),
                     &surface->base);

    surface->extents.x = 0;
    surface->extents.y = 0;
    surface->extents.width = surface->bitmap.bmWidth;
    surface->extents.height = surface->bitmap.bmHeight;

#ifdef _SAVE_INITIAL_CLIP
    surface->initial_clip_rgn = NULL;
    surface->had_simple_clip = FALSE;
#endif

    _cairo_surface_init (&surface->base,
             &cairo_minigui_surface_backend,
             device,
             _cairo_content_from_format (format),
             FALSE); /* is_vector */

    return &surface->base;

FAIL:
    free (surface);

    return _cairo_surface_create_in_error (status);
}

static cairo_surface_t *
_cairo_minigui_surface_create_similar (void            *abstract_src,
                                       cairo_content_t  content,
                                       int              width,
                                       int              height)
{
    cairo_device_t* device;
    cairo_minigui_surface_t *src = abstract_src;
    cairo_format_t format = _cairo_format_from_content (content);
    cairo_surface_t *new_surf = NULL;

    device = cairo_surface_get_device ((cairo_surface_t*)abstract_src);
    if (!(content & CAIRO_CONTENT_ALPHA)) {
        new_surf = cairo_minigui_surface_create_with_memdc (device,
                format, width, height);

        if (new_surf->status) {
            cairo_surface_destroy (new_surf);
            new_surf = NULL;
        }
    }

    if (new_surf == NULL) {
        new_surf = cairo_minigui_surface_create_with_memdc_similar (device,
                        src->dc, width, height);
    }

    return new_surf;
}

static cairo_surface_t *
_cairo_minigui_surface_create_similar_image (void          *abstract_other,
                                             cairo_format_t format,
                                             int            width,
                                             int            height)
{
    cairo_minigui_surface_t *surface = abstract_other;
    cairo_image_surface_t *image;

    surface = (cairo_minigui_surface_t *)
    _cairo_minigui_surface_create_internal (format, width, height);
    if (surface->base.status)
        return &surface->base;

    /* And clear in order to comply with our user API semantics */
    image = (cairo_image_surface_t *) surface->image;
    if (! image->base.is_clear) {
        memset (image->data, 0, image->stride * height);
        image->base.is_clear = TRUE;
    }

    return &image->base;
}

static void
_cairo_minigui_surface_discard_fallback (cairo_minigui_surface_t *surface)
{
    if (surface->fallback) {
        TRACE ((stderr, "%s (surface=%d)\n",
            __func__, surface->base.unique_id));

        cairo_surface_finish (surface->fallback);
        cairo_surface_destroy (surface->fallback);
        surface->fallback = NULL;
    }
}

static cairo_status_t
_cairo_minigui_surface_finish (void *abstract_surface)
{
    cairo_minigui_surface_t *surface = abstract_surface;

    if (surface->image && to_image_surface(surface->image)->parent) {
        assert (to_image_surface(surface->image)->parent == &surface->base);
        /* Unhook ourselves first to avoid the double-unref from the image */
        to_image_surface(surface->image)->parent = NULL;
        cairo_surface_finish (surface->image);
        cairo_surface_destroy (surface->image);
    }

    /* If we created the Bitmap and DC, destroy them */
    if (surface->new_memdc) {
        DeleteMemDC (surface->dc);
    }
    else {
        _cairo_minigui_restore_initial_clip (surface);
    }

#ifdef _SAVE_INITIAL_CLIP
    if (surface->initial_clip_rgn)
        DestroyClipRgn (surface->initial_clip_rgn);
#endif

    _cairo_minigui_surface_discard_fallback (surface);

    return CAIRO_STATUS_SUCCESS;
}

static cairo_image_surface_t *
_cairo_minigui_surface_map_to_image (void                        *abs_surface,
                                     const cairo_rectangle_int_t *extents)
{
    cairo_minigui_surface_t *surface = abs_surface;
    cairo_status_t status;

    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, surface->base.unique_id));

    if (surface->image)
        goto done;

    if (surface->fallback == NULL) {
        surface->fallback =
            _cairo_minigui_surface_create_internal (surface->format,
                                surface->extents.x + surface->extents.width,
                                surface->extents.y + surface->extents.height);
        if (unlikely (status = surface->fallback->status))
            goto err;

        BitBlt (to_minigui_surface(surface->fallback)->dc,
                 surface->extents.x, surface->extents.y,
                 surface->extents.width,
                 surface->extents.height,
                 surface->dc,
                 surface->extents.x,
                 surface->extents.y,
                 0);
#if 0
        status = _cairo_error (CAIRO_STATUS_DEVICE_ERROR);
        goto err;
#endif
    }

    surface = to_minigui_surface (surface->fallback);
done:
    return _cairo_surface_map_to_image (surface->image, extents);

err:
    cairo_surface_destroy (surface->fallback);
    surface->fallback = NULL;

    return _cairo_image_surface_create_in_error (status);
}

static cairo_int_status_t
_cairo_minigui_surface_unmap_image (void                    *abstract_surface,
                                    cairo_image_surface_t   *image)
{
    cairo_minigui_surface_t *surface = abstract_surface;

    /* Delay the download until the next flush, which means we also need
     * to make sure our sources rare flushed.
     */
    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, to_minigui_surface(surface)->base.unique_id));

    if (surface->fallback) {
        cairo_rectangle_int_t r;

        r.x = image->base.device_transform_inverse.x0;
        r.y = image->base.device_transform_inverse.y0;
        r.width  = image->width;
        r.height = image->height;

        TRACE ((stderr, "%s: adding damage (%d,%d)x(%d,%d)\n",
            __func__, r.x, r.y, r.width, r.height));
        surface->fallback->damage =
            _cairo_damage_add_rectangle (surface->fallback->damage, &r);
        surface = to_minigui_surface (surface->fallback);
    }

    return _cairo_surface_unmap_image (surface->image, image);
}

static cairo_bool_t
_cairo_minigui_surface_get_extents (void                    *abstract_surface,
                                    cairo_rectangle_int_t   *rectangle)
{
    cairo_minigui_surface_t *surface = abstract_surface;

    *rectangle = surface->extents;
    return TRUE;
}

static cairo_status_t
_cairo_minigui_surface_flush (void *abstract_surface, unsigned flags)
{
    cairo_minigui_surface_t *surface = abstract_surface;
    cairo_status_t status = CAIRO_STATUS_SUCCESS;

    if (flags)
        return CAIRO_STATUS_SUCCESS;

    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, surface->base.unique_id));
    if (surface->fallback == NULL)
        return CAIRO_STATUS_SUCCESS;

    if (surface->fallback->damage) {
        cairo_minigui_surface_t *fallback;
        cairo_damage_t *damage;

        damage = _cairo_damage_reduce (surface->fallback->damage);
        surface->fallback->damage = NULL;

        fallback = to_minigui_surface (surface->fallback);
        assert (fallback->image);

        TRACE ((stderr, "%s: flushing damage x %d\n", __func__,
            damage->region ? cairo_region_num_rectangles (damage->region) : 0));

        if (damage->status) {
            BitBlt (surface->dc,
                 surface->extents.x,
                 surface->extents.y,
                 surface->extents.width,
                 surface->extents.height,
                 fallback->dc,
                 surface->extents.x, surface->extents.y,
                 0);
    //        status = _cairo_minigui_print_gdi_error (__func__);
        } else if (damage->region) {
            int n = cairo_region_num_rectangles (damage->region), i;
            for (i = 0; i < n; i++) {
                cairo_rectangle_int_t rect;

                cairo_region_get_rectangle (damage->region, i, &rect);
                TRACE ((stderr, "%s: damage (%d,%d)x(%d,%d)\n", __func__,
                    rect.x, rect.y,
                    rect.width, rect.height));
                BitBlt (surface->dc,
                         rect.x,
                         rect.y,
                         rect.width, rect.height,
                         fallback->dc,
                         rect.x, rect.y,
                         0);
        //            status = _cairo_minigui_print_gdi_error (__func__);
        //            break;
            }
        }
        _cairo_damage_destroy (damage);
    }
    else {
        cairo_surface_destroy (surface->fallback);
        surface->fallback = NULL;
    }

    return status;
}

static cairo_status_t
_cairo_minigui_surface_mark_dirty (void *abstract_surface,
                     int x, int y, int width, int height)
{
    _cairo_minigui_surface_discard_fallback (abstract_surface);
    return CAIRO_STATUS_SUCCESS;
}

#if 0
static cairo_int_status_t
_cairo_minigui_surface_paint (void                  *surface,
                              cairo_operator_t       op,
                              const cairo_pattern_t *source,
                              const cairo_clip_t    *clip)
{
    return _cairo_surface_fallback_paint (surface, op, source, clip);
}

static cairo_int_status_t
_cairo_minigui_surface_mask (void                   *surface,
                             cairo_operator_t        op,
                             const cairo_pattern_t  *source,
                             const cairo_pattern_t  *mask,
                             const cairo_clip_t     *clip)
{
    return _cairo_surface_fallback_mask (surface, op, source, mask, clip);
}

static cairo_int_status_t
_cairo_minigui_surface_stroke (void                         *surface,
                               cairo_operator_t              op,
                               const cairo_pattern_t        *source,
                               const cairo_path_fixed_t     *path,
                               const cairo_stroke_style_t   *style,
                               const cairo_matrix_t         *ctm,
                               const cairo_matrix_t         *ctm_inverse,
                               double                        tolerance,
                               cairo_antialias_t             antialias,
                               const cairo_clip_t           *clip)
{
    return _cairo_surface_fallback_stroke (surface, op, source, path,
                     style, ctm, ctm_inverse,
                     tolerance, antialias, clip);
}

static cairo_int_status_t
_cairo_minigui_surface_fill (void                       *surface,
                             cairo_operator_t            op,
                             const cairo_pattern_t      *source,
                             const cairo_path_fixed_t   *path,
                             cairo_fill_rule_t           fill_rule,
                             double                      tolerance,
                             cairo_antialias_t           antialias,
                             const cairo_clip_t          *clip)
{
    return _cairo_surface_fallback_fill (surface, op, source, path,
                     fill_rule,
                     tolerance, antialias, clip);
}

static cairo_int_status_t
_cairo_minigui_surface_glyphs (void                    *surface,
                               cairo_operator_t         op,
                               const cairo_pattern_t   *source,
                               cairo_glyph_t           *glyphs,
                               int                      num_glyphs,
                               cairo_scaled_font_t     *scaled_font,
                               const cairo_clip_t      *clip)
{
    return _cairo_surface_fallback_glyphs (surface, op,
                       source, glyphs, num_glyphs,
                       scaled_font, clip);
}
#endif

static const cairo_surface_backend_t cairo_minigui_surface_backend = {
    CAIRO_SURFACE_TYPE_MINIGUI,
    _cairo_minigui_surface_finish,

    _cairo_default_context_create,

    _cairo_minigui_surface_create_similar,
    _cairo_minigui_surface_create_similar_image,
    _cairo_minigui_surface_map_to_image,
    _cairo_minigui_surface_unmap_image,

    _cairo_surface_default_source,
    _cairo_surface_default_acquire_source_image,
    _cairo_surface_default_release_source_image,
    NULL,  /* snapshot */

    NULL, /* copy_page */
    NULL, /* show_page */

    _cairo_minigui_surface_get_extents,
    NULL, /* get_font_options */

    _cairo_minigui_surface_flush,
    _cairo_minigui_surface_mark_dirty,

    _cairo_surface_fallback_paint,
    _cairo_surface_fallback_mask,
    _cairo_surface_fallback_stroke,
    _cairo_surface_fallback_fill,
    NULL, /* fill/stroke */
    _cairo_surface_fallback_glyphs,
};

#if defined(CAIRO_HAS_DRM_SURFACE) && defined(_MGGAL_DRM)

#include "cairo-drm.h"

static void _destroy_memdc (void *data)
{
    _WRN_PRINTF("here\n");

    HDC hdc = (HDC)data;

    if (hdc != HDC_INVALID) {
        DeleteMemDC(hdc);
    }
}

static cairo_user_data_key_t _dc_key = { _MINIGUI_VERSION_CODE };

/**
 * cairo_drm_surface_get_minigui_dc:
 * @surface: the cairo surface.
 *
 * Gets the DC associated with the surface.
 *
 * Return value: the DC or HDC_INVALID
 *
 * Since: 2.18
 **/
HDC
cairo_drm_surface_get_minigui_dc (cairo_surface_t *surface)
{
    HDC dc = (HDC)cairo_surface_get_user_data (surface, &_dc_key);

    if (dc == NULL)
        return HDC_INVALID;

    return dc;
}

#endif

/**
 * cairo_minigui_surface_create:
 * @device: the DRM device; can be NULL if not using DRM.
 * @hdc: the DC to create a surface for
 *
 * Creates a cairo surface that targets the given DC. If the given DC
 * is not a memory DC or screen DC, this function will create a memory
 * DC which is compatible to the DC first.
 *
 * Return value: the newly created surface; it may be a DRM surface
 * if the DC is allocated by MiniGUI DRI engine and @device is not NULL.
 *
 * Since: 2.18
 **/
cairo_surface_t *
cairo_minigui_surface_create (cairo_device_t *device, HDC hdc)
{
    cairo_format_t format;
    int width, height;

    if (hdc == HDC_INVALID) {
        return _cairo_surface_create_in_error (
                        _cairo_error (CAIRO_STATUS_INVALID_ARGUMENTS));
    }

    format = _cairo_format_from_dc (hdc);
    switch (format) {
    case CAIRO_FORMAT_RGB30:
    case CAIRO_FORMAT_RGB96F:
    case CAIRO_FORMAT_RGBA128F:
    case CAIRO_FORMAT_A1:
    case CAIRO_FORMAT_INVALID:
    default:
        return _cairo_surface_create_in_error (
                        _cairo_error (CAIRO_STATUS_INVALID_FORMAT));

    case CAIRO_FORMAT_A8:
    case CAIRO_FORMAT_ARGB32:
    case CAIRO_FORMAT_RGB24:
    case CAIRO_FORMAT_RGB16_565:
        break;
    }

    width = (int)GetGDCapability (hdc, GDCAP_HPIXEL);
    height = (int)GetGDCapability (hdc, GDCAP_VPIXEL);

    if (IsScreenDC (hdc) || IsMemDC (hdc)) {
#if defined(CAIRO_HAS_DRM_SURFACE) && defined(_MGGAL_DRM)
        GHANDLE vh;
        if (device && (vh = GetVideoHandle (hdc))) {
            DrmSurfaceInfo info;
            if (drmGetSurfaceInfo (vh, hdc, &info)) {
                cairo_surface_t* drm_surface = NULL;
                drm_surface = cairo_drm_surface_create_for_handle (device,
                        info.handle, info.size,
                        format, info.width, info.height, info.pitch);

                if (cairo_surface_get_type (drm_surface) == CAIRO_SURFACE_TYPE_DRM) {
                    cairo_surface_set_user_data (drm_surface, &_dc_key, (void*)hdc, NULL);
                    return drm_surface;
                }
            }
        }
#endif

        return _cairo_minigui_surface_create_on_dc (device, hdc, format, FALSE);
    }
    else {
        return cairo_minigui_surface_create_with_memdc (device, format, width, height);
    }
}

/**
 * cairo_minigui_surface_create_with_memdc:
 * @device: the DRM device; can be NULL if not using DRM.
 * @format: format of pixels in the surface to create
 * @width: width of the surface, in pixels
 * @height: height of the surface, in pixels
 *
 * Creates a surface which is associated with a new memory DC.
 *
 * Return value: the newly created surface; it may be a DRM surface
 * if the DC is allocated by MiniGUI DRI engine and @device is not NULL.
 *
 * Since: 2.18
 **/
cairo_surface_t *
cairo_minigui_surface_create_with_memdc (cairo_device_t * device,
                            cairo_format_t format,
                            int          width,
                            int          height)
{
    HDC memdc;
    width = (width <= 0) ? 1 : width;
    height = (height <= 0) ? 1 : height;

    memdc = _create_memdc (format, width, height);
    if (memdc == HDC_SCREEN) {
        return _cairo_surface_create_in_error (
                        _cairo_error (CAIRO_STATUS_INVALID_FORMAT));
    }
    else if (memdc == HDC_INVALID) {
        return _cairo_surface_create_in_error (
                        _cairo_error (CAIRO_STATUS_NO_MEMORY));
    }

#if defined(CAIRO_HAS_DRM_SURFACE) && defined(_MGGAL_DRM)
    if (device) {
        GHANDLE vh = GetVideoHandle (memdc);
        if (vh) {
            DrmSurfaceInfo info;
            if (drmGetSurfaceInfo (vh, memdc, &info)) {
                cairo_surface_t* drm_surface = NULL;
                drm_surface = cairo_drm_surface_create_for_handle (device,
                        info.handle, info.size,
                        format, info.width, info.height, info.pitch);

                if (cairo_surface_get_type (drm_surface) == CAIRO_SURFACE_TYPE_DRM) {
                    cairo_surface_set_user_data (drm_surface, &_dc_key, (void*)memdc, _destroy_memdc);
                    return drm_surface;
                }
                else {
                    cairo_surface_destroy (drm_surface);
                }
            }
        }
    }
#endif

    return _cairo_minigui_surface_create_on_dc (device, memdc, format, TRUE);
}

/**
 * cairo_minigui_surface_create_with_memdc_similar:
 * @device: the DRM device; can be NULL if not using DRM.
 * @ref_dc: a DC as the reference for pixel format.
 * @width: width of the surface, in pixels.
 * @height: height of the surface, in pixels.
 *
 * Creates a surface assoiciated with a new memory DC which
 * is compatible to the given DC but in the specified size.
 *
 * Return value: the newly created surface; it may be a DRM surface
 * if the DC is allocated by MiniGUI DRI engine and @device is not NULL.
 *
 * Since: 2.18
 **/
cairo_surface_t *
cairo_minigui_surface_create_with_memdc_similar (cairo_device_t* device,
                        HDC ref_dc,
                        int width,
                        int height)
{
    cairo_format_t format;

    if (ref_dc == HDC_INVALID) {
        return _cairo_surface_create_in_error (
                        _cairo_error (CAIRO_STATUS_INVALID_VISUAL));
    }

    format = _cairo_format_from_dc (ref_dc);
    return cairo_minigui_surface_create_with_memdc (device, format,
                width, height);
}

static inline cairo_bool_t
_cairo_surface_is_minigui (cairo_surface_t *surface)
{
    return surface->backend == &cairo_minigui_surface_backend;
}

/**
 * cairo_minigui_surface_get_dc:
 *
 * Returns the device context which is associated with the the surface.
 *
 * Return value: the handle to the device context.
 *
 * Since: 2.18
 **/
HDC
cairo_minigui_surface_get_dc (cairo_surface_t *surface)
{
    cairo_minigui_surface_t *ms = (cairo_minigui_surface_t*) surface;

    /* Throw an error for a non-MiniGUI surface */
    if (!_cairo_surface_is_minigui (surface)) {
        _cairo_error_throw (CAIRO_STATUS_SURFACE_TYPE_MISMATCH);
        return HDC_INVALID;
    }

    return ms->dc;
}

/**
 * cairo_minigui_surface_get_image:
 *
 * Returns the image surface which is equivalent to the MiniGUI surface.
 *
 * Return value: the pointer to the image surface.
 *
 * Since: 2.18
 **/
cairo_surface_t *
cairo_minigui_surface_get_image (cairo_surface_t *surface)
{
    cairo_minigui_surface_t *ms = (cairo_minigui_surface_t*) surface;

    /* Throw an error for a non-MiniGUI surface */
    if (!_cairo_surface_is_minigui (surface)) {
        return _cairo_surface_create_in_error (
                _cairo_error (CAIRO_STATUS_SURFACE_TYPE_MISMATCH));
    }

    return ms->image;
}

#endif /* CAIRO_HAS_MINIGUI_SURFACE */

