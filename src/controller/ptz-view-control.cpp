/* PTZ view control: a source's picture, that moves its camera
 *
 * SPDX-License-Identifier: GPLv2
 */
#include <algorithm>
#include <cmath>
#include <optional>
#include <obs-module.h>
#include <obs.hpp>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include "ptz-view-control.hpp"
#include "display-helpers.hpp"
#include "ptz-controls.hpp"
#include "ptz-list-model.hpp"

/* How often moves are sent, and how much one notch of the wheel zooms */
static constexpr int tickMs = 100;
static constexpr double zoomPerNotch = 0.1;
/* After this long without the wheel, zoom from where the camera says it is
 * rather than from where it was last sent */
static constexpr qint64 zoomSettleMs = 1000;

PTZViewControl::PTZViewControl(QWidget *parent) : PTZSourceDisplay(parent)
{
	m_pixelRatio = (float)devicePixelRatioF();
	/* Moves without a button down too, for the cursor readout */
	setMouseTracking(true);
	connect(&m_timer, &QTimer::timeout, this, &PTZViewControl::tick);
	m_timer.start(tickMs);

	/* The display is made, and made again on another screen, by
	 * OBSQTDisplay. PTZSourceDisplay says to draw the source into each as it
	 * is made, before this does, so this draws after it, over the source. */
	connect(this, &OBSQTDisplay::DisplayCreated, this,
		[this]() { obs_display_add_draw_callback(GetDisplay(), drawCallback, this); });
}

PTZViewControl::~PTZViewControl()
{
	/* The display goes in PTZSourceDisplay's destructor, after this class's
	 * members, which the draw callback reads, have gone: stop it drawing
	 * first. Taking the callback away waits for a draw in progress. */
	if (obs_display_t *display = GetDisplay())
		obs_display_remove_draw_callback(display, drawCallback, this);
	obs_enter_graphics();
	for (TextOverlay *overlay : {&m_positionReadout, &m_cursorReadout}) {
		if (overlay->texture)
			gs_texture_destroy(overlay->texture);
	}
	obs_leave_graphics();
}

/* Opaque white, as 0xAARRGGBB */
static constexpr uint32_t overlayColor = 0xffffffff;

/* A rectangle along the edges of where the source is drawn, a circle 4
 * pixels across in its middle, lines from the circle at 45 degrees to the
 * rectangle's sides, and short ones from it straight up, down, left and
 * right. In display pixels. */
static void drawOverlay(uint32_t cx, uint32_t cy, int x, int y, int width, int height, float pixelRatio)
{
	constexpr float pi = 3.14159265358979f;
	constexpr int circleSegments = 16;
	const float radius = 4.0f * pixelRatio;
	/* Lines on the middle of the edge pixels, so all of them shows */
	const float left = x + 0.5f, top = y + 0.5f;
	const float right = x + width - 0.5f, bottom = y + height - 0.5f;
	const float x0 = x + width / 2.0f, y0 = y + height / 2.0f;

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(0, 0, cx, cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), overlayColor);
	/* OBS's own vertex buffer: one made here would be left loaded once freed */
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(left, top);
		gs_vertex2f(right, top);
		gs_vertex2f(right, bottom);
		gs_vertex2f(left, bottom);
		gs_vertex2f(left, top);
		gs_render_stop(GS_LINESTRIP);

		gs_render_start(false);
		for (int i = 0; i <= circleSegments; i++) {
			float angle = 2.0f * pi * i / circleSegments;
			gs_vertex2f(x0 + radius * cosf(angle), y0 + radius * sinf(angle));
		}
		gs_render_stop(GS_LINESTRIP);

		/* As many pixels across as down, to the nearer pair of sides:
		 * the source isn't square */
		const float start = radius / sqrtf(2.0f);
		const float reach = std::min(right - x0, bottom - y0);
		gs_render_start(false);
		for (float dx : {1.0f, -1.0f}) {
			for (float dy : {1.0f, -1.0f}) {
				gs_vertex2f(x0 + start * dx, y0 + start * dy);
				gs_vertex2f(x0 + reach * dx, y0 + reach * dy);
			}
		}
		gs_render_stop(GS_LINES);

		/* 3 pixels long, from the circle's edge */
		const float tick = radius + 5.0f * pixelRatio;
		gs_render_start(false);
		gs_vertex2f(x0, y0 - radius);
		gs_vertex2f(x0, y0 - tick);
		gs_vertex2f(x0, y0 + radius);
		gs_vertex2f(x0, y0 + tick);
		gs_vertex2f(x0 - radius, y0);
		gs_vertex2f(x0 - tick, y0);
		gs_vertex2f(x0 + radius, y0);
		gs_vertex2f(x0 + tick, y0);
		gs_render_stop(GS_LINES);
	}

	gs_blend_state_pop();
	gs_viewport_pop();
	gs_projection_pop();
}

