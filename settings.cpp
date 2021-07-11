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

#include "ptz-device.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ui_settings.h"

/* ----------------------------------------------------------------- */

static PTZSettings *ptzSettingsWindow = nullptr;

/* ----------------------------------------------------------------- */

const char *description_text = "<html><head/><body>"
	"<p>OBS PTZ Controls Plugin<br>"
	TOSTRING(OBS_PTZ_VERSION) "<br>"
	"By Grant Likely &lt;grant.likely@secretlab.ca&gt;</p>"
	"<p><a href=\"https://github.com/glikely/obs-ptz\">"
		"<span style=\" text-decoration: underline; color:#7f7fff;\">"
		"https://github.com/glikely/obs-ptz</span></a></p>"
	"</body></html>";

PTZSettings::PTZSettings() : QWidget(nullptr), ui(new Ui_PTZSettings)
{
	ui->setupUi(this);
	RefreshLists();

	delete propertiesView;
	propertiesView = new QWidget();
	propertiesView->setSizePolicy(QSizePolicy::Expanding,
				      QSizePolicy::Expanding);
	ui->propertiesLayout->addWidget(propertiesView);
	ui->versionLabel->setText(description_text);

	config_t *global_config = obs_frontend_get_global_config();
	ui->deviceList->setModel(PTZDevice::model());
	int row = config_get_int(global_config, "ptz-controls", "prevPTZRow");
	ui->deviceList->setCurrentIndex(PTZDevice::model()->index(row, 0));

	QItemSelectionModel *selectionModel = ui->deviceList->selectionModel();
	connect(selectionModel, SIGNAL(currentChanged(QModelIndex,QModelIndex)),
		this, SLOT(currentChanged(QModelIndex,QModelIndex)));
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
		ui->deviceList->setCurrentIndex(PTZDevice::model()->index(row, 0));
}

void PTZSettings::RefreshLists()
{
	ui->gamepadCheckBox->setChecked(PTZControls::getInstance()->gamepadEnabled());
	ui->viscaPortComboBox->clear();
	Q_FOREACH(QSerialPortInfo port, QSerialPortInfo::availablePorts())
		ui->viscaPortComboBox->addItem(port.portName());
}

void PTZSettings::on_applyButton_clicked()
{
	int row = ui->deviceList->currentIndex().row();
	if (row < 0)
		return;
	PTZDevice *ptz = PTZDevice::get_device(row);
	OBSData cfg = ptz->get_config();
	std::string type = obs_data_get_string(cfg, "type");

	if ((type == "pelco-p") || (type == "visca")) {
		obs_data_set_int(cfg, "address", ui->viscaIDSpinBox->value());
		obs_data_set_string(cfg, "port", qPrintable(ui->viscaPortComboBox->currentText()));
	} else if (type == "visca-over-ip") {
		obs_data_set_string(cfg, "address", qPrintable(ui->ipAddressComboBox->currentText()));
		obs_data_set_int(cfg, "port", ui->udpPortSpinBox->value());
	} else {
		return;
	}

	ptz->set_config(cfg);
}

void PTZSettings::on_close_clicked()
{
	close();
}

void PTZSettings::on_addPTZ_clicked()
{
	QMenu addPTZContext;
	QAction *addViscaSerial = addPTZContext.addAction("VISCA Serial");
	QAction *addViscaIP = addPTZContext.addAction("VISCA over IP");
	QAction *addPelcoP = addPTZContext.addAction("Pelco P");
	QAction *action = addPTZContext.exec(QCursor::pos());

	if (action == addViscaSerial) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca");
		obs_data_set_string(cfg, "name", "PTZ");
		PTZDevice::make_device(cfg);
	}
	if (action == addViscaIP) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca-over-ip");
		obs_data_set_string(cfg, "name", "PTZ");
		obs_data_set_string(cfg, "address", "192.168.0.100");
		obs_data_set_int(cfg, "port", 52381);
		PTZDevice::make_device(cfg);
	}
	if (action == addPelcoP) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "pelco-p");
		obs_data_set_string(cfg, "name", "PTZ");
		PTZDevice::make_device(cfg);
	}
}

void PTZSettings::on_removePTZ_clicked()
{
	int row = ui->deviceList->currentIndex().row();
	if (row < 0)
		return;
	PTZDevice *ptz = PTZDevice::get_device(row);
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
	RefreshLists();

	ui->viscaPortComboBox->setEnabled(false);
	ui->viscaIDSpinBox->setEnabled(false);
	ui->ipAddressComboBox->setEnabled(false);
	ui->udpPortSpinBox->setEnabled(false);

	if (current.row() < 0)
		return;
	PTZDevice *ptz = PTZDevice::get_device(current.row());
	OBSData cfg = ptz->get_config();
	std::string type = obs_data_get_string(cfg, "type");
	if (type == "visca") {
		std::string port = obs_data_get_string(cfg, "port");
		ui->serialLabel->setText("Visca Serial");
		ui->viscaPortComboBox->setCurrentText(obs_data_get_string(cfg, "port"));
		ui->viscaIDSpinBox->setValue(obs_data_get_int(cfg, "address"));
		ui->viscaPortComboBox->setEnabled(true);
		ui->viscaIDSpinBox->setEnabled(true);
	}

	if (type == "visca-over-ip") {
		ui->ipAddressComboBox->setCurrentText(obs_data_get_string(cfg, "address"));
		ui->udpPortSpinBox->setValue(obs_data_get_int(cfg, "port"));
		ui->ipAddressComboBox->setEnabled(true);
		ui->udpPortSpinBox->setEnabled(true);
	}

	if (type == "pelco-p") {
		ui->serialLabel->setText("PELCO-P Serial");
		ui->viscaPortComboBox->setCurrentText(obs_data_get_string(cfg, "port"));
		ui->viscaIDSpinBox->setValue(obs_data_get_int(cfg, "address"));
		ui->viscaPortComboBox->setEnabled(true);
		ui->viscaIDSpinBox->setEnabled(true);
	}
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
