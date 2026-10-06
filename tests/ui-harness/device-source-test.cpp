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
 *   found        - whether there is such a device; nothing else is there
 *                  if not
 *   uuid         - the UUID of the device's filter
 *   name         - the device's name as the device list shows it
 *                  (PTZListModel's DisplayRole)
 *   bound        - whether the device currently resolves to a source
 *   source       - that source's name, "" if not bound
 *   source_uuid  - that source's UUID, "" if not bound. Unlike the name,
 *                  this tells apart a source that was removed and
 *                  recreated under the same name
 *   live/preview/locked
 *                - whether the device's source is in the program scene, in
 *                  the preview scene (Studio Mode only), and whether the
 *                  device is locked (PTZListModel's IsLiveRole/
 *                  IsPreviewRole/IsLockedRole). Only refreshed by scene
 *                  changes, and live/preview are only true if the device's
 *                  source is the very object in the scene, which makes
 *                  them a behavioural check on the binding, not just a
 *                  name comparison
 *
 *   proc_bound, proc_source, proc_source_uuid
 *                - the same three, as the device says them itself when asked
 *                  with its ptz_get_parent_source proc, through its proc
 *                  handler, as PTZListModel::parentSource() asks
 *
 * The source is found the way the rest of the plugin finds it
 * (ptz_device_get_parent_source(), i.e. PTZDevice::source()),
 * which is also what binds a device to a source that has only just
 * appeared, so asking is not a neutral observation. The proc is asked
 * first, so that when it is the one that binds a late source, it is what is
 * seen to. */
void runDeviceSourceTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_source: missing filename");
		return;
	}

	QModelIndex index;
	index = ptzUITestDeviceIndex(params.value(QStringLiteral("device")));

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "found", index.isValid());
	if (index.isValid()) {
		QString uuid = ptzDeviceList->data(index, PTZListModel::DeviceUuidRole).toString();

		calldata_t cd = {};
		ptzDeviceList->callDevice(index, "ptz_get_parent_source", &cd);
		OBSSourceAutoRelease procSource = static_cast<obs_source_t *>(calldata_ptr(&cd, "return"));
		calldata_free(&cd);

		obs_source_t *source = procSource;

		obs_data_set_bool(result, "proc_bound", procSource != nullptr);
		obs_data_set_string(result, "proc_source", procSource ? obs_source_get_name(procSource) : "");
		obs_data_set_string(result, "proc_source_uuid", procSource ? obs_source_get_uuid(procSource) : "");
		obs_data_set_string(result, "uuid", qUtf8Printable(uuid));
		obs_data_set_string(result, "name",
				    qUtf8Printable(ptzDeviceList->data(index, Qt::DisplayRole).toString()));
		obs_data_set_bool(result, "bound", source != nullptr);
		obs_data_set_string(result, "source", source ? obs_source_get_name(source) : "");
		obs_data_set_string(result, "source_uuid", source ? obs_source_get_uuid(source) : "");
		obs_data_set_bool(result, "live", ptzDeviceList->data(index, PTZListModel::IsLiveRole).toBool());
		obs_data_set_bool(result, "preview", ptzDeviceList->data(index, PTZListModel::IsPreviewRole).toBool());
		obs_data_set_bool(result, "locked", ptzDeviceList->data(index, PTZListModel::IsLockedRole).toBool());
	}

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

/* Locks or unlocks a device the way a click on its padlock in the device list
 * does: by setting PTZListModel's IsLockedRole */
void runSetDeviceLockedTest(const QMap<QString, QString> &params)
{
	QModelIndex index = ptzUITestDeviceIndex(params.value(QStringLiteral("device")));
	bool ok = index.isValid() && ptzDeviceList->setData(index, params.value(QStringLiteral("locked")) == QStringLiteral("1"),
							    PTZListModel::IsLockedRole);
	blog(LOG_INFO, "[ptz-ui-test] set_device_locked: %s", ok ? "set" : "unchanged or no such device");
}

} // namespace

/* get_device_source request params:
 *   device    - the device, by the UUID of its filter or the name of the source it is on
 *   filename  - where to write the {"found", "uuid", "name", "bound",
 *               "source", "source_uuid", "live", "preview", "locked"} JSON result. Only "found" is
 *               there if there is no such device
 *
 * set_device_locked request params:
 *   device    - as above
 *   locked    - "1" to lock the device, anything else to unlock it
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
	harness->registerTest(QStringLiteral("set_device_locked"), &runSetDeviceLockedTest);
}
