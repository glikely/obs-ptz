#include "ptz-state-view.hpp"

#include <functional>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <obs-module.h>

/* What each white balance mode is called, in the order the camera numbers
 * them */
static const char *const white_balance_modes[] = {
	"PTZ.WhiteBalance.Auto",    "PTZ.WhiteBalance.Indoor",    "PTZ.WhiteBalance.Outdoor",
	"PTZ.WhiteBalance.OnePush", "PTZ.WhiteBalance.AutoTrace", "PTZ.WhiteBalance.Manual",
};

/* A row made from the table in the constructor, for one state key */
struct PTZStateView::Field {
	const char *key;
	FieldKind kind;
	/* The state value is a bool, not a number: a checkbox's always, and a
	 * list's whose two items are true and false */
	bool boolValue = false;
	/* A checkbox for what can only be turned off from here, so it can only
	 * be clicked while it is on */
	bool offOnly = false;
	/* A list of steps, where a value the camera reports between two of them
	 * is shown as the one below it */
	bool stepped = false;
	/* How a text field shows the value, from the state object it is in */
	std::function<QString(obs_data_t *)> format;
	QLabel *label = nullptr; // none for a checkbox, which names itself
	QWidget *widget = nullptr;
	QGroupBox *group = nullptr;
	/* What the device last reported, null while it doesn't report it */
	QVariant reported;
	/* Started when the user edits it: the row goes back to what the device
	 * reports once this runs out, if the device hasn't done as asked */
	QTimer settle;
};

/* The Sony VISCA manual's "VISCA Command Setting Values" tables */

/* Shutter speeds, by position, at 59.94 or 29.97 Hz, and at 50 or 25 */
static const char *const shutter_speeds[2][0x16] = {
	{"1/1",   "1/2",   "1/4",   "1/8",   "1/15",   "1/30",   "1/60",   "1/90",   "1/100",  "1/125",  "1/180",
	 "1/250", "1/350", "1/500", "1/725", "1/1000", "1/1500", "1/2000", "1/3000", "1/4000", "1/6000", "1/10000"},
	{"1/1",   "1/2",   "1/3",   "1/6",   "1/12",   "1/25",   "1/50",   "1/75",   "1/100",  "1/120",  "1/150",
	 "1/215", "1/300", "1/425", "1/600", "1/1000", "1/1250", "1/1750", "1/2500", "1/3500", "1/6000", "1/10000"},
};

/* The F number of each iris position from 0x05 (F14) to 0x11 (F1.8); 0 is closed */
static const char *const iris_f_numbers[] = {"F14", "F11",  "F9.6", "F8",   "F6.8", "F5.6", "F4.8",
					     "F4",  "F3.4", "F2.8", "F2.4", "F2",   "F1.8"};

/* The gain of each gain position from 0x01 to 0x0f, in dB. A gain limit is
 * one of these too, from 0x04 up. */
static const int gain_db[] = {0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 43};

/* The focus near limit, whose steps are 0x1000 (far) to 0xe000 (near), and
 * the distance each is */
static const char *const near_limits[] = {nullptr, "10 m", "5 m",   "3.3 m", "2.5 m", "2 m",  "1.7 m",
					  "1.5 m", "1 m",  "50 cm", "30 cm", "15 cm", "6 cm", "1 cm"};

/* The video formats a camera can be switched to, by the value VISCA has for
 * each. From 0x08 on they are 50 Hz ones. */
static const std::pair<int, const char *> video_formats[] = {
	{0x00, "1080p/59.94"}, {0x02, "1080p/29.97"}, {0x03, "1080i/59.94"}, {0x04, "720p/59.94"}, {0x05, "720p/29.97"},
	{0x08, "1080p/50"},    {0x0a, "1080p/25"},    {0x0b, "1080i/50"},    {0x0c, "720p/50"},    {0x0d, "720p/25"},
};

static QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QString withSign(int value)
{
	return value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value);
}

/* How a read-only value that is one of a few is shown: by its name, or as its
 * number if it isn't one of them */
static std::function<QString(obs_data_t *)> named(const char *key, QList<std::pair<int, const char *>> names)
{
	return [key, names](obs_data_t *data) {
		int value = (int)obs_data_get_int(data, key);
		for (const auto &[at, name] : names)
			if (at == value)
				return text(name);
		return QString::number(value);
	};
}

/* A name and its number, for the camera's vendor and model: the name alone
 * says nothing for one the plugin doesn't know */
