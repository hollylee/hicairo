/* Cairo - a vector graphics library with display and print output
 *
 * Copyright © 2009 Chris Wilson
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
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
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
 * The Initial Developer of the Original Code is Chris Wilson.
 */

#include "cairoint.h"

#include "cairo-drm-private.h"

#include "cairo-device-private.h"
#include "cairo-error-private.h"

#define LIBUDEV_I_KNOW_THE_API_IS_SUBJECT_TO_CHANGE
#include <libudev.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h> /* open(), close() */

static cairo_drm_device_t *_cairo_drm_known_devices;
static cairo_drm_device_t *_cairo_drm_default_device;

static const char *
get_udev_property(struct udev_device *device, const char *name)
{
    struct udev_list_entry *entry;

    udev_list_entry_foreach (entry,
	                     udev_device_get_properties_list_entry (device))
    {
	if (strcmp (udev_list_entry_get_name (entry), name) == 0)
	    return udev_list_entry_get_value (entry);
    }

    return NULL;
}

static cairo_status_t
_device_flush (void *abstract_device)
{
    cairo_drm_device_t *device = abstract_device;

    return device->device.flush (device);
}

static void
_device_finish (void *abstract_device)
{
    cairo_drm_device_t *device = abstract_device;

    CAIRO_MUTEX_LOCK (_cairo_drm_device_mutex);
    if (device->prev != NULL)
	device->prev->next = device->next;
    else
	_cairo_drm_known_devices = device->next;
    if (device->next != NULL)
	device->next->prev = device->prev;

    CAIRO_MUTEX_UNLOCK (_cairo_drm_device_mutex);

    if (_cairo_atomic_ptr_cmpxchg ((void **)&_cairo_drm_default_device,
				   device, NULL))
    {
	cairo_device_destroy (&device->base);
    }
}

static void
_device_destroy (void *abstract_device)
{
    cairo_drm_device_t *device = abstract_device;

    device->device.destroy (device);
}

static const cairo_device_backend_t _cairo_drm_device_backend = {
    CAIRO_DEVICE_TYPE_DRM,

    NULL, NULL, /* lock, unlock */

    _device_flush,
    _device_finish,
    _device_destroy,
};

cairo_drm_device_t *
_cairo_drm_device_init (cairo_drm_device_t *dev,
			int fd,
			dev_t devid,
			int vendor_id,
			int chip_id,
			int max_surface_size)
{
    assert (CAIRO_MUTEX_IS_LOCKED (_cairo_drm_device_mutex));

    _cairo_device_init (&dev->base, &_cairo_drm_device_backend);

    dev->id = devid;
    dev->vendor_id = vendor_id;
    dev->chip_id = chip_id;
    dev->fd = fd;

    dev->max_surface_size = max_surface_size;

    dev->prev = NULL;
    dev->next = _cairo_drm_known_devices;
    if (_cairo_drm_known_devices != NULL)
	_cairo_drm_known_devices->prev = dev;
    _cairo_drm_known_devices = dev;

    if (_cairo_drm_default_device == NULL)
	_cairo_drm_default_device = (cairo_drm_device_t *) cairo_device_reference (&dev->base);

    return dev;
}