/* A readout, centered on the picture across, at `top`. In display pixels. */
static void drawText(gs_texture_t *text, uint32_t cx, uint32_t cy, int x, int width, int top)
{
	uint32_t textCx = gs_texture_get_width(text);
	uint32_t textCy = gs_texture_get_height(text);
	int left = x + (width - (int)textCx) / 2;

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(0, 0, cx, cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f((float)left, (float)top, 0.0f);

	/* gs_draw_sprite() draws with OBS's own vertex buffer, which is never freed */
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), text);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(text, 0, textCx, textCy);

	gs_matrix_pop();
	gs_blend_state_pop();
	gs_viewport_pop();
	gs_projection_pop();
}

/* Runs on the graphics thread, after PTZSourceDisplay has drawn the source */
void PTZViewControl::drawCallback(void *param, uint32_t cx, uint32_t cy)
{
	auto view = static_cast<PTZViewControl *>(param);
	OBSSource source = view->source();
	if (!source || !cx || !cy)
		return;

	uint32_t sourceCx = obs_source_get_width(source);
	uint32_t sourceCy = obs_source_get_height(source);
	if (!sourceCx || !sourceCy)
		return;

	/* Where PTZSourceDisplay drew the source, worked out as it does */
	int x, y;
	float scale;
	GetScaleAndCenterPos(sourceCx, sourceCy, cx, cy, x, y, scale);
	int width = int(scale * float(sourceCx));
	int height = int(scale * float(sourceCy));

	drawOverlay(cx, cy, x, y, width, height, view->m_pixelRatio);
	/* Inside the picture: the cursor just below its top edge and the
	 * position just above its bottom edge */
	const int gap = (int)(2 * view->m_pixelRatio);
	if (gs_texture_t *text = view->m_positionReadout.texture)
		drawText(text, cx, cy, x, width, y + height - gap - (int)gs_texture_get_height(text));
	if (gs_texture_t *text = view->m_cursorReadout.texture)
		drawText(text, cx, cy, x, width, y + gap);
}

QRectF PTZViewControl::sourceRect() const
{
	OBSSource source = this->source();
	if (!source)
		return QRectF();
	double sourceCx = obs_source_get_width(source);
	double sourceCy = obs_source_get_height(source);
	if (!sourceCx || !sourceCy)
		return QRectF();
	double scale = std::min(width() / sourceCx, height() / sourceCy);
	QSizeF size(sourceCx * scale, sourceCy * scale);
	return QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}

/* A device is named after its source. Looked up each time, so that one made
 * after setSource(), or a renamed source, is still found. */
QModelIndex PTZViewControl::device() const
{
	OBSSource source = this->source();
	if (!source || !ptzDeviceList)
		return QModelIndex();
	return ptzDeviceList->indexFromName(QString::fromUtf8(obs_source_get_name(source)));
}

/* The dock's "block moves of live cameras" applies here too */
bool PTZViewControl::liveLocked(const QModelIndex &index) const
{
	auto controls = PTZControls::getInstance();
	return controls && controls->liveMoveLockActive() && index.data(PTZListModel::IsLockedRole).toBool();
}

/* Where the camera is, if the device reads it back. Zoom is 0 if it doesn't
 * report that. */
bool PTZViewControl::readPosition(const QModelIndex &index, double &pan, double &tilt, double &zoom) const
{
	OBSDataAutoRelease state = obs_data_create();
	ptzDeviceList->saveState(index, state.Get());
	if (!obs_data_has_user_value(state, "pan") || !obs_data_has_user_value(state, "tilt"))
		return false;
	pan = obs_data_get_double(state, "pan");
	tilt = obs_data_get_double(state, "tilt");
	zoom = obs_data_get_double(state, "zoom");
	return true;
}

/* One calibration measurement: pressing on a point dx, dy source pixels from
 * the middle of the picture and dragging it there moved the camera by pan and
 * tilt, in the units of the movement API, at zoom */
struct ViewCalibration {
	double pan;
	double tilt;
	double zoom;
	int dx;
	int dy;
};

/* Measured on an OBSBOT Tail Air, in order of zoom */
static constexpr ViewCalibration viewCalibration[] = {
	{-0.040, 0.163, 0.300, -442, -412},
	{-0.023, 0.106, 0.600, -468, -476},
	{-0.014, 0.056, 1.000, -452, -479},
};

