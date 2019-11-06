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
 * The surface returned by the other minigui constructors is of surface type
 * %CAIRO_SURFACE_TYPE_MINIGUI and is a raster surface type.
 **/

/**
 * CAIRO_HAS_MINIGUI_SURFACE:
 *
 * Defined if the MiniGUI surface backend is available.
 * This macro can be used to conditionally compile backend-specific code.
 *
 * Since: 1.17
 **/

static cairo_device_t *
_cairo_minigui_device_get (void)
{
    return NULL;
}

static const cairo_surface_backend_t cairo_minigui_surface_backend;

typedef struct _cairo_minigui_surface {
    cairo_surface_t base;

    cairo_format_t format;

    /* We always create off-screen surfaces as memory DC */
    HDC dc;

    /* We construct the BITMAP structure with the attributes of the memory DC */
    BITMAP bitmap;

    /* We also construct an equivalent image surface */
    cairo_surface_t *image;

    cairo_surface_t *fallback;

    cairo_rectangle_int_t extents;

    /* Initial clip bits
     * We need these kept around so that we maintain
     * whatever clip was set on the original DC at creation
     * time when cairo is asked to reset the surface clip.
     */
    RECT clip_rect;
    PCLIPRGN initial_clip_rgn;
    cairo_bool_t had_simple_clip;

    /* Surface DC flags */
    uint32_t flags;
} cairo_minigui_surface_t;

#define to_minigui_surface(S) ((cairo_minigui_surface_t *)(S))

static cairo_status_t
_cairo_minigui_print_gdi_error (const char *context)
{
    fprintf (stderr, "%s: MiniGUI GDI error.\n", context);
    return _cairo_error (CAIRO_STATUS_NO_MEMORY);
}

#if 0
static uint32_t
_cairo_minigui_flags_for_dc (HDC dc, cairo_format_t format)
{
    uint32_t flags = CAIRO_MINIGUI_SURFACE_IS_DISPLAY;

    if (GetGDCapability (dc, GDCAP_DEPTH) > 8) {
        flags |= CAIRO_MINIGUI_SURFACE_CAN_BITBLT;
        flags |= CAIRO_MINIGUI_SURFACE_CAN_ALPHABLEND;
        flags |= CAIRO_MINIGUI_SURFACE_CAN_STRETCHBLT;
        flags |= CAIRO_MINIGUI_SURFACE_CAN_STRETCHDIB;
    }
    else {
        flags |= CAIRO_MINIGUI_SURFACE_CAN_BITBLT;
        flags |= CAIRO_MINIGUI_SURFACE_CAN_STRETCHBLT;
        flags |= CAIRO_MINIGUI_SURFACE_CAN_STRETCHDIB;
    }

    return flags;
}
#endif

static PBITMAP
construct_ddb_from_dc (HDC hdc, PBITMAP ddb)
{
    RECT rc = {0, 0, 1, 1};

    ddb->bmType = BMP_TYPE_NORMAL;
    ddb->bmBitsPerPixel = GetGDCapability (hdc, GDCAP_BITSPP);
    ddb->bmBytesPerPixel = GetGDCapability (hdc, GDCAP_BPP);
    ddb->bmAlpha = 0;
    ddb->bmColorKey = 0;
    ddb->bmWidth = GetGDCapability (hdc, GDCAP_HPIXEL);
    ddb->bmHeight = GetGDCapability (hdc, GDCAP_VPIXEL);
    ddb->bmBits = LockDC (hdc, &rc, NULL, NULL, (int*)&ddb->bmPitch);
    UnlockDC (hdc);

    return ddb;
}

