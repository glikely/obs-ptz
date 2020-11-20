#include <QPlainTextEdit>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QPushButton>
#include <QFontDatabase>
#include <QFont>
#include <QDialogButtonBox>
#include <QResizeEvent>
#include <QAction>
#include <QMessageBox>
#include <QMenu>
#include <QUrl>
#include <QDesktopServices>

#include <string>

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <obs-properties.h>

#include "ptz-device.hpp"
#include "settings.hpp"
#include "ui_settings.h"

/* ----------------------------------------------------------------- */

static PTZSettings *ptzSettingsWindow = nullptr;

/* ----------------------------------------------------------------- */

PTZSettings::PTZSettings() : QWidget(nullptr), ui(new Ui_PTZSettings)
{
	ui->setupUi(this);
	RefreshLists();

	delete propertiesView;
	propertiesView = new QWidget();
	propertiesView->setSizePolicy(QSizePolicy::Expanding,
				      QSizePolicy::Expanding);
	ui->propertiesLayout->addWidget(propertiesView);

	config_t *global_config = obs_frontend_get_global_config();
	ui->deviceList->setModel(PTZDevice::model());
	int row = config_get_int(global_config, "ptz-controls", "prevPTZRow");
	ui->deviceList->setCurrentIndex(PTZDevice::model()->index(row, 0));
}

PTZSettings::~PTZSettings()
{
	config_t *global_config = obs_frontend_get_global_config();
	config_set_int(global_config, "ptz-controls", "prevPTZRow", ui->deviceList->currentIndex().row());

	delete ui;
}

void PTZSettings::set_selected(unsigned int row)
{
	ui->deviceList->setCurrentIndex(PTZDevice::model()->index(row, 0));
}

void PTZSettings::RefreshLists()
{
}

void PTZSettings::on_close_clicked()
{
	close();
}

void PTZSettings::on_addPTZ_clicked()
{
}

void PTZSettings::on_removePTZ_clicked()
{
}

void PTZSettings::current_device_changed()
{
}

void PTZSettings::on_deviceList_clicked()
{
	current_device_changed();
}

/* ----------------------------------------------------------------- */

static void obs_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		delete ptzSettingsWindow;
}

void ptz_settings_show(int row)
{
	obs_frontend_push_ui_translation(obs_module_get_string);

	if (!ptzSettingsWindow)
		ptzSettingsWindow = new PTZSettings();
	if (row >= 0)
		ptzSettingsWindow->set_selected(row);
	ptzSettingsWindow->show();
	ptzSettingsWindow->raise();

	obs_frontend_pop_ui_translation();
}

extern "C" void ptz_init_settings()
{
	QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(
		obs_module_text("PTZ Devices"));

	auto cb = []() {
		ptz_settings_show(-1);
	};

	obs_frontend_add_event_callback(obs_event, nullptr);

	action->connect(action, &QAction::triggered, cb);
}
