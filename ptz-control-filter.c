/* Pan Tilt Zoom source plugin
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 *
 * This file implements an OBS source filter plugin that links a source with a PTZ device
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <callback/signal.h>
#include "ptz.h"

struct ptz_control_filter_data {
	obs_source_t *src;
};

static const char *ptz_control_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "PTZ Control";
}

static void *ptz_control_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct ptz_control_filter_data *context = bzalloc(sizeof(struct ptz_control_filter_data));

	context->src = source;

	return context;
}

static void ptz_control_filter_destroy(void *data)
{
	bfree(data);
}

static void ptz_control_filter_update(void *data, obs_data_t *settings)
{
}

static obs_properties_t *ptz_control_filter_get_properties(void *data)
{
	struct ptz_control_filter_data *context = (struct ptz_control_filter_data*)data;
	obs_properties_t *props = obs_properties_create();
	return props;
}

struct obs_source_info ptz_control_filter = {
	.id = "ptz_control_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.get_name = ptz_control_filter_get_name,
	.create = ptz_control_filter_create,
	.destroy = ptz_control_filter_destroy,
	.update = ptz_control_filter_update,
	.get_properties = ptz_control_filter_get_properties,
};

void ptz_load_control_filter(void)
{
	obs_register_source(&ptz_control_filter);
}

