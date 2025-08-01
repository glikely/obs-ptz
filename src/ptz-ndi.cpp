/* Pan Tilt Zoom over NDI
*
 * Copyright 2025 Christian Mäder <mail@cmaeder.ch>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-ndi.hpp"

#include "ndi-finder.hpp"

QString PTZNDI::description()
{
	return QString("NDI/%1").arg(source_name);
}

PTZNDI::PTZNDI(OBSData ptz_data) : PTZDevice(ptz_data), instance(nullptr)
{
	PTZNDI::set_config(ptz_data);
	ptz_debug("device created");
}

PTZNDI::~PTZNDI()
{
	if (!instance)
		return;

	ndi->lib->recv_destroy(instance);
}

void PTZNDI::do_update()
{
	status &= ~(STATUS_PANTILT_SPEED_CHANGED | STATUS_ZOOM_SPEED_CHANGED | STATUS_FOCUS_SPEED_CHANGED);
}

void PTZNDI::set_config(const OBSData ptz_data)
{
	source_name = obs_data_get_string(ptz_data, "source_name");

	if (instance) {
		ndi->lib->recv_destroy(instance);
		instance = nullptr;
	}

	if (strcmp(source_name, "")) {
		return;
	}

	ptz_info("connecting to source '%s'", source_name);

	NDIlib_source_t src;
	src.p_ndi_name = source_name;

	NDIlib_recv_create_v3_t recv;
	recv.bandwidth = NDIlib_recv_bandwidth_metadata_only;
	recv.source_to_connect_to = src;

	instance = ndi->lib->recv_create_v3(&recv);
	if (!instance) {
		ptz_log(LOG_WARNING, "unable to connect to source '%s'", source_name);
	}
}

OBSData PTZNDI::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_string(config, "source_name", source_name);
	return config;
}

obs_properties_t *PTZNDI::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZDevice::get_obs_properties();

	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, obs_module_text("PTZ.NDI.Name"));

	sources_list = obs_properties_add_list(config, "source_name", obs_module_text("PTZ.NDI.SourceName"),
					       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources_list, obs_module_text("PTZ.NDI.Loading"), "");

	auto finder_callback = [this](void *ndi_names) {
		const auto ndi_sources = static_cast<std::vector<std::string> *>(ndi_names);
		obs_property_list_clear(sources_list);

		ptz_debug("Callback from NDI");

		if (ndi_sources->empty()) {
			obs_property_list_add_string(sources_list, obs_module_text("PTZ.NDI.NoSourcesFound"), "");
		}
		for (auto &source : *ndi_sources) {
			obs_property_list_add_string(sources_list, source.c_str(), source.c_str());
		}
	};

	const auto ndi_sources = NDIFinder::getNDISourceList(finder_callback);
	for (auto &source : ndi_sources) {
		obs_property_list_add_string(sources_list, source.c_str(), source.c_str());
	}

	return ptz_props;
}

void PTZNDI::pantilt_abs(const double pan, const double tilt)
{
	if (pan < -1.0 || pan > 1.0)
		return;
	if (tilt < -1.0 || tilt > 1.0)
		return;

	ndi->lib->recv_ptz_pan_tilt(instance, static_cast<float>(pan), static_cast<float>(tilt));
	ptz_debug("pantilt_abs");
}

void PTZNDI::pantilt_rel(const double panSpeed, const double tiltSpeed)
{
	if (panSpeed < -1.0 || panSpeed > 1.0)
		return;
	if (tiltSpeed < -1.0 || tiltSpeed > 1.0)
		return;

	ndi->lib->recv_ptz_pan_tilt_speed(instance, static_cast<float>(panSpeed), static_cast<float>(tiltSpeed));
	ptz_debug("pantilt_rel");
}

void PTZNDI::pantilt_home()
{
	ndi->lib->recv_ptz_pan_tilt(instance, 0.0f, 0.0f);
	ptz_debug("pantilt_home");
}

void PTZNDI::zoom_speed_set(const double speed)
{
	zoom_speed = static_cast<float>(speed);
}

void PTZNDI::zoom_abs(const double pos)
{
	if (pos < 0.0 || pos > 1.0)
		return;

	ndi->lib->recv_ptz_zoom(instance, static_cast<float>(pos));
	ptz_debug("zoom_abs");
}

void PTZNDI::zoom_rel(const double speed) const
{
	if (speed < -1.0 || speed > 1.0)
		return;

	ndi->lib->recv_ptz_zoom_speed(instance, static_cast<float>(speed));
	ptz_debug("zoom_rel");
}

void PTZNDI::focus_abs(const double pos)
{
	if (pos < 0.0 || pos > 1.0)
		return;

	ndi->lib->recv_ptz_focus(instance, static_cast<float>(pos));
	ptz_debug("focus_abs");
}

void PTZNDI::focus_rel(const double speed) const
{
	if (speed < -1.0 || speed > 1.0)
		return;

	ndi->lib->recv_ptz_focus_speed(instance, static_cast<float>(speed));
	ptz_debug("focus_abs");
}

void PTZNDI::set_autofocus(const bool enabled)
{
	if (!enabled)
		return;

	ndi->lib->recv_ptz_auto_focus(instance);
	ptz_debug("autofocus");
}

void PTZNDI::memory_set(const int i)
{
	if (i < 0 || i > 99)
		return;

	ndi->lib->recv_ptz_store_preset(instance, i);
	ptz_debug("memory_set");
}

void PTZNDI::memory_recall(const int i)
{
	if (i < 0 || i > 99)
		return;

	ndi->lib->recv_ptz_recall_preset(instance, i, 0.5f);
	ptz_debug("memory_recall");
}
