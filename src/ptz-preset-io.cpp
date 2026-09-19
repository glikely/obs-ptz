/* Pan Tilt Zoom Controls - preset (de)serialization
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ptz-preset-io.hpp"
#include "protocol-helpers.hpp"

#include <algorithm>

namespace ptz_preset_io {

void exportPresets(OBSData config, size_t maxPresets, const QMap<size_t, QVariantMap> &presets,
		   const QList<size_t> &displayOrder)
{
	obs_data_set_int(config, "preset_max", (long long)maxPresets);

	OBSDataArrayAutoRelease preset_array = obs_data_array_create();
	for (auto id : displayOrder) {
		OBSDataAutoRelease data = variantMapToOBSData(presets[id]);
		obs_data_set_int(data, "id", id);
		obs_data_array_push_back(preset_array, data);
	}
	obs_data_set_array(config, "presets", preset_array);
}

size_t importPresets(OBSData config, size_t currentMax, QMap<size_t, QVariantMap> &presets, QList<size_t> &displayOrder)
{
	obs_data_set_default_int(config, "preset_max", (long long)currentMax);
	size_t maxPresets = std::clamp<size_t>(obs_data_get_int(config, "preset_max"), 1, 128);

	OBSDataArrayAutoRelease preset_array = obs_data_get_array(config, "presets");
	presets.clear();
	displayOrder.clear();
	for (size_t i = 0; i < obs_data_array_count(preset_array); i++) {
		OBSDataAutoRelease item = obs_data_array_item(preset_array, i);
		auto id = obs_data_get_int(item, "id");
		if (displayOrder.contains(id))
			continue;
		presets[id] = OBSDataToVariantMap(item.Get());
		displayOrder.append(id);
	}
	return maxPresets;
}

} // namespace ptz_preset_io