bool quadratic_coefficients(double x1, double y1, double x2, double y2, double x3, double y3, double *a, double *b,
			    double *c)
{
	double d1 = (x1 - x2) * (x1 - x3);
	double d2 = (x2 - x1) * (x2 - x3);
	double d3 = (x3 - x1) * (x3 - x2);

	// A quadratic cannot be uniquely determined if
	// any two x-coordinates are equal.
	if (x1 == x2 || x1 == x3 || x2 == x3)
		return false;

	*a = y1 / d1 + y2 / d2 + y3 / d3;

	*b = -y1 * (x2 + x3) / d1 - y2 * (x1 + x3) / d2 - y3 * (x1 + x2) / d3;

	*c = y1 * x2 * x3 / d1 + y2 * x1 * x3 / d2 + y3 * x1 * x2 / d3;

	return true;
}

void PTZViewControl::setSource(OBSSource source)
{
	if (source.Get() == this->source().Get())
		return;
	PTZSourceDisplay::setSource(source);
	m_dragging = false;
	m_wheelDelta = 0;
	m_lastZoom.invalidate();

	// Development work
	if (std::size(viewCalibration) >= 3) {
		quadratic_coefficients(viewCalibration[0].zoom, viewCalibration[0].pan / viewCalibration[0].dx,
				       viewCalibration[1].zoom, viewCalibration[1].pan / viewCalibration[1].dx,
				       viewCalibration[2].zoom, viewCalibration[2].pan / viewCalibration[2].dx, &pa,
				       &pb, &pc);

		quadratic_coefficients(viewCalibration[0].zoom, viewCalibration[0].tilt / viewCalibration[0].dy,
				       viewCalibration[1].zoom, viewCalibration[1].tilt / viewCalibration[1].dy,
				       viewCalibration[2].zoom, viewCalibration[2].tilt / viewCalibration[2].dy, &ta,
				       &tb, &tc);
	}
}

void PTZViewControl::resizeEvent(QResizeEvent *event)
{
	PTZSourceDisplay::resizeEvent(event);
	m_pixelRatio = (float)devicePixelRatioF();
}

/* A drag can't go on while it can't be seen */
void PTZViewControl::hideEvent(QHideEvent *event)
{
	PTZSourceDisplay::hideEvent(event);
	m_dragging = false;
}

void PTZViewControl::mousePressEvent(QMouseEvent *event)
{
	QWidget::mousePressEvent(event);
	if (event->button() != Qt::LeftButton)
		return;
	auto index = device();
	if (!index.isValid() || liveLocked(index) || !PTZListModel::hasFeature(index, "pantilt_abs"))
		return;
	if (!readPosition(index, m_panStart, m_tiltStart, m_dragZoom))
		return;
	m_dragging = true;
	m_dragMoved = false;
	m_dragStart = m_dragPos = event->position();
	/* What the readouts show the difference from, until it is let go */
	m_pressSource = sourcePixel(event->position());
	updateReadout(index);
	updateCursorReadout();
}

void PTZViewControl::mouseMoveEvent(QMouseEvent *event)
{
	QWidget::mouseMoveEvent(event);
	m_cursorPos = event->position();
	m_cursorOver = true;
	updateCursorReadout();
	if (!m_dragging)
		return;
	m_dragPos = event->position();
	m_dragMoved = true;
}

/* Moves the camera so that what was double clicked is in the middle of the
 * picture: as far as dragging it there would, at this zoom. Not passed on, so
 * that it doesn't start a drag as a second press. */
void PTZViewControl::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	m_dragging = false;
	auto index = device();
	if (!index.isValid() || liveLocked(index) || !PTZListModel::hasFeature(index, "pantilt_abs"))
		return;
	std::optional<QPoint> clicked = sourcePixel(event->position());
	OBSSource source = this->source();
	double pan, tilt, zoom;
	if (!clicked || !source || !readPosition(index, pan, tilt, zoom))
		return;

	/* From where it was clicked to the middle, in the source's pixels, as
	 * the calibration has them */
	double dx = obs_source_get_width(source) / 2.0 - clicked->x();
	double dy = obs_source_get_height(source) / 2.0 - clicked->y();
	double panPerPixel = pa * zoom * zoom + pb * zoom + pc;
	double tiltPerPixel = ta * zoom * zoom + tb * zoom + tc;
	pan = std::clamp(pan + dx * panPerPixel, -1.0, 1.0);
	tilt = std::clamp(tilt + dy * tiltPerPixel, -1.0, 1.0);

	calldata_t cd = {};
	calldata_set_float(&cd, "pan", pan);
	calldata_set_float(&cd, "tilt", tilt);
	ptzDeviceList->callDevice(index, "ptz_move_abs", &cd);
	calldata_free(&cd);
}