static std::function<QString(obs_data_t *)> namedId(const char *nameKey, const char *idKey)
{
	return [nameKey, idKey](obs_data_t *data) {
		return QStringLiteral("%1 (0x%2)")
			.arg(QString::fromUtf8(obs_data_get_string(data, nameKey)))
			.arg(obs_data_get_int(data, idKey), 4, 16, QChar('0'));
	};
}

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
	m_connected = makeIndicator("PTZ.Device.Status.Connected");
	top->addRow(m_connected);

	m_power = new QCheckBox(obs_module_text("PTZ.Device.State.Power"));
	m_focusAuto = new QCheckBox(obs_module_text("PTZ.Device.State.Autofocus"));
	m_tally = new QCheckBox(obs_module_text("PTZ.Device.State.Tally"));
	m_tallyPreview = new QCheckBox(obs_module_text("PTZ.Device.State.TallyPreview"));
	top->addRow(m_power);
	top->addRow(m_focusAuto);
	top->addRow(m_tally);
	top->addRow(m_tallyPreview);
	m_power->hide();
	m_focusAuto->hide();
	m_tally->hide();
	m_tallyPreview->hide();
	page->addLayout(top);

	/* The rest of what a camera reports, in rows made from these. A number
	 * or list the user edits is asked of the camera; a text field, or an
	 * indicator, can't be edited. */
	auto flag = [this](QFormLayout *form, const char *key, const char *label) {
		return addField(form, key, FlagField, label);
	};
	auto indicator = [this](QFormLayout *form, const char *key, const char *label) {
		Field *field = addField(form, key, FlagField, label);
		field->widget->setAttribute(Qt::WA_TransparentForMouseEvents);
		field->widget->setFocusPolicy(Qt::NoFocus);
		return field;
	};
	auto choice = [this](QFormLayout *form, const char *key, const char *label,
			     const QList<std::pair<int, QString>> &items) {
		Field *field = addField(form, key, ChoiceField, label);
		auto combo = static_cast<QComboBox *>(field->widget);
		for (const auto &[value, name] : items)
			combo->addItem(name, value);
		combo->setCurrentIndex(-1);
		return field;
	};
	auto onOff = [this](QFormLayout *form, const char *key, const char *label, const char *on, const char *off) {
		Field *field = addField(form, key, ChoiceField, label);
		field->boolValue = true;
		auto combo = static_cast<QComboBox *>(field->widget);
		combo->addItem(text(on), true);
		combo->addItem(text(off), false);
		combo->setCurrentIndex(-1);
		return field;
	};
	auto number = [this](QFormLayout *form, const char *key, const char *label, int max,
			     const QString &suffix = QString()) {
		Field *field = addField(form, key, NumberField, label);
		auto spin = static_cast<QSpinBox *>(field->widget);
		spin->setRange(0, max);
		spin->setSuffix(suffix);
		return field;
	};
	auto readOnly = [this](QFormLayout *form, const char *key, const char *label,
			       std::function<QString(obs_data_t *)> format) {
		Field *field = addField(form, key, TextField, label);
		field->format = std::move(format);
		return field;
	};
	auto shown = [](const char *key) {
		return [key](obs_data_t *data) {
			return QString::number(obs_data_get_int(data, key));
		};
	};
	auto upTo = [](int last, const QString &zero = QString()) {
		QList<std::pair<int, QString>> items;
		for (int value = 0; value <= last; value++)
			items.append({value, value == 0 && !zero.isEmpty() ? zero : QString::number(value)});
		return items;
	};

	QFormLayout *focus = addGroup(page, "PTZ.Device.State.FocusGroup");
	choice(focus, "focus_af_mode", "PTZ.Device.State.AFMode",
	       {{0, text("PTZ.Device.State.AFMode.Normal")},
		{1, text("PTZ.Device.State.AFMode.Interval")},
		{2, text("PTZ.Device.State.AFMode.ZoomTrigger")}});
	onOff(focus, "focus_af_sensitivity", "PTZ.Device.State.AFSensitivity", "PTZ.Device.State.Normal",
	      "PTZ.Device.State.Low");
	number(focus, "focus_af_move_time", "PTZ.Device.State.AFMoveTime", 0xff, QStringLiteral(" s"));
	number(focus, "focus_af_interval_time", "PTZ.Device.State.AFIntervalTime", 0xff, QStringLiteral(" s"));
	QList<std::pair<int, QString>> limits;
	for (int step = 1; step <= 0xe; step++)
		limits.append({step << 12, step == 1 ? text("PTZ.Device.State.FocusNearLimit.OverInfinity")
						     : QString::fromUtf8(near_limits[step - 1])});
	choice(focus, "focus_near_limit", "PTZ.Device.State.FocusNearLimit", limits)->stepped = true;
	choice(focus, "ir_correction", "PTZ.Device.State.IRCorrection",
	       {{0, text("PTZ.Device.State.Standard")}, {1, text("PTZ.Device.State.IRCorrection.IRLight")}});
	flag(focus, "dzoom_on", "PTZ.Device.State.DZoom");
	readOnly(focus, "dzoom_pos", "PTZ.Device.State.DZoomPos", shown("dzoom_pos"));
	indicator(focus, "low_contrast", "PTZ.Device.State.LowContrast");

	QFormLayout *exposure = addGroup(page, "PTZ.Device.State.Exposure");
	choice(exposure, "ae_mode", "PTZ.Device.State.AEMode",
	       {{0x0, text("PTZ.Device.State.AEMode.FullAuto")},
		{0x3, text("PTZ.Device.State.AEMode.Manual")},
		{0xa, text("PTZ.Device.State.AEMode.ShutterPriority")},
		{0xb, text("PTZ.Device.State.AEMode.IrisPriority")},
		{0xd, text("PTZ.Device.State.AEMode.Bright")}});
	flag(exposure, "slow_shutter", "PTZ.Device.State.SlowShutter");
	choice(exposure, "shutter_pos", "PTZ.Device.State.Shutter", upTo(0x15)); // named by showShutterSpeeds()
	QList<std::pair<int, QString>> irises = {{0, text("PTZ.Device.State.Iris.Closed")}};
	for (int at = 0x05; at <= 0x11; at++)
		irises.append({at, QString::fromUtf8(iris_f_numbers[at - 0x05])});
	choice(exposure, "iris_pos", "PTZ.Device.State.Iris", irises);
	QList<std::pair<int, QString>> gains, gainLimits;
	for (int at = 0x01; at <= 0x0f; at++) {
		QString db = QStringLiteral("%1 dB").arg(withSign(gain_db[at - 1]));
		gains.append({at, db});
		if (at >= 0x04)
			gainLimits.append({at, db});
	}
	choice(exposure, "gain_pos", "PTZ.Device.State.Gain", gains);
	choice(exposure, "gain_limit", "PTZ.Device.State.GainLimit", gainLimits);
	/* The bright mode's steps open the iris, then add gain */
	QList<std::pair<int, QString>> brights = {{0, text("PTZ.Device.State.Iris.Closed")}};
	for (int at = 0x05; at <= 0x1f; at++) {
		QString iris = QString::fromUtf8(iris_f_numbers[std::min(at, 0x11) - 0x05]);
		brights.append(
			{at, at <= 0x11 ? iris : QStringLiteral("%1 %2 dB").arg(iris, withSign(gain_db[at - 0x11]))});
	}
	choice(exposure, "bright_pos", "PTZ.Device.State.Bright", brights);
	flag(exposure, "exposure_comp", "PTZ.Device.State.ExposureComp");
	QList<std::pair<int, QString>> compensations;
	for (int at = 0; at <= 0xe; at++)
		compensations.append(
			{at, QStringLiteral("%1 (%2 dB)").arg(withSign(at - 7)).arg((at - 7) * 1.5, 0, 'f', 1)});
	choice(exposure, "exposure_comp_pos", "PTZ.Device.State.ExposureCompLevel", compensations);
	flag(exposure, "back_light", "PTZ.Device.State.BackLight");
	choice(exposure, "wd_mode", "PTZ.Device.State.WD",
	       {{0, text("PTZ.Device.State.Off")},
		{1, text("PTZ.Device.State.Low")},
		{2, text("PTZ.Device.State.Mid")},
		{3, text("PTZ.Device.State.High")}});
	flag(exposure, "defog_mode", "PTZ.Device.State.Defog");
	flag(exposure, "high_sensitivity", "PTZ.Device.State.HighSensitivity");

	m_whiteBalanceGroup = new QGroupBox(obs_module_text("PTZ.WhiteBalance"));
	auto whiteBalance = new QFormLayout(m_whiteBalanceGroup);
	m_whiteBalanceMode = new QComboBox();
	m_whiteBalanceMode->setObjectName("wb_mode");
	for (int mode = 0; mode < (int)(sizeof(white_balance_modes) / sizeof(*white_balance_modes)); mode++)
		m_whiteBalanceMode->addItem(obs_module_text(white_balance_modes[mode]), mode);
	m_whiteBalanceMode->setCurrentIndex(-1);
	whiteBalance->addRow(new QLabel(obs_module_text("PTZ.WhiteBalance.Mode")), m_whiteBalanceMode);
	m_onePush = new QPushButton(obs_module_text("PTZ.WhiteBalance.OnePushButton"));
	whiteBalance->addRow(m_onePush);
	/* Only taken in the manual mode */
	number(whiteBalance, "r_gain", "PTZ.Device.State.RGain", 0xff);
	number(whiteBalance, "b_gain", "PTZ.Device.State.BGain", 0xff);
	m_whiteBalanceGroup->hide();
	page->addWidget(m_whiteBalanceGroup);
	m_groups.push_back(m_whiteBalanceGroup);

	QFormLayout *picture = addGroup(page, "PTZ.Device.State.Picture");
	number(picture, "aperture_gain", "PTZ.Device.State.Aperture", 0xf);
	flag(picture, "high_resolution", "PTZ.Device.State.HighResolution");
	choice(picture, "nr_level", "PTZ.Device.State.NR", upTo(5, text("PTZ.Device.State.Off")));
	choice(picture, "gamma", "PTZ.Device.State.Gamma",
	       {{0, text("PTZ.Device.State.Standard")}, {1, text("PTZ.Device.State.Off")}});
	choice(picture, "chroma_suppress", "PTZ.Device.State.ChromaSuppress", upTo(3, text("PTZ.Device.State.Off")));
	QList<std::pair<int, QString>> colorGains, colorHues;
	for (int at = 0; at <= 0xe; at++) {
		colorGains.append({at, QStringLiteral("%1%").arg(60 + at * 10)});
		colorHues.append({at, QStringLiteral("%1°").arg(withSign(at * 2 - 14))});
	}
	choice(picture, "color_gain", "PTZ.Device.State.ColorGain", colorGains);
	choice(picture, "color_hue", "PTZ.Device.State.ColorHue", colorHues);
	choice(picture, "picture_effect", "PTZ.Device.State.PictureEffect",
	       {{0, text("PTZ.Device.State.Off")},
		{2, text("PTZ.Device.State.PictureEffect.NegArt")},
		{4, text("PTZ.Device.State.PictureEffect.BW")}});

	QFormLayout *system = addGroup(page, "PTZ.Device.State.System");
	readOnly(system, "vendor_name", "PTZ.Device.State.Vendor", namedId("vendor_name", "vendor_id"));
	readOnly(system, "model_name", "PTZ.Device.State.Model", namedId("model_name", "model_id"));
	readOnly(system, "rom_version", "PTZ.Device.State.ROMVersion", [](obs_data_t *data) {
		return QStringLiteral("%1").arg(obs_data_get_int(data, "rom_version"), 4, 16, QChar('0'));
	});
	readOnly(system, "socket_number", "PTZ.Device.State.Sockets", shown("socket_number"));
	auto cameraId =
		static_cast<QSpinBox *>(number(system, "camera_id", "PTZ.Device.State.CameraID", 0xffff)->widget);
	cameraId->setDisplayIntegerBase(16);
	cameraId->setPrefix(QStringLiteral("0x"));
	QList<std::pair<int, QString>> formats;
	for (const auto &[value, name] : video_formats)
		formats.append({value, QString::fromUtf8(name)});
	choice(system, "video_format", "PTZ.Device.State.VideoFormat", formats);
	indicator(system, "video_50hz", "PTZ.Device.State.Video50Hz");
	choice(system, "color_system", "PTZ.Device.State.ColorSystem",
	       {{0, QStringLiteral("HDMI YUV")},
		{1, QStringLiteral("HDMI GBR")},
		{2, QStringLiteral("DVI GBR")},
		{3, QStringLiteral("DVI YUV")}});
	flag(system, "low_latency", "PTZ.Device.State.LowLatency");
	flag(system, "info_display", "PTZ.Device.State.InfoDisplay");
	flag(system, "menu_on", "PTZ.Device.State.Menu")->offOnly = true;
	flag(system, "ir_receive", "PTZ.Device.State.IRReceive");
	readOnly(system, "ir_condition", "PTZ.Device.State.IRCondition",
		 named("ir_condition", {{0, "PTZ.Device.State.IRCondition.Stable"},
					{1, "PTZ.Device.State.IRCondition.Unstable"},
					{2, "PTZ.Device.State.IRCondition.Unknown"}}));

	QFormLayout *motion = addGroup(page, "PTZ.Device.State.Motion");
	readOnly(motion, "pantilt_init_status", "PTZ.Device.State.PanTiltInit",
		 named("pantilt_init_status", {{0, "PTZ.Device.State.PanTiltInit.None"},
					       {1, "PTZ.Device.State.PanTiltInit.Running"},
					       {2, "PTZ.Device.State.Done"},
					       {3, "PTZ.Device.State.Failed"}}));
	readOnly(motion, "pantilt_move_status", "PTZ.Device.State.PanTiltMove",
		 named("pantilt_move_status", {{0, "PTZ.Device.State.PanTiltMove.None"},
					       {1, "PTZ.Device.State.PanTiltMove.Running"},
					       {2, "PTZ.Device.State.Done"},
					       {3, "PTZ.Device.State.Failed"}}));
	indicator(motion, "pantilt_pan_error", "PTZ.Device.State.PanError");
	indicator(motion, "pantilt_tilt_error", "PTZ.Device.State.TiltError");
	indicator(motion, "pantilt_at_left_limit", "PTZ.Device.State.AtLeftLimit");
	indicator(motion, "pantilt_at_right_limit", "PTZ.Device.State.AtRightLimit");
	indicator(motion, "pantilt_at_upper_limit", "PTZ.Device.State.AtUpperLimit");
	indicator(motion, "pantilt_at_lower_limit", "PTZ.Device.State.AtLowerLimit");
	indicator(motion, "zoom_command_running", "PTZ.Device.State.Zooming");
	indicator(motion, "focus_command_running", "PTZ.Device.State.Focusing");
	indicator(motion, "memory_recall_running", "PTZ.Device.State.RecallingPreset");
	readOnly(motion, "pan_max_speed", "PTZ.Device.State.PanMaxSpeed", shown("pan_max_speed"));
	readOnly(motion, "tilt_max_speed", "PTZ.Device.State.TiltMaxSpeed", shown("tilt_max_speed"));
	showShutterSpeeds();

	m_diagnosticsGroup = new QGroupBox(obs_module_text("PTZ.Device.State.Diagnostics"));
	m_diagnosticsGroup->setObjectName("diagnostics");
	auto diagnostics = new QVBoxLayout(m_diagnosticsGroup);
	auto statistics = new QGridLayout();
	auto addHeader = [statistics](const char *text, int column) {
		auto header = new QLabel(QString::fromUtf8(obs_module_text(text)));
		header->setAlignment(Qt::AlignRight);
		statistics->addWidget(header, 0, column);
	};
	addHeader("PTZ.Device.State.StatisticTotal", 1);
	addHeader("PTZ.Device.State.StatisticRate", 2);
	m_statisticRows = {
		{"polls", "PTZ.Device.State.PollCount", "visca_poll_count", "visca_polls_per_second", "%.2f /s"},
		{"sent_packets", "PTZ.Device.State.SentPackets", "visca_sent_count", "visca_sent_packets_per_second",
		 "%.1f /s"},
		{"recv_packets", "PTZ.Device.State.RecvPackets", "visca_recv_count", "visca_recv_packets_per_second",
		 "%.1f /s"},
		{"sent_bytes", "PTZ.Device.State.SentBytes", "visca_sent_bytes", "visca_sent_bytes_per_second",
		 "%.0f B/s"},
		{"recv_bytes", "PTZ.Device.State.RecvBytes", "visca_recv_bytes", "visca_recv_bytes_per_second",
		 "%.0f B/s"},
		{"errors", "PTZ.Device.State.Errors", "visca_error_count", "visca_errors_per_second", "%.2f /s"},
	};
	int statisticRow = 1;
	for (auto &row : m_statisticRows) {
		auto cell = [&](const QString &objectName, int column) {
			auto value = new QLabel(QStringLiteral("-"));
			value->setObjectName(objectName);
			value->setAlignment(Qt::AlignRight);
			value->setTextInteractionFlags(Qt::TextSelectableByMouse);
			statistics->addWidget(value, statisticRow, column);
			return value;
		};
		statistics->addWidget(new QLabel(QString::fromUtf8(obs_module_text(row.label))), statisticRow, 0);
		row.totalLabel = cell(QString::fromUtf8(row.name) + "Total", 1);
		row.rateLabel = cell(QString::fromUtf8(row.name) + "Rate", 2);
		statisticRow++;
	}
	statistics->setColumnStretch(0, 1);
	diagnostics->addLayout(statistics);
	auto pollTime = new QFormLayout();
	m_pollCycle = new QLabel(QStringLiteral("-"));
	m_pollCycle->setObjectName("pollCycle");
	m_pollCycle->setTextInteractionFlags(Qt::TextSelectableByMouse);
	pollTime->addRow(QString::fromUtf8(obs_module_text("PTZ.Device.State.PollCycle")), m_pollCycle);
	diagnostics->addLayout(pollTime);
	auto addAction = [this, diagnostics](const char *name, const char *text, const char *trigger) {
		auto button = new QPushButton(obs_module_text(text));
		button->setObjectName(name);
		diagnostics->addWidget(button);
		connect(button, &QPushButton::clicked, this, [this, trigger]() { emit actionRequested(trigger); });
	};
	auto report = new QPushButton(obs_module_text("PTZ.CameraReport.Create"));
	report->setObjectName("cameraReport");
	diagnostics->addWidget(report);
	connect(report, &QPushButton::clicked, this, &PTZStateView::cameraReportRequested);
	addAction("discoverLimits", "PTZ.Visca.Debug.DiscoverLimits", "discover_limits");
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
	connect(m_tally, &QCheckBox::clicked, this, [this](bool checked) {
		OBSDataAutoRelease requested = obs_data_create();
		obs_data_set_bool(requested, "tally_on", checked);
		emit stateRequested(OBSData(requested.Get()));
	});
	connect(m_tallyPreview, &QCheckBox::clicked, this, [this](bool checked) {
		OBSDataAutoRelease requested = obs_data_create();
		obs_data_set_bool(requested, "tally_preview", checked);
		emit stateRequested(OBSData(requested.Get()));
	});

	/* The list follows the camera, not the user: if the camera hasn't taken
	 * up what was asked for by now, show what it says it is doing */
	m_whiteBalanceSettle.setSingleShot(true);
	m_whiteBalanceSettle.setInterval(1500);
	connect(&m_whiteBalanceSettle, &QTimer::timeout, this, [this]() { showWhiteBalance(m_reportedWhiteBalance); });
}

