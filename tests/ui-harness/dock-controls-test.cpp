/* PTZ UI test harness: what the dock's hotkeys are called
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <cstring>

namespace {

struct HotkeyFind {
	const char *name;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	QString description;
};

bool findHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	auto *find = static_cast<HotkeyFind *>(data);
	if (strcmp(obs_hotkey_get_name(key), find->name) != 0)
		return true;
	find->id = id;
	find->description = QString::fromUtf8(obs_hotkey_get_description(key));
	return false;
}

/* Writes {"found", "description"} of a hotkey, to `filename` */
void runGetHotkeyTest(const QMap<QString, QString> &params)
{
	QByteArray name = params.value(QStringLiteral("name")).toUtf8();
	HotkeyFind find{name.constData()};
	obs_enum_hotkeys(findHotkey, &find);
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "found", find.id != OBS_INVALID_HOTKEY_ID);
	obs_data_set_string(result, "description", qUtf8Printable(find.description));
	obs_data_save_json_safe(result, qUtf8Printable(params.value(QStringLiteral("filename"))), "tmp", "bak");
}

} // namespace

/* get_hotkey request params:
 *   name     - the hotkey's name, such as "PTZ.Recall2"
 *   filename - where to write the {"found", "description"} JSON result
 */
void registerDockControlsTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_hotkey"), &runGetHotkeyTest);
}
