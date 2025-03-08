#include <QPlainTextEdit>
#include <QComboBox>
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
#include <QStringList>
#include <QJsonDocument>

#include <string>

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <obs-properties.h>
#include "imported/qjoysticks/QJoysticks.h"

#include "ptz.h"
#include "ptz-device.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ui_settings.h"

/* ----------------------------------------------------------------- */

static PTZSettings *ptzSettingsWindow = nullptr;

/* ----------------------------------------------------------------- */

const char *description_text =
	"<html><head/><body>"
	"<p>OBS PTZ Controls Plugin<br>" PLUGIN_VERSION "<br>"
	"By Grant Likely &lt;grant.likely@secretlab.ca&gt;</p>"
	"<p><a href=\"https://obsproject.com/forum/resources/ptz-controls.1284/\">"
	"<span style=\" text-decoration: underline; color:#7f7fff;\">"
	"https://obsproject.com/forum/resources/ptz-controls.1284/</a></p>"
	"<p><a href=\"https://github.com/glikely/obs-ptz\">"
	"<span style=\" text-decoration: underline; color:#7f7fff;\">"
	"https://github.com/glikely/obs-ptz</span></a></p>"
	"<p>Contributors:<br/>"
	"Norihiro Kamae<br/>"
	"Luuk Verhagen<br/>"
	"Jonata Bolzan Loss<br/>"
	"Fabio Ferrari<br/>"
	"Jim Hauxwell<br/>"
	"Eric Schmidt</p>"
	"</body></html>";

QString SourceNameDelegate::displayText(const QVariant &value,
					const QLocale &locale) const
{
	auto string = QStyledItemDelegate::displayText(value, locale);
	auto ptz = ptzDeviceList.getDeviceByName(string);
	if (ptz)
		return ptz->description() + " - " + string;
	return string;
}

obs_properties_t *PTZSettings::getProperties(void)
{
	auto applycb = [](obs_properties_t *, obs_property_t *, void *data_) {
		auto s = static_cast<PTZSettings *>(data_);
		PTZDevice *ptz = ptzDeviceList.getDevice(
			s->ui->deviceList->currentIndex());
		if (ptz)
			ptz->set_config(s->propertiesView->GetSettings());
		return true;
	};
	auto cb = [](obs_properties_t *, obs_property_t *, void *data_) {
		auto data = static_cast<obs_data_t *>(data_);
		blog(LOG_INFO, "%s", obs_data_get_string(data, "debug_info"));
		return true;
	};

	PTZDevice *ptz =
		ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (!ptz)
		return obs_properties_create();

	auto props = ptz->get_obs_properties();
	auto p = obs_properties_get(props, "interface");
	if (p) {
		auto iface = obs_property_group_content(p);
		if (iface)
			obs_properties_add_button2(iface, "iface_apply",
						   "Apply", applycb, this);
	}
	if (ptz_debug_level <= LOG_INFO) {
		auto debug = obs_properties_create();
		obs_properties_add_text(debug, "debug_info", NULL,
					OBS_TEXT_INFO);
		obs_properties_add_button2(debug, "dbgdump", "Write to OBS log",
					   cb, settings);
		obs_properties_add_group(props, "debug", "Debug",
					 OBS_GROUP_NORMAL, debug);
	}
	return props;
}

void PTZSettings::updateProperties(OBSData old_settings, OBSData new_settings)
{
	PTZDevice *ptz =
		ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (ptz)
		ptz->set_settings(new_settings);
	Q_UNUSED(old_settings);
}

PTZSettings::PTZSettings() : QWidget(nullptr), ui(new Ui_PTZSettings)
{
	settings = obs_data_create();
	obs_data_release(settings);

	ui->setupUi(this);

	ui->livemoveCheckBox->setChecked(
		PTZControls::getInstance()->liveMovesDisabled());
	ui->enableDebugLogCheckBox->setChecked(ptz_debug_level <= LOG_INFO);

	auto snd = new SourceNameDelegate(this);
	ui->deviceList->setModel(&ptzDeviceList);
	ui->deviceList->setItemDelegateForColumn(0, snd);

	QItemSelectionModel *selectionModel = ui->deviceList->selectionModel();
	connect(selectionModel,
		SIGNAL(currentChanged(QModelIndex, QModelIndex)), this,
		SLOT(currentChanged(QModelIndex, QModelIndex)));

	auto reload_cb = [](void *obj) {
		return static_cast<PTZSettings *>(obj)->getProperties();
	};
	auto update_cb = [](void *obj, obs_data_t *oldset, obs_data_t *newset) {
		static_cast<PTZSettings *>(obj)->updateProperties(oldset,
								  newset);
	};
	propertiesView =
		new OBSPropertiesView(settings, this, reload_cb, update_cb);
	propertiesView->setSizePolicy(QSizePolicy::Expanding,
				      QSizePolicy::Expanding);
	ui->propertiesLayout->insertWidget(0, propertiesView, 0);

	joystickSetup();

	ui->versionLabel->setText(description_text);
}

