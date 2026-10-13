/* PTZ UI test harness: the discovery instances' registration of the PTZ API
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include "ptz-driver-kinds.hpp"
#include "ptz-list-model.hpp"

namespace {

/* Makes, and destroys again, one instance of each discovery kind, which
 * registers its procs and signals, so that a build with the harness logs them
 * as "[ptz-api]" lines (see ptz_proc_add()) as a device's are when it is made.
 * Writes the kinds it made one of, each with what its ptz_get_api_info returned
 * as "api". */
void runMakeDiscoveryInstancesTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] make_discovery_instances: missing filename");
		return;
	}

	OBSDataArrayAutoRelease made = obs_data_array_create();
	for (const auto &kind : ptz_discovery_kinds()) {
		OBSSourceAutoRelease instance =
			obs_source_create_private(qUtf8Printable(kind.kind), "ptz-ui-test", nullptr);
		if (!instance)
			continue;
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "kind", qUtf8Printable(kind.kind));
		/* What the instance says the API has, as another plugin would ask it */
		calldata_t cd = {};
		if (proc_handler_call(obs_source_get_proc_handler(instance), "ptz_get_api_info", &cd)) {
			OBSDataAutoRelease api = static_cast<obs_data_t *>(calldata_ptr(&cd, "return"));
			obs_data_set_obj(item, "api", api);
		}
		calldata_free(&cd);
		obs_data_array_push_back(made, item);
	}
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_array(result, "made", made);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] make_discovery_instances: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* make_discovery_instances request params:
 *   filename - where to write {"made": [{"kind", "api"}...]}: the kinds it made an
 *              instance of, each with what the instance's ptz_get_api_info returned
 */
void registerApiVersionTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("make_discovery_instances"), &runMakeDiscoveryInstancesTest);
}

/* make_discovery_instances request params:
 *   filename  - where to write the {"made": [{"kind"}...]} JSON result
 */
