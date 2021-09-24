/* Pan Tilt Zoom source plugin
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 *
 * This file implements an OBS source plugin that triggers PTZ device actions,
 * like recalling a preset or initiating a camera move.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <callback/signal.h>
#include "ptz.h"

enum ptz_action_trigger_type {
	PTZ_ACTION_TRIGGER_PROGRAM_ACTIVE = 0,
	PTZ_ACTION_TRIGGER_PREVIEW_ACTIVE,
};

enum ptz_action_type {
	PTZ_ACTION_POWER_OFF = 0,
	PTZ_ACTION_POWER_ON,
	PTZ_ACTION_PRESET_RECALL,
	PTZ_ACTION_PAN_TILT,
	PTZ_ACTION_STOP,
};

struct ptz_action_source_data {
	enum ptz_action_trigger_type trigger;
	unsigned int camera;
	enum ptz_action_type action;
	unsigned int preset;
	double pan_speed;
	double tilt_speed;
	obs_source_t *src;
};

static const char *ptz_action_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "PTZ Action";
}

static void ptz_action_source_update(void *data, obs_data_t *settings)
{
	struct ptz_action_source_data *context = data;

	context->trigger = (unsigned int) obs_data_get_int(settings, "trigger");
	context->camera = (unsigned int) obs_data_get_int(settings, "camera");
	context->action = (unsigned int) obs_data_get_int(settings, "action");
	context->preset = (unsigned int) obs_data_get_int(settings, "preset");
	context->pan_speed = obs_data_get_double(settings, "pan_speed");
	context->tilt_speed = obs_data_get_double(settings, "tilt_speed");
}

static void ptz_action_source_handler(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(calldata);
}

static void ptz_action_source_do_action(struct ptz_action_source_data *context)
{
	calldata_t cd = {0};
	calldata_set_int(&cd, "device_id", context->camera);
	switch (context->action) {
	case PTZ_ACTION_PRESET_RECALL:
		calldata_set_int(&cd, "preset_id", context->preset);
		proc_handler_call(obs_get_proc_handler(), "ptz_preset_recall", &cd);
		break;
	case PTZ_ACTION_PAN_TILT:
		calldata_set_float(&cd, "pan", context->pan_speed);
		calldata_set_float(&cd, "tilt", context->tilt_speed);
		proc_handler_call(obs_get_proc_handler(), "ptz_pantilt", &cd);
		break;
	case PTZ_ACTION_STOP:
		calldata_set_float(&cd, "pan", 0.0);
		calldata_set_float(&cd, "tilt", 0.0);
		proc_handler_call(obs_get_proc_handler(), "ptz_pantilt", &cd);
		break;
	}
	calldata_free(&cd);
}

static void ptz_action_source_fe_callback(enum obs_frontend_event event, void *data)
{
	struct ptz_action_source_data *context = data;
	obs_source_t *source = NULL;

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		if (context->trigger == PTZ_ACTION_TRIGGER_PROGRAM_ACTIVE)
			source = obs_frontend_get_current_scene();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		if (context->trigger == PTZ_ACTION_TRIGGER_PREVIEW_ACTIVE)
			source = obs_frontend_get_current_preview_scene();
		break;
	default:
		return;
	}

	/* For scene changed events, check if the ptz action source is in the
	 * chosen scene. If so then trigger the action. */
	if (source) {
		obs_scene_t *scene = obs_scene_from_source(source);
		obs_sceneitem_t *item = obs_scene_sceneitem_from_source(scene, context->src);
		if (item) {
			obs_sceneitem_release(item);
			ptz_action_source_do_action(context);
		}
		obs_scene_release(scene);
	}
}

static void *ptz_action_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct ptz_action_source_data *context = bzalloc(sizeof(struct ptz_action_source_data));

	context->src = source;
	ptz_action_source_update(context, settings);

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, "hide", ptz_action_source_handler, context);
	signal_handler_connect(sh, "activate", ptz_action_source_handler, context);
	signal_handler_connect(sh, "deactivate", ptz_action_source_handler, context);

	obs_frontend_add_event_callback(ptz_action_source_fe_callback, context);

	return context;
}