static cairo_status_t
_create_dc_and_bitmap (cairo_minigui_surface_t *surface,
                       HDC                      orgdc,
                       cairo_format_t           format,
                       int                      width,
                       int                      height,
                       unsigned char          **bits_out,
                       int                     *rowstride_out)
{
    cairo_status_t status;
    int i;

    surface->dc = HDC_INVALID;
    width = (width == 0) ? (int)GetGDCapability(orgdc, GDCAP_HPIXEL) : width;
    height = (height == 0) ? (int)GetGDCapability(orgdc, GDCAP_VPIXEL) : height;

    switch (format) {
    case CAIRO_FORMAT_RGB16_565:
        surface->dc = CreateMemDC (width, height,
                        16, MEMDC_FLAG_HWSURFACE,
                        0xF800, 0x07E0, 0x001F, 0x0000);
        break;

    case CAIRO_FORMAT_RGB24:
        /* We treat RGB24 format like 32bpp. */
        surface->dc = CreateMemDC (width, height,
                        32, MEMDC_FLAG_HWSURFACE,
                        0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000);
        break;

    case CAIRO_FORMAT_ARGB32:
        surface->dc = CreateMemDC (width, height,
                        32, MEMDC_FLAG_HWSURFACE,
                        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        break;

    case CAIRO_FORMAT_A8:
        surface->dc = CreateMemDC (width, height,
                        8, MEMDC_FLAG_HWSURFACE,
                        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        {
            GAL_Color cmap [256];
            for (i = 0; i < 256; i++) {
                cmap[i].r = i;
                cmap[i].g = i;
                cmap[i].b = i;
                cmap[i].a = 0;
            }
            SetPalette (surface->dc, 0, 256, cmap);
        }
        break;

    case CAIRO_FORMAT_RGB30:
    case CAIRO_FORMAT_RGB96F:
    case CAIRO_FORMAT_RGBA128F:
    case CAIRO_FORMAT_A1:
    case CAIRO_FORMAT_INVALID:
        return CAIRO_STATUS_INVALID_FORMAT;
    }

    if (surface->dc == HDC_INVALID)
        goto FAIL;

    construct_ddb_from_dc (surface->dc, &surface->bitmap);
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
_cairo_minigui_surface_create_for_dc (HDC            org_dc,
                                      cairo_format_t format,
                                      int            width,
                                      int            height)
{
    cairo_status_t status;
    cairo_device_t *device;
    cairo_minigui_surface_t *surface;
    unsigned char *bits;
    int rowstride;

    surface = _cairo_malloc (sizeof (*surface));
    if (surface == NULL)
        return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_NO_MEMORY));

    surface->fallback = NULL;

    status = _create_dc_and_bitmap (surface, org_dc, format,
                    width, height, &bits, &rowstride);
    if (status)
        goto FAIL;

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

    surface->initial_clip_rgn = NULL;
    surface->had_simple_clip = FALSE;

    device = _cairo_minigui_device_get ();

    _cairo_surface_init (&surface->base,
             &cairo_minigui_surface_backend,
             device,
             _cairo_content_from_format (format),
             FALSE); /* is_vector */

    cairo_device_destroy (device);

    return &surface->base;

FAIL:
    if (surface->bitmap.bmBits) {
        DeleteMemDC (surface->dc);
    }
    free (surface);

    return _cairo_surface_create_in_error (status);
}

static cairo_surface_t *
_cairo_minigui_surface_create_similar (void            *abstract_src,
                                       cairo_content_t  content,
                                       int              width,
                                       int              height)
{
    cairo_minigui_surface_t *src = abstract_src;
    cairo_format_t format = _cairo_format_from_content (content);
    cairo_surface_t *new_surf = NULL;

    if (!(content & CAIRO_CONTENT_ALPHA)) {
        /* try to create a ddb */
        new_surf = cairo_minigui_surface_create_with_format_size (src->dc,
                CAIRO_FORMAT_RGB24, width, height);

        if (new_surf->status)
            new_surf = NULL;
    }

    if (new_surf == NULL) {
        new_surf = _cairo_minigui_surface_create_for_dc (src->dc,
            format, width, height);
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
    _cairo_minigui_surface_create_for_dc (surface->dc,
                            format, width, height);
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
    if (surface->bitmap.bmBits) {
        DeleteMemDC (surface->dc);
    }

    _cairo_minigui_surface_discard_fallback (surface);

#if 0
    if (surface->initial_clip_rgn)
    DeleteObject (surface->initial_clip_rgn);
#endif

    return CAIRO_STATUS_SUCCESS;
}

static cairo_image_surface_t *
_cairo_minigui_surface_map_to_image (void                    *abstract_surface,
                       const cairo_rectangle_int_t   *extents)
{
    cairo_minigui_surface_t *surface = abstract_surface;
    cairo_status_t status;

    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, surface->base.unique_id));

    if (surface->image)
    goto done;

    if (surface->fallback == NULL) {
    surface->fallback =
        _cairo_minigui_surface_create_for_dc (surface->dc,
                            surface->format,
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
    //GdiFlush();
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
_cairo_minigui_surface_get_extents (void          *abstract_surface,
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
    } else {
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

static cairo_int_status_t
_cairo_minigui_save_initial_clip (HDC hdc, cairo_minigui_surface_t *surface)
{
#if 0
    RECT rect;
    int clipBoxType;
    int gm;
    XFORM saved_xform;

    /* GetClipBox/GetClipRgn and friends interact badly with a world transform
     * set.  GetClipBox returns values in logical (transformed) coordinates;
     * it's unclear what GetClipRgn returns, because the region is empty in the
     * case of a SIMPLEREGION clip, but I assume device (untransformed) coordinates.
     * Similarly, IntersectClipRect works in logical units, whereas SelectClipRgn
     * works in device units.
     *
     * So, avoid the whole mess and get rid of the world transform
     * while we store our initial data and when we restore initial coordinates.
     *
     * XXX we may need to modify x/y by the ViewportOrg or WindowOrg
     * here in GM_COMPATIBLE; unclear.
     */
    gm = GetGraphicsMode (hdc);
    if (gm == GM_ADVANCED) {
    GetWorldTransform (hdc, &saved_xform);
    ModifyWorldTransform (hdc, NULL, MWT_IDENTITY);
    }

    clipBoxType = GetClipBox (hdc, &rect);
    if (clipBoxType == ERROR) {
    _cairo_minigui_print_gdi_error (__func__);
    SetGraphicsMode (hdc, gm);
    /* XXX: Can we make a more reasonable guess at the error cause here? */
    return _cairo_error (CAIRO_STATUS_DEVICE_ERROR);
    }

    surface->extents.x = rect.left;
    surface->extents.y = rect.top;
    surface->extents.width = rect.right - rect.left;
    surface->extents.height = rect.bottom - rect.top;

    surface->initial_clip_rgn = NULL;
    surface->had_simple_clip = FALSE;

    if (clipBoxType == COMPLEXREGION) {
    surface->initial_clip_rgn = CreateRectRgn (0, 0, 0, 0);
    if (GetClipRgn (hdc, surface->initial_clip_rgn) <= 0) {
        DeleteObject(surface->initial_clip_rgn);
        surface->initial_clip_rgn = NULL;
    }
    } else if (clipBoxType == SIMPLEREGION) {
    surface->had_simple_clip = TRUE;
    }

    if (gm == GM_ADVANCED)
    SetWorldTransform (hdc, &saved_xform);
#endif

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_minigui_surface_set_clip (cairo_minigui_surface_t *surface,
                       cairo_clip_t *clip)
{
#if 0
    char stack[512];
    cairo_rectangle_int_t extents;
    int num_rects;
    RGNDATA *data;
    size_t data_size;
    RECT *rects;
    int i;
    HRGN gdi_region;
    cairo_status_t status;
    cairo_region_t *region;

    /* The semantics we want is that any clip set by cairo combines
     * is intersected with the clip on device context that the
     * surface was created for. To implement this, we need to
     * save the original clip when first setting a clip on surface.
     */

    assert (_cairo_clip_is_region (clip));
    region = _cairo_clip_get_region (clip);
    if (region == NULL)
    return CAIRO_STATUS_SUCCESS;

    cairo_region_get_extents (region, &extents);
    num_rects = cairo_region_num_rectangles (region);

    /* XXX see notes in _cairo_minigui_save_initial_clip --
     * this code will interact badly with a HDC which had an initial
     * world transform -- we should probably manually transform the
     */

    data_size = sizeof (RGNDATAHEADER) + num_rects * sizeof (RECT);
    if (data_size > sizeof (stack)) {
    data = _cairo_malloc (data_size);
    if (!data)
        return _cairo_error(CAIRO_STATUS_NO_MEMORY);
    } else
    data = (RGNDATA *)stack;

    data->rdh.dwSize = sizeof (RGNDATAHEADER);
    data->rdh.iType = RDH_RECTANGLES;
    data->rdh.nCount = num_rects;
    data->rdh.nRgnSize = num_rects * sizeof (RECT);
    data->rdh.rcBound.left = extents.x;
    data->rdh.rcBound.top = extents.y;
    data->rdh.rcBound.right = extents.x + extents.width;
    data->rdh.rcBound.bottom = extents.y + extents.height;

    rects = (RECT *)data->Buffer;
    for (i = 0; i < num_rects; i++) {
    cairo_rectangle_int_t rect;

    cairo_region_get_rectangle (region, i, &rect);

    rects[i].left   = rect.x;
    rects[i].top    = rect.y;
    rects[i].right  = rect.x + rect.width;
    rects[i].bottom = rect.y + rect.height;
    }

    gdi_region = ExtCreateRegion (NULL, data_size, data);
    if ((char *)data != stack)
    free (data);

    if (!gdi_region)
    return _cairo_error (CAIRO_STATUS_NO_MEMORY);

    /* AND the new region into our DC */
    status = CAIRO_STATUS_SUCCESS;
    if (ExtSelectClipRgn (surface->dc, gdi_region, RGN_AND) == ERROR)
    status = _cairo_minigui_print_gdi_error (__func__);

    DeleteObject (gdi_region);

    return status;
#endif

    return CAIRO_STATUS_SUCCESS;
}

static void
_cairo_minigui_surface_unset_clip (cairo_minigui_surface_t *surface)
{
#if 0
    XFORM saved_xform;
    int gm = GetGraphicsMode (surface->dc);
    if (gm == GM_ADVANCED) {
    GetWorldTransform (surface->dc, &saved_xform);
    ModifyWorldTransform (surface->dc, NULL, MWT_IDENTITY);
    }

    /* initial_clip_rgn will either be a real region or NULL (which means reset to no clip region) */
    SelectClipRgn (surface->dc, surface->initial_clip_rgn);

    if (surface->had_simple_clip) {
    /* then if we had a simple clip, intersect */
    IntersectClipRect (surface->dc,
               surface->extents.x,
               surface->extents.y,
               surface->extents.x + surface->extents.width,
               surface->extents.y + surface->extents.height);
    }

    if (gm == GM_ADVANCED)
    SetWorldTransform (surface->dc, &saved_xform);
#endif
}

static cairo_int_status_t
_cairo_minigui_surface_paint (void            *surface,
                    cairo_operator_t         op,
                    const cairo_pattern_t    *source,
                    const cairo_clip_t        *clip)
{
#if 0
    cairo_minigui_device_t *device = to_minigui_device_from_surface (surface);

    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, to_minigui_surface(surface)->base.unique_id));

    if (clip == NULL &&
    (op == CAIRO_OPERATOR_SOURCE || op == CAIRO_OPERATOR_CLEAR))
    _cairo_minigui_surface_discard_fallback (surface);

    return _cairo_compositor_paint (device->compositor,
                    surface, op, source, clip);
#else
    return _cairo_surface_fallback_paint (surface, op, source, clip);
#endif
}

static cairo_int_status_t
_cairo_minigui_surface_mask (void                *surface,
                   cairo_operator_t         op,
                   const cairo_pattern_t    *source,
                   const cairo_pattern_t    *mask,
                   const cairo_clip_t        *clip)
{
#if 0
    cairo_minigui_device_t *device = to_minigui_device_from_surface (surface);

    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, to_minigui_surface(surface)->base.unique_id));

    if (clip == NULL && op == CAIRO_OPERATOR_SOURCE)
    _cairo_minigui_surface_discard_fallback (surface);

    return _cairo_compositor_mask (device->compositor,
                   surface, op, source, mask, clip);
#else
    return _cairo_surface_fallback_mask (surface, op, source, mask, clip);
#endif
}

static cairo_int_status_t
_cairo_minigui_surface_stroke (void            *surface,
                     cairo_operator_t         op,
                     const cairo_pattern_t    *source,
                     const cairo_path_fixed_t    *path,
                     const cairo_stroke_style_t    *style,
                     const cairo_matrix_t    *ctm,
                     const cairo_matrix_t    *ctm_inverse,
                     double             tolerance,
                     cairo_antialias_t         antialias,
                     const cairo_clip_t        *clip)
{
#if 0
    cairo_minigui_device_t *device = to_minigui_device_from_surface (surface);

    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, to_minigui_surface(surface)->base.unique_id));

    return _cairo_compositor_stroke (device->compositor, surface,
                     op, source, path,
                     style, ctm, ctm_inverse,
                     tolerance, antialias, clip);
