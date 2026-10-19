/* PTZ view control: a source's picture, that moves its camera
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <obs.hpp>
#include <QElapsedTimer>
#include <QModelIndex>
#include <QMutex>
#include <QPointF>
#include <QTimer>
#include <QWidget>

/* How much of the camera's travel the picture spans at its widest, in the
 * units of the movement API (pan and tilt each span 2.0), and how many times
 * narrower it is at full zoom. Guesses, not measured on any camera. */
struct PTZViewControlConfig {
	double panPerWidth = 0.41;
	double tiltPerHeight = 0.44;
	double maxMagnification = 4.0;
};

/* Shows a source, and moves the camera of the PTZ device that has it as its
 * source: drag to point the camera, and the wheel to zoom. Needs a device
 * that reads its position back and can move to one. */
class PTZViewControl : public QWidget {
	Q_OBJECT

public:
	PTZViewControl(QWidget *parent = nullptr);
	~PTZViewControl();
	void setSource(obs_source_t *source);
	/* Drawn by OBS, not Qt */
	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

private:
	static void drawCallback(void *param, uint32_t cx, uint32_t cy);
	void createDisplay();
	/* Where the source is drawn, in widget coordinates */
	QRectF sourceRect() const;
	QModelIndex device() const;
	bool liveLocked(const QModelIndex &index) const;
	bool readPosition(const QModelIndex &index, double &pan, double &tilt, double &zoom) const;
	void tick();
	/* Temporary: logs what the view control did, or why it did nothing */
	void trace(const QString &what);
	QString m_lastTrace;

	obs_display_t *m_display = nullptr;
	/* Read by the draw callback, on the graphics thread */
	mutable QMutex m_sourceMutex;
	OBSWeakSource m_source;
	PTZViewControlConfig m_config;

	/* Moves are sent at most once a tick, so a drag doesn't queue up more
	 * than the camera can do */
	QTimer m_timer;
	bool m_dragging = false;
	bool m_dragMoved = false;
	QPointF m_dragStart;
	QPointF m_dragPos;
	double m_panStart = 0.0;
	double m_tiltStart = 0.0;
	double m_dragZoom = 0.0;
	int m_wheelDelta = 0;
	double m_zoomTarget = 0.0;
	QElapsedTimer m_lastZoom;
};
