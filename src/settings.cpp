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
#include <QTimer>
#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QJsonDocument>
#include <QDialog>
#include <QFormLayout>
#include <QDateTime>
#include <QLocale>
#include <QStandardItemModel>

#include <string>
#include <cstring>

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <obs-properties.h>
#include <qt-wrappers.hpp>

#include "ptz.h"
#include "ptz-list-model.hpp"
#include "ptz-discovery.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ptz-camera-report.hpp"
#include "ptz-state-view.hpp"
#include "contributors-generated.hpp"
#include "translators-generated.hpp"
#include "ui_settings.h"

/* ----------------------------------------------------------------- */

static PTZSettings *ptzSettingsWindow = nullptr;

/* ----------------------------------------------------------------- */

obs_properties_t *PTZSettings::getProperties(void)
{
	obs_properties_t *props = ptzDeviceList->getProperties(ui->deviceList->currentIndex());
	/* Hold edits until Apply, instead of the view sending each one 500 ms
	 * after it was made */
	obs_properties_set_flags(props, obs_properties_get_flags(props) | OBS_PROPERTIES_DEFER_UPDATE);
	return props;
}

void PTZSettings::updateProperties(OBSData, OBSData new_settings)
{
	ptzDeviceList->update(ui->deviceList->currentIndex(), new_settings);
}

QString PTZSettings::currentDeviceUuid() const
{
	return ui->deviceList->currentIndex().data(PTZListModel::DeviceUuidRole).toString();
}