/* VW: pass fd to avoid duplicated openning the device */
static cairo_device_t *
_cairo_drm_device_get_internal (struct udev_device *device, int fd)
{
    static const struct dri_driver_entry {
	uint32_t vendor_id;
	uint32_t chip_id;
	cairo_drm_device_create_func_t create_func;
	const char* chip_name;
    } driver_map[] = {
	{ 0x8086, 0x29a2, _cairo_drm_i965_device_create, "Intel(R) 965G (I965_G/i965)" },
	{ 0x8086, 0x2982, _cairo_drm_i965_device_create, "Intel(R) 965G (G35_G/i965)" },
	{ 0x8086, 0x2992, _cairo_drm_i965_device_create, "Intel(R) 965Q (I965_Q/i965)" },
	{ 0x8086, 0x2972, _cairo_drm_i965_device_create, "Intel(R) 946GZ (I946_GZ/i965)" },
	{ 0x8086, 0x2a02, _cairo_drm_i965_device_create, "Intel(R) 965GM (I965_GM/i965)" },
	{ 0x8086, 0x2a12, _cairo_drm_i965_device_create, "Intel(R) 965GME/GLE (I965_GME/i965)" },
	{ 0x8086, 0x2e02, _cairo_drm_i965_device_create, "Intel(R) Integrated Graphics Device (IGD_E_G/g4x)" },
	{ 0x8086, 0x2e22, _cairo_drm_i965_device_create, "Intel(R) G45/G43 (G45_G/g4x" },
	{ 0x8086, 0x2e12, _cairo_drm_i965_device_create, "Intel(R) Q45/Q43 (Q45_G/g4x)" },
	{ 0x8086, 0x2e32, _cairo_drm_i965_device_create, "Intel(R) G41 (G41_G/g4x)" },
	{ 0x8086, 0x2a42, _cairo_drm_i965_device_create, "Mobile Intel® GM45 Express Chipset (GM45_G/g4x)" },
	// VW { 0x8086, 0x0412, _cairo_drm_i965_device_create, "Haswell Desktop (GT2/hsw_gt2)" },

	{ 0x8086, 0x2582, _cairo_drm_i915_device_create, "Intel(R) 915G (I915_G/i915)" },
	{ 0x8086, 0x2592, _cairo_drm_i915_device_create, "Intel(R) 915GM (I915_GM/i915)" },
	{ 0x8086, 0x258a, _cairo_drm_i915_device_create, "Intel(R) E7221G (E7221_G/i915)" },
	{ 0x8086, 0x2772, _cairo_drm_i915_device_create, "Intel(R) 945G (I945_G/i915)" },
	{ 0x8086, 0x27a2, _cairo_drm_i915_device_create, "Intel(R) 945GM (I945_GM/i915)" },
	{ 0x8086, 0x27ae, _cairo_drm_i915_device_create, "Intel(R) 945GME (I945_GME/i915)" },
	{ 0x8086, 0x29c2, _cairo_drm_i915_device_create, "Intel(R) G33 (G33_G/i915)" },
	{ 0x8086, 0x29b2, _cairo_drm_i915_device_create, "Intel(R) Q35 (Q35_G/i915)" },
	{ 0x8086, 0x29d2, _cairo_drm_i915_device_create, "Intel(R) Q33 (Q33_G/i915)" },
	{ 0x8086, 0xa011, _cairo_drm_i915_device_create, "Intel(R) Pineview M (IGD_GM/i915)" },
	{ 0x8086, 0xa001, _cairo_drm_i915_device_create, "Intel(R) Pineview (IGD_G/i915)" },

	/* XXX i830 */

	{ 0x8086, ~0, _cairo_drm_intel_device_create, "Fallback for other Intel Graphics Devices" },

	{ 0x1002, ~0, _cairo_drm_radeon_device_create, "Fallback for AMD Radeon Graphics Devices" },
#if CAIRO_HAS_GALLIUM_SURFACE
	{ ~0, ~0, _cairo_drm_gallium_device_create, "Gallium (not completed)" },
#endif
    };

    cairo_drm_device_t *dev;
    dev_t devid;
    struct udev_device *parent;
    const char *pci_id;
    uint32_t vendor_id, chip_id;
    const char *path = NULL;
    int i;

    devid = udev_device_get_devnum (device);

    CAIRO_MUTEX_LOCK (_cairo_drm_device_mutex);
    for (dev = _cairo_drm_known_devices; dev != NULL; dev = dev->next) {
	if (dev->id == devid) {
	    dev = (cairo_drm_device_t *) cairo_device_reference (&dev->base);
	    goto DONE;
	}
    }

    parent = udev_device_get_parent (device);
    pci_id = get_udev_property (parent, "PCI_ID");
    if (pci_id == NULL || sscanf (pci_id, "%x:%x", &vendor_id, &chip_id) != 2) {
        dev = NULL;
	goto DONE;
    }

    //fprintf(stderr, "VW: DRI device vendor id (0x%x) chip id (0x%x)\n", vendor_id, chip_id);

#if CAIRO_HAS_GALLIUM_SURFACE
    if (getenv ("CAIRO_GALLIUM_FORCE"))
    {
	i = ARRAY_LENGTH (driver_map) - 1;
    }
    else
#endif
    {
	for (i = 0; i < ARRAY_LENGTH (driver_map); i++) {
	    if (driver_map[i].vendor_id == ~0U)
		break;

	    if (driver_map[i].vendor_id == vendor_id &&
		(driver_map[i].chip_id == ~0U || driver_map[i].chip_id == chip_id))
		break;
	}

	if (i == ARRAY_LENGTH (driver_map)) {
	    dev = (cairo_drm_device_t *)
		_cairo_device_create_in_error (CAIRO_STATUS_DEVICE_ERROR);
	    goto DONE;
	}
    }

    /* VW: only open the device if fd < 0 */
    if (fd < 0) {
	path = udev_device_get_devnode (device);
	if (path == NULL)
	    path = "/dev/dri/card0"; /* XXX buggy udev? */

	fd = open (path, O_RDWR);
	if (fd == -1) {
	    /* XXX more likely to be a permissions issue... */
	    _cairo_error_throw (CAIRO_STATUS_FILE_NOT_FOUND);
	    dev = NULL;
	    goto DONE;
	}
    }

    fprintf (stderr, "hiCairo: found a device with vendor_id(%X), chip_id(%X): %s\n",
                vendor_id, chip_id, driver_map[i].chip_name);

    dev = driver_map[i].create_func (fd, devid, vendor_id, chip_id);
    /* VW: use path as the flag that indicates fd is newly opened */
    if (dev == NULL && path != NULL)
	close (fd);

  DONE:
    CAIRO_MUTEX_UNLOCK (_cairo_drm_device_mutex);

    if (dev == NULL)
        return _cairo_device_create_in_error (CAIRO_STATUS_DEVICE_ERROR);
    else
        return &dev->base;
}

