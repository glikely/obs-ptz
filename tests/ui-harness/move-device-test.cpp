/* PTZ UI test harness: absolute/relative pan-tilt-zoom move test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include "ptz-list-model.hpp"

namespace {

/* Drives PTZDevice::move_abs()/move_rel() (the "ptz_move_abs"/
 * "ptz_move_rel" proc handlers, see src/ptz-device.cpp) directly.
 * There's no ptz_action_source action type for either -- its PTZ_ACTION_*
 * enum (src/ptz-action-source.c) only covers continuous pan/tilt, stop,
 * presets and power -- so this is the only way
 * tests/obs-integration/test_absolute_relative_moves.py can exercise
 * PTZVisca::pantilt_abs()/pantilt_rel()/zoom_abs() end to end. */
void runMoveDeviceTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	QString mode = params.value(QStringLiteral("mode"));
	if (!deviceIdOk || (mode != QStringLiteral("abs") && mode != QStringLiteral("rel"))) {
		blog(LOG_INFO, "[ptz-ui-test] move_device: missing/invalid device_id or mode");
		return;
	}

	QModelIndex index = ptzDeviceList->indexFromDeviceId(deviceId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] move_device: device_id %u not found", deviceId);
		return;
	}

	/* PTZDevice::move_abs()/move_rel() only touch the axes actually
	 * present in calldata (pan/tilt together, zoom independently -- see
	 * their own implementations), so leaving an axis out of params
	 * leaves it alone rather than forcing it to 0. */
	calldata cd = {};
	if (params.contains(QStringLiteral("pan")))
		calldata_set_float(&cd, "pan", params.value(QStringLiteral("pan")).toDouble());
	if (params.contains(QStringLiteral("tilt")))
		calldata_set_float(&cd, "tilt", params.value(QStringLiteral("tilt")).toDouble());
	if (params.contains(QStringLiteral("zoom")))
		calldata_set_float(&cd, "zoom", params.value(QStringLiteral("zoom")).toDouble());

	const char *method = (mode == QStringLiteral("abs")) ? "ptz_move_abs" : "ptz_move_rel";
	ptzDeviceList->callDevice(index, method, &cd);
	calldata_free(&cd);

	blog(LOG_INFO, "[ptz-ui-test] move_device device_id=%u mode=%s", deviceId, qUtf8Printable(mode));
}

} // namespace

/* Request params:
 *   device_id     - the target device's numeric id
 *   mode          - "abs" (dispatches "ptz_move_abs") or "rel"
 *                   (dispatches "ptz_move_rel")
 *   pan/tilt/zoom - optional; only the axes provided are set
 */
void registerMoveDeviceTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("move_device"), &runMoveDeviceTest);
}
