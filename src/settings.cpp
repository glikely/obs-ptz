#include <QPlainTextEdit>
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QPushButton>
#include <QFontDatabase>
#include <QFont>
#include <QDialogButtonBox>
#include <QLabel>
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
#include <qt-wrappers.hpp>

#include "ptz.h"
#include "ptz-list-model.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"
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

/**
 * Camera-type entries shared by the "+" dialog's type picker and the
 * Cameras tab's own type combo (used to change an existing device's type).
 * Duplicates ptz_filter_get_properties()'s "type" list in ptz-device.cpp
 * rather than sharing it -- that function is static, returns an
 * obs_properties_t tree (not a QComboBox), and conditionally embeds a
 * whole device settings group -- reusing it here would be more machinery
 * than copying this short, rarely-changed list once.
 */
static void addDeviceTypeItems(QComboBox *combo)
{
#if defined(ENABLE_SERIALPORT)
	combo->addItem(obs_module_text("PTZ.Visca.Serial.Name"), "visca");
#endif
	combo->addItem(obs_module_text("PTZ.Visca.UDP.Name"), "visca-over-ip");
	combo->addItem(obs_module_text("PTZ.Visca.TCP.Name"), "visca-over-tcp");
#if defined(ENABLE_SERIALPORT)
	/* Pelco-D vs Pelco-P is a separate "use_pelco_d" property on the
	 * resulting PTZPelco device, not a distinct type here -- same
	 * reasoning as ptz_filter_get_properties()'s identical list. */
	combo->addItem(obs_module_text("PTZ.Pelco.Name"), "pelco");
#endif
#if defined(ENABLE_ONVIF)
	combo->addItem(obs_module_text("PTZ.ONVIF.Name"), "onvif");
#endif
#if defined(ENABLE_USB_CAM)
	combo->addItem(obs_module_text("PTZ.UVC.Name"), "usb-cam");
#endif
}

/**
 * Prompts for the two things a fresh "PTZ Control" filter needs that the
 * Filters dialog would otherwise ask for one at a time (which source to
 * attach to, via its own Filters dialog; which camera type, via the
 * filter's own properties once added) -- this collects both up front so
 * PTZSettings::on_addPTZ_clicked() can create an already-typed filter in
 * one step. A filter created with no type set would construct no
 * PTZDevice at all (see ptz_device_create()'s dispatch-by-type in
 * ptz-device.cpp), and so wouldn't show up in this dialog's own device
 * list -- leaving type selection for later would make the "+" button look
 * like it silently did nothing.
 */
class PTZAddDeviceDialog : public QDialog {
	Q_DISABLE_COPY(PTZAddDeviceDialog)

public:
	PTZAddDeviceDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(obs_module_text("PTZ.AddDevice.Title"));
		auto layout = new QVBoxLayout(this);

		layout->addWidget(new QLabel(obs_module_text("PTZ.AddDevice.SourceLabel"), this));
		sourceCombo = new QComboBox(this);
		layout->addWidget(sourceCombo);

		auto src_cb = [](void *data, obs_source_t *src) {
			auto combo = static_cast<QComboBox *>(data);
			if (obs_source_get_type(src) != OBS_SOURCE_TYPE_SCENE)
				combo->addItem(QT_UTF8(obs_source_get_name(src)));
			return true;
		};
		obs_enum_sources(src_cb, sourceCombo);

		if (sourceCombo->count() == 0) {
			sourceCombo->setEnabled(false);
			layout->addWidget(new QLabel(obs_module_text("PTZ.AddDevice.NoSources"), this));
		}

		layout->addWidget(new QLabel(obs_module_text("PTZ.AddDevice.TypeLabel"), this));
		typeCombo = new QComboBox(this);
		layout->addWidget(typeCombo);
		addDeviceTypeItems(typeCombo);

		auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		buttons->button(QDialogButtonBox::Ok)->setEnabled(sourceCombo->count() > 0);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);
	}

	QString selectedSourceName() const { return sourceCombo->currentText(); }
	QString selectedType() const { return typeCombo->currentData().toString(); }

private:
	QComboBox *sourceCombo;
	QComboBox *typeCombo;
};

/**
 * Sourced from the filter's own get_properties() (ptz_filter_get_properties()
 * in ptz-device.cpp) rather than the device's get_obs_properties() alone, so
 * this panel matches what the source's own Filters dialog shows: the "type"
 * combo up top, plus the same per-driver connection fields underneath,
 * nested in the same "device" group. Values still come from `settings`
 * (kept in sync with the selected device via ptzDeviceList->save() in
 * currentChanged()), so this only changes where the property *metadata*
 * comes from, not where the values live.
 */