PTZSettings::PTZSettings() : QWidget(nullptr), ui(new Ui_PTZSettings)
{
	settings = obs_data_create();
	obs_data_release(settings);

	/* The video display is a widget with a window of its own, which Qt
	 * makes when the widget is, and parents to the nearest ancestor that has
	 * one. If this window didn't exist yet, it would have none, and on
	 * macOS the display ended up away from where its widget is. So make
	 * this one first. */
	createWinId();
	ui->setupUi(this);

	/* The camera list is as wide as it was, whatever the window or the
	 * camera shown does: settings.ui gives the device pane all the stretch,
	 * whose own width is nothing to it (the status line asks for none), so
	 * the list keeps its width when the window is resized.
	 *
	 * A splitter that has to give up width, because the window got narrower,
	 * takes it from every child that can spare any, the list included. So
	 * the list's minimum width is the width it has, which the window can't
	 * go below, and the device pane is what has none to spare. It has to
	 * come off while the handle is dragged, or the list could only be made
	 * wider: see eventFilter(). */
	const int listWidth = ui->widget_1->sizeHint().width();
	ui->widget_1->setMinimumWidth(listWidth);
	ui->splitter->setSizes({listWidth, 1000});
	ui->splitter->handle(1)->installEventFilter(this);

	/* The controls and video can be put away, leaving the status line. It is
	 * the theme's expand and collapse checkbox, as the Sources list has for
	 * its groups, which is checked while what it controls is collapsed. */
#ifdef __APPLE__
	ui->topToggle->setAttribute(Qt::WA_LayoutUsesWidgetRect);
#endif
	connect(ui->topToggle, &QCheckBox::toggled, this, [this](bool collapsed) {
		ui->topSection->setVisible(!collapsed);
		ui->topToggle->setToolTip(
			obs_module_text(collapsed ? "PTZ.Settings.Controls.Show" : "PTZ.Settings.Controls.Hide"));
	});
	ui->topToggle->setToolTip(obs_module_text("PTZ.Settings.Controls.Hide"));

	/* The controls are the size the theme gives a control, and don't stretch
	 * to the window as the dock's do, and the video is as tall as they are.
	 * The theme's density can change, so follow it, after everything that
	 * resizes the controls for it has. */
	ui->movementControls->setThemeSized(true);
	matchVideoHeight();
	connect(PTZControls::getInstance(), &PTZControls::themeRefreshed, this,
		[this]() { QTimer::singleShot(0, this, &PTZSettings::matchVideoHeight); });

	connect(ptzDeviceList, &PTZListModel::deviceSettingsUpdated, this, &PTZSettings::deviceSettingsUpdated);
	connect(ptzDeviceList, &PTZListModel::deviceStateUpdated, this, &PTZSettings::deviceStateUpdated);
	/* A device can be moved to another source, which changes what it shows */
	connect(ptzDeviceList, &PTZListModel::dataChanged, this,
		[this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
			QModelIndex current = ui->deviceList->currentIndex();
			if (QItemSelectionRange(topLeft, bottomRight).contains(current))
				ui->sourcePreview->setSource(ptzDeviceList->parentSource(current));
		});

	ui->autoselectCheckBox->setChecked(PTZControls::getInstance()->autoselectEnabled());
	connect(PTZControls::getInstance(), &PTZControls::autoselectEnabledChanged, ui->autoselectCheckBox,
		&QCheckBox::setChecked);
	connect(ui->autoselectCheckBox, &QCheckBox::clicked, PTZControls::getInstance(),
		&PTZControls::setAutoselectEnabled);

	ui->livemoveCheckBox->setChecked(PTZControls::getInstance()->liveMoveLockEnabled());
	connect(PTZControls::getInstance(), &PTZControls::liveMoveLockEnabledChanged, ui->livemoveCheckBox,
		&QCheckBox::setChecked);
	connect(ui->livemoveCheckBox, &QCheckBox::clicked, PTZControls::getInstance(),
		&PTZControls::setLiveMoveLockEnabled);

	ui->speedRampCheckBox->setChecked(PTZControls::getInstance()->speedRampEnabled());
	connect(PTZControls::getInstance(), &PTZControls::speedRampEnabledChanged, ui->speedRampCheckBox,
		&QCheckBox::setChecked);
	connect(ui->speedRampCheckBox, &QCheckBox::clicked, PTZControls::getInstance(),
		&PTZControls::setSpeedRampEnabled);

	ui->refreshThumbnailCheckBox->setChecked(PTZControls::getInstance()->refreshThumbnailOnRecall());
	connect(PTZControls::getInstance(), &PTZControls::refreshThumbnailOnRecallChanged, ui->refreshThumbnailCheckBox,
		&QCheckBox::setChecked);
	connect(ui->refreshThumbnailCheckBox, &QCheckBox::clicked, PTZControls::getInstance(),
		&PTZControls::setRefreshThumbnailOnRecall);

	statisticsTimer.setInterval(1000);
	connect(&statisticsTimer, &QTimer::timeout, this, &PTZSettings::refreshStatistics);
	statisticsTimer.start();

	ui->deviceList->setModel(ptzDeviceList);
	ui->deviceList->setItemDelegate(new PTZDeviceListDelegate(ui->deviceList));

	QItemSelectionModel *selectionModel = ui->deviceList->selectionModel();
	connect(selectionModel, &QItemSelectionModel::currentChanged, this, &PTZSettings::currentChanged);

	auto reload_cb = [](void *obj) {
		return static_cast<PTZSettings *>(obj)->getProperties();
	};
	auto update_cb = [](void *obj, obs_data_t *oldset, obs_data_t *newset) {
		static_cast<PTZSettings *>(obj)->updateProperties(oldset, newset);
	};
	propertiesView = new OBSPropertiesView(settings, this, reload_cb, update_cb);
	propertiesView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	propertiesView->setScrolling(false);

	ui->settingsViewLayout->addWidget(propertiesView);

	connect(propertiesView, &OBSPropertiesView::Changed, this, &PTZSettings::settingsEdited);
	/* Whenever the view redraws without there being edits to keep, what it
	 * shows is what the device has */
	connect(propertiesView, &OBSPropertiesView::PropertiesRefreshed, this, [this]() {
		if (!settingsDirty)
			settingsBaseline = editedSettings();
	});
	setSettingsDirty(false);

	/* The diagnostics are the state view's, but get a tab of their own,
	 * which is there only while the device has any */
	QGroupBox *diagnostics = ui->stateView->diagnosticsGroup();
	/* The tab says what it is, so the group has no need of a title, nor of
	 * the room the theme keeps for one */
	diagnostics->setTitle(QString());
	diagnostics->setStyleSheet("QGroupBox { padding-top: 4px; }");
	ui->diagnosticsLayout->insertWidget(0, diagnostics);
	ui->deviceTabs->setTabVisible(ui->deviceTabs->indexOf(ui->diagnosticsTab), false);
	connect(ui->stateView, &PTZStateView::diagnosticsAvailableChanged, this, [this](bool available) {
		ui->deviceTabs->setTabVisible(ui->deviceTabs->indexOf(ui->diagnosticsTab), available);
	});
	connect(ui->stateView, &PTZStateView::shownChanged, this, &PTZSettings::updateHeader);
	updateHeader();

	/* What the user asks of the camera in the state view is a request, not
	 * an update: the camera changes what it reports of itself once it has
	 * done as asked, and that comes back as deviceStateUpdated() */
	connect(ui->stateView, &PTZStateView::stateRequested, this,
		[this](OBSData requested) { ptzDeviceList->setState(ui->deviceList->currentIndex(), requested); });
	connect(ui->stateView, &PTZStateView::cameraReportRequested, this,
		[this]() { (new PTZCameraReportDialog(currentDeviceUuid(), this))->show(); });
	connect(ui->stateView, &PTZStateView::actionRequested, this, [this](const QString &action) {
		calldata_t cd = {};
		calldata_set_string(&cd, "name", qUtf8Printable(action));
		ptzDeviceList->callDevice(ui->deviceList->currentIndex(), "ptz_trigger", &cd);
		calldata_free(&cd);
	});

	joystickSetup();

	QString basic_info = QString("<p>%1<br/>%2<br/>%3 %4</p>")
				     .arg(obs_module_text("PTZ.About.Name"))
				     .arg(ptz_plugin_version)
				     .arg(obs_module_text("PTZ.About.By"))
				     .arg("Grant Likely");
	QString url_format = "<a href=\"%1\"><span style=\"text-decoration: underline; color:#7f7fff;\">%1</a>";
	const QStringList url_list = {
		url_format.arg("https://obsproject.com/forum/resources/ptz-controls.1284"),
		url_format.arg("https://github.com/glikely/obs-ptz"),
	};
	const QString urls = QString("<p>%1</p>").arg(url_list.join("<br/>"));
	QStringList contrib_list = {obs_module_text("PTZ.About.Contributors")};
	contrib_list += ptz_code_contributors;
	const QString contributors = QString("<p>%1</p>").arg(contrib_list.join("<br/>"));
	QStringList translator_list = {obs_module_text("PTZ.About.Translators")};
	translator_list += ptz_translator_credits;
	const QString translators = QString("<p>%1</p>").arg(translator_list.join("<br/>"));
	ui->versionLabel->setText(QString("<html><head/><body>%1%2%3%4</body></html>")
					  .arg(basic_info)
					  .arg(urls)
					  .arg(contributors)
					  .arg(translators));
}