#else
    return _cairo_surface_fallback_stroke (surface, op, source, path,
                     style, ctm, ctm_inverse,
                     tolerance, antialias, clip);
#endif
}

static cairo_int_status_t
_cairo_minigui_surface_fill (void                *surface,
                   cairo_operator_t         op,
                   const cairo_pattern_t    *source,
                   const cairo_path_fixed_t    *path,
                   cairo_fill_rule_t         fill_rule,
                   double             tolerance,
                   cairo_antialias_t         antialias,
                   const cairo_clip_t        *clip)
{
#if 0
    cairo_minigui_device_t *device = to_minigui_device_from_surface (surface);

    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, to_minigui_surface(surface)->base.unique_id));

    return _cairo_compositor_fill (device->compositor, surface,
                   op, source, path,
                   fill_rule, tolerance, antialias,
                   clip);
#else
    return _cairo_surface_fallback_fill (surface, op, source, path,
                     fill_rule,
                     tolerance, antialias, clip);
#endif
}

static cairo_int_status_t
_cairo_minigui_surface_glyphs (void              *surface,
                   cairo_operator_t           op,
                   const cairo_pattern_t  *source,
                   cairo_glyph_t          *glyphs,
                   int               num_glyphs,
                   cairo_scaled_font_t    *scaled_font,
                   const cairo_clip_t     *clip)
{
#if 0
    cairo_minigui_device_t *device = to_minigui_device_from_surface (surface);

    TRACE ((stderr, "%s (surface=%d)\n",
        __func__, to_minigui_surface(surface)->base.unique_id));

    return _cairo_compositor_glyphs (device->compositor, surface,
                     op, source,
                     glyphs, num_glyphs, scaled_font,
                     clip);
#else
    return _cairo_surface_fallback_glyphs (surface, op,
                       source, glyphs, num_glyphs,
                       scaled_font, clip);
#endif
}

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

    _cairo_minigui_surface_paint,
    _cairo_minigui_surface_mask,
    _cairo_minigui_surface_stroke,
    _cairo_minigui_surface_fill,
    NULL, /* fill/stroke */
    _cairo_minigui_surface_glyphs,
};

