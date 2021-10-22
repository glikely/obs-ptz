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
#include <QGamepadManager>
#include <QSerialPortInfo>

#include <string>

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <obs-properties.h>

#include "ptz.h"
#include "ptz-device.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ui_settings.h"

/* ----------------------------------------------------------------- */

static PTZSettings *ptzSettingsWindow = nullptr;

/* ----------------------------------------------------------------- */

const char *description_text = "<html><head/><body>"
	"<p>OBS PTZ Controls Plugin<br>"
	PLUGIN_VERSION "<br>"
	"By Grant Likely &lt;grant.likely@secretlab.ca&gt;</p>"
	"<p><a href=\"https://github.com/glikely/obs-ptz\">"
		"<span style=\" text-decoration: underline; color:#7f7fff;\">"
		"https://github.com/glikely/obs-ptz</span></a></p>"
	"</body></html>";

obs_properties_t *PTZSettings::getProperties(void)
{
	PTZDevice *ptz = ptzDeviceList.get_device(ui->deviceList->currentIndex().row());
	return ptz ? ptz->get_obs_properties() : obs_properties_create();
}

void PTZSettings::updateProperties(OBSData old_settings, OBSData new_settings)
{
	PTZDevice *ptz = ptzDeviceList.get_device(ui->deviceList->currentIndex().row());
	if (ptz)
		ptz->set_settings(new_settings);
	Q_UNUSED(old_settings);
}

PTZSettings::PTZSettings() : QWidget(nullptr), ui(new Ui_PTZSettings)
{
	settings = obs_data_create();
	obs_data_release(settings);

	ui->setupUi(this);

	ui->gamepadCheckBox->setChecked(PTZControls::getInstance()->gamepadEnabled());

	config_t *global_config = obs_frontend_get_global_config();
	ui->deviceList->setModel(&ptzDeviceList);
	int row = config_get_int(global_config, "ptz-controls", "prevPTZRow");
	ui->deviceList->setCurrentIndex(ptzDeviceList.index(row, 0));

	QItemSelectionModel *selectionModel = ui->deviceList->selectionModel();
	connect(selectionModel, SIGNAL(currentChanged(QModelIndex,QModelIndex)),
		this, SLOT(currentChanged(QModelIndex,QModelIndex)));

	auto reload_cb = [](void *obj) {
		return static_cast<PTZSettings *>(obj)->getProperties();
	};
	auto update_cb = [](void *obj, obs_data_t *oldset, obs_data_t *newset) {
		static_cast<PTZSettings *>(obj)->updateProperties(oldset, newset);
	};
	propertiesView = new OBSPropertiesView(settings, this, reload_cb, update_cb);
	propertiesView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	ui->propertiesLayout->insertWidget(1, propertiesView, 1);
	ui->versionLabel->setText(description_text);
}

PTZSettings::~PTZSettings()
{
	config_t *global_config = obs_frontend_get_global_config();
	config_set_int(global_config, "ptz-controls", "prevPTZRow", ui->deviceList->currentIndex().row());

	delete ui;
}

void PTZSettings::set_selected(unsigned int row)
{
	ui->deviceList->setCurrentIndex(QModelIndex());
	if (row >= 0)
		ui->deviceList->setCurrentIndex(ptzDeviceList.index(row, 0));
}

void PTZSettings::on_applyButton_clicked()
{
	int row = ui->deviceList->currentIndex().row();
	if (row < 0)
		return;
	PTZDevice *ptz = ptzDeviceList.get_device(row);
	ptz->set_config(propertiesView->GetSettings());
}

void PTZSettings::on_close_clicked()
{
	close();
}

void PTZSettings::on_addPTZ_clicked()
{
	QMenu addPTZContext;
	QAction *addViscaSerial = addPTZContext.addAction("VISCA Serial");
	QAction *addViscaUDP = addPTZContext.addAction("VISCA over UDP");
	QAction *addViscaTCP = addPTZContext.addAction("VISCA over TCP");
	QAction *addPelcoD = addPTZContext.addAction("Pelco D");
	QAction *addPelcoP = addPTZContext.addAction("Pelco P");
	QAction *action = addPTZContext.exec(QCursor::pos());

	if (action == addViscaSerial) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca");
		obs_data_set_string(cfg, "name", "PTZ");
		ptzDeviceList.make_device(cfg);
	}
	if (action == addViscaUDP) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca-over-ip");
		obs_data_set_string(cfg, "name", "PTZ");
		obs_data_set_string(cfg, "address", "192.168.0.100");
		obs_data_set_int(cfg, "port", 52381);
		ptzDeviceList.make_device(cfg);
	}
	if (action == addViscaTCP) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca-over-tcp");
		obs_data_set_string(cfg, "name", "PTZ");
		obs_data_set_string(cfg, "host", "localhost");
		obs_data_set_int(cfg, "port", 5678);
		ptzDeviceList.make_device(cfg);
	}
	if (action == addPelcoD) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "pelco");
		obs_data_set_string(cfg, "name", "PTZ");
		obs_data_set_bool(cfg, "use_pelco_d", true);
		ptzDeviceList.make_device(cfg);
	}
	if (action == addPelcoP) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "pelco");
		obs_data_set_string(cfg, "name", "PTZ");
		obs_data_set_bool(cfg, "use_pelco_d", false);
		ptzDeviceList.make_device(cfg);
	}
}

void PTZSettings::on_removePTZ_clicked()
{
	int row = ui->deviceList->currentIndex().row();
	if (row < 0)
		return;
	PTZDevice *ptz = ptzDeviceList.get_device(row);
	if (!ptz)
		return;

	delete ptz;
}

void PTZSettings::on_gamepadCheckBox_stateChanged(int state)
{
	PTZControls::getInstance()->setGamepadEnabled(ui->gamepadCheckBox->isChecked());
}

void PTZSettings::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
	Q_UNUSED(previous);

	obs_data_clear(settings);
	PTZDevice *ptz = ptzDeviceList.get_device(current.row());
	if (ptz) {
		obs_data_apply(settings, ptz->get_settings());

		/* The settings dialog doesn't touch presets or the device name, so remove them */
		obs_data_erase(settings, "name");
		obs_data_erase(settings, "presets");
	}

	propertiesView->ReloadProperties();
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
	ptzSettingsWindow->set_selected(row);
	ptzSettingsWindow->show();
	ptzSettingsWindow->raise();

	obs_frontend_pop_ui_translation();
}

extern "C" void ptz_load_settings()
{
	QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(
		obs_module_text("PTZ Devices"));

	auto cb = []() {
		ptz_settings_show(-1);
	};

	obs_frontend_add_event_callback(obs_event, nullptr);

	action->connect(action, &QAction::triggered, cb);
}
