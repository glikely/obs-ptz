/* PTZ UI test harness: device source binding test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include "ptz.h"
#include "ptz-list-model.hpp"

namespace {

/* Reports which OBS source a device is bound to, and what the device
 * itself is called, as JSON:
 *
 *   name         - the device's name as the device list shows it
 *                  (PTZListModel's DisplayRole)
 *   config_name  - the "name" the device would save to the config file
 *                  (PTZDevice::save())
 *   bound        - whether the device currently resolves to a source
 *   source       - that source's name, "" if not bound
 *   source_uuid  - that source's UUID, "" if not bound. Unlike the name,
 *                  this tells apart a source that was removed and
 *                  recreated under the same name
 *   live/locked  - whether the device's source is in the program scene
 *                  (PTZListModel's IsLiveRole/IsLockedRole). Only refreshed
 *                  by scene changes, and it is only true if the device's
 *                  source is the very object in the scene, which makes it
 *                  a behavioural check on the binding, not just a name
 *                  comparison
 *
 * The source is found the way the rest of the plugin finds it
 * (ptz_device_get_parent_source(), i.e. PTZDevice::source()),
 * which is also what binds a device to a source that has only just
 * appeared, so asking is not a neutral observation. */
void runDeviceSourceTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceIdOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_source: missing/invalid device_id or filename");
		return;
	}

	QModelIndex index = ptzDeviceList->indexFromDeviceId(deviceId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_source: device_id %u not found", deviceId);
		return;
	}

	OBSDataAutoRelease config = obs_data_create();
	ptzDeviceList->save(index, config.Get());

	OBSSourceAutoRelease source = ptz_device_get_parent_source(deviceId);

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_string(result, "name", qUtf8Printable(ptzDeviceList->data(index, Qt::DisplayRole).toString()));
	obs_data_set_string(result, "config_name", obs_data_get_string(config, "name"));
	obs_data_set_bool(result, "bound", source != nullptr);
	obs_data_set_string(result, "source", source ? obs_source_get_name(source) : "");
	obs_data_set_string(result, "source_uuid", source ? obs_source_get_uuid(source) : "");
	obs_data_set_bool(result, "live", ptzDeviceList->data(index, PTZListModel::IsLiveRole).toBool());
	obs_data_set_bool(result, "locked", ptzDeviceList->data(index, PTZListModel::IsLockedRole).toBool());

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_device_source: failed to write %s", qUtf8Printable(filename));
}

/* Takes (or drops) a strong reference to a source, the way another plugin,
 * a dock or a script holding on to a source would. A source that has been
 * removed from OBS isn't destroyed until every reference to it is gone, so
 * this keeps a removed source alive to see what a device does about it.
 * Removing an input over obs-websocket is otherwise no test of that: how
 * soon the removed source is destroyed depends on what else happens to be
 * referencing it (which varies with the platform and the other plugins
 * loaded), so a device that only notices a source when it is destroyed
 * passes or fails depending on that. */
QMap<QString, OBSSource> heldSources;

void runHoldSourceTest(const QMap<QString, QString> &params)
{
	QString name = params.value(QStringLiteral("name"));
	QString filename = params.value(QStringLiteral("filename"));
	bool release = params.value(QStringLiteral("release")) == QStringLiteral("1");

	bool ok = false;
	if (release) {
		ok = heldSources.remove(name) > 0;
	} else {
		OBSSourceAutoRelease source = obs_get_source_by_name(qUtf8Printable(name));
		if (source) {
			heldSources[name] = OBSSource(source.Get());
			ok = true;
		}
	}

	if (filename.isEmpty())
		return;
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "ok", ok);
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] hold_source: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   device_id - the target device's numeric id
 *   filename  - where to write the {"name", "config_name", "bound",
 *               "source", "source_uuid", "live", "locked"} JSON result
 *
 * hold_source request params:
 *   name      - the source's name
 *   release   - "1" to drop the reference held for `name` instead of
 *               taking one
 *   filename  - optional; where to write {"ok"}, whether there was a
 *               source to hold or a reference to drop
 */
void registerDeviceSourceTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_device_source"), &runDeviceSourceTest);
	harness->registerTest(QStringLiteral("hold_source"), &runHoldSourceTest);
}