PTZStateView::~PTZStateView() = default;

/* Both halves of a form row go together: hiding just the field would leave
 * its label behind */
void PTZStateView::setRowVisible(QWidget *label, QWidget *field, bool visible)
{
	label->setVisible(visible);
	field->setVisible(visible);
}

/* A group of fields, hidden until one of them is shown */
QFormLayout *PTZStateView::addGroup(QVBoxLayout *page, const char *title)
{
	auto group = new QGroupBox(obs_module_text(title));
	auto form = new QFormLayout(group);
	group->hide();
	page->addWidget(group);
	m_groups.push_back(group);
	return form;
}

/* A row for one state key, named by its key so that it can be found. Hidden
 * until the device reports the key. What the user does with it is asked of
 * the device: activated(), clicked() and a spin box's valueChanged() are only
 * the user, since the view sets a spin box with its signals blocked. */
PTZStateView::Field *PTZStateView::addField(QFormLayout *form, const char *key, FieldKind kind, const char *label)
{
	auto owned = std::make_unique<Field>();
	Field *field = owned.get();
	field->key = key;
	field->kind = kind;
	field->group = qobject_cast<QGroupBox *>(form->parentWidget());
	switch (kind) {
	case FlagField: {
		auto box = new QCheckBox(obs_module_text(label));
		field->boolValue = true;
		connect(box, &QCheckBox::clicked, this, [this, field](bool checked) { requestField(field, checked); });
		field->widget = box;
		break;
	}
	case ChoiceField: {
		auto combo = new QComboBox();
		connect(combo, &QComboBox::activated, this,
			[this, field, combo](int index) { requestField(field, combo->itemData(index)); });
		field->widget = combo;
		break;
	}
	case NumberField: {
		auto spin = new QSpinBox();
		/* Asked for once it has been typed in, not at every digit */
		spin->setKeyboardTracking(false);
		connect(spin, &QSpinBox::valueChanged, this, [this, field](int value) { requestField(field, value); });
		field->widget = spin;
		break;
	}
	case TextField: {
		auto value = new QLabel();
		value->setTextInteractionFlags(Qt::TextSelectableByMouse);
		field->widget = value;
		break;
	}
	}
	field->widget->setObjectName(key);
	if (kind == FlagField) {
		form->addRow(field->widget);
		field->widget->hide();
	} else {
		field->label = new QLabel(obs_module_text(label));
		form->addRow(field->label, field->widget);
		setRowVisible(field->label, field->widget, false);
	}
	field->settle.setSingleShot(true);
	field->settle.setInterval(1500);
	connect(&field->settle, &QTimer::timeout, this, [this, field]() { showField(field, field->reported); });
	m_fields.push_back(std::move(owned));
	return field;
}