/* Notes:
 *
 * Win32 alpha-understanding functions
 *
 * BitBlt - will copy full 32 bits from a 32bpp DIB to result
 *          (so it's safe to use for ARGB32->ARGB32 SOURCE blits)
 *          (but not safe going RGB24->ARGB32, if RGB24 is also represented
 *           as a 32bpp DIB, since the alpha isn't discarded!)
 *
 * AlphaBlend - if both the source and dest have alpha, even if AC_SRC_ALPHA isn't set,
 *              it will still copy over the src alpha, because the SCA value (255) will be
 *              multiplied by all the src components.
 */

/**
 * cairo_minigui_surface_create_with_format:
 * @hdc: the DC to create a surface for
 * @format: format of pixels in the surface to create
 *
 * Creates a cairo surface that targets the given DC.  The DC will be
 * queried for its initial clip extents, and this will be used as the
 * size of the cairo surface.
 *
 * Supported formats are:
 * %CAIRO_FORMAT_ARGB32
 * %CAIRO_FORMAT_RGB24
 *
 * Note: @format only tells cairo how to draw on the surface, not what
 * the format of the surface is. Namely, cairo does not (and cannot)
 * check that @hdc actually supports alpha-transparency.
 *
 * Return value: the newly created surface, NULL on failure
 *
 * Since: 1.17
 **/