/**
 * cairo_drm_device_get:
 * @device: The udev device object of the DRI device.
 *
 * Returns the cairo device object for the given udev device.
 *
 * Return value: the pointer to the cairo device.
 *
 * Since: 2.18
 **/
cairo_device_t *
cairo_drm_device_get (struct udev_device *device)
{
    return _cairo_drm_device_get_internal (device, -1);
}
slim_hidden_def (cairo_drm_device_get);

/**
 * cairo_drm_device_get_for_fd:
 * @fd: The file descriptor of the opened DRI device file.
 *
 * Returns the cairo device object for the given file descriptor.
 *
 * Return value: the pointer to the cairo device.
 *
 * Since: 2.18
 **/
cairo_device_t *
cairo_drm_device_get_for_fd (int fd)
{
    struct stat st;
    struct udev *udev;
    struct udev_device *device;
    cairo_device_t *dev = NULL;

    if (fstat (fd, &st) < 0 || ! S_ISCHR (st.st_mode)) {
	//_cairo_error_throw (CAIRO_STATUS_INVALID_DEVICE);
	return _cairo_device_create_in_error (CAIRO_STATUS_NO_MEMORY);
    }

    udev = udev_new ();

    device = udev_device_new_from_devnum (udev, 'c', st.st_rdev);
    if (device != NULL) {
	dev = _cairo_drm_device_get_internal (device, fd);
	udev_device_unref (device);
    }

    udev_unref (udev);

    return dev;
}
slim_hidden_def (cairo_drm_device_get_for_fd);

