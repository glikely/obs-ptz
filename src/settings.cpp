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

#include "ptz.h"
#include "ptz-device.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ui_settings.h"

/* ----------------------------------------------------------------- */

static PTZSettings *ptzSettingsWindow = nullptr;

/* ----------------------------------------------------------------- */

QString SourceNameDelegate::displayText(const QVariant &value, const QLocale &locale) const
{
	auto string = QStyledItemDelegate::displayText(value, locale);
	auto ptz = ptzDeviceList.getDeviceByName(string);
	if (ptz)
		return ptz->description() + " - " + string;
	return string;
}

obs_properties_t *PTZSettings::getProperties(void)
{
	auto cb = [](obs_properties_t *, obs_property_t *, void *data_) {
		auto data = static_cast<obs_data_t *>(data_);
		blog(LOG_INFO, "%s", obs_data_get_string(data, "debug_info"));
		return true;
	};

	PTZDevice *ptz = ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (!ptz)
		return obs_properties_create();

	auto props = ptz->get_obs_properties();
	auto debug = obs_properties_create();
	obs_properties_add_text(debug, "debug_info", NULL, OBS_TEXT_INFO);
	obs_properties_add_button2(debug, "dbgdump", "Write to OBS log", cb, settings);
	obs_properties_add_group(props, "debug", "Full Details", OBS_GROUP_NORMAL, debug);
	return props;
}

void PTZSettings::updateProperties(OBSData old_settings, OBSData new_settings)
{
	PTZDevice *ptz = ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (ptz)
		ptz->set_settings(new_settings);
	Q_UNUSED(old_settings);
}

PTZSettings::PTZSettings() : QWidget(nullptr), ui(new Ui_PTZSettings)
{
	settings = obs_data_create();
	obs_data_release(settings);

	ui->setupUi(this);

	ui->autoselectCheckBox->setChecked(PTZControls::getInstance()->autoselectEnabled());
	connect(PTZControls::getInstance(), SIGNAL(autoselectEnabledChanged(bool)), ui->autoselectCheckBox,
		SLOT(setChecked(bool)));
	connect(ui->autoselectCheckBox, SIGNAL(clicked(bool)), PTZControls::getInstance(),
		SLOT(setAutoselectEnabled(bool)));

	ui->livemoveCheckBox->setChecked(PTZControls::getInstance()->liveMovesDisabled());
	connect(PTZControls::getInstance(), SIGNAL(liveMovesDisabledChanged(bool)), ui->livemoveCheckBox,
		SLOT(setChecked(bool)));
	connect(ui->livemoveCheckBox, SIGNAL(clicked(bool)), PTZControls::getInstance(),
		SLOT(setDisableLiveMoves(bool)));
	connect(PTZControls::getInstance(), SIGNAL(joystickAxisActionChanged(size_t, ptz_joy_action_id)), this,
		SLOT(joystickAxisMappingChanged(size_t, ptz_joy_action_id)));

	ui->speedRampCheckBox->setChecked(PTZControls::getInstance()->speedRampEnabled());
	connect(PTZControls::getInstance(), SIGNAL(speedRampEnabledChanged(bool)), ui->speedRampCheckBox,
		SLOT(setChecked(bool)));
	connect(ui->speedRampCheckBox, SIGNAL(clicked(bool)), PTZControls::getInstance(),
		SLOT(setSpeedRampEnabled(bool)));

	auto snd = new SourceNameDelegate(this);
	ui->deviceList->setModel(&ptzDeviceList);
	ui->deviceList->setItemDelegateForColumn(0, snd);

	QItemSelectionModel *selectionModel = ui->deviceList->selectionModel();
	connect(selectionModel, SIGNAL(currentChanged(QModelIndex, QModelIndex)), this,
		SLOT(currentChanged(QModelIndex, QModelIndex)));

	auto reload_cb = [](void *obj) {
		return static_cast<PTZSettings *>(obj)->getProperties();
	};
	auto update_cb = [](void *obj, obs_data_t *oldset, obs_data_t *newset) {
		static_cast<PTZSettings *>(obj)->updateProperties(oldset, newset);
	};
	propertiesView = new OBSPropertiesView(settings, this, reload_cb, update_cb);
	propertiesView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	ui->propertiesLayout->insertWidget(0, propertiesView, 0);

	joystickSetup();

	ui->versionLabel->setText(QString(obs_module_text("PTZ.About.Info")).arg(PLUGIN_VERSION));
}

PTZSettings::~PTZSettings()
{
	delete ui;
}