cairo_surface_t *
cairo_minigui_surface_create_with_format (HDC hdc, cairo_format_t format)
{
    cairo_minigui_surface_t *surface;

    cairo_status_t status;
    cairo_device_t *device;

    switch (format) {
    case CAIRO_FORMAT_RGB30:
    case CAIRO_FORMAT_RGB96F:
    case CAIRO_FORMAT_RGBA128F:
    case CAIRO_FORMAT_A1:
    case CAIRO_FORMAT_A8:
    case CAIRO_FORMAT_INVALID:
    default:
        return _cairo_surface_create_in_error (
                        _cairo_error (CAIRO_STATUS_INVALID_FORMAT));

    case CAIRO_FORMAT_ARGB32:
    case CAIRO_FORMAT_RGB24:
    case CAIRO_FORMAT_RGB16_565:
        break;
    }

    surface = _cairo_malloc (sizeof (*surface));
    if (surface == NULL)
        return _cairo_surface_create_in_error (
                        _cairo_error (CAIRO_STATUS_NO_MEMORY));

    status = _cairo_minigui_save_initial_clip (hdc, surface);
    if (status) {
        free (surface);
        return _cairo_surface_create_in_error (status);
    }

    surface->image = NULL;
    surface->fallback = NULL;
    surface->format = format;

    surface->dc = hdc;

    //surface->saved_dc_bitmap = NULL;
    //surface->flags = _cairo_minigui_flags_for_dc (surface->dc, format);

    device = _cairo_minigui_device_get ();

    _cairo_surface_init (&surface->base,
             &cairo_minigui_surface_backend,
             device,
             _cairo_content_from_format (format),
             FALSE); /* is_vector */

    cairo_device_destroy (device);

    return &surface->base;
}

