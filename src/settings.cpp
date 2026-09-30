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
#include <QLabel>
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
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ptz-state-view.hpp"
#include "contributors-generated.hpp"
#include "translators-generated.hpp"
#include "ui_settings.h"

/* ----------------------------------------------------------------- */

static PTZSettings *ptzSettingsWindow = nullptr;

/* ----------------------------------------------------------------- */

class SourceNameDelegate : public QStyledItemDelegate {
	Q_DISABLE_COPY(SourceNameDelegate)

public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void fixName(QStyleOptionViewItem *opt, const QModelIndex &index) const
	{
		initStyleOption(opt, index);
		opt->text = opt->text + " [" + index.data(PTZListModel::DescriptionRole).toString() + "]";
	}
	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		Q_ASSERT(index.isValid());
		QStyleOptionViewItem opt = option;
		fixName(&opt, index);
		QStyle *style = option.widget ? option.widget->style() : QApplication::style();
		style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, option.widget);
	}
	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		Q_ASSERT(index.isValid());
		QStyleOptionViewItem opt = option;
		fixName(&opt, index);
		QStyle *style = option.widget ? option.widget->style() : QApplication::style();
		return style->sizeFromContents(QStyle::CT_ItemViewItem, &opt, QSize(), option.widget);
	}
};

obs_properties_t *PTZSettings::getProperties(void)
{
	return ptzDeviceList->getProperties(ui->deviceList->currentIndex());
}

void PTZSettings::updateProperties(OBSData, OBSData new_settings)
{
	ptzDeviceList->update(ui->deviceList->currentIndex(), new_settings);
}

uint32_t PTZSettings::currentDeviceId() const
{
	return ui->deviceList->currentIndex().data(PTZListModel::DeviceIdRole).toUInt();
}

PTZSettings::PTZSettings() : QWidget(nullptr), ui(new Ui_PTZSettings)
{
	settings = obs_data_create();
	obs_data_release(settings);

	ui->setupUi(this);

	connect(ptzDeviceList, &PTZListModel::deviceSettingsUpdated, this, &PTZSettings::deviceSettingsUpdated);
	connect(ptzDeviceList, &PTZListModel::deviceStateUpdated, this, &PTZSettings::deviceStateUpdated);

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

	auto snd = new SourceNameDelegate(this);
	ui->deviceList->setModel(ptzDeviceList);
	ui->deviceList->setItemDelegateForColumn(0, snd);

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

	/* The properties view does its own scrolling by default (it's a
	 * QScrollArea itself); turn that off so it instead sizes itself to its
	 * content, and stack it with the state view on the one page in
	 * settings.ui, whose propertiesScroll is the only thing that scrolls. */
	propertiesView->setScrolling(false);
	ui->settingsViewLayout->addWidget(propertiesView);

	/* What the user asks of the camera in the state view is a request, not
	 * an update: the camera changes what it reports of itself once it has
	 * done as asked, and that comes back as deviceStateUpdated() */
	connect(ui->stateView, &PTZStateView::stateRequested, this,
		[this](OBSData requested) { ptzDeviceList->setState(ui->deviceList->currentIndex(), requested); });
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

/* A new device is a PTZ Control filter on a source, so ask which source,
 * and which protocol, or which removed device's backup (see
 * ptz_device_backups_get()) to restore, which brings its protocol with it */
void PTZSettings::addDevice()
{
	/* The devices that control a source; one whose source was deleted can
	 * linger, under its name, until OBS lets go of its filter */
	OBSDataArrayAutoRelease devices = ptz_devices_get_config();
	QList<OBSData> live;
	QStringList used;
	for (size_t i = 0; i < obs_data_array_count(devices); i++) {
		OBSDataAutoRelease item = obs_data_array_item(devices, i);
		OBSSourceAutoRelease src = ptz_device_get_parent_source((uint32_t)obs_data_get_int(item, "id"));
		if (!src)
			continue;
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
	/* A heading over each group: shown, but not something to pick */
	auto addHeading = [&](const char *text) {
		deviceCombo->addItem(obs_module_text(text), -1);
		auto model = qobject_cast<QStandardItemModel *>(deviceCombo->model());
		if (model)
			model->item(deviceCombo->count() - 1)->setFlags(Qt::NoItemFlags);
	};
	/* Each item's data is its index in choices, and for a protocol, the
	 * type it makes; for a backup, the name of its source */
	addHeading("PTZ.AddDevice.NewHeading");
	for (int i = 0; i < protocols.size(); i++) {
		deviceCombo->addItem(protocols[i].first, i);
		deviceCombo->setItemData(deviceCombo->count() - 1, obs_data_get_string(protocols[i].second, "type"),
					 Qt::UserRole + 1);
	}
	if (!backups.isEmpty()) {
		deviceCombo->insertSeparator(deviceCombo->count());
		addHeading("PTZ.AddDevice.RestoreHeading");
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

	/* Offer the backup of a device that was on a source of the same name;
	 * otherwise go back to the protocol last picked */
	int protocolRow = deviceCombo->findData(0);
	connect(deviceCombo, &QComboBox::activated, &dialog, [&](int row) {
		int choice = deviceCombo->itemData(row).toInt();
		if (choice >= 0 && choice < protocols.size())
			protocolRow = row;
	});
	auto matchBackup = [&](const QString &source) {
		int row = deviceCombo->findData(source, Qt::UserRole + 2);
		deviceCombo->setCurrentIndex(row >= 0 ? row : protocolRow);
	};
	connect(sourceCombo, &QComboBox::currentTextChanged, &dialog, matchBackup);
	matchBackup(sourceCombo->currentText());

	if (dialog.exec() != QDialog::Accepted)
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
}

void PTZSettings::currentChanged(const QModelIndex &current, const QModelIndex &)
{
	obs_data_clear(settings);
	ptzDeviceList->save(current, settings);
	propertiesView->ReloadProperties();

	OBSDataAutoRelease state = obs_data_create();
	ptzDeviceList->saveState(current, state.Get());
	ui->stateView->setState(state.Get());
}

/* Only the settings view hears about this: state changing all the time
 * (connection, live, what the camera reports) used to have it re-save and
 * refresh the settings under the user's cursor */
void PTZSettings::deviceSettingsUpdated(uint32_t device_id)
{
	if (device_id != currentDeviceId())
		return;

	ptzDeviceList->save(ui->deviceList->currentIndex(), settings);
	QMetaObject::invokeMethod(propertiesView, "RefreshProperties", Qt::QueuedConnection);
}

/* Fold in only what the device says changed. The state view changes just
 * the widgets that shows, in place, so it can take every change as it comes,
 * however often the camera's position does. */
void PTZSettings::deviceStateUpdated(uint32_t device_id, OBSData changed)
{
	if (device_id != currentDeviceId())
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
