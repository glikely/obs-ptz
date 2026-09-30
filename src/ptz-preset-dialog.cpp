/* Dialog for editing one PTZ preset
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ptz-preset-dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
#include <obs-module.h>

#include "ptz-list-model.hpp"
#include "protocol-helpers.hpp"

static QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString PTZPresetDialog::keyLabel(const QString &key)
{
	/* The same names the settings dialog's state view shows them by */
	static const QMap<QString, const char *> labels = {
		{"pan", "PTZ.Device.State.Pan"},
		{"tilt", "PTZ.Device.State.Tilt"},
		{"zoom", "PTZ.Device.State.Zoom"},
		{"focus", "PTZ.Device.State.Focus"},
		{"pan_pos", "PTZ.Device.State.Pan"},
		{"tilt_pos", "PTZ.Device.State.Tilt"},
		{"zoom_pos", "PTZ.Device.State.Zoom"},
		{"focus_pos", "PTZ.Device.State.Focus"},
		{"focus_af_enabled", "PTZ.Device.State.Autofocus"},
		{"focus_af_mode", "PTZ.Device.State.AFMode"},
		{"focus_af_sensitivity", "PTZ.Device.State.AFSensitivity"},
		{"focus_near_limit", "PTZ.Device.State.FocusNearLimit"},
		{"dzoom_on", "PTZ.Device.State.DZoom"},
		{"ir_correction", "PTZ.Device.State.IRCorrection"},
		{"wb_mode", "PTZ.WhiteBalance"},
		{"r_gain", "PTZ.Device.State.RGain"},
		{"b_gain", "PTZ.Device.State.BGain"},
		{"ae_mode", "PTZ.Device.State.AEMode"},
		{"slow_shutter", "PTZ.Device.State.SlowShutter"},
		{"shutter_pos", "PTZ.Device.State.Shutter"},
		{"iris_pos", "PTZ.Device.State.Iris"},
		{"gain_pos", "PTZ.Device.State.Gain"},
		{"gain_limit", "PTZ.Device.State.GainLimit"},
		{"bright_pos", "PTZ.Device.State.Bright"},
		{"exposure_comp", "PTZ.Device.State.ExposureComp"},
		{"exposure_comp_pos", "PTZ.Device.State.ExposureCompLevel"},
		{"back_light", "PTZ.Device.State.BackLight"},
		{"wd_mode", "PTZ.Device.State.WD"},
		{"defog_mode", "PTZ.Device.State.Defog"},
		{"high_sensitivity", "PTZ.Device.State.HighSensitivity"},
		{"aperture_gain", "PTZ.Device.State.Aperture"},
		{"high_resolution", "PTZ.Device.State.HighResolution"},
		{"nr_level", "PTZ.Device.State.NR"},
		{"gamma", "PTZ.Device.State.Gamma"},
		{"chroma_suppress", "PTZ.Device.State.ChromaSuppress"},
		{"color_gain", "PTZ.Device.State.ColorGain"},
		{"color_hue", "PTZ.Device.State.ColorHue"},
		{"picture_effect", "PTZ.Device.State.PictureEffect"},
	};
	auto label = labels.value(key, nullptr);
	return label ? text(label) : key;
}

PTZPresetDialog::PTZPresetDialog(const QModelIndex &presetIndex, QWidget *parent)
	: QDialog(parent),
	  m_index(presetIndex)
{
	setWindowTitle(text("PTZ.Preset.Edit.Title"));
	setAttribute(Qt::WA_DeleteOnClose);

	auto layout = new QVBoxLayout(this);
	auto form = new QFormLayout();
	m_name = new QLineEdit();
	m_name->setObjectName("presetName");
	m_name->setPlaceholderText(QString(text("PTZ.PresetNum")).arg(presetIndex.data(Qt::UserRole).toInt()));
	form->addRow(text("PTZ.Preset.Edit.Name"), m_name);
	m_storage = new QComboBox();
	m_storage->setObjectName("presetStorage");
	m_storage->addItem(text("PTZ.Preset.Edit.Storage.Camera"), false);
	m_storage->addItem(text("PTZ.Preset.Edit.Storage.Local"), true);
	form->addRow(text("PTZ.Preset.Edit.Storage"), m_storage);
	layout->addLayout(form);

	m_valuesGroup = new QGroupBox(text("PTZ.Preset.Edit.Values"));
	auto groupLayout = new QVBoxLayout(m_valuesGroup);
	auto help = new QLabel(text("PTZ.Preset.Edit.Values.Help"));
	help->setWordWrap(true);
	groupLayout->addWidget(help);
	m_empty = new QLabel(text("PTZ.Preset.Edit.Values.Empty"));
	m_empty->setWordWrap(true);
	groupLayout->addWidget(m_empty);
	auto scroll = new QScrollArea();
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto values = new QWidget();
	m_valuesGrid = new QGridLayout(values);
	m_valuesGrid->setColumnStretch(1, 1);
	scroll->setWidget(values);
	groupLayout->addWidget(scroll, 1);
	m_capture = new QPushButton(text("PTZ.Preset.Edit.Capture"));
	m_capture->setObjectName("presetCapture");
	groupLayout->addWidget(m_capture, 0, Qt::AlignLeft);
	layout->addWidget(m_valuesGroup, 1);

	auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	layout->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		apply();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_storage, &QComboBox::currentIndexChanged, this, &PTZPresetDialog::updateStorage);
	connect(m_capture, &QPushButton::clicked, this, &PTZPresetDialog::saveCurrentState);
	/* The preset can go away while the dialog is open, with its camera */
	connect(ptzDeviceList, &QAbstractItemModel::modelReset, this, &QDialog::reject);
	connect(ptzDeviceList, &QAbstractItemModel::rowsRemoved, this, [this]() {
		if (!m_index.isValid())
			reject();
	});

	load();
	resize(420, 480);
}

