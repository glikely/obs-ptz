#include "ptz-state-view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <obs-module.h>

/* What each white balance mode is called, in the order the camera numbers
 * them */
static const char *const white_balance_modes[] = {
	"PTZ.WhiteBalance.Auto",    "PTZ.WhiteBalance.Indoor",    "PTZ.WhiteBalance.Outdoor",
	"PTZ.WhiteBalance.OnePush", "PTZ.WhiteBalance.AutoTrace", "PTZ.WhiteBalance.Manual",
};

/* A checkbox that shows a yes or no and can't be changed. Not disabled,
 * which would grey it out: it just ignores the mouse and the keyboard. */
static QCheckBox *makeIndicator(const char *text)
{
	auto box = new QCheckBox(obs_module_text(text));
	box->setAttribute(Qt::WA_TransparentForMouseEvents);
	box->setFocusPolicy(Qt::NoFocus);
	return box;
}

PTZStateView::PTZStateView(QWidget *parent) : QWidget(parent)
{
	auto page = new QVBoxLayout(this);
	page->setContentsMargins(0, 0, 0, 0);

	auto top = new QFormLayout();
	m_name = new QLabel();
	m_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
	top->addRow(new QLabel(obs_module_text("PTZ.Device.State.Name")), m_name);
	m_connected = makeIndicator("PTZ.Device.Status.Connected");
	m_live = makeIndicator("PTZ.Device.State.Live");
	m_preview = makeIndicator("PTZ.Device.State.Preview");
	m_locked = makeIndicator("PTZ.Dock.Lock.Name");
	top->addRow(m_connected);
	top->addRow(m_live);
	top->addRow(m_preview);
	top->addRow(m_locked);

	m_power = new QCheckBox(obs_module_text("PTZ.Device.State.Power"));
	m_focusAuto = new QCheckBox(obs_module_text("PTZ.Device.State.Autofocus"));
	top->addRow(m_power);
	top->addRow(m_focusAuto);
	m_power->hide();
	m_focusAuto->hide();
	page->addLayout(top);

	m_positionGroup = new QGroupBox(obs_module_text("PTZ.Device.State.Position"));
	auto position = new QFormLayout(m_positionGroup);
	const char *axisText[AxisCount] = {"PTZ.Device.State.Pan", "PTZ.Device.State.Tilt", "PTZ.Device.State.Zoom",
					   "PTZ.Device.State.Focus"};
	for (int i = 0; i < AxisCount; i++) {
		m_axisLabels[i] = new QLabel(obs_module_text(axisText[i]));
		m_axisValues[i] = new QLabel();
		m_axisValues[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
		position->addRow(m_axisLabels[i], m_axisValues[i]);
		setRowVisible(m_axisLabels[i], m_axisValues[i], false);
	}
	m_positionGroup->hide();
	page->addWidget(m_positionGroup);

	m_whiteBalanceGroup = new QGroupBox(obs_module_text("PTZ.WhiteBalance"));
	auto whiteBalance = new QFormLayout(m_whiteBalanceGroup);
	m_whiteBalanceMode = new QComboBox();
	for (int mode = 0; mode < (int)(sizeof(white_balance_modes) / sizeof(*white_balance_modes)); mode++)
		m_whiteBalanceMode->addItem(obs_module_text(white_balance_modes[mode]), mode);
	m_whiteBalanceMode->setCurrentIndex(-1);
	whiteBalance->addRow(new QLabel(obs_module_text("PTZ.WhiteBalance.Mode")), m_whiteBalanceMode);
	m_onePush = new QPushButton(obs_module_text("PTZ.WhiteBalance.OnePushButton"));
	whiteBalance->addRow(m_onePush);
	m_whiteBalanceGroup->hide();
	page->addWidget(m_whiteBalanceGroup);

	m_diagnosticsGroup = new QGroupBox(obs_module_text("PTZ.Device.State.Diagnostics"));
	m_diagnosticsGroup->setObjectName("diagnostics");
	auto diagnostics = new QVBoxLayout(m_diagnosticsGroup);
	auto addAction = [this, diagnostics](const char *name, const char *text, const char *trigger) {
		auto button = new QPushButton(obs_module_text(text));
		button->setObjectName(name);
		diagnostics->addWidget(button);
		connect(button, &QPushButton::clicked, this, [this, trigger]() { emit actionRequested(trigger); });
	};
	addAction("scanInquiries", "PTZ.Visca.Debug.ScanInquiries", "scan_inquiries");
	addAction("repliesToLog", "PTZ.Visca.Debug.RepliesToLog", "replies_to_log");
	m_diagnosticsGroup->hide();
	page->addWidget(m_diagnosticsGroup);
	page->addStretch(1);

	/* activated() is only the user picking something, not the list being set
	 * to follow the camera, so there is nothing to block while following it */
	connect(m_whiteBalanceMode, &QComboBox::activated, this, [this](int index) {
		OBSDataAutoRelease requested = obs_data_create();
		obs_data_set_int(requested, "wb_mode", m_whiteBalanceMode->itemData(index).toInt());
		emit stateRequested(OBSData(requested.Get()));
		m_whiteBalanceSettle.start();
	});
	connect(m_onePush, &QPushButton::clicked, this, [this]() { emit actionRequested("wb_onepush"); });

	/* clicked(), not toggled(): a programmatic setChecked() below (the
	 * device catching up, or resetting to what it last reported) must not
	 * feed back as a request */
	connect(m_power, &QCheckBox::clicked, this, [this](bool checked) {
		OBSDataAutoRelease requested = obs_data_create();
		obs_data_set_bool(requested, "power_on", checked);
		emit stateRequested(OBSData(requested.Get()));
	});
	connect(m_focusAuto, &QCheckBox::clicked, this, [this](bool checked) {
		OBSDataAutoRelease requested = obs_data_create();
		obs_data_set_bool(requested, "focus_af_enabled", checked);
		emit stateRequested(OBSData(requested.Get()));
	});

	/* The list follows the camera, not the user: if the camera hasn't taken
	 * up what was asked for by now, show what it says it is doing */
	m_whiteBalanceSettle.setSingleShot(true);
	m_whiteBalanceSettle.setInterval(1500);
	connect(&m_whiteBalanceSettle, &QTimer::timeout, this, [this]() { showWhiteBalance(m_reportedWhiteBalance); });
}

/* Both halves of a form row go together: hiding just the field would leave
 * its label behind */
void PTZStateView::setRowVisible(QWidget *label, QWidget *field, bool visible)
{
	label->setVisible(visible);
	field->setVisible(visible);
}

void PTZStateView::showWhiteBalance(int mode)
{
	int at = m_whiteBalanceMode->findData(mode);
	if (m_whiteBalanceMode->currentIndex() != at)
		m_whiteBalanceMode->setCurrentIndex(at);
}

void PTZStateView::setState(OBSData state)
{
	applyData(state, true);
}

void PTZStateView::applyChanges(OBSData changed)
{
	applyData(changed, false);
}

/* `all` is a whole state, in which a key that isn't there means the device
 * doesn't report it; otherwise only what changed, and a key that isn't there
 * hasn't. Nothing is set that already has that value, so a widget is only
 * touched when there is something new to show in it. */
void PTZStateView::applyData(obs_data_t *data, bool all)
{
	bool changed = false;
	auto has = [data](const char *key) {
		return obs_data_has_user_value(data, key);
	};
	auto setText = [&changed](QLabel *label, const QString &text) {
		if (label->text() != text) {
			label->setText(text);
			changed = true;
		}
	};
	auto setFlag = [&](QCheckBox *box, const char *key) {
		if (!all && !has(key))
			return;
		bool value = obs_data_get_bool(data, key);
		if (box->isChecked() != value) {
			box->setChecked(value);
			changed = true;
		}
	};

	if (all || has("name"))
		setText(m_name, QString::fromUtf8(obs_data_get_string(data, "name")));
	setFlag(m_connected, "connected");
	setFlag(m_live, "live");
	setFlag(m_preview, "preview");
	setFlag(m_locked, "locked");

	/* Commandable, unlike the indicators above: shown only while the
	 * device reports the key at all, like an axis row */
	auto setCommandableFlag = [&](QCheckBox *box, const char *key) {
		bool shown = !box->isHidden();
		if (has(key)) {
			bool value = obs_data_get_bool(data, key);
			if (box->isChecked() != value) {
				box->setChecked(value);
				changed = true;
			}
			if (!shown) {
				box->show();
				changed = true;
			}
		} else if (all && shown) {
			box->hide();
			changed = true;
		}
	};
	setCommandableFlag(m_power, "power_on");
	setCommandableFlag(m_focusAuto, "focus_af_enabled");

	for (int i = 0; i < AxisCount; i++) {
		const char *key = m_axisKeys[i];
		bool shown = !m_axisLabels[i]->isHidden();
		if (has(key)) {
			setText(m_axisValues[i], QString::asprintf("%.3f", obs_data_get_double(data, key)));
			if (!shown) {
				setRowVisible(m_axisLabels[i], m_axisValues[i], true);
				changed = true;
			}
		} else if (all && shown) {
			setRowVisible(m_axisLabels[i], m_axisValues[i], false);
			changed = true;
		}
	}
	bool anyAxis = false;
	for (int i = 0; i < AxisCount; i++)
		anyAxis |= !m_axisLabels[i]->isHidden();
	if (anyAxis == m_positionGroup->isHidden()) {
		m_positionGroup->setVisible(anyAxis);
		changed = true;
	}

	if (has("wb_mode")) {
		int mode = (int)obs_data_get_int(data, "wb_mode");
		if (m_reportedWhiteBalance != mode || m_whiteBalanceGroup->isHidden()) {
			m_reportedWhiteBalance = mode;
			showWhiteBalance(mode);
			m_whiteBalanceGroup->show();
			changed = true;
		}
	} else if (all && !m_whiteBalanceGroup->isHidden()) {
		m_reportedWhiteBalance = -1;
		m_whiteBalanceGroup->hide();
		changed = true;
	}

	if (all || has("supports_diagnostics")) {
		bool diagnostics = obs_data_get_bool(data, "supports_diagnostics");
		if (diagnostics == m_diagnosticsGroup->isHidden()) {
			m_diagnosticsGroup->setVisible(diagnostics);
			changed = true;
		}
	}

	if (changed)
		m_updateCount++;
}

QVariantMap PTZStateView::shownValues() const
{
	QVariantMap shown;
	shown["name"] = m_name->text();
	shown["connected"] = m_connected->isChecked();
	shown["live"] = m_live->isChecked();
	shown["preview"] = m_preview->isChecked();
	shown["locked"] = m_locked->isChecked();
	if (!m_power->isHidden())
		shown["power_on"] = m_power->isChecked();
	if (!m_focusAuto->isHidden())
		shown["focus_af_enabled"] = m_focusAuto->isChecked();
	for (int i = 0; i < AxisCount; i++)
		if (!m_axisLabels[i]->isHidden())
			shown[m_axisKeys[i]] = m_axisValues[i]->text().toDouble();
	if (!m_whiteBalanceGroup->isHidden())
		shown["wb_mode"] = m_whiteBalanceMode->currentData().toInt();
	return shown;
}