void PTZViewControl::leaveEvent(QEvent *event)
{
	QWidget::leaveEvent(event);
	m_cursorOver = false;
	updateCursorReadout();
}

void PTZViewControl::mouseReleaseEvent(QMouseEvent *event)
{
	QWidget::mouseReleaseEvent(event);
	if (event->button() != Qt::LeftButton || !m_dragging)
		return;
	/* Send where it was let go, which the last tick may not have */
	m_dragPos = event->position();
	m_dragMoved = true;
	tick();
	copyReadout();
	m_dragging = false;
	updateReadout(device());
	updateCursorReadout();
}

/* For pasting where the movement is worked out, separated by tabs: pan, tilt
 * and zoom as the readout shows them as it is let go, then X and Y from where
 * the mouse was pressed to the middle of the source, in its pixels */
void PTZViewControl::copyReadout()
{
	auto number = [](std::optional<double> value) {
		return value ? QString::asprintf("%.3f", *value) : QString();
	};
	QString x, y;
	OBSSource source = this->source();
	if (source && m_pressSource) {
		x = QString::number((int)obs_source_get_width(source) / 2 - m_pressSource->x());
		y = QString::number((int)obs_source_get_height(source) / 2 - m_pressSource->y());
	}
	QGuiApplication::clipboard()->setText(
		QStringList{number(m_shownPan), number(m_shownTilt), number(m_shownZoom), x, y}.join('\t'));
}

void PTZViewControl::wheelEvent(QWheelEvent *event)
{
	m_wheelDelta += event->angleDelta().y();
	event->accept();
}

/* Where the camera says it is pointing, in the units of the movement API,
 * each value always as wide as -x.xxx so that none of them moves; while the
 * mouse is pressed, pan and tilt are how far they are from where they were
 * when it was */
void PTZViewControl::updateReadout(const QModelIndex &index)
{
	OBSDataAutoRelease state = obs_data_create();
	if (index.isValid())
		ptzDeviceList->saveState(index, state.Get());
	auto read = [&](const char *key, double from) -> std::optional<double> {
		if (!obs_data_has_user_value(state, key))
			return std::nullopt;
		return obs_data_get_double(state, key) - from;
	};
	/* Pan and tilt from where they were when the mouse was pressed */
	m_shownPan = read("pan", m_dragging ? m_panStart : 0.0);
	m_shownTilt = read("tilt", m_dragging ? m_tiltStart : 0.0);
	m_shownZoom = read("zoom", 0.0);
	auto value = [](std::optional<double> value) {
		return value ? QString::asprintf("% .3f", *value) : QString(6, ' ');
	};
	QString text = QString("%1:%2 %3:%4 %5:%6")
			       .arg("Pan", value(m_shownPan), "Tilt", value(m_shownTilt), "Zoom", value(m_shownZoom));
	setText(m_positionReadout, text);
}

/* Where `pos` is on the picture, in the source's pixels: 0, 0 at its top left
 * and the source's width and height at its bottom right. Nothing when it
 * isn't over the picture. */
std::optional<QPoint> PTZViewControl::sourcePixel(const QPointF &pos) const
{
	OBSSource source = this->source();
	const QRectF rect = sourceRect();
	if (!source || rect.isEmpty() || !rect.contains(pos))
		return std::nullopt;
	/* The last pixel across is the source's width, the last down its height */
	double sourceCx = obs_source_get_width(source);
	double sourceCy = obs_source_get_height(source);
	double across = std::max(1.0, rect.width() - 1.0), down = std::max(1.0, rect.height() - 1.0);
	return QPoint((int)std::lround(std::clamp((pos.x() - rect.left()) / across, 0.0, 1.0) * sourceCx),
		      (int)std::lround(std::clamp((pos.y() - rect.top()) / down, 0.0, 1.0) * sourceCy));
}

/* Where the cursor is on the picture, in the source's pixels, blank when it
 * isn't over it; while the mouse is pressed, how far it is from where it was
 * pressed */