#if defined(ENABLE_JOYSTICK)
void PTZSettings::joystickSetup()
{
	auto joysticks = QJoysticks::getInstance();
	auto controls = PTZControls::getInstance();
	ui->joystickNamesListView->setModel(&m_joystickNamesModel);
	ui->joystickGroupBox->setChecked(controls->joystickEnabled());
	ui->joystickSpeedSlider->setDoubleConstraints(0.25, 1.75, 0.05, controls->joystickSpeed());
	ui->joystickDeadzoneSlider->setDoubleConstraints(0.01, 0.15, 0.01, controls->joystickDeadzone());

	connect(joysticks, SIGNAL(countChanged()), this, SLOT(joystickUpdate()));
	connect(joysticks, SIGNAL(axisEvent(const QJoystickAxisEvent)), this,
		SLOT(joystickAxisEvent(const QJoystickAxisEvent)));

	auto selectionModel = ui->joystickNamesListView->selectionModel();
	connect(selectionModel, SIGNAL(currentChanged(QModelIndex, QModelIndex)), this,
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

void PTZSettings::on_joystickAxisActionChanged(int idx)
{
	auto controls = PTZControls::getInstance();
	auto cb = qobject_cast<QComboBox *>(sender());
	if (cb == nullptr)
		return;
	auto axis = cb->property("axis-id");
	ptz_joy_action_id action = cb->itemData(idx).toInt();
	if (axis.isValid())
		controls->setJoystickAxisAction(axis.toInt(), action);
}

void PTZSettings::on_joystickButtonActionChanged(int idx)
{
	auto controls = PTZControls::getInstance();
	auto cb = qobject_cast<QComboBox *>(sender());
	if (cb == nullptr)
		return;
	auto axis = cb->property("button-id");
	ptz_joy_action_id action = cb->itemData(idx).toInt();
	if (axis.isValid())
		controls->setJoystickButtonAction(axis.toInt(), action);
}

PTZJoyButtonMapper::PTZJoyButtonMapper(QWidget *parent, size_t _button) : QPushButton(parent), button(_button)
{
	setCheckable(true);
	auto m = new QMenu;
	for (int i = PTZ_JOY_ACTION_NONE; i < PTZ_JOY_ACTION_LAST_VALUE; i++) {
		auto a = m->addAction(obs_module_text(ptz_joy_action_button_names[i]));
		a->setData(i);
		connect(a, SIGNAL(triggered(bool)), this, SLOT(on_joystickButtonMapping_triggered()));
	}
	setMenu(m);

	connect(PTZControls::getInstance(), SIGNAL(joystickButtonActionChanged(size_t, ptz_joy_action_id)), this,
		SLOT(on_joystickButtonMappingChanged(size_t, ptz_joy_action_id)));
	connect(QJoysticks::getInstance(), SIGNAL(buttonEvent(const QJoystickButtonEvent)), this,
		SLOT(on_joystickButtonEvent(const QJoystickButtonEvent)));

	on_joystickButtonMappingChanged(button, PTZControls::getInstance()->joystickButtonAction(button));
}

void PTZJoyButtonMapper::on_joystickButtonMapping_triggered()
{
	auto controls = PTZControls::getInstance();
	auto a = qobject_cast<QAction *>(sender());
	if (a == nullptr)
		return;
	controls->setJoystickButtonAction(button, a->data().toInt());
}

void PTZJoyButtonMapper::on_joystickButtonMappingChanged(size_t _button, ptz_joy_action_id action)
{
	if (_button != button)
		return;
	setText(obs_module_text(ptz_joy_action_button_names[action]));
}

void PTZJoyButtonMapper::on_joystickButtonEvent(const QJoystickButtonEvent evt)
{
	auto jid = PTZControls::getInstance()->joystickId();
	if (evt.joystick->id != jid || evt.button != (int)button)
		return;
	setChecked(evt.pressed);
}

#define BUTTON_ROW_OFFSET 100
void PTZSettings::joystickUpdate()
{
	auto joysticks = QJoysticks::getInstance();
	auto controls = PTZControls::getInstance();
	m_joystickNamesModel.setStringList(joysticks->deviceNames());
	auto jid = controls->joystickId();
	auto idx = m_joystickNamesModel.index(jid, 0);
	if (idx.isValid()) {
		ui->joystickNamesListView->setCurrentIndex(idx);
		for (int i = joystickAxisLabels.count(); i < joysticks->getNumAxes(jid); i++) {
			const int numcols = 4;
			auto label = new QLabel(this);
			auto cb = new QComboBox(this);
			label->setText(QString(obs_module_text("PTZ.Settings.Joystick.AxisNum")).arg(i).arg(0));
			for (int i = PTZ_JOY_ACTION_NONE; i <= PTZ_JOY_ACTION_FOCUS_INVERT; i++)
				cb->addItem(obs_module_text(ptz_joy_action_axis_names[i]), i);
			cb->setProperty("axis-id", i);
			joystickAxisLabels.append(label);
			joystickAxisCBs.append(cb);
			ui->joystickMapGridLayout->addWidget(label, 2 * (i / numcols), i % numcols);
			ui->joystickMapGridLayout->addWidget(cb, 2 * (i / numcols) + 1, i % numcols);
			connect(cb, SIGNAL(currentIndexChanged(int)), this, SLOT(on_joystickAxisActionChanged(int)));
		}

		for (int i = joystickButtonButtons.count(); i < joysticks->getNumButtons(jid); i++) {
			const int numcols = 4;
			auto b = new PTZJoyButtonMapper(this, i);
			joystickButtonButtons.append(b);
			ui->joystickButtonGridLayout->addWidget(b, i / numcols + BUTTON_ROW_OFFSET, i % numcols);
		}

		for (int i = 0; i < joystickAxisCBs.size(); i++)
			joystickAxisMappingChanged(i, controls->joystickAxisAction(i));
	}
}

void PTZSettings::joystickAxisMappingChanged(size_t axis, ptz_joy_action_id action)
{
	if (axis >= (size_t)joystickAxisCBs.size())
		return;
	auto cb = joystickAxisCBs.at(axis);
	auto idx = cb->findData(QVariant((int)action));
	cb->setCurrentIndex(idx >= 0 ? idx : 0);
}

void PTZSettings::joystickCurrentChanged(QModelIndex current, QModelIndex previous)
{
	Q_UNUSED(previous);
	PTZControls::getInstance()->setJoystickId(current.row());
}

void PTZSettings::joystickAxisEvent(const QJoystickAxisEvent evt)
{
	auto jid = PTZControls::getInstance()->joystickId();
	if (evt.joystick->id != jid || evt.axis >= joystickAxisLabels.size())
		return;
	auto label = joystickAxisLabels.at(evt.axis);
	label->setText(QString(obs_module_text("PTZ.Settings.Joystick.AxisNum")).arg(evt.axis).arg(evt.value));
}

#else
void PTZSettings::joystickSetup()
{
	ui->joystickGroupBox->setVisible(false);
}
#endif /* ENABLE_JOYSTICK */

void PTZSettings::set_selected(uint32_t device_id)
{
	ui->deviceList->setCurrentIndex(ptzDeviceList.indexFromDeviceId(device_id));
}

void PTZSettings::on_addPTZ_clicked()
{
	QMenu addPTZContext;
#if defined(ENABLE_SERIALPORT)
	QAction *addViscaSerial = addPTZContext.addAction(obs_module_text("PTZ.Visca.Serial.Name"));
#endif
	QAction *addViscaUDP = addPTZContext.addAction(obs_module_text("PTZ.Visca.UDP.Name"));
	QAction *addViscaTCP = addPTZContext.addAction(obs_module_text("PTZ.Visca.TCP.Name"));
#if defined(ENABLE_SERIALPORT)
	QAction *addPelcoD = addPTZContext.addAction(obs_module_text("PTZ.PelcoD.Name"));
	QAction *addPelcoP = addPTZContext.addAction(obs_module_text("PTZ.PelcoP.Name"));
#endif
#if defined(ENABLE_ONVIF) // ONVIF disabled until code is reworked
	QAction *addOnvif = addPTZContext.addAction(obs_module_text("PTZ.ONVIF.Name"));
#endif
#if defined(ENABLE_USB_CAM)
	QAction *addUsbCam = addPTZContext.addAction(obs_module_text("PTZ.UVC.Name"));
#endif
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
#if defined(ENABLE_USB_CAM)
	if (action == addUsbCam) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "usb-cam");
		ptzDeviceList.make_device(cfg);
	}
#endif
}