/* The splitter handle: free the list's width while it is dragged, and hold
 * the list at the width it was left at */
bool PTZSettings::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == ui->splitter->handle(1)) {
		if (event->type() == QEvent::MouseButtonPress)
			ui->widget_1->setMinimumWidth(0);
		else if (event->type() == QEvent::MouseButtonRelease)
			ui->widget_1->setMinimumWidth(ui->widget_1->width());
	}
	return QWidget::eventFilter(watched, event);
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
	ui->joystickDeadzoneSlider->setDoubleConstraints(0.01, 0.5, 0.01, controls->joystickDeadzone());

	connect(joysticks, &QJoysticks::countChanged, this, &PTZSettings::joystickUpdate);
	connect(joysticks, &QJoysticks::axisEvent, this, &PTZSettings::joystickAxisEvent);
	connect(PTZControls::getInstance(), &PTZControls::joystickAxisActionChanged, this,
		&PTZSettings::joystickAxisMappingChanged);

	auto selectionModel = ui->joystickNamesListView->selectionModel();
	connect(selectionModel, &QItemSelectionModel::currentChanged, this, &PTZSettings::joystickCurrentChanged);
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

PTZJoyButtonMapper::PTZJoyButtonMapper(QWidget *parent, size_t _button) : QPushButton(parent), button(_button)
{
	setCheckable(true);
	auto m = new QMenu;
	auto none_action = m->addAction(obs_module_text("None"));
	connect(none_action, &QAction::triggered, this, &PTZJoyButtonMapper::on_menuAction);
	auto data = std::make_tuple(m, this);
	using data_t = decltype(data);
	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id, obs_hotkey_t *key) {
			data_t &d = *static_cast<data_t *>(data);
			if (obs_hotkey_get_registerer_type(key) != OBS_HOTKEY_REGISTERER_FRONTEND)
				return true; /* todo: extra logic needed for other hotkey registerers */
			auto a = std::get<0>(d)->addAction(obs_hotkey_get_description(key));
			a->setData(obs_hotkey_get_name(key));
			connect(a, &QAction::triggered, std::get<1>(d), &PTZJoyButtonMapper::on_menuAction);
			return true;
		},
		&data);
	setMenu(m);

	connect(PTZControls::getInstance(), &PTZControls::joystickButtonHotkeyChanged, this,
		&PTZJoyButtonMapper::on_hotkeyChanged);
	connect(QJoysticks::getInstance(), &QJoysticks::buttonEvent, this, &PTZJoyButtonMapper::on_joystickButtonEvent);

	on_hotkeyChanged(button, PTZControls::getInstance()->joystickButtonHotkey(button));
}