/**
 * cairo_minigui_surface_create:
 * @hdc: the DC to create a surface for
 *
 * Creates a cairo surface that targets the given DC.  The DC will be
 * queried for its initial clip extents, and this will be used as the
 * size of the cairo surface.  The resulting surface will always be of
 * format %CAIRO_FORMAT_RGB24; if you need another surface format,
 * you will need to create one through
 * cairo_minigui_surface_create_with_format() or
 * cairo_minigui_surface_create_with_format_size().
 *
 * Return value: the newly created surface, NULL on failure
 *
 * Since: 1.17
 **/
cairo_surface_t *
cairo_minigui_surface_create (HDC hdc)
{
    return cairo_minigui_surface_create_with_format (hdc, CAIRO_FORMAT_RGB24);
}

/**
 * cairo_minigui_surface_create_with_format_size:
 * @format: format of pixels in the surface to create
 * @width: width of the surface, in pixels
 * @height: height of the surface, in pixels
 *
 * Creates a device-independent-bitmap surface not associated with
 * any particular existing surface or device context. The created
 * bitmap will be uninitialized.
 *
 * Return value: the newly created surface
 *
 * Since: 1.17
 **/
cairo_surface_t *
cairo_minigui_surface_create_with_format_size (HDC          hdc,
                           cairo_format_t format,
                           int          width,
                           int          height)
{
    if (!CAIRO_FORMAT_VALID (format))
        return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_INVALID_FORMAT));

    return _cairo_minigui_surface_create_for_dc (NULL, format, width, height);
}