void PTZStateView::requestField(Field *field, const QVariant &value)
{
	OBSDataAutoRelease requested = obs_data_create();
	if (field->boolValue)
		obs_data_set_bool(requested, field->key, value.toBool());
	else
		obs_data_set_int(requested, field->key, value.toInt());
	emit stateRequested(OBSData(requested.Get()));
	field->settle.start();
}

/* Shows `value` in the field's widget; says whether that changed it */
bool PTZStateView::showField(Field *field, const QVariant &value)
{
	if (!value.isValid())
		return false;
	switch (field->kind) {
	case FlagField: {
		auto box = static_cast<QCheckBox *>(field->widget);
		bool on = value.toBool(), changed = false;
		if (field->offOnly && box->isEnabled() != on) {
			box->setEnabled(on);
			changed = true;
		}
		if (box->isChecked() != on) {
			box->setChecked(on);
			changed = true;
		}
		return changed;
	}
	case ChoiceField: {
		auto combo = static_cast<QComboBox *>(field->widget);
		int at = combo->findData(value);
		for (int i = 0; at < 0 && field->stepped && i < combo->count(); i++)
			if (combo->itemData(i).toInt() <= value.toInt() &&
			    (i + 1 == combo->count() || combo->itemData(i + 1).toInt() > value.toInt()))
				at = i;
		if (combo->currentIndex() == at)
			return false;
		combo->setCurrentIndex(at);
		return true;
	}
	case NumberField: {
		auto spin = static_cast<QSpinBox *>(field->widget);
		if (spin->value() == value.toInt())
			return false;
		const QSignalBlocker blocker(spin);
		spin->setValue(value.toInt());
		return true;
	}
	case TextField: {
		auto text = static_cast<QLabel *>(field->widget);
		if (text->text() == value.toString())
			return false;
		text->setText(value.toString());
		return true;
	}
	}
	return false;
}

