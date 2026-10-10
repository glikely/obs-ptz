/* PTZ UI test harness: the driver kinds the controller finds
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs-module.h>
#include <obs.hpp>

#include "ptz-driver-kinds.hpp"

namespace {

void runDriverKindsTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] driver_kinds: missing filename");
		return;
	}

	OBSDataArrayAutoRelease kinds = obs_data_array_create();
	for (const auto &driver : ptz_driver_kinds()) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "kind", qUtf8Printable(driver.kind));
		obs_data_set_string(item, "name", qUtf8Printable(driver.name));
		/* The defaults as values, which is what JSON can hold */
		OBSDataAutoRelease defaults = obs_data_get_defaults(driver.defaults);
		obs_data_set_obj(item, "defaults", defaults);
		obs_data_array_push_back(kinds, item);
	}
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_array(result, "kinds", kinds);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] driver_kinds: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* driver_kinds request params:
 *   filename  - where to write the {"kinds": [{"kind", "name", "defaults"}...]}
 *               JSON result of ptz_driver_kinds(), "defaults" being the kind's
 *               defaults as an object
 */
void registerDriverKindsTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("driver_kinds"), &runDriverKindsTest);
}