void PTZViewControl::updateCursorReadout()
{
	std::optional<QPoint> at = m_cursorOver ? sourcePixel(m_cursorPos) : std::nullopt;
	if (at && m_dragging)
		at = m_pressSource ? std::optional<QPoint>(*at - *m_pressSource) : std::nullopt;
	m_shownX = at ? std::optional<int>(at->x()) : std::nullopt;
	m_shownY = at ? std::optional<int>(at->y()) : std::nullopt;
	auto value = [](std::optional<int> value) {
		return value ? QString::asprintf("%5d", *value) : QString(5, ' ');
	};
	setText(m_cursorReadout, QString("X:%1, Y:%2").arg(value(m_shownX), value(m_shownY)));
}

/* Paints `text` with Qt into a new texture for `overlay`, if it changed. Drawn
 * by OBS so that it can be over the picture with nothing behind it: a widget
 * can't be over a display. */
void PTZViewControl::setText(TextOverlay &overlay, const QString &text)
{
	/* Text as tall as the dock's own, in display pixels */
	int size = (int)(QFontInfo(font()).pixelSize() * devicePixelRatioF());
	if (text == overlay.text && size == overlay.size)
		return;
	overlay.text = text;
	overlay.size = size;

	QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	mono.setPixelSize(size);
	QFontMetrics metrics(mono);
	QImage image(std::max(1, metrics.horizontalAdvance(text)), std::max(1, metrics.height()),
		     QImage::Format_ARGB32);
	image.fill(Qt::transparent);
	{
		QPainter painter(&image);
		painter.setFont(mono);
		painter.setPen(Qt::white);
		painter.drawText(0, metrics.ascent(), text);
	}

	/* Format_ARGB32 is B, G, R, A in memory, each row exactly as wide as the
	 * image. Swapped while the graphics thread can't be drawing it. */
	const uint8_t *bits = image.constBits();
	obs_enter_graphics();
	gs_texture_t *texture = gs_texture_create(image.width(), image.height(), GS_BGRA, 1, &bits, 0);
	if (overlay.texture)
		gs_texture_destroy(overlay.texture);
	overlay.texture = texture;
	obs_leave_graphics();
}

void PTZViewControl::tick()
{
	auto index = device();
	updateReadout(index);
	/* The picture can move under a cursor that doesn't */
	updateCursorReadout();
	if (!index.isValid() || liveLocked(index)) {
		m_wheelDelta = 0;
		m_dragging = false;
		return;
	}

	if (m_wheelDelta && PTZListModel::hasFeature(index, "zoom_abs")) {
		if (!m_lastZoom.isValid() || m_lastZoom.elapsed() > zoomSettleMs) {
			OBSDataAutoRelease state = obs_data_create();
			ptzDeviceList->saveState(index, state.Get());
			if (!obs_data_has_user_value(state, "zoom")) {
				m_wheelDelta = 0;
				return;
			}
			m_zoomTarget = obs_data_get_double(state, "zoom");
		}
		/* 120 is one notch of an ordinary wheel */
		m_zoomTarget = std::clamp(m_zoomTarget + m_wheelDelta / 120.0 * zoomPerNotch, 0.0, 1.0);
		m_wheelDelta = 0;
		m_lastZoom.start();

		calldata_t cd = {};
		calldata_set_float(&cd, "zoom", m_zoomTarget);
		ptzDeviceList->callDevice(index, "ptz_move_abs", &cd);
		calldata_free(&cd);
	}

	if (m_dragging && m_dragMoved) {
		m_dragMoved = false;
		QRectF rect = sourceRect();
		OBSSource source = this->source();
		if (rect.isEmpty() || !source)
			return;
		/* How far the mouse moved, in the source's pixels, scaled as the
		 * cursor readout has them: Y is down, as it is in the source */
		double dx = (m_dragPos.x() - m_dragStart.x()) * obs_source_get_width(source) /
			    std::max(1.0, rect.width() - 1.0);
		double dy = (m_dragPos.y() - m_dragStart.y()) * obs_source_get_height(source) /
			    std::max(1.0, rect.height() - 1.0);
		/* Pan and tilt for each pixel at this zoom, from the calibration */
		double z = m_dragZoom;
		double panPerPixel = pa * z * z + pb * z + pc;
		double tiltPerPixel = ta * z * z + tb * z + tc;
		double pan = std::clamp(m_panStart + dx * panPerPixel, -1.0, 1.0);
		double tilt = std::clamp(m_tiltStart + dy * tiltPerPixel, -1.0, 1.0);

		calldata_t cd = {};
		calldata_set_float(&cd, "pan", pan);
		calldata_set_float(&cd, "tilt", tilt);
		ptzDeviceList->callDevice(index, "ptz_move_abs", &cd);
		calldata_free(&cd);
	}
}
