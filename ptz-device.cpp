/* Pan Tilt Zoom device factory
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QDebug>
#include <QObject>
#include <QtGlobal>
#include "../../obs-app.hpp"
#include "ptz-device.hpp"
#include "ptz-visca.hpp"

PTZDevice *PTZDevice::make_device(obs_data_t *config)
{
	std::string type = obs_data_get_string(config, "type");

	if (type == "sim")
		return new PTZSimulator(config);
	if (type == "visca")
		return new PTZVisca(config);
	return NULL;
}
