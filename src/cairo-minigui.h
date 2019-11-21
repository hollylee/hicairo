/* cairo - a vector graphics library with display and print output
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
 * The Initial Developer of the Original Code is FMSoft Technologies.
 *
 * Contributor(s):
 *     WEI Yongming <vincent@minigui.org>
 */

#ifndef _CAIRO_MINIGUI_H_
#define _CAIRO_MINIGUI_H_

#include "cairo.h"

#if CAIRO_HAS_MINIGUI_SURFACE

#include <minigui/common.h>
#include <minigui/minigui.h>
#include <minigui/gdi.h>

CAIRO_BEGIN_DECLS

cairo_public cairo_surface_t *
cairo_minigui_surface_create (cairo_device_t *device, HDC dc);

cairo_public cairo_surface_t *
cairo_minigui_surface_create_with_memdc (cairo_device_t *device,
                        cairo_format_t format, int width, int height);

cairo_public cairo_surface_t *
cairo_minigui_surface_create_with_memdc_similar (cairo_device_t *device,
                        HDC ref_dc, int width, int height);

cairo_public HDC
cairo_minigui_surface_get_dc (cairo_surface_t *surface);

cairo_public cairo_surface_t *
cairo_minigui_surface_get_image (cairo_surface_t *surface);

#if defined(CAIRO_HAS_DRM_SURFACE) && defined(_MGGAL_DRM)
cairo_public HDC
cairo_drm_surface_get_minigui_dc (cairo_surface_t *surface);
#endif

CAIRO_END_DECLS

#else  /* CAIRO_HAS_MINIGUI_SURFACE */
# error Cairo was not compiled with support for the MiniGUI backend
#endif /* CAIRO_HAS_MINIGUI_SURFACE */

#endif /* _CAIRO_MINIGUI_H_ */