void PTZJoyButtonMapper::on_menuAction()
{
	auto controls = PTZControls::getInstance();
	auto a = qobject_cast<QAction *>(sender());
	if (a == nullptr)
		return;
	controls->setJoystickButtonHotkey(button, a->data().toString());
}

void PTZJoyButtonMapper::on_hotkeyChanged(size_t _button, QString hotkey_name)
{
	if (_button != button)
		return;
	setText(obs_module_text("None"));
	auto data = std::make_tuple(hotkey_name, this);
	using data_t = decltype(data);
	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id, obs_hotkey_t *key) {
			data_t &d = *static_cast<data_t *>(data);
			if (std::get<0>(d) == obs_hotkey_get_name(key)) {
				std::get<1>(d)->setText(obs_hotkey_get_description(key));
				return false;
			}
			return true;
		},
		&data);
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
			connect(cb, &QComboBox::currentIndexChanged, this, &PTZSettings::on_joystickAxisActionChanged);
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

void PTZSettings::on_addPTZ_clicked()
{
	addDevice();
}

/* The protocols a new device can use, as the name to show and the settings
 * to start from */
static QList<QPair<QString, OBSData>> ptz_protocols()
{
	QList<QPair<QString, OBSData>> protocols;
	auto add = [&](const char *name, const char *type) {
		OBSDataAutoRelease cfg = obs_data_create();
		obs_data_set_string(cfg, "type", type);
		if (strcmp(type, "pelco") == 0)
			obs_data_set_bool(cfg, "use_pelco_d", true);
		protocols.append({obs_module_text(name), cfg.Get()});
	};
	add("PTZ.Visca.Name", "visca-over-ip");
#if defined(ENABLE_SERIALPORT)
	add("PTZ.Pelco.Name", "pelco");
#endif
#if defined(ENABLE_ONVIF) // ONVIF disabled until code is reworked
	add("PTZ.ONVIF.Name", "onvif");
#endif
#if defined(ENABLE_USB_CAM)
	add("PTZ.UVC.Name", "usb-cam");
#endif
	return protocols;
}

/* The name of the protocol a device type belongs to */
static QString ptz_protocol_name(const char *type)
{
	const char *kind = ptz_device_filter_kind(type);
	for (const auto &protocol : ptz_protocols())
		if (kind && strcmp(kind, ptz_device_filter_kind(obs_data_get_string(protocol.second, "type"))) == 0)
			return protocol.first;
	return QT_UTF8(type);
}

/* Whether every setting in `settings` has the same value in `config`: for a
 * detected device, whether `config` is a device for it already */
static bool ptz_settings_contain(obs_data_t *config, obs_data_t *settings)
{
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		const char *key = obs_data_item_get_name(item);
		bool same = true;
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			same = strcmp(obs_data_item_get_string(item), obs_data_get_string(config, key)) == 0;
			break;
		case OBS_DATA_NUMBER:
			same = obs_data_item_get_int(item) == obs_data_get_int(config, key);
			break;
		case OBS_DATA_BOOLEAN:
			same = obs_data_item_get_bool(item) == obs_data_get_bool(config, key);
			break;
		default:
			break;
		}
		if (!same) {
			obs_data_item_release(&item);
			return false;
		}
	}
	return true;
}

/* A new device is a PTZ Control filter on a source, so ask which source,
 * and which protocol, or which device the drivers detected (see
 * PTZDiscovery), or which removed device's backup (see
 * ptz_device_backups_get()) to restore, either of which brings its protocol
 * with it */
