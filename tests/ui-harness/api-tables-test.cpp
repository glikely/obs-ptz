/* PTZ UI test harness: the PTZ API's tables (src/ptz-api.cpp), which this
 * module links as every module of the plugin does, for
 * tests/obs-integration/test_api_doc.py to hold the doc to. The websocket
 * binding's ptz_list_api has only a device's.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include "ptz-api.hpp"

namespace {

void setProcs(obs_data_t *result, const char *key, PTZApiProcs table)
{
	OBSDataArrayAutoRelease list = obs_data_array_create();
	for (const PTZApiProc &entry : table) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "name", entry.name);
		obs_data_set_string(item, "decl", entry.decl);
		obs_data_set_string(item, "access", ptz_api_access_name(entry.access));
		obs_data_array_push_back(list, item);
	}
	obs_data_set_array(result, key, list);
}

void setSignals(obs_data_t *result, const char *key, PTZApiSignals table)
{
	OBSDataArrayAutoRelease list = obs_data_array_create();
	for (const PTZApiSignal &entry : table) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "name", entry.name);
		obs_data_set_string(item, "decl", entry.decl);
		obs_data_array_push_back(list, item);
	}
	obs_data_set_array(result, key, list);
}

void runApiTablesTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] api_tables: no filename");
		return;
	}
	OBSDataAutoRelease result = obs_data_create();
	setProcs(result, "device-proc", ptz_api_device_procs());
	setSignals(result, "device-signal", ptz_api_device_signals());
	setProcs(result, "discovery-proc", ptz_api_discovery_procs());
	setSignals(result, "discovery-signal", ptz_api_discovery_signals());
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] api_tables: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* api_tables: filename - where to write the four tables, "device-proc",
 *   "device-signal", "discovery-proc" and "discovery-signal", each an array of
 *   {"name", "decl"} and, for a proc, "access", as ptz_list_api gives them */
void registerApiTablesTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("api_tables"), &runApiTablesTest);
}