/**
 * cairo_minigui_surface_create_with_size:
 * @hdc: a DC compatible with the surface to create
 * @format: format of pixels in the surface to create
 * @width: width of the surface, in pixels
 * @height: height of the surface, in pixels
 *
 * Creates a surface compatible to the given hdc but in the specified size.
 *
 * Return value: the newly created surface
 *
 * Since: 1.17
 **/
cairo_surface_t *
cairo_minigui_surface_create_with_size (HDC hdc,
                     int width,
                     int height)
{
#if 0
    cairo_minigui_surface_t *new_surf;
    HBITMAP ddb;
    HDC screen_dc, ddb_dc;
    HBITMAP saved_dc_bitmap;

    switch (format) {
    default:
/* XXX handle these eventually */
    case CAIRO_FORMAT_A8:
    case CAIRO_FORMAT_A1:
    return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_INVALID_FORMAT));
    case CAIRO_FORMAT_ARGB32:
    case CAIRO_FORMAT_RGB24:
    break;
    }

    if (!hdc) {
    screen_dc = GetDC (NULL);
    hdc = screen_dc;
    } else {
    screen_dc = NULL;
    }

    ddb_dc = CreateCompatibleDC (hdc);
    if (ddb_dc == NULL) {
    new_surf = (cairo_minigui_surface_t*) _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_NO_MEMORY));
    goto FINISH;
    }

    ddb = CreateCompatibleBitmap (hdc, width, height);
    if (ddb == NULL) {
    DeleteDC (ddb_dc);

    /* Note that if an app actually does hit this out of memory
     * condition, it's going to have lots of other issues, as
     * video memory is probably exhausted.  However, it can often
     * continue using DIBs instead of DDBs.
     */
    new_surf = (cairo_minigui_surface_t*) _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_NO_MEMORY));
    goto FINISH;
    }

    saved_dc_bitmap = SelectObject (ddb_dc, ddb);

    new_surf = (cairo_minigui_surface_t*) cairo_minigui_surface_create (ddb_dc);
    new_surf->bitmap = ddb;
    new_surf->saved_dc_bitmap = saved_dc_bitmap;
    new_surf->is_dib = FALSE;

FINISH:
    if (screen_dc)
    ReleaseDC (NULL, screen_dc);

    return &new_surf->base;
#endif

    return NULL;
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
 * Since: 1.17
 **/
HDC
cairo_minigui_surface_get_dc (cairo_surface_t *surface)
{
    cairo_minigui_surface_t *ms = (cairo_minigui_surface_t*) surface;

    /* Throw an error for a non-MiniGUI surface */
    if (!_cairo_surface_is_minigui (surface)) {
        _cairo_error_throw (CAIRO_STATUS_SURFACE_TYPE_MISMATCH);
        return NULL;
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
 * Since: 1.17
 **/
cairo_surface_t *
cairo_minigui_surface_get_image (cairo_surface_t *surface)
{
    cairo_minigui_surface_t *ms = (cairo_minigui_surface_t*) surface;

    /* Throw an error for a non-MiniGUI surface */
    if (!_cairo_surface_is_minigui (surface)) {
        return _cairo_surface_create_in_error (_cairo_error (CAIRO_STATUS_SURFACE_TYPE_MISMATCH));
    }

    return ms->image;
}

#endif /* CAIRO_HAS_MINIGUI_SURFACE */

