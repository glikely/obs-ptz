/* PTZ UI test harness: device change signals test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>

#include "ptz-list-model.hpp"

namespace {

/* What PTZListModel's deviceSettingsUpdated()/deviceStateUpdated() have
 * said about one device since watching began */
struct Seen {
	int settings = 0;
	int state = 0;
	QSet<QString> stateKeys; /* every key any state change reported as changed */
};

QMap<QString, Seen> seen;
QObject *listener = nullptr;

/* Starts (or restarts) listening to the model's signals, forgetting
 * whatever was seen before. Has to be asked for, rather than always
 * listening, since the model is only there once OBS has loaded the plugin. */
void runWatchDeviceSignalsTest(const QMap<QString, QString> &)
{
	seen.clear();
	if (listener)
		return;
	listener = new QObject(ptzDeviceList);
	QObject::connect(ptzDeviceList, &PTZListModel::deviceSettingsUpdated, listener,
			 [](const QString &id) { seen[id].settings++; });
	QObject::connect(ptzDeviceList, &PTZListModel::deviceStateUpdated, listener,
			 [](const QString &id, OBSData changed) {
				 Seen &s = seen[id];
				 s.state++;
				 for (obs_data_item_t *item = obs_data_first(changed); item; obs_data_item_next(&item))
					 s.stateKeys.insert(obs_data_item_get_name(item));
			 });
}

void runGetDeviceSignalsTest(const QMap<QString, QString> &params)
{
	QString deviceName = params.value(QStringLiteral("device"));
	bool deviceOk = !deviceName.isEmpty();
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_signals: missing/invalid device or filename");
		return;
	}

	QModelIndex index = ptzUITestDeviceIndex(deviceName);
	const Seen s = seen.value(index.data(PTZListModel::DeviceUuidRole).toString());
	OBSDataArrayAutoRelease keys = obs_data_array_create();
	for (const QString &key : s.stateKeys) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "key", qUtf8Printable(key));
		obs_data_array_push_back(keys, item);
	}
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_int(result, "settings_count", s.settings);
	obs_data_set_int(result, "state_count", s.state);
	obs_data_set_array(result, "state_keys", keys);
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_device_signals: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* watch_device_signals takes no params: it starts counting.
 *
 * get_device_signals request params:
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   filename  - where to write the {"settings_count", "state_count",
 *               "state_keys": [{"key"}...]} JSON result: how many times
 *               each signal was emitted for the device since
 *               watch_device_signals, and every key a state change
 *               reported as changed
 */
void registerDeviceSignalsTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("watch_device_signals"), &runWatchDeviceSignalsTest);
	harness->registerTest(QStringLiteral("get_device_signals"), &runGetDeviceSignalsTest);
}
