/* Pan Tilt Zoom Controls - preset (de)serialization
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <obs.hpp>
#include <QMap>
#include <QList>
#include <QVariantMap>

/* PTZDevice::exportPresets()/importPresets() delegate to these free
 * functions for the actual "preset_max"/"presets" array (de)serialization,
 * so that logic can be unit tested (tests/preset-io/) without needing a
 * live PTZDevice - which drags in proc_handler_t, the ptzDeviceList
 * singleton, obs-frontend-api, and every concrete camera-protocol
 * subclass just to construct one. */
namespace ptz_preset_io {

/* Serialize maxPresets/presets/displayOrder into config's "preset_max" and
 * "presets" fields, in display order. PTZDevice::save() (via
 * PTZDevice::exportPresets()) and the standalone preset-export feature
 * both write this exact same shape, so an exported file can also be
 * merged directly into a device's config. */
void exportPresets(OBSData config, size_t maxPresets, const QMap<size_t, QVariantMap> &presets,
		   const QList<size_t> &displayOrder);

/* Parse config's "preset_max"/"presets" fields back into presets and
 * displayOrder (both cleared first), returning the resulting preset_max,
 * clamped to [1,128] - the same range enforced by the properties slider,
 * so a corrupt or hand-edited config can't yield an absurd preset count.
 * If config has no "preset_max" value, currentMax is kept rather than
 * substituting an arbitrary default. A preset id repeated in the array
 * keeps only its first occurrence. */
size_t importPresets(OBSData config, size_t currentMax, QMap<size_t, QVariantMap> &presets,
		     QList<size_t> &displayOrder);

} // namespace ptz_preset_io