obs_properties_t *PTZSettings::getProperties(void)
{
	auto cb = [](obs_properties_t *, obs_property_t *, void *data_) {
		auto data = static_cast<obs_data_t *>(data_);
		blog(LOG_INFO, "%s", obs_data_get_string(data, "debug_info"));
		return true;
	};

	auto index = ui->deviceList->currentIndex();
	uint32_t device_id = index.data(PTZListModel::DeviceIdRole).toUInt();
	OBSSourceAutoRelease filter = ptz_device_find_filter_source(device_id);
	auto props = filter ? obs_source_properties(filter) : obs_properties_create();

	auto debug = obs_properties_create();
	obs_properties_add_text(debug, "debug_info", NULL, OBS_TEXT_INFO);
	obs_properties_add_button2(debug, "dbgdump", "Write to OBS log", cb, settings);
	obs_properties_add_group(props, "debug", "Full Details", OBS_GROUP_NORMAL, debug);
	return props;
}

/**
 * The single write path for the Cameras tab's properties panel -- scoped to
 * the selected device's *filter* (obs_source_update(), same as the source's
 * own Filters dialog), not to its device_id. device_id is not a stable
 * identity to update against here: a "type" edit destroys the old PTZDevice
 * and constructs a new one with a *different* device_id (see
 * ptz_filter_update() in ptz-device.cpp), so a device_id captured before the
 * call may already refer to a device that no longer exists by the time it
 * returns. The filter itself doesn't get destroyed by a type change, only
 * the PTZDevice it owns, so it's the right thing to key this on.
 *
 * ptz_filter_update() forwards ordinary field edits (host, port, speeds,
 * ...) to the live device unchanged, and swaps the device out when "type"
 * itself changed -- so this same call is correct either way, with no need
 * to special-case a type change here. See restoreSelection() for why the
 * selection needs restoring afterward when it does.
 */
void PTZSettings::applyToFilter(OBSData new_settings)
{
	auto index = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;

	uint32_t device_id = index.data(PTZListModel::DeviceIdRole).toUInt();
	OBSSourceAutoRelease filter = ptz_device_find_filter_source(device_id);
	if (!filter)
		return;

	m_selectedDeviceName = index.data(Qt::DisplayRole).toString();
	obs_source_update(filter, new_settings);
}

void PTZSettings::updateProperties(OBSData, OBSData new_settings)
{
	applyToFilter(new_settings);
}

PTZSettings::PTZSettings() : QWidget(nullptr), ui(new Ui_PTZSettings)
{
	settings = obs_data_create();
	obs_data_release(settings);

	ui->setupUi(this);

	connect(ptzDeviceList, &PTZListModel::dataChanged, this, &PTZSettings::settingsChanged);

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

	/* Changing a device's type (see updateProperties() below) destroys
	 * and recreates it, which goes through the same device-create/
	 * destroy signal pair add/remove already do -- each end resets
	 * ptzDeviceList entirely (do_reset()), which clears ui->deviceList's
	 * current index. Restore it by name once a reset settles, rather
	 * than leaving the properties panel blank after what the user
	 * experiences as an in-place edit. */
	connect(ptzDeviceList, &QAbstractItemModel::modelReset, this, &PTZSettings::restoreSelection);

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
	const QStringList contrib_list = {obs_module_text("PTZ.About.Contributors"),
					  "Fabio Ferrari",
					  "Norihiro Kamae",
					  "Luuk Verhagen",
					  "Trouffman",
					  "Kaito Udagawa",
					  "Jonatã Bolzan Loss",
					  "Eddy Weiz",
					  "Jim Hauxwell",
					  "Jason Lanclos",
					  "Eric Schmidt",
					  "BitRate27",
					  "Anthony Roberts"};
	const QString contributors = QString("<p>%1</p>").arg(contrib_list.join("<br/>"));
	const QStringList translator_list = {
		obs_module_text("PTZ.About.Translators"),
		"cassiopetry (Portuguese, Brazilian)",
		"ETE-Design (Danish)",
		"Manoah Tervoort (Dutch)",
		"이지행(Korean)",
		"Norman Hansen (German)",
		"Giuseppe Chiodaroli (Italian)",
		"arthur_fr (French)",
		"alanfermtz (Spanish)",
		"John Hanssen Kolstad (Norwegian)",
		"danvoulez (Portuguese)",
		"Luca Montibeller Nunes (Portuguese, Brazilian)",
		"Valdinel Lankewicz (Portuguese)",
	};
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
	PTZAddDeviceDialog dialog(this);
	if (dialog.exec() != QDialog::Accepted)
		return;
	QString type = dialog.selectedType();
	if (type.isEmpty())
		return;

	OBSSourceAutoRelease parent = obs_get_source_by_name(QT_TO_UTF8(dialog.selectedSourceName()));
	if (!parent)
		return;

	/* Only "type" needs to be set -- obs_source_create() always runs
	 * .get_defaults()/PTZDevice::getDefaults() for whatever keys aren't
	 * already present, the same mechanism the Filters-dialog-driven flow
	 * already relies on when a type is picked from its own dropdown. */
	OBSDataAutoRelease cfg = obs_data_create();
	obs_data_set_string(cfg, "type", QT_TO_UTF8(type));

	OBSSourceAutoRelease filter = obs_source_create("PTZ Control", obs_module_text("PTZ.Filter.Name"), cfg, nullptr);
	if (filter)
		obs_source_filter_add(parent, filter);
}