static cairo_device_t *
_cairo_drm_device_default_internal (void)
{
    struct udev *udev;
    struct udev_enumerate *e;
    struct udev_list_entry *entry;
    cairo_device_t *dev;

    /* optimistic atomic pointer read */
    dev = &_cairo_drm_default_device->base;
    if (dev != NULL)
	return dev;

    udev = udev_new();
    if (udev == NULL)
	return _cairo_device_create_in_error (CAIRO_STATUS_NO_MEMORY);

    e = udev_enumerate_new (udev);
    udev_enumerate_add_match_subsystem (e, "drm");
    udev_enumerate_scan_devices (e);
    udev_list_entry_foreach (entry, udev_enumerate_get_list_entry (e)) {
	struct udev_device *device;

	device =
	    udev_device_new_from_syspath (udev,
		    udev_list_entry_get_name (entry));

	dev = _cairo_drm_device_get_internal (device, -1);

	udev_device_unref (device);

	if (dev != NULL) {
	    if (((cairo_drm_device_t *) dev)->fd == -1) {
		/* try again, we may find a usable card */
		cairo_device_destroy (dev);
		dev = NULL;
	    } else
		break;
	}
    }
    udev_enumerate_unref (e);
    udev_unref (udev);

    cairo_device_destroy (dev); /* owned by _cairo_drm_default_device */
    return dev;
}

#ifdef CAIRO_HAS_MINIGUI_SURFACE
#include <minigui/common.h>
#include <minigui/minigui.h>
#include <minigui/gdi.h>
#endif

/**
 * cairo_drm_device_default:
 *
 * Returns the default DRM device. If MiniGUI backend is enabled,
 * this function will try to use the DRI device file descriptor
 * opened by MiniGUI to create the cairo device. Otherwise,
 * this function will try to open the default DRI device and
 * return the cairo device.
 *
 * Return value: the pointer to the cairo device.
 *
 * Since: 2.18
 **/
cairo_device_t *
cairo_drm_device_default (void)
{
#if defined(CAIRO_HAS_MINIGUI_SURFACE) && defined(_MGGAL_DRM)
    GHANDLE vh;
    int fd;

    vh = GetVideoHandle (HDC_SCREEN);
    if (!vh) {
        goto fallback;
    }

    fd = drmGetDeviceFD(vh);
    if (fd < 0) {
        goto fallback;
    }

    return cairo_drm_device_get_for_fd (fd);
#endif

 fallback:
    return _cairo_drm_device_default_internal ();
}
slim_hidden_def (cairo_drm_device_default);

void
_cairo_drm_device_reset_static_data (void)
{
    if (_cairo_drm_default_device != NULL) {
	cairo_device_t *device = &_cairo_drm_default_device->base;
	_cairo_drm_default_device = NULL;
	cairo_device_destroy (device);
    }
}

/**
 * cairo_drm_device_get_fd:
 *
 * Returns the file descriptor which corresponds to the cairo device.
 *
 * Return value: the file descriptor of the DRI device, -1 on error.
 *
 * Since: 2.18
 **/
int
cairo_drm_device_get_fd (cairo_device_t *abstract_device)
{
    cairo_drm_device_t *device = (cairo_drm_device_t *) abstract_device;

    if (device->base.status)
	return -1;

    return device->fd;
}

void
_cairo_drm_device_fini (cairo_drm_device_t *device)
{
    if (device->fd != -1)
	close (device->fd);
}

/**
 * cairo_drm_device_throttle:
 * @abstract_device: The cairo device object for the DRI device.
 *
 * Throttles the cairo device object.
 *
 * Since: 2.18
 **/
void
cairo_drm_device_throttle (cairo_device_t *abstract_device)
{
    cairo_drm_device_t *device = (cairo_drm_device_t *) abstract_device;
    cairo_status_t status;

    if (unlikely (device->base.status))
	return;

    if (device->device.throttle == NULL)
	return;

    status = device->device.throttle (device);
    if (unlikely (status))
	_cairo_status_set_error (&device->base.status, status);
}
slim_hidden_def (cairo_drm_device_throttle);

cairo_bool_t
_cairo_drm_size_is_valid (cairo_device_t *abstract_device,
			  int width, int height)
{
    cairo_drm_device_t *device = (cairo_drm_device_t *) abstract_device;

    if (unlikely (device->base.status))
	return FALSE;

    return width  <= device->max_surface_size &&
	   height <= device->max_surface_size;
}
