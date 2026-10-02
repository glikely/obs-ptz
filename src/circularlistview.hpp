/* Circular list view widget
 *
 * Copyright 2023 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2+
 */
#pragma once
#include <QListView>

class CircularListView : public QListView {
	Q_OBJECT

public:
	CircularListView(QWidget *parent = nullptr) : QListView(parent) {};

public slots:
	void cursorUp();
	void cursorDown();

protected:
	QModelIndex moveCursor(QAbstractItemView::CursorAction action, Qt::KeyboardModifiers modifiers);
};