/* The shutter speeds are named for the video format's frequency */
void PTZStateView::showShutterSpeeds()
{
	for (const auto &field : m_fields) {
		if (strcmp(field->key, "shutter_pos") != 0)
			continue;
		auto combo = static_cast<QComboBox *>(field->widget);
		for (int i = 0; i < combo->count(); i++)
			combo->setItemText(i, QString::fromUtf8(shutter_speeds[m_50Hz][combo->itemData(i).toInt()]));
	}
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
	auto setFlag = [&](QCheckBox *box, const char *key) {
		if (!all && !has(key))
			return;
		bool value = obs_data_get_bool(data, key);
		if (box->isChecked() != value) {
			box->setChecked(value);
			changed = true;
		}
	};

	setFlag(m_connected, "connected");

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
	setCommandableFlag(m_tally, "tally_on");
	setCommandableFlag(m_tallyPreview, "tally_preview");

	for (int i = 0; i < AxisCount; i++) {
		const char *key = m_axisKeys[i];
		if (has(key)) {
			double value = QString::asprintf("%.3f", obs_data_get_double(data, key)).toDouble();
			if (m_axis[i] != value) {
				m_axis[i] = value;
				changed = true;
			}
		} else if (all && m_axis[i]) {
			m_axis[i].reset();
			changed = true;
		}
	}

	if (has("wb_mode")) {
		int mode = (int)obs_data_get_int(data, "wb_mode");
		if (m_reportedWhiteBalance != mode) {
			m_reportedWhiteBalance = mode;
			showWhiteBalance(mode);
			changed = true;
		}
	} else if (all && m_reportedWhiteBalance >= 0) {
		m_reportedWhiteBalance = -1;
		changed = true;
	}

	/* A field's widget is set when what the device reports changes, not
	 * whenever it is reported: the user may have just picked something
	 * else, which is left until the device has done it or the field
	 * settles back */
	for (const auto &owned : m_fields) {
		Field *field = owned.get();
		bool shown = !field->widget->isHidden();
		if (has(field->key)) {
			QVariant value = field->kind == TextField ? QVariant(field->format(data))
					 : field->boolValue       ? QVariant(obs_data_get_bool(data, field->key))
								  : QVariant((int)obs_data_get_int(data, field->key));
			if (value != field->reported) {
				field->reported = value;
				changed |= showField(field, value);
			}
			if (!shown) {
				if (field->label)
					setRowVisible(field->label, field->widget, true);
				else
					field->widget->show();
				changed = true;
			}
		} else if (all && shown) {
			field->reported = QVariant();
			if (field->label)
				setRowVisible(field->label, field->widget, false);
			else
				field->widget->hide();
			changed = true;
		}
	}

	bool is50Hz = m_50Hz;
	if (has("video_format"))
		is50Hz = obs_data_get_int(data, "video_format") >= 0x08;
	else if (has("video_50hz"))
		is50Hz = obs_data_get_bool(data, "video_50hz");
	if (is50Hz != m_50Hz) {
		m_50Hz = is50Hz;
		showShutterSpeeds();
		changed = true;
	}

	for (QGroupBox *group : m_groups) {
		bool any = group == m_whiteBalanceGroup && m_reportedWhiteBalance >= 0;
		for (const auto &field : m_fields)
			any |= field->group == group && !field->widget->isHidden();
		if (any == group->isHidden()) {
			group->setVisible(any);
			changed = true;
		}
	}

	if (all || has("features")) {
		OBSDataAutoRelease features = obs_data_get_obj(data, "features");
		bool diagnostics = features && obs_data_get_bool(features, "diagnostics");
		if (diagnostics == m_diagnosticsGroup->isHidden()) {
			m_diagnosticsGroup->setVisible(diagnostics);
			changed = true;
			emit diagnosticsAvailableChanged(diagnostics);
		}
	}

	if (changed) {
		m_updateCount++;
		emit shownChanged();
	}
}

