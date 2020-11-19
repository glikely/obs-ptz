/* Pan Tilt Zoom device factory
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-device.hpp"
#include "ptz-visca.hpp"

QStringListModel PTZDevice::name_list_model;
std::vector<PTZDevice *> PTZDevice::devices;

PTZDevice *PTZDevice::make_device(obs_data_t *config)
{
	PTZDevice *ptz = nullptr;
	std::string type = obs_data_get_string(config, "type");

	if (type == "sim")
		ptz = new PTZSimulator(config);
	if (type == "visca")
		ptz = new PTZVisca(config);
	return ptz;
}
