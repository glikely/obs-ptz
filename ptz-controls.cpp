/* Pan Tilt Zoom camera controls
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ptz-controls.hpp"
#include <QToolTip>

#include <obs-module.h>
#include <QMainWindow>
#include <QMenuBar>
#include <QVBoxLayout>
#include "ui_ptz-controls.h"

#include "../../item-widget-helpers.hpp"
#include "../../obs-app.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Grant Likely <grant.likely@secretlab.ca>");
OBS_MODULE_USE_DEFAULT_LOCALE("media-controls", "en-US")

bool obs_module_load()
{
	const auto main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	obs_frontend_push_ui_translation(obs_module_get_string);
	auto *tmp = new PTZControls(main_window);
	obs_frontend_add_dock(tmp);
	obs_frontend_pop_ui_translation();
	return true;
}

void obs_module_unload() {}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("PTZControls");
}

/*
void PTZControls::OBSSignal(void *data, const char *signal,
			      calldata_t *call_data)
{
	obs_source_t *source =
		static_cast<obs_source_t *>(calldata_ptr(call_data, "source"));
	if (!source)
		return;
	uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_CONTROLLABLE_MEDIA) == 0 ||
	    strcmp(signal, "source_destroy") == 0 ||
	    strcmp(signal, "source_remove") == 0)
		return;

	//PTZControls *controls = static_cast<PTZControls *>(data);
	//QMetaObject::invokeMethod(controls, "SignalMediaSource");
}
*/

/*
void PTZControls::OBSFrontendEvent(enum obs_frontend_event event, void *ptr)
{
	//PTZControls *controls = reinterpret_cast<PTZControls *>(ptr);

	switch (event) {
	default:
		break;
	}
}
*/

PTZControls::PTZControls(QWidget *parent)
	: QDockWidget(parent),
	  ui(new Ui::PTZControls),
	  tty_dev("/dev/ttyUSB1")
{
	ui->setupUi(this);

	char *file = obs_module_config_path("config.json");
	obs_data_t *data = nullptr;
	if (file) {
		data = obs_data_create_from_json_file(file);
		bfree(file);
	}
	if (data) {
	}

	OpenInterface();

	connect(ui->dockWidgetContents, &QWidget::customContextMenuRequested,
		this, &PTZControls::ControlContextMenu);

	//signal_handler_connect_global(obs_get_signal_handler(), OBSSignal, this);
	//obs_frontend_add_event_callback(OBSFrontendEvent, this);

	hide();
}

PTZControls::~PTZControls()
{
	//signal_handler_disconnect_global(obs_get_signal_handler(), OBSSignal, this);
	char *file = obs_module_config_path("config.json");
	if (!file) {
		deleteLater();
		return;
	}
	obs_data_t *data = obs_data_create_from_json_file(file);

	CloseInterface();

	if (!data)
		data = obs_data_create();
	obs_data_set_bool(data, "ptzTestBool", true);
	if (!obs_data_save_json(data, file)) {
		char *path = obs_module_config_path("");
		if (path) {
			os_mkdirs(path);
			bfree(path);
		}
		obs_data_save_json(data, file);
	}
	obs_data_release(data);
	bfree(file);
	deleteLater();
}

void PTZControls::OpenInterface()
{
	int camera_count;

	CloseInterface();

	if (!tty_dev)
		return;

	if (VISCA_open_serial(&interface, tty_dev) != VISCA_SUCCESS) {
		return;
	}

	interface.broadcast = 0;
	VISCA_set_address(&interface, &camera_count);
	printf("VISCA Camera count: %i\n", camera_count);
	for (int i = 0; i < camera_count; i++) {
		camera = new PTZCamera(&interface, i+1);
		camera->setParent(this);
	}
}

void PTZControls::CloseInterface()
{
	VISCA_close_serial(&interface);
}

void PTZControls::ControlContextMenu()
{

	/*
	QAction showTimeDecimalsAction(obs_module_text("ShowTimeDecimals"),
				       this);
	showTimeDecimalsAction.setCheckable(true);
	showTimeDecimalsAction.setChecked(showTimeDecimals);

	connect(&showTimeDecimalsAction, &QAction::toggled, this,
		&diaControls::ToggleShowTimeDecimals, Qt::DirectConnection);
	*/

	QMenu popup;
	/*
	popup.addAction(&showTimeDecimalsAction);
	*/
	popup.addSeparator();
	popup.exec(QCursor::pos());
}

/* The pan/tilt buttons are a large block of simple and mostly identical code.
 * Use C preprocessor macro to create all the duplicate functions */
#define button_pantilt_actions(direction) \
	void PTZControls::on_panTiltButton_##direction##_pressed() \
	{ if (camera) camera->pantilt_##direction(10, 10); } \
	void PTZControls::on_panTiltButton_##direction##_released() \
	{ if (camera) camera->pantilt_stop(); }

button_pantilt_actions(up);
button_pantilt_actions(upleft);
button_pantilt_actions(upright);
button_pantilt_actions(left);
button_pantilt_actions(right);
button_pantilt_actions(down);
button_pantilt_actions(downleft);
button_pantilt_actions(downright);

/* There are fewer buttons for zoom or focus; so don't bother with macros */
void PTZControls::on_zoomButton_tele_pressed()
{
	if (camera)
		camera->zoom_tele();
}
void PTZControls::on_zoomButton_tele_released()
{
	if (camera)
		camera->zoom_stop();
}
void PTZControls::on_zoomButton_wide_pressed()
{
	if (camera)
		camera->zoom_wide();
}
void PTZControls::on_zoomButton_wide_released()
{
	if (camera)
		camera->zoom_stop();
}