/* How the camera and the traffic to and from it are doing, read from the
 * device when wanted rather than told. Not counted as the view changing: they
 * change all the time, whatever else does. */
void PTZStateView::setStatistics(OBSData stats)
{
	auto has = [&stats](const char *key) {
		return obs_data_has_user_value(stats, key);
	};
	m_pollCycle->setText(has("visca_poll_cycle_ms")
				     ? QString::asprintf("%.0f ms", obs_data_get_double(stats, "visca_poll_cycle_ms"))
				     : QStringLiteral("-"));
	for (const auto &row : m_statisticRows) {
		/* a count that has never been counted isn't there, once there is
		 * a rate of it: it is none */
		row.totalLabel->setText(has(row.total)
						? QLocale().toString((qlonglong)obs_data_get_int(stats, row.total))
					: has(row.rate) ? QLocale().toString(0)
							: QStringLiteral("-"));
		row.rateLabel->setText(has(row.rate)
					       ? QString::asprintf(row.rateFormat, obs_data_get_double(stats, row.rate))
					       : QStringLiteral("-"));
	}
}

QVariantMap PTZStateView::shownValues() const
{
	QVariantMap shown;
	shown["connected"] = m_connected->isChecked();
	if (!m_power->isHidden())
		shown["power_on"] = m_power->isChecked();
	if (!m_focusAuto->isHidden())
		shown["focus_af_enabled"] = m_focusAuto->isChecked();
	if (!m_tally->isHidden())
		shown["tally_on"] = m_tally->isChecked();
	if (!m_tallyPreview->isHidden())
		shown["tally_preview"] = m_tallyPreview->isChecked();
	for (int i = 0; i < AxisCount; i++)
		if (m_axis[i])
			shown[m_axisKeys[i]] = *m_axis[i];
	if (m_reportedWhiteBalance >= 0)
		shown["wb_mode"] = m_whiteBalanceMode->currentData().toInt();
	for (const auto &field : m_fields) {
		if (field->widget->isHidden())
			continue;
		switch (field->kind) {
		case FlagField:
			shown[field->key] = static_cast<QCheckBox *>(field->widget)->isChecked();
			break;
		case ChoiceField:
			shown[field->key] = static_cast<QComboBox *>(field->widget)->currentData();
			break;
		case NumberField:
			shown[field->key] = static_cast<QSpinBox *>(field->widget)->value();
			break;
		case TextField:
			shown[field->key] = static_cast<QLabel *>(field->widget)->text();
			break;
		}
	}
	if (!m_diagnosticsGroup->isHidden()) {
		shown["poll_cycle"] = m_pollCycle->text();
		for (const auto &row : m_statisticRows) {
			shown[QString::fromUtf8(row.name) + "_total"] = row.totalLabel->text();
			shown[QString::fromUtf8(row.name) + "_rate"] = row.rateLabel->text();
		}
	}
	return shown;
}