void PTZSettings::on_removePTZ_clicked()
{
	auto index = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;
	uint32_t device_id = index.data(PTZListModel::DeviceIdRole).toUInt();

	QString name = index.data(Qt::DisplayRole).toString();
	auto button = QMessageBox::question(this, obs_module_text("PTZ.RemoveDevice.Tooltip"),
					    QString(obs_module_text("PTZ.RemoveDevice.ConfirmText")).arg(name));
	if (button != QMessageBox::Yes)
		return;

	OBSSourceAutoRelease filter = ptz_device_find_filter_source(device_id);
	if (!filter)
		return;
	/* obs_filter_get_parent() returns a borrowed pointer -- OBSSource's
	 * converting constructor (unlike OBSSourceAutoRelease's) takes its
	 * own new ref via obs_source_get_ref(), so this is safe to hold and
	 * releases correctly when it goes out of scope. */
	OBSSource parent = obs_filter_get_parent(filter);
	obs_source_filter_remove(parent, filter);
}

/**
 * A type change (in updateProperties() above) destroys the old PTZDevice
 * and creates a new one, which -- like the "+"/"-" buttons -- goes through
 * ptzDeviceList's device-create/destroy signals, each of which resets the
 * whole model and so clears ui->deviceList's current index. Re-selects the
 * device that was selected before the reset, by name, once one settles; a
 * name that no longer exists (e.g. after a real removal) leaves the
 * selection cleared, which is correct there.
 */
void PTZSettings::restoreSelection()
{
	if (m_selectedDeviceName.isEmpty())
		return;

	auto idx = ptzDeviceList->indexFromName(m_selectedDeviceName);
	if (idx.isValid()) {
		ui->deviceList->setCurrentIndex(idx);
		return;
	}

	/* The device is genuinely gone -- e.g. after picking "unset" from
	 * the type combo, which destroys the old PTZDevice with nothing to
	 * replace it -- so there's no row left to reselect.
	 * QAbstractItemView::reset() (run in response to the modelReset this
	 * slot is connected to) already cleared ui->deviceList's own current
	 * index via QItemSelectionModel::reset(), but that call is documented
	 * to do so *without* emitting currentChanged. Without an explicit
	 * call here, the properties panel is left showing stale values for a
	 * device that no longer exists. */
	m_selectedDeviceName.clear();
	currentChanged(QModelIndex(), QModelIndex());
}

void PTZSettings::on_applyButton_clicked()
{
	applyToFilter(propertiesView->GetSettings());
}

void PTZSettings::currentChanged(const QModelIndex &current, const QModelIndex &)
{
	obs_data_clear(settings);

	ptzDeviceList->save(current, settings);
	auto rawjson = obs_data_get_json(settings);
	/* Use QJsonDocument for nice formatting */
	auto json = QJsonDocument::fromJson(rawjson).toJson();
	obs_data_set_string(settings, "debug_info", json.constData());

	propertiesView->ReloadProperties();

	m_selectedDeviceName = current.isValid() ? current.data(Qt::DisplayRole).toString() : QString();
}

void PTZSettings::settingsChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
	auto idx = ui->deviceList->currentIndex();
	QItemSelectionRange range(topLeft, bottomRight);
	if (!range.contains(idx))
		return;

	ptzDeviceList->save(idx, settings);
	obs_data_erase(settings, "debug_info");
	auto json = QJsonDocument::fromJson(obs_data_get_json(settings)).toJson();
	obs_data_set_string(settings, "debug_info", json.constData());
	QMetaObject::invokeMethod(propertiesView, "RefreshProperties", Qt::QueuedConnection);
}

void PTZSettings::showDevice(const QModelIndex &index)
{
	if (index.isValid()) {
		ui->deviceList->setCurrentIndex(index);
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