void PTZSettings::addDevice()
{
	/* The devices that control a source; one whose source was deleted can
	 * linger, under its name, until OBS lets go of its filter */
	QList<OBSData> live;
	QStringList used;
	for (int row = 0; row < ptzDeviceList->rowCount(); row++) {
		QModelIndex index = ptzDeviceList->index(row, 0);
		OBSSource src = ptzDeviceList->parentSource(index);
		if (!src)
			continue;
		OBSDataAutoRelease item = obs_data_create();
		ptzDeviceList->save(index, item.Get());
		/* A backup says the source it was on by "name", so this does too */
		obs_data_set_string(item, "name", obs_source_get_name(src));
		live.append(item.Get());
		used.append(QT_UTF8(obs_source_get_name(src)));
	}

	/* Every source not already controlled by a device */
	QStringList sources;
	auto src_cb = [](void *data, obs_source_t *src) {
		if (obs_source_get_type(src) != OBS_SOURCE_TYPE_SCENE)
			static_cast<QStringList *>(data)->append(QT_UTF8(obs_source_get_name(src)));
		return true;
	};
	obs_enum_sources(src_cb, &sources);
	for (const auto &name : used)
		sources.removeAll(name);
	if (sources.isEmpty()) {
		QMessageBox::information(this, obs_module_text("PTZ.AddDevice.Tooltip"),
					 obs_module_text("PTZ.AddDevice.NoSources"));
		return;
	}

	/* What the device can start from: each protocol's defaults, then the
	 * backups this build can make a device from, other than of one that
	 * exists */
	auto protocols = ptz_protocols();
	QList<OBSData> choices;
	for (const auto &protocol : protocols)
		choices.append(protocol.second);
	OBSDataArrayAutoRelease allBackups = ptz_device_backups_get();
	QList<OBSData> backups;
	for (size_t i = 0; i < obs_data_array_count(allBackups); i++) {
		OBSDataAutoRelease item = obs_data_array_item(allBackups, i);
		const char *type = obs_data_get_string(item, "type");
		if (!ptz_device_filter_kind(type))
			continue;
		bool exists = false;
		for (const auto &dev : live) {
			if (strcmp(obs_data_get_string(dev, "name"), obs_data_get_string(item, "name")) == 0 &&
			    strcmp(obs_data_get_string(dev, "type"), type) == 0)
				exists = true;
		}
		if (!exists)
			backups.append(item.Get());
	}

	QDialog dialog(this);
	dialog.setWindowTitle(obs_module_text("PTZ.AddDevice.Tooltip"));
	auto layout = new QFormLayout(&dialog);
	auto sourceCombo = new QComboBox(&dialog);
	sourceCombo->setObjectName("sourceList");
	sourceCombo->addItems(sources);
	layout->addRow(obs_module_text("PTZ.Source"), sourceCombo);
	auto deviceCombo = new QComboBox(&dialog);
	deviceCombo->setObjectName("deviceTypeList");
	auto model = qobject_cast<QStandardItemModel *>(deviceCombo->model());
	/* A heading over each group, or a note in one: shown, but not
	 * something to pick */
	auto insertNote = [&](int row, const QString &text) {
		deviceCombo->insertItem(row, text, -1);
		if (model)
			model->item(row)->setFlags(Qt::NoItemFlags);
	};
	/* Each item's data is its index in choices, and for a protocol, the
	 * type it makes; for a backup, the name of its source; for a detected
	 * device, its id */
	insertNote(deviceCombo->count(), obs_module_text("PTZ.AddDevice.NewHeading"));
	for (int i = 0; i < protocols.size(); i++) {
		deviceCombo->addItem(protocols[i].first, i);
		deviceCombo->setItemData(deviceCombo->count() - 1, obs_data_get_string(protocols[i].second, "type"),
					 Qt::UserRole + 1);
	}

	/* Devices the drivers detect, added as they are found, above a note
	 * saying they are still looking, or found nothing */
	auto discoveries = ptz_discovery_create_all(&dialog);
	QStandardItem *detectedNote = nullptr;
	if (!discoveries.isEmpty()) {
		insertNote(deviceCombo->count(), obs_module_text("PTZ.AddDevice.DetectedHeading"));
		insertNote(deviceCombo->count(), obs_module_text("PTZ.AddDevice.Searching"));
		detectedNote = model ? model->item(deviceCombo->count() - 1) : nullptr;
	}

	if (!backups.isEmpty()) {
		insertNote(deviceCombo->count(), obs_module_text("PTZ.AddDevice.RestoreHeading"));
	}
	for (const auto &backup : backups) {
		OBSDataArrayAutoRelease presets = obs_data_get_array(backup, "presets");
		auto when = QDateTime::fromSecsSinceEpoch(obs_data_get_int(backup, "backup_time"));
		QString name = QT_UTF8(obs_data_get_string(backup, "name"));
		deviceCombo->addItem(QString(obs_module_text("PTZ.AddDevice.Restore"))
					     .arg(name)
					     .arg(ptz_protocol_name(obs_data_get_string(backup, "type")))
					     .arg(obs_data_array_count(presets))
					     .arg(QLocale().toString(when, QLocale::ShortFormat)),
				     (int)choices.size());
		deviceCombo->setItemData(deviceCombo->count() - 1, name, Qt::UserRole + 2);
		choices.append(backup);
	}
	layout->addRow(obs_module_text("PTZ.AddDevice.Device"), deviceCombo);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addRow(buttons);

	int searching = 0;
	int detected = 0;
	for (auto discovery : discoveries) {
		connect(discovery, &PTZDiscovery::deviceFound, &dialog, [&](const PTZDetectedDevice &device) {
			if (!detectedNote || deviceCombo->findData(device.id, Qt::UserRole + 3) >= 0)
				return;
			for (const auto &dev : live)
				if (ptz_settings_contain(dev, device.settings))
					return; /* already has a device */
			int row = detectedNote->row();
			deviceCombo->insertItem(row, QString("%1 (%2)").arg(device.name, device.address),
						(int)choices.size());
			deviceCombo->setItemData(row, device.id, Qt::UserRole + 3);
			choices.append(device.settings);
			detected++;
		});
		connect(discovery, &PTZDiscovery::finished, &dialog, [&]() {
			if (--searching > 0 || !detectedNote)
				return;
			if (detected)
				deviceCombo->removeItem(detectedNote->row());
			else
				detectedNote->setText(obs_module_text("PTZ.AddDevice.NoneDetected"));
			detectedNote = nullptr;
		});
	}

	/* Offer the backup of a device that was on a source of the same name;
	 * otherwise go back to the protocol or detected device last picked */
	int lastPicked = 0;
	connect(deviceCombo, &QComboBox::activated, &dialog, [&](int row) {
		int choice = deviceCombo->itemData(row).toInt();
		if (choice >= 0 && deviceCombo->itemData(row, Qt::UserRole + 2).isNull())
			lastPicked = choice;
	});
	auto matchBackup = [&](const QString &source) {
		int row = deviceCombo->findData(source, Qt::UserRole + 2);
		deviceCombo->setCurrentIndex(row >= 0 ? row : deviceCombo->findData(lastPicked));
	};
	connect(sourceCombo, &QComboBox::currentTextChanged, &dialog, matchBackup);
	matchBackup(sourceCombo->currentText());

	searching = discoveries.size();
	for (auto discovery : discoveries)
		discovery->start();
	int accepted = dialog.exec();
	for (auto discovery : discoveries) {
		discovery->disconnect(&dialog);
		discovery->stop();
	}
	if (accepted != QDialog::Accepted)
		return;

	int choice = deviceCombo->currentData().toInt();
	if (choice < 0 || choice >= choices.size())
		return;
	OBSSourceAutoRelease parent = obs_get_source_by_name(QT_TO_UTF8(sourceCombo->currentText()));
	if (!parent)
		return;
	OBSSourceAutoRelease filter = ptz_device_create_filter(parent, choices[choice]);
	if (!filter)
		return;
	QModelIndex index = ptzDeviceList->indexFromFilter(filter);
	if (index.isValid())
		ui->deviceList->setCurrentIndex(index);
}

