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
	"<p>Contributors:<br/>"
	"Luuk Verhagen<br/>"
	"Norihiro Kamae<br/>"
	"Jim Hauxwell</p>"
	"</body></html>";

QWidget *SourceNameDelegate::createEditor(QWidget *parent,
					const QStyleOptionViewItem &option,
					const QModelIndex &index) const
{
	QComboBox *cb = new QComboBox(parent);
	cb->setEditable(true);
	const int row = index.row();

	// Get list of all sources
	auto src_cb = [](void *data, obs_source_t* src) {
		auto srcnames = static_cast<QStringList*>(data);
		srcnames->append(obs_source_get_name(src));
		return true;
	};
	QStringList srcnames;
	obs_enum_sources(src_cb, &srcnames);

	// Remove the ones already assigned
	QStringListIterator i(ptzDeviceList.getDeviceNames());
	while (i.hasNext())
		srcnames.removeAll(i.next());
	cb->addItems(srcnames);

	// Put the current name at the top of the list
	cb->insertItem(0, index.data(Qt::EditRole).toString());
	return cb;
}

void SourceNameDelegate::setEditorData(QWidget *editor,
					const QModelIndex &index) const
{
	QComboBox *cb = qobject_cast<QComboBox *>(editor);
	Q_ASSERT(cb);
	cb->setCurrentText(index.data(Qt::EditRole).toString());
}

void SourceNameDelegate::setModelData(QWidget *editor,
					QAbstractItemModel *model,
					const QModelIndex &index) const
{
	QComboBox *cb = qobject_cast<QComboBox *>(editor);
	Q_ASSERT(cb);
	model->setData(index, cb->currentText(), Qt::EditRole);
}

obs_properties_t *PTZSettings::getProperties(void)
{
	PTZDevice *ptz = ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	return ptz ? ptz->get_obs_properties() : obs_properties_create();
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

#ifdef CONFIG_USE_GAMEPAD
	ui->gamepadCheckBox->setChecked(PTZControls::getInstance()->gamepadEnabled());
#else
	ui->gamepadCheckBox->setVisible(false);
#endif
	ui->livemoveCheckBox->setChecked(PTZControls::getInstance()->liveMovesDisabled());

	auto snd = new SourceNameDelegate(this);
	ui->deviceList->setModel(&ptzDeviceList);
	ui->deviceList->setItemDelegateForColumn(0, snd);

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
	ui->propertiesLayout->insertWidget(2, propertiesView, 1);
	ui->versionLabel->setText(description_text);
}

PTZSettings::~PTZSettings()
{
	delete ui;
}

void PTZSettings::set_selected(uint32_t device_id)
{
	ui->deviceList->setCurrentIndex(ptzDeviceList.indexFromDeviceId(device_id));
}

void PTZSettings::on_applyButton_clicked()
{
	PTZDevice *ptz = ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (ptz)
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
	QAction *addIPC365 = addPTZContext.addAction("IPC365");
	QAction *addPelcoD = addPTZContext.addAction("Pelco D");
	QAction *addPelcoP = addPTZContext.addAction("Pelco P");
	QAction *action = addPTZContext.exec(QCursor::pos());

	if (action == addViscaSerial) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "visca");
		ptzDeviceList.make_device(cfg);
	}
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
	if (action == addIPC365) {
		OBSData cfg = obs_data_create();
		obs_data_release(cfg);
		obs_data_set_string(cfg, "type", "ipc365");
		obs_data_set_int(cfg, "port", 23456);
		ptzDeviceList.make_device(cfg);
	}
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
}

void PTZSettings::on_removePTZ_clicked()
{
	PTZDevice *ptz = ptzDeviceList.getDevice(ui->deviceList->currentIndex());
	if (!ptz)
		return;
	delete ptz;
}

#ifdef CONFIG_USE_GAMEPAD
void PTZSettings::on_gamepadCheckBox_stateChanged(int state)
{
	PTZControls::getInstance()->setGamepadEnabled(ui->gamepadCheckBox->isChecked());
}
#endif

void PTZSettings::on_livemoveCheckBox_stateChanged(int state)
{
	PTZControls::getInstance()->setDisableLiveMoves(ui->livemoveCheckBox->isChecked());
}

void PTZSettings::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
	Q_UNUSED(previous);

	obs_data_clear(settings);
	PTZDevice *ptz = ptzDeviceList.getDevice(current);
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

void ptz_settings_show(uint32_t device_id)
{
	obs_frontend_push_ui_translation(obs_module_get_string);

	if (!ptzSettingsWindow)
		ptzSettingsWindow = new PTZSettings();
	ptzSettingsWindow->set_selected(device_id);
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
