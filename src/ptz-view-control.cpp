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
#include <QWindow>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif
#include "ptz-view-control.hpp"
#include <qt-wrappers.hpp>
#include "ptz-controls.hpp"
#include "ptz-list-model.hpp"

/* How often moves are sent, and how much one notch of the wheel zooms */
static constexpr int tickMs = 100;
static constexpr double zoomPerNotch = 0.1;
/* After this long without the wheel, zoom from where the camera says it is
 * rather than from where it was last sent */
static constexpr qint64 zoomSettleMs = 1000;

static bool toGSWindow(QWindow *window, gs_window &gswindow)
{
#ifdef _WIN32
	gswindow.hwnd = (void *)window->winId();
#elif defined(__APPLE__)
	gswindow.view = (id)window->winId();
#else
	/* Wayland needs Qt's private native interface; not supported here */
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL)
		return false;
	gswindow.id = window->winId();
	gswindow.display = obs_get_nix_platform_display();
#endif
	return true;
}

PTZViewControl::PTZViewControl(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	/* Or the dock it is in becomes a native window too */
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	/* Moves without a button down too, for the cursor readout */
	setMouseTracking(true);
	connect(&m_timer, &QTimer::timeout, this, &PTZViewControl::tick);
	m_timer.start(tickMs);
}

PTZViewControl::~PTZViewControl()
{
	if (m_display) {
		obs_display_remove_draw_callback(m_display, drawCallback, this);
		obs_display_destroy(m_display);
		m_display = nullptr;
	}
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

/* Runs on the graphics thread */
void PTZViewControl::drawCallback(void *param, uint32_t cx, uint32_t cy)
{
	auto view = static_cast<PTZViewControl *>(param);
	OBSSourceAutoRelease source;
	{
		QMutexLocker locker(&view->m_sourceMutex);
		source = obs_weak_source_get_source(view->m_source);
	}
	if (!source)
		return;

	uint32_t sourceCx = obs_source_get_width(source);
	uint32_t sourceCy = obs_source_get_height(source);
	if (!sourceCx || !sourceCy)
		return;

	/* Fit the source in the display, keeping its aspect, centered */
	float scale = std::min((float)cx / sourceCx, (float)cy / sourceCy);
	int width = (int)(sourceCx * scale);
	int height = (int)(sourceCy * scale);

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport((cx - width) / 2, (cy - height) / 2, width, height);
	gs_ortho(0.0f, (float)sourceCx, 0.0f, (float)sourceCy, -100.0f, 100.0f);
	obs_source_video_render(source);
	gs_viewport_pop();
	gs_projection_pop();

	drawOverlay(cx, cy, (cx - width) / 2, (cy - height) / 2, width, height, view->m_pixelRatio);
	/* The position just below the picture and the cursor just above it, or
	 * as near as they fit in the display */
	const int x = (cx - width) / 2, y = (cy - height) / 2;
	const int gap = (int)(2 * view->m_pixelRatio);
	if (gs_texture_t *text = view->m_positionReadout.texture)
		drawText(text, cx, cy, x, width,
			 std::min(y + height + gap, (int)cy - (int)gs_texture_get_height(text)));
	if (gs_texture_t *text = view->m_cursorReadout.texture)
		drawText(text, cx, cy, x, width, std::max(y - gap - (int)gs_texture_get_height(text), 0));
}

void PTZViewControl::createDisplay()
{
	if (m_display)
		return;
	winId();
	QWindow *window = windowHandle();
	if (!window)
		return;

	m_pixelRatio = (float)devicePixelRatioF();
	QSize size = this->size() * devicePixelRatioF();
	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	if (!toGSWindow(window, info.window))
		return;

	m_display = obs_display_create(&info, 0);
	if (m_display)
		obs_display_add_draw_callback(m_display, drawCallback, this);
}

QRectF PTZViewControl::sourceRect() const
{
	OBSSourceAutoRelease source;
	{
		QMutexLocker locker(&m_sourceMutex);
		source = obs_weak_source_get_source(m_source);
	}
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
	OBSSourceAutoRelease source;
	{
		QMutexLocker locker(&m_sourceMutex);
		source = obs_weak_source_get_source(m_source);
	}
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

void PTZViewControl::trace(const QString &what)
{
	if (what == m_lastTrace)
		return;
	m_lastTrace = what;
	blog(LOG_INFO, "[ptz] view control: %s", QT_TO_UTF8(what));
}

static QString describe(const QModelIndex &index)
{
	OBSDataAutoRelease state = obs_data_create();
	ptzDeviceList->saveState(index, state.Get());
	auto has = [&](const char *key) {
		return obs_data_has_user_value(state, key) ? QString::number(obs_data_get_double(state, key))
							   : QString("none");
	};
	return QString("device '%1' connected=%2 live=%3 locked=%4 pantilt_abs=%5 zoom_abs=%6 pan=%7 tilt=%8 zoom=%9")
		.arg(index.data(Qt::DisplayRole).toString())
		.arg(obs_data_get_bool(state, "connected"))
		.arg(obs_data_get_bool(state, "live"))
		.arg(obs_data_get_bool(state, "locked"))
		.arg(PTZListModel::hasFeature(index, "pantilt_abs"))
		.arg(PTZListModel::hasFeature(index, "zoom_abs"))
		.arg(has("pan"), has("tilt"), has("zoom")) +
	       QString(" pan_pos=%1 zoom_pos=%2 statistics=%3")
		       .arg(has("pan_pos"), has("zoom_pos"))
		       .arg(QString::fromUtf8(obs_data_get_json(OBSDataAutoRelease(obs_data_get_obj(state, "statistics")))));
}

void PTZViewControl::setSource(obs_source_t *source)
{
	QMutexLocker locker(&m_sourceMutex);
	if (source ? obs_weak_source_references_source(m_source, source) : !m_source)
		return;
	m_source = OBSGetWeakRef(source);
	m_dragging = false;
	m_wheelDelta = 0;
	m_lastZoom.invalidate();
}

void PTZViewControl::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	m_pixelRatio = (float)devicePixelRatioF();
	if (m_display) {
		QSize size = this->size() * devicePixelRatioF();
		obs_display_resize(m_display, size.width(), size.height());
	}
}

void PTZViewControl::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	createDisplay();
	if (m_display)
		obs_display_set_enabled(m_display, true);
}