/* Show the preset as the device has it */
void PTZPresetDialog::load()
{
	if (!m_index.isValid())
		return;
	OBSDataAutoRelease info = obs_data_create();
	ptzDeviceList->presetInfo(m_index, info.Get());
	QVariantMap map = OBSDataToVariantMap(info.Get());

	m_name->setText(map.value("name").toString());
	/* A camera that can't store presets only has local ones */
	bool deviceLocalOnly = m_index.parent().data(PTZListModel::PresetLocalRole).toBool();
	m_storage->setEnabled(!deviceLocalOnly);
	m_storage->setCurrentIndex(map.value("local").toBool() ? 1 : 0);
	m_state = map.value("state").toMap();
	m_recall = map.value("recall").toMap();
	showValues();
	updateStorage();
}

void PTZPresetDialog::showValues()
{
	for (const auto &row : m_rows) {
		delete row.recall;
		delete row.editor;
	}
	m_rows.clear();

	int line = 0;
	for (auto it = m_state.cbegin(); it != m_state.cend(); ++it, ++line) {
		const QString &key = it.key();
		const QVariant &value = it.value();
		Row row;
		row.recall = new QCheckBox(keyLabel(key));
		row.recall->setObjectName("recall_" + key);
		row.recall->setChecked(m_recall.value(key).toBool());
		if (value.typeId() == QMetaType::Bool) {
			auto box = new QCheckBox();
			box->setChecked(value.toBool());
			row.editor = box;
		} else if (value.typeId() == QMetaType::Double) {
			auto spin = new QDoubleSpinBox();
			/* Positions are in the units of the movement API */
			spin->setRange(key == "pan" || key == "tilt" ? -1.0 : 0.0, 1.0);
			spin->setDecimals(3);
			spin->setSingleStep(0.01);
			spin->setValue(value.toDouble());
			row.editor = spin;
		} else {
			auto spin = new QSpinBox();
			/* A camera's own position can be signed */
			spin->setRange(-0x8000, 0xffff);
			spin->setValue(value.toInt());
			row.editor = spin;
		}
		row.editor->setObjectName("value_" + key);
		row.editor->setEnabled(row.recall->isChecked());
		connect(row.recall, &QCheckBox::toggled, row.editor, &QWidget::setEnabled);
		m_valuesGrid->addWidget(row.recall, line, 0);
		m_valuesGrid->addWidget(row.editor, line, 1);
		m_rows[key] = row;
	}
	m_empty->setVisible(m_state.isEmpty());
}

void PTZPresetDialog::updateStorage()
{
	m_valuesGroup->setVisible(m_storage->currentData().toBool());
}

/* Capture what the camera has now, as the dock's Save does, and show it.
 * The preset has to be local for that, so store it that way first. */
void PTZPresetDialog::saveCurrentState()
{
	if (!m_index.isValid())
		return;
	OBSDataAutoRelease info = obs_data_create();
	obs_data_set_bool(info, "local", true);
	ptzDeviceList->setPresetInfo(m_index, info.Get());
	ptzDeviceList->preset_save(m_index.parent().data(PTZListModel::DeviceIdRole).toUInt(),
				   m_index.data(Qt::UserRole).toInt());
	QString name = m_name->text();
	load();
	m_name->setText(name);
}

void PTZPresetDialog::apply()
{
	if (!m_index.isValid())
		return;
	OBSDataAutoRelease info = obs_data_create();
	bool local = m_storage->currentData().toBool();
	obs_data_set_bool(info, "local", local);
	if (local) {
		QVariantMap recall, values;
		for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
			recall[it.key()] = it->recall->isChecked();
			if (auto box = qobject_cast<QCheckBox *>(it->editor))
				values[it.key()] = box->isChecked();
			else if (auto dspin = qobject_cast<QDoubleSpinBox *>(it->editor))
				values[it.key()] = dspin->value();
			else if (auto spin = qobject_cast<QSpinBox *>(it->editor))
				values[it.key()] = spin->value();
		}
		obs_data_set_obj(info, "recall", variantMapToOBSData(recall));
		obs_data_set_obj(info, "state", variantMapToOBSData(values));
	}
	ptzDeviceList->setPresetInfo(m_index, info.Get());
	if (m_name->text() != m_index.data(Qt::EditRole).toString())
		ptzDeviceList->setData(m_index, m_name->text());
}