void PTZSettings::on_removePTZ_clicked()
{
	QModelIndex index = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;
	auto answer = QMessageBox::question(
		this, obs_module_text("PTZ.RemoveDevice.Tooltip"),
		QString(obs_module_text("PTZ.RemoveDevice.Confirm")).arg(index.data(Qt::DisplayRole).toString()));
	if (answer == QMessageBox::Yes)
		ptzDeviceList->removeDevice(index);
}

void PTZSettings::on_applyButton_clicked()
{
	ptzDeviceList->update(ui->deviceList->currentIndex(), propertiesView->GetSettings());
	settingsBaseline = editedSettings();
	setSettingsDirty(false);
}

/* Throw away what was edited and show what the device has */
void PTZSettings::on_revertButton_clicked()
{
	reloadSettings();
}

QJsonObject PTZSettings::editedSettings() const
{
	/* A QJsonObject compares by key, whatever order they were added in */
	return QJsonDocument::fromJson(obs_data_get_json(propertiesView->GetSettings())).object();
}

/* The view says something changed, which it also does when it redraws
 * itself. It is an edit only if what it holds now differs from what it held
 * when it matched the device. */
void PTZSettings::settingsEdited()
{
	setSettingsDirty(editedSettings() != settingsBaseline);
}

void PTZSettings::setSettingsDirty(bool dirty)
{
	settingsDirty = dirty;
	/* Blank, not hidden, so the row doesn't change as it comes and goes */
	ui->unsavedLabel->setText(dirty ? obs_module_text("PTZ.Settings.Unsaved") : QString());
	ui->applyButton->setEnabled(dirty);
	ui->revertButton->setEnabled(dirty);
	/* Seen from the other tabs too */
	ui->deviceTabs->setTabText(ui->deviceTabs->indexOf(ui->settingsTab),
				   QString(obs_module_text("PTZ.Settings.DeviceSettings")) + (dirty ? " \u2022" : ""));
}