static void ptz_action_source_destroy(void *data)
{
	bfree(data);
}

static bool ptz_action_source_device_changed_cb(obs_properties_t *props,
					obs_property_t *prop_camera, obs_data_t *settings)
{
	obs_property_t *prop_preset = obs_properties_get(props, "preset");
	obs_property_list_clear(prop_preset);

	/* Find the camera config */
	int64_t camera = obs_data_get_int(settings, "camera");
	obs_data_array_t *array = ptz_devices_get_config();
	obs_data_t *config = obs_data_array_item(array, camera);

	obs_data_array_t *preset_array = obs_data_get_array(config, "presets");
	if (preset_array) {
		for (int i = 0; i < obs_data_array_count(preset_array); i++) {
			obs_data_t *preset = obs_data_array_item(preset_array, i);
			obs_property_list_add_int(prop_preset, obs_data_get_string(preset, "name"),
						obs_data_get_int(preset, "id"));
			obs_data_release(preset);
		}
		obs_data_array_release(preset_array);
	}

	obs_data_release(config);
	obs_data_array_release(array);
	return true;
}

static void property_set_visible(obs_properties_t *props, const char *name, bool visible)
{
	obs_property_t *prop = obs_properties_get(props, name);
	if (prop)
		obs_property_set_visible(prop, visible);
}

static bool ptz_action_source_action_changed_cb(obs_properties_t *props,
					obs_property_t *prop, obs_data_t *settings)
{
	int64_t action = obs_data_get_int(settings, "action");
	property_set_visible(props, "preset", action == PTZ_ACTION_PRESET_RECALL);
	property_set_visible(props, "pan_speed", action == PTZ_ACTION_PAN_TILT);
	property_set_visible(props, "tilt_speed", action == PTZ_ACTION_PAN_TILT);
	return true;
}

static bool ptz_action_source_test_clicked_cb(obs_properties_t *props, obs_property_t *property,
					void *data)
{
	ptz_action_source_do_action(data);
	return false;
}

static obs_properties_t *ptz_action_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	/* List the possible trigger events */
	obs_property_t *prop = obs_properties_add_list(props, "trigger", "Action Trigger",
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Scene becomes active program",
				PTZ_ACTION_TRIGGER_PROGRAM_ACTIVE);
	obs_property_list_add_int(prop, "Scene becomes active preview",
				PTZ_ACTION_TRIGGER_PREVIEW_ACTIVE);

	/* Enumerate the cameras */
	prop = obs_properties_add_list(props, "camera", "Camera",
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(prop, ptz_action_source_device_changed_cb);
	obs_data_array_t *array = ptz_devices_get_config();
	for (int i = 0; i < obs_data_array_count(array); i++) {
		obs_data_t *config = obs_data_array_item(array, i);
		obs_property_list_add_int(prop, obs_data_get_string(config, "name"), i);
		obs_data_release(config);
	}
	obs_data_array_release(array);

	/* List the possible actions */
	prop = obs_properties_add_list(props, "action", "Action",
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(prop, ptz_action_source_action_changed_cb);
	obs_property_list_add_int(prop, "Preset Recall", PTZ_ACTION_PRESET_RECALL);
	obs_property_list_add_int(prop, "Pan/Tilt", PTZ_ACTION_PAN_TILT);
	obs_property_list_add_int(prop, "Stop", PTZ_ACTION_STOP);

	obs_properties_add_list(props, "preset", "Preset",
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_float_slider(props, "pan_speed", "Pan Speed", -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, "tilt_speed", "Tilt Speed", -1.0, 1.0, 0.01);
	obs_properties_add_button(props, "run_action", "Test Action",
				ptz_action_source_test_clicked_cb);

	return props;
}

struct obs_source_info ptz_action_source = {
	.id = "ptz_action_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.get_name = ptz_action_source_get_name,
	.create = ptz_action_source_create,
	.destroy = ptz_action_source_destroy,
	.update = ptz_action_source_update,
	.get_properties = ptz_action_source_get_properties,
};

void ptz_load_action_source(void)
{
	obs_register_source(&ptz_action_source);
}