/* Nothing to draw for while it can't be seen, as on another tab */
void PTZViewControl::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (m_display)
		obs_display_set_enabled(m_display, false);
	m_dragging = false;
}

void PTZViewControl::mousePressEvent(QMouseEvent *event)
{
	QWidget::mousePressEvent(event);
	if (event->button() != Qt::LeftButton)
		return;
	auto index = device();
	if (!index.isValid()) {
		trace("press: no device for this source");
		return;
	}
	trace("press: " + describe(index));
	if (liveLocked(index) || !PTZListModel::hasFeature(index, "pantilt_abs"))
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
	OBSSourceAutoRelease source;
	{
		QMutexLocker locker(&m_sourceMutex);
		source = obs_weak_source_get_source(m_source);
	}
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
	trace(QString("wheel: %1").arg(event->angleDelta().y()));
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
			       .arg(obs_module_text("PTZ.Device.State.Pan"), value(m_shownPan),
				    obs_module_text("PTZ.Device.State.Tilt"), value(m_shownTilt),
				    obs_module_text("PTZ.Device.State.Zoom"), value(m_shownZoom));
	setText(m_positionReadout, text);
}

/* Where `pos` is on the picture, in the source's pixels: 0, 0 at its top left
 * and the source's width and height at its bottom right. Nothing when it
 * isn't over the picture. */
std::optional<QPoint> PTZViewControl::sourcePixel(const QPointF &pos) const
{
	OBSSourceAutoRelease source;
	{
		QMutexLocker locker(&m_sourceMutex);
		source = obs_weak_source_get_source(m_source);
	}
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
		painter.setPen(QColor(0xc0, 0xc0, 0xc0));
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
	if ((m_wheelDelta || m_dragging) && !index.isValid())
		trace("tick: no device for this source");
	else if ((m_wheelDelta || m_dragging) && liveLocked(index))
		trace("tick: live locked");
	else if (m_wheelDelta && !PTZListModel::hasFeature(index, "zoom_abs"))
		trace("tick: no zoom_abs; " + describe(index));
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
				trace("tick: no zoom position; " + describe(index));
				m_wheelDelta = 0;
				return;
			}
			m_zoomTarget = obs_data_get_double(state, "zoom");
		}
		/* 120 is one notch of an ordinary wheel */
		m_zoomTarget = std::clamp(m_zoomTarget + m_wheelDelta / 120.0 * zoomPerNotch, 0.0, 1.0);
		m_wheelDelta = 0;
		m_lastZoom.start();

		trace(QString("tick: zoom to %1").arg(m_zoomTarget));
		calldata_t cd = {};
		calldata_set_float(&cd, "zoom", m_zoomTarget);
		ptzDeviceList->callDevice(index, "ptz_move_abs", &cd);
		calldata_free(&cd);
	}

	if (m_dragging && m_dragMoved) {
		m_dragMoved = false;
		QRectF rect = sourceRect();
		if (rect.isEmpty()) {
			trace("tick: source has no size");
			return;
		}
		/* The camera turns the way the mouse moves, by as much of its
		 * field of view as the mouse moved across the picture */
		double magnification = 1.0 + (m_config.maxMagnification - 1.0) * m_dragZoom;
		double dx = (m_dragPos.x() - m_dragStart.x()) / rect.width();
		double dy = (m_dragStart.y() - m_dragPos.y()) / rect.height();
		double pan = std::clamp(m_panStart + dx * m_config.panPerWidth / magnification, -1.0, 1.0);
		double tilt = std::clamp(m_tiltStart + dy * m_config.tiltPerHeight / magnification, -1.0, 1.0);

		trace(QString("tick: pan/tilt to %1, %2").arg(pan).arg(tilt));
		calldata_t cd = {};
		calldata_set_float(&cd, "pan", pan);
		calldata_set_float(&cd, "tilt", tilt);
		ptzDeviceList->callDevice(index, "ptz_move_abs", &cd);
		calldata_free(&cd);
	}
}