/* Replace what is in the settings view with what the device saves */
void PTZSettings::reloadSettings()
{
	QStringList keys;
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item))
		keys << obs_data_item_get_name(item);
	for (const auto &key : keys)
		obs_data_erase(settings, qUtf8Printable(key));
	ptzDeviceList->save(ui->deviceList->currentIndex(), settings);
	propertiesView->ReloadProperties();
	settingsBaseline = editedSettings();
	setSettingsDirty(false);
}

/* The video is as tall as the controls beside it, and 16:9 wide, which is
 * what it is scaled to fit, so it needs no more room than that */
void PTZSettings::matchVideoHeight()
{
	const int height = ui->movementControls->sizeHint().height();
	ui->sourcePreview->setFixedSize(height * 16 / 9, height);
}

/* The status line, the header of the group over the controls and video: what
 * to see of the device at a glance, which stays in view whichever tab is
 * showing, and while the rest of the group is hidden */
void PTZSettings::updateHeader()
{
	const QVariantMap shown = ui->stateView->shownValues();
	if (!ui->deviceList->currentIndex().isValid()) {
		ui->deviceName->clear();
		ui->statusHeader->clear();
		updateAutofocusIcon(false, false);
		return;
	}

	auto badge = [](const char *dot, const char *color, const QString &text) {
		return QString("<span style='color:%1'>%2</span>&nbsp;%3").arg(color, dot, text.toHtmlEscaped());
	};
	QStringList parts;
	if (shown.value("connected").toBool())
		parts << badge("\u25cf", "#3cb44b", obs_module_text("PTZ.Device.Status.Connected"));
	else
		parts << badge("\u25cb", "#e74c3c", obs_module_text("PTZ.Device.Status.Disconnected"));
	if (shown.value("live").toBool())
		parts << badge("\u25cf", "#e74c3c", obs_module_text("PTZ.Settings.Header.Live"));
	if (shown.value("preview").toBool())
		parts << badge("\u25cf", "#3cb44b", obs_module_text("PTZ.Settings.Header.Preview"));
	if (shown.value("locked").toBool())
		parts << badge("\u25cf", "#e0a030", obs_module_text("PTZ.Settings.Header.Locked"));

	/* A camera that is switched off isn't anywhere in particular, so its
	 * position isn't shown, whatever it last reported */
	const bool powerKnown = shown.contains("power_on");
	const bool poweredOff = powerKnown && !shown.value("power_on").toBool();
	if (powerKnown) {
		if (poweredOff)
			parts << badge("\u25cb", "#999999", obs_module_text("PTZ.Settings.Header.PowerOff"));
		else
			parts << badge("\u25cf", "#3cb44b", obs_module_text("PTZ.Settings.Header.PowerOn"));
	}

	QStringList position;
	if (!poweredOff)
		for (const char *axis : {"pan", "tilt", "zoom", "focus"})
			if (shown.contains(axis))
				position << QString("%1&nbsp;%2")
						    .arg(QString(axis).left(1).toUpper(),
							 QString::asprintf("%.3f", shown.value(axis).toDouble()));
	if (!position.isEmpty())
		parts << position.join("&nbsp;&nbsp;");

	updateAutofocusIcon(!poweredOff && shown.contains("focus_af_enabled"),
			    shown.value("focus_af_enabled").toBool());

	ui->deviceName->setText(ui->deviceList->currentIndex().data(Qt::DisplayRole).toString());
	ui->statusHeader->setText(parts.join("&nbsp;&nbsp;&nbsp;"));
}

/* The AF icon in the header, for a camera that reports whether it is
 * focusing by itself: as it is when on, faded and struck through when off */