void PTZSettings::on_removePTZ_clicked()
{
	PTZDevice *ptz = ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (!ptz)
		return;
	delete ptz;
}

void PTZSettings::on_applyButton_clicked()
{
	PTZDevice *ptz = ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (ptz)
		ptz->set_config(propertiesView->GetSettings());
}

void PTZSettings::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
	auto ptz = ptzDeviceList.getDevice(previous);
	if (ptz)
		ptz->disconnect(this);

	obs_data_clear(settings);

	ptz = ptzDeviceList.getDevice(current);
	if (ptz) {
		obs_data_apply(settings, ptz->get_settings());

		auto rawjson = obs_data_get_json(settings);
		/* Use QJsonDocument for nice formatting */
		auto json = QJsonDocument::fromJson(rawjson).toJson();
		obs_data_set_string(settings, "debug_info", json.constData());

		/* The settings dialog doesn't touch presets, so remove them */
		obs_data_erase(settings, "presets");

		ptz->connect(ptz, SIGNAL(settingsChanged(OBSData)), this, SLOT(settingsChanged(OBSData)));
	}

	propertiesView->ReloadProperties();
}

void PTZSettings::settingsChanged(OBSData changed)
{
	obs_data_apply(settings, changed);
	obs_data_erase(settings, "debug_info");
	auto json = QJsonDocument::fromJson(obs_data_get_json(settings)).toJson();
	obs_data_set_string(settings, "debug_info", json.constData());
	QMetaObject::invokeMethod(propertiesView, "RefreshProperties", Qt::QueuedConnection);
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
	QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(obs_module_text("PTZ.Settings.PTZDevices"));

	auto cb = []() {
		ptz_settings_show(0);
	};

	obs_frontend_add_event_callback(obs_event, nullptr);

	action->connect(action, &QAction::triggered, cb);
}
