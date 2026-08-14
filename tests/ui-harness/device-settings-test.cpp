/* PTZ UI test harness: device settings vs. properties test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QStringList>

#include "ptz-device.hpp"
#include "ptz-list-model.hpp"
#include "ptz.h"

namespace {

/* Adds the name of every property that holds a *value* -- not the groups
 * around them, and not the buttons and info text that don't hold one -- to
 * `keys`, descending into groups. */
void collectValueKeys(obs_properties_t *props, QStringList &keys)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		switch (obs_property_get_type(p)) {
		case OBS_PROPERTY_GROUP:
			collectValueKeys(obs_property_group_content(p), keys);
			break;
		case OBS_PROPERTY_BUTTON:
			break;
		case OBS_PROPERTY_TEXT:
			if (obs_property_text_type(p) == OBS_TEXT_INFO)
				break;
			[[fallthrough]];
		default:
			keys << obs_property_name(p);
		}
	}
}

/* Adds each string list property, and the values it offers, to `out`, as
 * {"key", "values": [...]}, descending into groups */
void collectStringLists(obs_properties_t *props, obs_data_array_t *out)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		if (obs_property_get_type(p) == OBS_PROPERTY_GROUP) {
			collectStringLists(obs_property_group_content(p), out);
			continue;
		}
		if (obs_property_get_type(p) != OBS_PROPERTY_LIST ||
		    obs_property_list_format(p) != OBS_COMBO_FORMAT_STRING)
			continue;
		OBSDataArrayAutoRelease values = obs_data_array_create();
		for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
			OBSDataAutoRelease value = obs_data_create();
			obs_data_set_string(value, "value", obs_property_list_item_string(p, i));
			obs_data_array_push_back(values, value);
		}
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "key", obs_property_name(p));
		obs_data_set_array(entry, "values", values);
		obs_data_array_push_back(out, entry);
	}
}

/* The keys of the settings that would be persisted for a device's PTZ
 * filter, after the filter's .save has had its say: what the scene collection
 * would hold. Adds nothing if the device isn't one of this plugin's filters.
 *
 * It is run on a copy of the settings. obs_source_save() runs it on the
 * filter's own settings, and what it writes there is the device's values,
 * over a change made to the settings that the device has yet to be updated
 * with: a test that looks while it waits for one would undo it. */
void collectFilterKeys(obs_source_t *parent, obs_data_array_t *out, obs_data_t *defaultsOut)
{
	struct Find {
		obs_source_t *filter = nullptr;
	} find;
	if (!parent)
		return;
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *filter, void *data) {
			auto f = static_cast<Find *>(data);
			if (!f->filter && ptz_source_is_device(filter))
				f->filter = filter;
		},
		&find);
	if (!find.filter || !QString(obs_source_get_id(find.filter)).startsWith("ca.secretlab.obs-ptz."))
		return;
	OBSDataAutoRelease current = obs_source_get_settings(find.filter);
	/* The string defaults of the live settings, which are not saved: what the device
	 * says for a controller, such as the placeholder of a field */
	OBSDataAutoRelease defaults = obs_data_get_defaults(current);
	for (obs_data_item_t *item = obs_data_first(defaults); item; obs_data_item_next(&item))
		if (obs_data_item_gettype(item) == OBS_DATA_STRING)
			obs_data_set_string(defaultsOut, obs_data_item_get_name(item), obs_data_item_get_string(item));
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_apply(settings, current);
	ptz_filter_save(obs_obj_get_data(find.filter), settings);
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "key", obs_data_item_get_name(item));
		obs_data_array_push_back(out, entry);
	}
}

/* Reports which keys a device's settings properties tree edits
 * ("property_keys", what the Filters dialog and PTZSettings both show, see
 * PTZDevice::get_obs_properties()) against which keys the same device
 * writes when saved ("save_keys", see PTZDevice::save()), as JSON. A
 * property key that save() doesn't write is either not a setting at all,
 * such as state that belongs on the camera's status page instead, or a
 * setting that would be lost on the next save. */
void runGetDeviceSettingsTest(const QMap<QString, QString> &params)
{
	QString deviceName = params.value(QStringLiteral("device"));
	bool deviceOk = !deviceName.isEmpty();
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_settings: missing/invalid device or filename");
		return;
	}

	QModelIndex index = ptzUITestDeviceIndex(deviceName);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_settings: device %s not found", qUtf8Printable(deviceName));
		return;
	}

	QStringList propertyKeys;
	OBSDataArrayAutoRelease listArray = obs_data_array_create();
	obs_properties_t *props = ptzDeviceList->getProperties(index);
	collectValueKeys(props, propertyKeys);
	collectStringLists(props, listArray);
	obs_properties_destroy(props);

	OBSDataAutoRelease saved = obs_data_create();
	ptzDeviceList->save(index, saved.Get());
	OBSDataArrayAutoRelease propertyArray = obs_data_array_create();
	OBSDataArrayAutoRelease saveArray = obs_data_array_create();

	for (const QString &key : propertyKeys) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "key", qUtf8Printable(key));
		obs_data_array_push_back(propertyArray, item);
	}
	for (obs_data_item_t *item = obs_data_first(saved); item; obs_data_item_next(&item)) {
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "key", obs_data_item_get_name(item));
		obs_data_array_push_back(saveArray, entry);
	}

	OBSDataArrayAutoRelease filterArray = obs_data_array_create();
	OBSDataAutoRelease filterDefaults = obs_data_create();
	OBSSource parent = ptzDeviceList->parentSource(index);
	collectFilterKeys(parent, filterArray, filterDefaults);

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_array(result, "property_keys", propertyArray);
	obs_data_set_array(result, "save_keys", saveArray);
	obs_data_set_array(result, "filter_keys", filterArray);
	obs_data_set_obj(result, "filter_defaults", filterDefaults);
	obs_data_set_array(result, "lists", listArray);
	obs_data_set_obj(result, "saved", saved);
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_device_settings: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   filename  - where to write the {"property_keys": [{"key"}...],
 *               "save_keys": [{"key"}...], "filter_keys": [{"key"}...],
 *               "saved": {...what save() wrote, with its values},
 *               "filter_defaults": {...the string defaults of the filter's
 *               live settings, which are not saved},
 *               "lists": [{"key", "values": [{"value"}...]}...], what
 *               each string list property offers} JSON
 *               result. filter_keys is empty unless a PTZ filter owns the
 *               device
 */
void registerDeviceSettingsTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_device_settings"), &runGetDeviceSettingsTest);
}