PTZSettings::~PTZSettings()
{
	delete ui;
}

#ifdef ENABLE_JOYSTICK
void PTZSettings::joystickSetup()
{
	auto joysticks = QJoysticks::getInstance();
	auto controls = PTZControls::getInstance();
	ui->joystickNamesListView->setModel(&m_joystickNamesModel);
	ui->joystickGroupBox->setChecked(controls->joystickEnabled());
	ui->joystickSpeedSlider->setDoubleConstraints(
		0.25, 1.75, 0.05, controls->joystickSpeed());
	ui->joystickDeadzoneSlider->setDoubleConstraints(
		0.01, 0.15, 0.01, controls->joystickDeadzone());

	connect(joysticks, SIGNAL(countChanged()), this,
		SLOT(joystickUpdate()));
	connect(joysticks, SIGNAL(axisEvent(const QJoystickAxisEvent)), this,
		SLOT(joystickAxisEvent(const QJoystickAxisEvent)));

	auto selectionModel = ui->joystickNamesListView->selectionModel();
	connect(selectionModel,
		SIGNAL(currentChanged(QModelIndex, QModelIndex)), this,
		SLOT(joystickCurrentChanged(QModelIndex, QModelIndex)));
	joystickUpdate();
}

void PTZSettings::on_joystickSpeedSlider_doubleValChanged(double val)
{
	PTZControls::getInstance()->setJoystickSpeed(val);
}

void PTZSettings::on_joystickDeadzoneSlider_doubleValChanged(double val)
{
	PTZControls::getInstance()->setJoystickDeadzone(val);
}

void PTZSettings::on_joystickGroupBox_toggled(bool checked)
{
	PTZControls::getInstance()->setJoystickEnabled(checked);
	ui->joystickNamesListView->setEnabled(checked);
}

void PTZSettings::joystickUpdate()
{
	auto joysticks = QJoysticks::getInstance();
	auto controls = PTZControls::getInstance();
	m_joystickNamesModel.setStringList(joysticks->deviceNames());
	auto idx = m_joystickNamesModel.index(controls->joystickId(), 0);
	if (idx.isValid())
		ui->joystickNamesListView->setCurrentIndex(idx);
}

void PTZSettings::joystickCurrentChanged(QModelIndex current,
					 QModelIndex previous)
{
	Q_UNUSED(previous);
	PTZControls::getInstance()->setJoystickId(current.row());
}
#else
void PTZSettings::joystickSetup()
{
	ui->joystickGroupBox->setVisible(false);
}
#endif /* ENABLE_JOYSTICK */

void PTZSettings::set_selected(uint32_t device_id)
{
	ui->deviceList->setCurrentIndex(
		ptzDeviceList.indexFromDeviceId(device_id));
}

void PTZSettings::on_addPTZ_clicked()
{
	QMenu addPTZContext;
#if defined(ENABLE_SERIALPORT)
	QAction *addViscaSerial = addPTZContext.addAction("VISCA (Serial)");
#endif
	QAction *addViscaUDP = addPTZContext.addAction("VISCA (UDP)");
	QAction *addViscaTCP = addPTZContext.addAction("VISCA (TCP)");
#if defined(ENABLE_SERIALPORT)
	QAction *addPelcoD = addPTZContext.addAction("Pelco D");
	QAction *addPelcoP = addPTZContext.addAction("Pelco P");
#endif
#if defined(ENABLE_ONVIF) // ONVIF disabled until code is reworked
	QAction *addOnvif = addPTZContext.addAction("ONVIF (experimental)");
#endif
    QAction *addUsbCam = addPTZContext.addAction("USB Camera (UVC)");
	QAction *action = addPTZContext.exec(QCursor::pos());

#if defined(ENABLE_SERIALPORT)
	if (action == addViscaSerial) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca");
		ptzDeviceList.make_device(cfg);
	}