void PTZSettings::updateAutofocusIcon(bool known, bool on)
{
	ui->autofocusIcon->setVisible(known);
	if (!known)
		return;

	const bool dark = obs_frontend_is_theme_dark();
	const int size = ui->statusHeader->fontMetrics().height();
	const QString key = QString("%1%2%3").arg(on).arg(dark).arg(size);
	if (key == autofocusIconKey)
		return;
	autofocusIconKey = key;

	QIcon icon(dark ? ":/icons/icons/focus_auto_dark.svg" : ":/icons/icons/focus_auto_light.svg");
	QPixmap pixmap = icon.pixmap(QSize(size, size));
	if (!on) {
		QPixmap off(pixmap.size());
		off.setDevicePixelRatio(pixmap.devicePixelRatio());
		off.fill(Qt::transparent);
		QPainter painter(&off);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setOpacity(0.4);
		painter.drawPixmap(0, 0, pixmap);
		painter.setOpacity(1.0);
		painter.setPen(QPen(QColor("#e74c3c"), 1.5));
		const QSizeF logical = pixmap.deviceIndependentSize();
		painter.drawLine(QPointF(1, logical.height() - 1), QPointF(logical.width() - 1, 1));
		pixmap = off;
	}
	ui->autofocusIcon->setPixmap(pixmap);
	ui->autofocusIcon->setToolTip(
		obs_module_text(on ? "PTZ.Settings.Header.AutofocusOn" : "PTZ.Settings.Header.AutofocusOff"));
}

void PTZSettings::currentChanged(const QModelIndex &current, const QModelIndex &)
{
	ui->movementControls->setDevice(current);
	ui->sourcePreview->setSource(ptzDeviceList->parentSource(current));

	/* Start from nothing: obs_data_clear() would keep the last device's
	 * keys, only without their values */
	reloadSettings();

	OBSDataAutoRelease state = obs_data_create();
	ptzDeviceList->saveState(current, state.Get());
	ui->stateView->setState(state.Get());
	updateHeader();
	refreshStatistics();
}

void PTZSettings::refreshStatistics()
{
	if (!isVisible())
		return;
	OBSDataAutoRelease statistics = obs_data_create();
	calldata_t cd = {};
	calldata_set_ptr(&cd, "statistics", statistics.Get());
	bool called = ptzDeviceList->callDevice(ui->deviceList->currentIndex(), "ptz_get_statistics", &cd);
	calldata_free(&cd);
	if (called)
		ui->stateView->setStatistics(statistics.Get());
}

/* Only the settings view hears about this: state changing all the time
 * (connection, live, what the camera reports) used to have it re-save and
 * refresh the settings under the user's cursor */
void PTZSettings::deviceSettingsUpdated(const QString &uuid)
{
	if (uuid != currentDeviceUuid())
		return;

	ptzDeviceList->save(ui->deviceList->currentIndex(), settings);
	QMetaObject::invokeMethod(propertiesView, "RefreshProperties", Qt::QueuedConnection);
	/* What was edited is gone, replaced by what the device now has */
	settingsBaseline = editedSettings();
	setSettingsDirty(false);
}

/* Fold in only what the device says changed. The state view changes just
 * the widgets that shows, in place, so it can take every change as it comes,
 * however often the camera's position does. */
void PTZSettings::deviceStateUpdated(const QString &uuid, OBSData changed)
{
	if (uuid != currentDeviceUuid())
		return;

	ui->stateView->applyChanges(changed);
}

void PTZSettings::showDevice(const QModelIndex &index)
{
	if (index.isValid()) {
		ui->deviceList->setCurrentIndex(index);
		ui->tabWidget->setCurrentWidget(ui->devicesTab);
	} else {
		ui->tabWidget->setCurrentWidget(ui->generalTab);
	}
	show();
	raise();
}

/* ----------------------------------------------------------------- */

static void obs_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		obs_frontend_remove_event_callback(obs_event, nullptr);
		delete ptzSettingsWindow;
		ptzSettingsWindow = nullptr;
	}
}

void ptz_settings_show(const QModelIndex &index)
{
	obs_frontend_push_ui_translation(obs_module_get_string);

	if (!ptzSettingsWindow)
		ptzSettingsWindow = new PTZSettings();
	ptzSettingsWindow->showDevice(index);

	obs_frontend_pop_ui_translation();
}

extern "C" void ptz_load_settings()
{
	QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(obs_module_text("PTZ.Settings.PTZDevices"));

	obs_frontend_add_event_callback(obs_event, nullptr);

	action->connect(action, &QAction::triggered, []() { ptz_settings_show(); });
}
