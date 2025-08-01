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
	if (status & STATUS_PANTILT_SPEED_CHANGED) {
		pantilt_rel();
	}
	if (status & STATUS_FOCUS_SPEED_CHANGED) {
		focus_rel();
	}
	if (status & STATUS_ZOOM_SPEED_CHANGED) {
		zoom_rel();
	}

	status &= ~(STATUS_PANTILT_SPEED_CHANGED | STATUS_ZOOM_SPEED_CHANGED | STATUS_FOCUS_SPEED_CHANGED);
	ptz_info("do_update");
}

void PTZNDI::set_config(const OBSData ptz_data)
{
	source_name = obs_data_get_string(ptz_data, "source_name");
	ptz_debug("source_name: '%s'", source_name);

	if (instance) {
		ptz_debug("cleanup instance");
		ndi->lib->recv_destroy(instance);
		instance = nullptr;
		ptz_debug("cleanup instance done");
	}

	if (!strcmp(source_name, "")) {
		return;
	}

	ptz_debug("connecting to source '%s'", source_name);

	NDIlib_source_t src;
	src.p_ndi_name = source_name;

	NDIlib_recv_create_v3_t recv;
	recv.bandwidth = NDIlib_recv_bandwidth_metadata_only;
	recv.source_to_connect_to = src;

	instance = ndi->lib->recv_create_v3(&recv);
	if (!instance) {
		ptz_log(LOG_WARNING, "unable to connect to NDI source '%s'", source_name);
		return;
	}
	ptz_info("connected to source '%s'", source_name);

	if (ndi->lib->recv_ptz_is_supported(instance)) {
		ptz_log(LOG_WARNING, "NDI source '%s' does not support PTZ", source_name);
		return;
	}
	ptz_debug("source '%s' supports ptz", source_name);
}

OBSData PTZNDI::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_string(config, "source_name", source_name);
	obs_data_set_default_string(config, "source_name", "");
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
	obs_property_list_item_disable(sources_list, 0, true);

	auto finder_callback = [this](void *ndi_names) {
		const auto ndi_sources = static_cast<std::vector<std::string> *>(ndi_names);
		obs_property_list_clear(sources_list);

		if (ndi_sources->empty()) {
			obs_property_list_add_string(sources_list, obs_module_text("PTZ.NDI.NoSourcesFound"), "");
			obs_property_list_item_disable(sources_list, 0, true);
			return;
		}

		ptz_debug("Found %lu NDI sources via callback", ndi_sources->size());
		for (auto &source : *ndi_sources) {
			obs_property_list_add_string(sources_list, source.c_str(), source.c_str());
		}
	};

	const auto ndi_sources = NDIFinder::getNDISourceList(finder_callback);
	ptz_debug("Got %lu NDI sources directly", ndi_sources.size());
	for (auto &source : ndi_sources) {
		obs_property_list_add_string(sources_list, source.c_str(), source.c_str());
	}

	return ptz_props;
}

void PTZNDI::pantilt_rel() const
{
	ptz_info("pantilt_rel");
	if (pan_speed < -1.0 || pan_speed > 1.0)
		return;
	if (tilt_speed < -1.0 || tilt_speed > 1.0)
		return;

	const auto sent = ndi->lib->recv_ptz_pan_tilt_speed(instance, static_cast<float>(pan_speed),
							    static_cast<float>(tilt_speed));
	if (!sent) {
		ptz_log(LOG_WARNING, "Couldn't send recv_ptz_pan_tilt_speed(%f, %f) to '%s'",
			static_cast<float>(pan_speed), static_cast<float>(tilt_speed), source_name);
	}
}

void PTZNDI::pantilt_home()
{
	ptz_info("pantilt_home");
	const auto sent = ndi->lib->recv_ptz_pan_tilt(instance, 0.0f, 0.0f);
	if (!sent) {
		ptz_log(LOG_WARNING, "Couldn't send recv_ptz_pan_tilt(0,0) to '%s'", source_name);
	}
}

void PTZNDI::zoom_rel() const
{
	ptz_info("zoom_rel");
	if (zoom_speed < -1.0 || zoom_speed > 1.0)
		return;

	const auto sent = ndi->lib->recv_ptz_zoom_speed(instance, static_cast<float>(zoom_speed));
	if (!sent) {
		ptz_log(LOG_WARNING, "Couldn't send recv_ptz_zoom_speed to '%s'", source_name);
	}
}

void PTZNDI::focus_rel() const
{
	ptz_info("focus_abs");
	if (focus_speed < -1.0 || focus_speed > 1.0)
		return;

	const auto sent = ndi->lib->recv_ptz_focus_speed(instance, static_cast<float>(focus_speed));
	if (!sent) {
		ptz_log(LOG_WARNING, "Couldn't send recv_ptz_focus_speed to '%s'", source_name);
	}
}

void PTZNDI::set_autofocus(const bool enabled)
{
	ptz_info("autofocus");
	if (!enabled)
		return;

	const auto sent = ndi->lib->recv_ptz_auto_focus(instance);
	if (!sent) {
		ptz_log(LOG_WARNING, "Couldn't send recv_ptz_auto_focus to '%s'", source_name);
	}
}

void PTZNDI::memory_set(const int i)
{
	ptz_info("memory_set");
	if (i < 0 || i > 99)
		return;

	const auto sent = ndi->lib->recv_ptz_store_preset(instance, i);
	if (!sent) {
		ptz_log(LOG_WARNING, "Couldn't send recv_ptz_store_preset to '%s'", source_name);
	}
}

void PTZNDI::memory_recall(const int i)
{
	ptz_info("memory_recall");
	if (i < 0 || i > 99)
		return;

	const auto sent = ndi->lib->recv_ptz_recall_preset(instance, i, 0.5f);
	if (!sent) {
		ptz_log(LOG_WARNING, "Couldn't send recv_ptz_recall_preset to '%s'", source_name);
	}
}