#endif
	if (action == addViscaUDP) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca-over-ip");
		obs_data_set_int(cfg, "port", 52381);
		ptzDeviceList.make_device(cfg);
	}
	if (action == addViscaTCP) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca-over-tcp");
		obs_data_set_int(cfg, "port", 5678);
		ptzDeviceList.make_device(cfg);
	}
#if defined(ENABLE_SERIALPORT)
	if (action == addPelcoD) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "pelco");
		obs_data_set_bool(cfg, "use_pelco_d", true);
		ptzDeviceList.make_device(cfg);
	}
	if (action == addPelcoP) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "pelco");
		obs_data_set_bool(cfg, "use_pelco_d", false);
		ptzDeviceList.make_device(cfg);
	}
#endif
#if defined(ENABLE_ONVIF)
	if (action == addOnvif) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "onvif");
		ptzDeviceList.make_device(cfg);
	}
#endif
if (action == addUsbCam) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "usb-cam");
		ptzDeviceList.make_device(cfg);
	}
}

void PTZSettings::on_removePTZ_clicked()
{
	PTZDevice *ptz =
		ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (!ptz)
		return;
	delete ptz;
}

void PTZSettings::on_livemoveCheckBox_stateChanged(int state)
{
	PTZControls::getInstance()->setDisableLiveMoves(state != Qt::Unchecked);
}

void PTZSettings::on_enableDebugLogCheckBox_stateChanged(int state)
{
	ptz_debug_level = (state == Qt::Unchecked) ? LOG_DEBUG : LOG_INFO;
}

void PTZSettings::currentChanged(const QModelIndex &current,
				 const QModelIndex &previous)
{
	auto ptz = ptzDeviceList.getDevice(previous);
	if (ptz)
		ptz->disconnect(this);

	obs_data_clear(settings);

	ptz = ptzDeviceList.getDevice(current);
	if (ptz) {
		obs_data_apply(settings, ptz->get_settings());

		/* For debug, display all data in JSON format */
		if (ptz_debug_level <= LOG_INFO) {
			auto rawjson = obs_data_get_json(settings);
			/* Use QJsonDocument for nice formatting */
			auto json = QJsonDocument::fromJson(rawjson).toJson();
			obs_data_set_string(settings, "debug_info",
					    json.constData());
		}

		/* The settings dialog doesn't touch presets, so remove them */
		obs_data_erase(settings, "presets");

		ptz->connect(ptz, SIGNAL(settingsChanged(OBSData)), this,
			     SLOT(settingsChanged(OBSData)));
	}

	propertiesView->ReloadProperties();
}

void PTZSettings::settingsChanged(OBSData changed)
{
	obs_data_apply(settings, changed);
	obs_data_erase(settings, "debug_info");
	auto json =
		QJsonDocument::fromJson(obs_data_get_json(settings)).toJson();
	obs_data_set_string(settings, "debug_info", json.constData());
	propertiesView->RefreshProperties();
}

void PTZSettings::showDevice(uint32_t device_id)
{
	if (device_id) {
		set_selected(device_id);
		ui->tabWidget->setCurrentWidget(ui->camerasTab);
	} else {
		ui->tabWidget->setCurrentWidget(ui->generalTab);
	}
	show();
	raise();
}

/* ----------------------------------------------------------------- */

static void obs_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		delete ptzSettingsWindow;
}

void ptz_settings_show(uint32_t device_id)
{
	obs_frontend_push_ui_translation(obs_module_get_string);

	if (!ptzSettingsWindow)
		ptzSettingsWindow = new PTZSettings();
	ptzSettingsWindow->showDevice(device_id);

	obs_frontend_pop_ui_translation();
}

extern "C" void ptz_load_settings()
{
	QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(
		obs_module_text("PTZ Devices"));

	auto cb = []() { ptz_settings_show(0); };

	obs_frontend_add_event_callback(obs_event, nullptr);

	action->connect(action, &QAction::triggered, cb);
}
