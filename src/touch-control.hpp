/* Touch control widget
 *
 * Copyright 2023 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QWidget>

class TouchControl : public QWidget {
	Q_OBJECT

public:
	TouchControl(QWidget *parent = Q_NULLPTR,
		     Qt::WindowFlags f = Qt::WindowFlags());
	double x() const;
	double y() const;

signals:
	void positionChanged(double x, double y);

private:
	double m_x;
	double m_y;
	double m_inner_radius;
	double m_outer_radius;

	void setPosition(QPointF pos);
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
};
