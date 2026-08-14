/* Pan Tilt Zoom camera controls
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QToolButton>
#include <QResizeEvent>

/**
 * class SquareToolButton - QToolButton whose icon scales with its size
 *
 * sizeHint()/minimumSizeHint() are pinned to a small constant rather than
 * the QStyle-computed value (which factors in iconSize()), so growing the
 * icon here can never itself grow the button's layout-imposed size: doing
 * that would let the enlarged icon size raise sizeHint, prompting more
 * layout space, which grows the icon again, without bound.
 */
class SquareToolButton : public QToolButton {
public:
	explicit SquareToolButton(QWidget *parent = nullptr) : QToolButton(parent) {}

	QSize sizeHint() const override { return minimumSizeHint(); }
	QSize minimumSizeHint() const override { return QSize(16, 16); }

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QToolButton::resizeEvent(event);
		int side = qMax(16, qMin(event->size().width(), event->size().height()) * 3 / 4);
		setIconSize(QSize(side, side));
	}
};
