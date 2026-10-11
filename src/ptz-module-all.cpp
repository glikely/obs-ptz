/* Pan Tilt Zoom Controls - loading the devices and the controller together
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The one place that reaches across the controller, the device core and the
 * drivers, while they are still one module. It goes away when each driver
 * becomes its own module, which then loads itself; until then
 * scripts/check-module-boundaries.py allows it, and only it, to.
 */

#include <obs-frontend-api.h>
#include "ptz-device.hpp"
#include "ptz-list-model.hpp"
#include "ptz-legacy-migration.hpp"
#include "ptz-visca.hpp"
#include "ptz-soft-cam.hpp"
#include "ptz.h"

/* The optional drivers' headers are only on the include path when they are built */
#if defined(ENABLE_SERIALPORT)
#include "ptz-pelco.hpp"
#endif
#if defined(ENABLE_ONVIF)
#include "ptz-onvif.hpp"
#endif
#if defined(ENABLE_USB_CAM)
#include "ptz-usb-cam.hpp"
#endif

void ptz_load_devices()
{
	obs_frontend_add_event_callback(ptz_module_frontend_event, nullptr);

	/* Constructed here rather than as a plain static-storage global so
	 * its constructor happens at a well-defined point in the module load
	 * instead of at plugin-library-load time -- see PTZListModel::create() */
	PTZListModel::create();

	/* Each backend driver registers its own "<Driver> PTZ Control" OBS filter */
	ptz_visca_register_filter();
#if defined(ENABLE_SERIALPORT)
	ptz_pelco_register_filter();
#endif /* ENABLE_SERIALPORT */
#if defined(ENABLE_ONVIF)
	ptz_onvif_register_filter();
#endif /* ENABLE_ONVIF */
#if defined(ENABLE_USB_CAM)
	ptz_usb_cam_register_filter();
#endif /* ENABLE_USB_CAM */
	/* The Add Device dialog lists the kinds in the order they register */
	ptz_soft_cam_register_filter();
}

void ptz_unload_devices(void)
{
	/* Reverse of construction order */
	obs_frontend_remove_event_callback(ptz_module_frontend_event, nullptr);
	PTZListModel::destroy();
	ptz_legacy_unload();
}
